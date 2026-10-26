#[compute]

#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_shader_atomic_int64 : require

#VERSION_DEFINES

#ifdef VG_MASKED
#include "virtual_geometry_quad_inc.glsl"
#endif
/* clang-format off */
#include "virtual_geometry_raster_inc.glsl"
#include "virtual_geometry_raster_cull_inc.glsl"
#include "virtual_geometry_write_inc.glsl"
/* clang-format on */

#ifdef VG_MASKED
#define VG_COMPUTE_FRAGMENT
#include "virtual_geometry_raster_coverage_inc.glsl"
#endif
#include "virtual_geometry_raster_triangle_inc.glsl"

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

#ifdef VG_GLOBAL_RASTER
shared bool group_refused;
#endif

void vg_raster_triangle(uint index, uvec2 selected, uint triangle, uint lane) {
	if (!vg_raster_material_active(selected, triangle)) {
		return;
	}
	uvec3 vertices = vg_triangle(selected.y, triangle);
	vec4 clip[3];
	vec3 shadow_clips;
	for (uint corner = 0u; corner < 3u; ++corner) {
		vec3 point = vg_raster_position(selected.x, selected.y, vertices[corner]);
		clip[corner] = vg_project(selected.x, point);
		shadow_clips[corner] = vg_shadow_clip(selected.x, point);
	}
	vg_raster_prepared_triangle(index, triangle, lane, clip[0], clip[1], clip[2], shadow_clips);
}

void main() {
#ifdef VG_MASKED
	uint quad, lane;
	if (!vg_quad_address(quad, lane)) {
		atomicOr(vg_status.data[0], 2u);
		return;
	}
#endif
	uint job = gl_WorkGroupID.x + gl_WorkGroupID.y * 65535u;
#ifdef VG_GLOBAL_RASTER
	if (job >= bins.data[vg_raster_bin_header() + 1u] || bins.data[vg_raster_bin_header() + 2u] != 0u) {
		return;
	}
	// A changing view status must never make only some lanes skip a barrier.
	if (gl_LocalInvocationIndex == 0u) {
		group_refused = atomicOr(vg_status.data[0], 0u) != 0u;
	}
	barrier();
	if (group_refused) {
		return;
	}
	uint index = vg_raster_software(job);
#else
	if (job >= bins.data[1] || bins.data[2] != 0u) {
		return;
	}
	uint index = bins.data[4u + raster_info.raster.w + job];
#endif
	uvec2 selected = vg_selected(index);
	uint count = geometry.data[vg_record(selected.y) + 5u];
#ifdef VG_MASKED
	for (uint triangle = quad % 16u; triangle < count; triangle += 16u) {
		vg_raster_triangle(index, selected, triangle, lane);
	}
#else
	for (uint triangle = gl_LocalInvocationIndex; triangle < count; triangle += 64u) {
		vg_raster_triangle(index, selected, triangle, 0u);
	}
#endif
}
