#[compute]

#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

#VERSION_DEFINES

#include "virtual_geometry_quad_inc.glsl"
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 1, std430) readonly buffer Geometry {
	uint data[];
}
geometry;
#include "virtual_geometry_attributes_inc.glsl"
#include "virtual_geometry_scene_inc.glsl"
layout(set = 0, binding = 9, std430) buffer Shading {
	uint data[];
}
shading;
layout(set = 0, binding = 16, rgba16f) uniform writeonly image2D vg_albedo;
layout(set = 0, binding = 17, rgba16f) uniform writeonly image2D vg_normal;
layout(set = 0, binding = 18, rgba16f) uniform writeonly image2D vg_emission;
layout(set = 0, binding = 19, rgba16f) uniform writeonly image2D vg_specular;
layout(set = 0, binding = 20, rgba16f) uniform writeonly image2D vg_backlight;
layout(set = 0, binding = 21, rgba16f) uniform writeonly image2D vg_anisotropy;
layout(set = 0, binding = 22, r32ui) uniform writeonly uimage2D vg_flags;
layout(set = 0, binding = 23, rgba16f) uniform writeonly image2D vg_tangent;
layout(set = 0, binding = 24, rgba16f) uniform writeonly image2D vg_geo_normal;
layout(set = 0, binding = 25, rg16f) uniform writeonly image2D vg_velocity;
layout(set = 0, binding = 29, rg16f) uniform writeonly image2D vg_lightmap_uv;
layout(set = 0, binding = 27, r32f) uniform writeonly image2D vg_reactivity;
layout(set = 0, binding = 30, std430) readonly buffer PreviousKeys {
	uvec4 data[];
}
previous_keys;
layout(set = 0, binding = 31, std430) writeonly buffer CurrentKeys {
	uvec4 data[];
}
current_keys;
layout(push_constant, std430) uniform Parameters {
	uint material;
	uint materials;
	uint entries_word;
}
params;

#define VG_GLOBALS_BINDING 11
#define VG_MATERIAL_INDEX params.material
#include "virtual_geometry_material_vertex_inc.glsl"
#define VG_EVALUATE_VERTEX vg_evaluate_vertex
#include "virtual_geometry_surface_inc.glsl"
#define VG_COMPUTE_FRAGMENT
#include "virtual_geometry_material_fragment_inc.glsl"
#include "virtual_geometry_motion_inc.glsl"

void main() {
	uint quad, lane;
	if (!vg_quad_address(quad, lane)) {
		atomicOr(shading.data[1], 4u);
		return;
	}
	if (atomicAdd(shading.data[1], 0u) != 0u || atomicAdd(vg_status.data[0], 0u) != 0u || quad >= shading.data[2u + params.material]) {
		return;
	}
	uint index = shading.data[2u + params.materials + params.material] + quad;
	uint word = params.entries_word + index * 3u;
	uint origin = shading.data[word];
	uint payload = shading.data[word + 1u];
	uvec2 point = uvec2(origin % uint(vg_view.dimensions.x), origin / uint(vg_view.dimensions.x)) + uvec2(lane & 1u, lane >> 1u);
	vg_setup_vertex(vg_visible_reference((payload >> 7u) - 1u).x);
	vg_killed = false;
	VGSurface surface;
	bool valid = vg_surface(payload, vec2(point) + 0.5, projection_matrix, vg_view.dimensions.xy, vg_view.dimensions.zw, surface);
	bool complete = subgroupQuadBroadcast(valid, 0u) && subgroupQuadBroadcast(valid, 1u) && subgroupQuadBroadcast(valid, 2u) && subgroupQuadBroadcast(valid, 3u);
	if (!complete) {
		atomicOr(shading.data[1], 8u);
		return;
	}
	vg_evaluate_fragment(surface, vec2(point) + 0.5);
	vec3 normal = normalize(normal_highp);
#ifdef NORMAL_MAP_USED
	normal_map.xy = normal_map.xy * 2.0 - 1.0;
	normal_map.z = sqrt(max(0.0, 1.0 - dot(normal_map.xy, normal_map.xy)));
	normal = normalize(mix(normal, tangent * normal_map.x + binormal * normal_map.y + normal * normal_map.z, normal_map_depth));
#endif
	// Helpers execute all material code. Only covered lanes publish a surface.
	if ((shading.data[word + 2u] & (1u << lane)) == 0u || vg_killed) {
		return;
	}
	ivec2 pixel = ivec2(point);
	imageStore(vg_albedo, pixel, vec4(albedo_highp, metallic_highp));
	imageStore(vg_normal, pixel, vec4(normal, roughness_highp));
	imageStore(vg_emission, pixel, vec4(emission, ao));
	imageStore(vg_specular, pixel, vec4(specular, rim, rim_tint, clearcoat));
	imageStore(vg_backlight, pixel, vec4(backlight, ao_light_affect));
	imageStore(vg_anisotropy, pixel, vec4(clearcoat_roughness, anisotropy, anisotropy_flow));
	vec3 geometric_normal = normalize(normal_highp);
	float frame_sign = dot(cross(geometric_normal, tangent), binormal) < 0.0 ? -1.0 : 1.0;
	imageStore(vg_tangent, pixel, vec4(normalize(tangent), frame_sign));
	imageStore(vg_geo_normal, pixel, vec4(geometric_normal, 0.0));
	imageStore(vg_lightmap_uv, pixel, vec4(uv2_interp, 0.0, 0.0));
	uint flags = vg_source + 1u;
	if (vg_history_view.state.y != 0.0) {
		vec2 velocity = vg_motion(payload, surface);
		imageStore(vg_velocity, pixel, vec4(velocity, 0.0, 0.0));
		uvec2 source_id = uvec2(vg_source_data(vg_source, 60u), vg_source_data(vg_source, 61u));
		uvec2 mesh_id = uvec2(vg_source_data(vg_source, 62u), vg_source_data(vg_source, 63u));
		// Temporal tags are hashes, never object or asset authority.
		uvec2 tag = (source_id ^ (mesh_id.yx * uvec2(0x9e3779b9u, 0x85ebca6bu))) * uvec2(0xc2b2ae35u, 0x27d4eb2fu);
		uvec2 reference = vg_visible_reference((payload >> 7u) - 1u);
		// Physical slots can be reused for another page between frames. The
		// model's logical cluster identity survives relocation and distinguishes
		// replacement geometry even when its physical address is identical.
		uint cluster = geometry.data[vg_record(reference.y)];
		uvec4 key = uvec4(tag, vg_source_data(vg_source, 44u), cluster);
		uint index = point.y * uint(vg_view.dimensions.x) + point.x;
		current_keys.data[index] = key;
		vec2 previous_point = vec2(point) + 0.5 + velocity * vg_view.dimensions.xy + (vg_history_view.state.zw - vg_view.dimensions.zw) * 0.5 * vg_view.dimensions.xy;
		bool stable = vg_view.flags.z != 0u && all(greaterThanEqual(previous_point, vec2(0.0))) && all(lessThan(previous_point, vg_view.dimensions.xy));
		if (stable) {
			uvec2 previous_pixel = uvec2(previous_point);
			stable = all(equal(previous_keys.data[previous_pixel.y * uint(vg_view.dimensions.x) + previous_pixel.x], key));
		}
		if (!stable) {
			flags |= 1u << 25u;
		}
		imageStore(vg_reactivity, pixel, vec4(stable ? 0.0 : 1.0));
	}
#ifdef MODE_UNSHADED
	flags |= 1u << 24u;
#endif
	imageStore(vg_flags, pixel, uvec4(flags));
}
