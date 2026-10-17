#[vertex]

#version 450

#VERSION_DEFINES

/* clang-format off */
#include "virtual_geometry_raster_inc.glsl"
#include "virtual_geometry_raster_cull_inc.glsl"
/* clang-format on */

layout(location = 0) flat out uint pixel_value;
layout(location = 1) out float shadow_clip;

void main() {
#ifdef VG_GLOBAL_RASTER
	if (atomicOr(vg_status.data[0], 0u) != 0u) {
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		pixel_value = 0u;
		shadow_clip = -1.0;
		return;
	}
	uint index = vg_raster_hardware(uint(gl_InstanceIndex));
#else
	uint index = bins.data[4u + uint(gl_InstanceIndex)];
#endif
	uvec2 selected = vg_selected(index);
	uint triangle = uint(gl_VertexIndex) / 3u;
	pixel_value = vg_pixel_payload(index, triangle);
	if (triangle >= geometry.data[vg_record(selected.y) + 5u] || !vg_raster_material_active(selected, triangle)) {
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		return;
	}
	uvec3 vertices = vg_triangle(selected.y, triangle);
	vec3 point = vg_raster_position(selected.x, selected.y, vertices[uint(gl_VertexIndex) % 3u]);
	vec4 clip = vg_project(selected.x, point);
	shadow_clip = vg_shadow_clip(selected.x, point);
	gl_Position = vg_snap_clip(clip, vec2(raster_info.raster.xy));
}

#[fragment]

#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_shader_atomic_int64 : require

#VERSION_DEFINES

layout(push_constant, std430) uniform RasterParameters {
	uvec4 raster;
	vec2 jitter;
	uint source_base;
	uint material;
}
raster_info;
layout(location = 0) flat in uint pixel_value;
layout(location = 1) in float shadow_clip;

#ifdef VG_RASTER_MATERIALS
layout(set = 0, binding = 1, std430) readonly buffer Geometry {
	uint data[];
}
geometry;
#endif
#include "virtual_geometry_raster_cull_inc.glsl"
#include "virtual_geometry_write_inc.glsl"

#ifdef VG_MASKED
#include "virtual_geometry_attributes_inc.glsl"
#define VG_GLOBALS_BINDING 14
#define VG_MATERIAL_INDEX raster_info.material
#include "virtual_geometry_material_vertex_inc.glsl"
#include "virtual_geometry_raster_coverage_inc.glsl"
#endif

void main() {
	if (shadow_clip > 0.0) {
		discard;
	}
	if (vg_cull_material(pixel_value, gl_FrontFacing)) {
		discard;
	}
#ifdef VG_MASKED
	if (!vg_raster_coverage(pixel_value, gl_FragCoord.xy)) {
		discard;
	}
#endif
	vg_write_pixel(ivec2(gl_FragCoord.xy), gl_FragCoord.z, pixel_value);
}
