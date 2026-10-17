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
layout(push_constant, std430) uniform Parameters {
	uint material;
	uint materials;
	uint entries_word;
}
params;

#define VG_GLOBALS_BINDING 11
#define VG_MATERIAL_INDEX params.material
#include "virtual_geometry_material_vertex_inc.glsl"
void vg_user_fragment() {
#CODE : FRAGMENT
}
#define VG_EVALUATE_VERTEX vg_evaluate_vertex
#include "virtual_geometry_surface_inc.glsl"

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
	vec3 b = surface.barycentrics.value;
#CODE : VIRTUAL_GEOMETRY_VARYINGS
	vertex = surface.view_position;
	normal_highp = mat3(vg_view.view) * surface.normal;
	tangent = mat3(vg_view.view) * surface.tangent;
	binormal = mat3(vg_view.view) * surface.binormal;
	vg_front_facing = surface.barycentrics.front_facing;
#ifdef DO_SIDE_CHECK
	if (!vg_front_facing) {
		normal_highp = -normal_highp;
	}
#endif
	view_highp = -normalize(vertex);
	uv_interp = surface.uv[0];
	uv2_interp = surface.uv[1];
	color_interp = surface.color;
	vg_fragcoord = vec4(vec2(point) + 0.5, surface.depth, surface.reciprocal_w);
	albedo_highp = vec3(1.0);
	alpha_highp = 1.0;
	metallic_highp = 0.0;
	roughness_highp = 1.0;
	specular = 0.5;
	emission = vec3(0.0);
	backlight = vec3(0.0);
	normal_map = vec3(0.5);
	normal_map_depth = 1.0;
	ao = 1.0;
	ao_light_affect = 0.0;
	rim = 0.0;
	rim_tint = 0.0;
	clearcoat = 0.0;
	clearcoat_roughness = 0.0;
	anisotropy = 0.0;
	anisotropy_flow = vec2(1.0, 0.0);
	vg_user_fragment();
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
	uint flags = vg_source + 1u;
#ifdef MODE_UNSHADED
	flags |= 1u << 24u;
#endif
	imageStore(vg_flags, pixel, uvec4(flags));
}
