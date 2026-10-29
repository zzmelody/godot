#[compute]

#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_shader_atomic_int64 : require

#VERSION_DEFINES

#define VG_BIN_BUILD
#define VG_SCENE_WRITE
#define VG_MATERIAL_LOOKUP
#if defined(VG_FUSED_SOFTWARE) && defined(VG_GLOBAL_RASTER)
layout(constant_id = 1) const bool vg_direct_software = false;
#endif
/* clang-format off */
#include "virtual_geometry_raster_inc.glsl"
#include "virtual_geometry_raster_cull_inc.glsl"
#include "virtual_geometry_scene_inc.glsl"
/* clang-format on */

#ifdef VG_FUSED_SOFTWARE
/* clang-format off */
#include "virtual_geometry_write_inc.glsl"
#include "virtual_geometry_raster_triangle_inc.glsl"
/* clang-format on */
shared bool group_software;
// Specialization removes the second prepared triangle from small-cluster
// pipelines. GPU material bins admit only clusters of at most 64 triangles.
layout(constant_id = 0) const bool vg_small_cluster = false;
#endif

struct VGPreparedTriangle {
	vec4 clip0;
	vec4 clip1;
	vec4 clip2;
	vec3 shadow;
	bool has_triangle;
};

VGPreparedTriangle vg_classify_triangle(uvec2 selected, uint count, uint triangle, inout uint edge_bits, inout uint hardware) {
	VGPreparedTriangle prepared;
	prepared.has_triangle = triangle < count;
	if (!prepared.has_triangle) {
		return prepared;
	}
	prepared.has_triangle = vg_raster_material_active(selected, triangle);
	if (!prepared.has_triangle) {
		return prepared;
	}
	uvec3 vertices = vg_triangle(selected.y, triangle);
	vec3 points[3];
	vec4 clips[3];
	vec2 lower = vec2(3.402823e38);
	vec2 upper = vec2(-3.402823e38);
	for (uint corner = 0u; corner < 3u; ++corner) {
		points[corner] = vg_raster_position(selected.x, selected.y, vertices[corner]);
		vec4 clip = vg_project(selected.x, points[corner]);
		clips[corner] = clip;
#ifdef VG_FUSED_SOFTWARE
		prepared.shadow[corner] = vg_shadow_clip(selected.x, points[corner]);
#endif
		if (clip.w <= 0.0 || clip.z <= 0.0 || clip.z >= clip.w || any(isnan(clip)) || any(isinf(clip))) {
			hardware = 1u;
		} else {
			vec2 pixel = vg_screen(clip);
			lower = min(lower, pixel);
			upper = max(upper, pixel);
			if (any(greaterThan(abs(pixel), vec2(1048576.0)))) {
				hardware = 1u;
			}
		}
	}
	prepared.clip0 = clips[0];
	prepared.clip1 = clips[1];
	prepared.clip2 = clips[2];
	float edge = max(max(length(points[1] - points[0]), length(points[2] - points[1])), length(points[0] - points[2]));
	edge_bits = max(edge_bits, floatBitsToUint(edge));
	// Any clipped or large triangle routes the complete cluster to hardware.
	if (any(greaterThan(upper - lower, vec2(63.0)))) {
		hardware = 1u;
	}
	return prepared;
}

// Programmable vertex evaluation retains its measured 64-lane layout.
// Fixed-position decoding uses one 32-lane group per cluster.
#if defined(VG_PROGRAMMABLE) || defined(VG_FUSED_SOFTWARE)
#define VG_BIN_GROUP_SIZE 64
#else
#define VG_BIN_GROUP_SIZE 32
#endif
layout(local_size_x = VG_BIN_GROUP_SIZE, local_size_y = 1, local_size_z = 1) in;
shared uint maximum_edge;
shared uint needs_hardware;
shared uint active_triangles;
#ifdef VG_GLOBAL_RASTER
shared bool group_refused;
#endif

void main() {
	uint index = gl_WorkGroupID.x + gl_WorkGroupID.y * 65535u;
#ifdef VG_GLOBAL_RASTER
	if (index >= vg_raster_bucket_count() || work.data[1] != 0u) {
		return;
	}
	index = vg_raster_input(index);
#else
	if (index >= work.data[7]) {
		return;
	}
#endif
	uint lane = gl_LocalInvocationIndex;
	uvec2 selected = vg_selected(index);
	uint record = vg_record(selected.y);
#if defined(VG_FUSED_SOFTWARE) && defined(VG_GLOBAL_RASTER)
	if (vg_direct_software) {
		// No group reduction or barrier follows this specialized path. Each
		// invocation may refuse independently, including a partial final group.
		if (atomicOr(vg_status.data[0], 0u) != 0u) {
			return;
		}
		uint count = geometry.data[record + 5u];
		if (lane < count) {
			vg_raster_triangle(index, selected, lane, 0u);
		}
		if (!vg_small_cluster && lane + 64u < count) {
			vg_raster_triangle(index, selected, lane + 64u, 0u);
		}
		return;
	}
#endif
	if (lane == 0u) {
#ifdef VG_GLOBAL_RASTER
		// Other groups can set this flag during the dispatch. Sample it once
		// for the whole group so no invocation abandons the later barriers.
		group_refused = atomicOr(vg_status.data[0], 0u) != 0u;
#endif
		maximum_edge = 0u;
		needs_hardware = 0u;
		active_triangles = 0u;
#ifdef VG_FUSED_SOFTWARE
		group_software = false;
#endif
	}
	barrier();
#ifdef VG_GLOBAL_RASTER
	if (group_refused) {
		return;
	}
#endif
	uint lane_maximum_edge = 0u;
	uint lane_needs_hardware = 0u;
	bool lane_active = false;
	uint count = geometry.data[record + 5u];
#ifdef VG_FUSED_SOFTWARE
	// Admitted clusters have at most 128 triangles. Two explicit values avoid
	// dynamically indexed per-lane storage across the classification barrier.
	VGPreparedTriangle first = vg_classify_triangle(selected, count, lane, lane_maximum_edge, lane_needs_hardware);
	VGPreparedTriangle second;
	second.has_triangle = false;
	if (!vg_small_cluster) {
		second = vg_classify_triangle(selected, count, lane + 64u, lane_maximum_edge, lane_needs_hardware);
	}
	lane_active = first.has_triangle || second.has_triangle;
#else
	for (uint triangle = lane; triangle < count; triangle += VG_BIN_GROUP_SIZE) {
		VGPreparedTriangle prepared = vg_classify_triangle(selected, count, triangle, lane_maximum_edge, lane_needs_hardware);
		lane_active = lane_active || prepared.has_triangle;
	}
#endif
	if (lane_active) {
		atomicOr(active_triangles, 1u);
		atomicMax(maximum_edge, lane_maximum_edge);
	}
	if (lane_needs_hardware != 0u) {
		atomicOr(needs_hardware, lane_needs_hardware);
	}
	barrier();
#ifndef VG_GLOBAL_RASTER
	if (lane == 0u) {
		uint visible_word = scene.data[1] + (raster_info.raster.z + index) * 2u;
		scene.data[visible_word] = raster_info.source_base + selected.x;
		scene.data[visible_word + 1u] = selected.y;
	}
#endif
	if (lane == 0u && active_triangles != 0u) {
		vec4 sphere = uintBitsToFloat(uvec4(geometry.data[record + 12u], geometry.data[record + 13u], geometry.data[record + 14u], geometry.data[record + 15u]));
#ifdef VG_GLOBAL_RASTER
		float scale = uintBitsToFloat(vg_source_data(selected.x, 50u));
#else
		float scale = uintBitsToFloat(work.data[instance_base() + selected.x * 16u + 13u]);
#endif
		// UE SmallEnoughToDraw hardware threshold, default MinPixelsPerEdgeHW=32.
		// Prototype derives maximum edge from decoded geometry on the GPU;
		// it is not a per-cluster CPU draw or a stored UE half-float field.
		bool hardware = needs_hardware != 0u || projected_edge(selected.x, sphere, instance_transform(selected.x)) < scale * uintBitsToFloat(maximum_edge) * params.lod.y / 32.0;
#ifdef VG_FUSED_SOFTWARE
		group_software = !hardware;
		// Software clusters finish in this dispatch. Only hardware work is
		// published to the existing bounded queue and indirect arguments.
		if (hardware) {
#endif
			uint count_word = hardware ? 0u : 1u;
#ifdef VG_GLOBAL_RASTER
			count_word += vg_raster_bin_header();
			uint position = atomicAdd(bins.data[count_word], 1u);
			if (position < vg_raster_bucket_count()) {
				bins.data[vg_raster_bin_base() + (hardware ? 0u : vg_raster_bucket_count()) + position] = index;
			} else {
				atomicOr(bins.data[vg_raster_bin_header() + 2u], 1u);
				atomicOr(vg_status.data[0], 4u);
			}
#else
		uint position = atomicAdd(bins.data[count_word], 1u);
		if (position < raster_info.raster.w) {
			bins.data[4u + (hardware ? 0u : raster_info.raster.w) + position] = index;
		} else {
			atomicOr(bins.data[2], 1u);
		}
#endif
#ifdef VG_FUSED_SOFTWARE
		}
#endif
	}
#ifdef VG_FUSED_SOFTWARE
#ifndef VG_GLOBAL_RASTER
	// The local interface publishes its visibility lookup in this dispatch.
	memoryBarrierBuffer();
#endif
	barrier();
	if (group_software) {
		if (first.has_triangle) {
			vg_raster_prepared_triangle(index, lane, 0u, first.clip0, first.clip1, first.clip2, first.shadow);
		}
		if (!vg_small_cluster && second.has_triangle) {
			vg_raster_prepared_triangle(index, lane + 64u, 0u, second.clip0, second.clip1, second.clip2, second.shadow);
		}
	}
#endif
}
