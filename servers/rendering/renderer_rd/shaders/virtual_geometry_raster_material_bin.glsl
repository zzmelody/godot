#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 1, std430) readonly buffer Geometry {
	uint data[];
}
geometry;
#define VG_MATERIAL_LOOKUP
#include "virtual_geometry_scene_inc.glsl"
#define VG_GLOBAL_RASTER
layout(set = 0, binding = 3, std140) uniform ViewParameters {
	mat4 model;
	mat4 world_to_view;
	mat4 projection;
	vec4 lod;
	uvec4 source;
	uvec4 limits;
	uvec4 offsets;
}
params;
#include "virtual_geometry_view_inc.glsl"
layout(set = 0, binding = 9, std430) buffer Candidates {
	uint data[];
}
candidates;
layout(set = 0, binding = 11, std430) readonly buffer Materials {
	uint data[];
}
materials;
layout(set = 0, binding = 15, std430) buffer Status {
	uint data[];
}
status;
layout(set = 0, binding = 10, std430) buffer Pending {
	uint data[];
}
pending;
layout(set = 0, binding = 30) uniform sampler2D hierarchy;
layout(set = 0, binding = 31, std140) uniform Occlusion {
	mat4 world_to_clip;
	uvec4 flags;
	vec4 raster_view; // jitter xy and raster dimensions zw
}
occlusion;
#include "virtual_geometry_raster_bounds_inc.glsl"
layout(push_constant, std430) uniform Parameters {
	uint references;
	uint buckets;
	uint entries_word;
	uint capacity;
	uint scatter;
	uint phase;
}
bin_info;
layout(set = 0, binding = 28, std430) readonly buffer SourceTasks {
	uint data[];
}
source_tasks;

bool vg_cluster_visible(uvec2 reference) {
	if (occlusion.flags.x == 0u) {
		return true;
	}
	uint record = vg_record(reference.y);
	vec3 lower = uintBitsToFloat(uvec3(geometry.data[record + 18u], geometry.data[record + 19u], geometry.data[record + 20u]));
	vec3 upper = uintBitsToFloat(uvec3(geometry.data[record + 21u], geometry.data[record + 22u], geometry.data[record + 23u]));
	mat4 world = vg_source_matrix(reference.x, 0u);
	float displacement = uintBitsToFloat(vg_source_data(reference.x, 48u));
	vec3 world_min = vec3(3.402823e38), world_max = vec3(-3.402823e38);
	for (uint corner = 0u; corner < 8u; ++corner) {
		vec3 point = mix(lower, upper, bvec3((corner & 1u) != 0u, (corner & 2u) != 0u, (corner & 4u) != 0u));
		vec3 position = (world * vec4(point, 1.0)).xyz;
		world_min = min(world_min, position);
		world_max = max(world_max, position);
	}
	world_min -= vec3(displacement);
	world_max += vec3(displacement);
	vec2 rect_min = vec2(1.0), rect_max = vec2(-1.0);
	float nearest = 0.0;
	for (uint corner = 0u; corner < 8u; ++corner) {
		vec3 point = mix(world_min, world_max, bvec3((corner & 1u) != 0u, (corner & 2u) != 0u, (corner & 4u) != 0u));
		vec4 clip = occlusion.world_to_clip * vec4(point, 1.0);
		if (any(isnan(clip)) || any(isinf(clip)) || clip.w <= 0.0 || clip.z >= clip.w) {
			return true;
		}
		vec3 ndc = clip.xyz / clip.w;
		rect_min = min(rect_min, ndc.xy);
		rect_max = max(rect_max, ndc.xy);
		nearest = max(nearest, ndc.z);
	}
	vec2 base = vec2(textureSize(hierarchy, 0));
	vec2 lo_uv = clamp(rect_min * 0.5 + 0.5 - 1.0 / base, vec2(0.0), vec2(1.0));
	vec2 hi_uv = clamp(rect_max * 0.5 + 0.5 + 1.0 / base, vec2(0.0), vec2(1.0));
	float extent = max((hi_uv.x - lo_uv.x) * base.x, (hi_uv.y - lo_uv.y) * base.y);
	int level = clamp(int(ceil(log2(max(extent, 1.0)))), 0, textureQueryLevels(hierarchy) - 1);
	ivec2 size = textureSize(hierarchy, level);
	ivec2 lo = clamp(ivec2(floor(lo_uv * vec2(size))), ivec2(0), size - 1);
	ivec2 hi = clamp(ivec2(floor(hi_uv * vec2(size))), ivec2(0), size - 1);
	for (int y = lo.y; y <= hi.y; ++y) {
		for (int x = lo.x; x <= hi.x; ++x) {
			if (nearest + 0.00001 >= texelFetch(hierarchy, ivec2(x, y), level).r) {
				return true;
			}
		}
	}
	return false;
}

uint vg_palette_pipeline(uint source, uint palette, uint entry, uint triangles, out bool custom_depth, out bool fused) {
	custom_depth = false;
	fused = false;
	uint slot = geometry.data[palette + entry];
	if (slot >= vg_source_data(source, 52u)) {
		atomicOr(status.data[0], 4u);
		return 0xffffffffu;
	}
	uint material = scene.data[scene.data[2] + vg_source_data(source, 47u) + slot];
	if (material >= scene.data[3]) {
		atomicOr(status.data[0], 4u);
		return 0xffffffffu;
	}
	uint flags = materials.data[material * 4u + 2u];
	custom_depth = (flags & 2u) != 0u;
	fused = (flags & 4u) != 0u;
	// Only opaque programmable materials have the fused specialization.
	// Each reference still enters exactly one bucket per distinct material.
	return flags == 0u ? 0u : material + 1u + (((flags & 4u) != 0u && triangles > 64u) ? scene.data[3] : 0u);
}

uint vg_candidate_pipeline(uint index, uint pipeline, bool fused) {
	if ((pending.data[index] & 4u) != 0u && fused) {
		uint material = pipeline > scene.data[3] ? pipeline - scene.data[3] - 1u : pipeline - 1u;
		uint owner = materials.data[scene.data[3] * 4u + material];
		if (owner >= scene.data[3] || (materials.data[owner * 4u + 2u] & 4u) == 0u || materials.data[scene.data[3] * 4u + owner] != owner) {
			atomicOr(status.data[0], 4u);
			return 0xffffffffu;
		}
		return owner + 1u + scene.data[3] * 2u;
	}
	return pipeline;
}

void vg_emit_candidate(uint index, uint pipeline) {
	if (pipeline >= bin_info.buckets) {
		return;
	}
	if (bin_info.scatter == 0u) {
		atomicAdd(candidates.data[2u + pipeline], 1u);
	} else {
		uint offset = candidates.data[2u + bin_info.buckets + pipeline] + atomicAdd(candidates.data[2u + bin_info.buckets * 2u + pipeline], 1u);
		if (offset < bin_info.capacity) {
			candidates.data[bin_info.entries_word + offset] = index;
		} else {
			atomicOr(candidates.data[1], 1u);
			atomicOr(status.data[0], 4u);
		}
	}
}

void main() {
	uint index = (gl_WorkGroupID.x + gl_WorkGroupID.y * 65535u) * 64u + gl_LocalInvocationIndex;
	if (index >= min(bin_info.references, source_tasks.data[0])) {
		return;
	}
	// Each view owns this buffer. Count records visibility once; scatter uses
	// that decision with the same geometry, transform and Hi-Z version.
	// Recovery visits only primary-occluded entries and records recovered = 2.
	if (bin_info.phase != 0u) {
		if ((pending.data[index] & 3u) != (bin_info.scatter == 0u ? 1u : 2u)) {
			return;
		}
	} else if (bin_info.scatter != 0u) {
		if ((pending.data[index] & 3u) != 0u) {
			return;
		}
	} else {
		pending.data[index] = 1u;
	}
	uvec2 reference = vg_visible_reference(index);
	if (reference.x == 0xffffffffu || reference.y == 0xffffffffu) {
		return;
	}
	uint record = vg_record(reference.y);
	uint triangles = geometry.data[record + 5u];
	uint count = geometry.data[record + 6u];
	if (count == 0u || count > 64u || reference.x >= (scene.data[1] - scene.data[0]) / 48u) {
		atomicOr(status.data[0], 4u);
		return;
	}
	uint palette = (reference.y >> 8u) * 8192u + (geometry.data[record + 68u] >> 2u);
	uint first_pipeline = 0xffffffffu;
	bool first_fused = false;
	bool same_pipeline = true;
	bool custom_depth = false;
	bool has_fused = false;
	for (uint entry = 0u; entry < count; ++entry) {
		bool entry_depth;
		bool entry_fused;
		uint pipeline = vg_palette_pipeline(reference.x, palette, entry, triangles, entry_depth, entry_fused);
		if (pipeline == 0xffffffffu) {
			return;
		}
		if (entry == 0u) {
			first_pipeline = pipeline;
			first_fused = entry_fused;
		} else {
			same_pipeline = same_pipeline && pipeline == first_pipeline;
		}
		custom_depth = custom_depth || entry_depth;
		has_fused = has_fused || entry_fused;
	}
	if (bin_info.scatter == 0u) {
		// Absolute DEPTH may pull a geometrically hidden surface forward.
		// Its original bounds cannot prove occlusion in either Hi-Z phase.
		bool visible = custom_depth || vg_cluster_visible(reference);
		pending.data[index] = visible ? (bin_info.phase == 0u ? 0u : 2u) : 1u;
		if (!visible) {
			return;
		}
		// Count proves this once. Scatter reuses the decision and the same
		// source/view generation, including primary versus recovered state.
		if (has_fused && vg_bounds_prove_software(reference, record)) {
			pending.data[index] |= 4u;
		}
	}
	// The common one-pipeline case needs no per-invocation palette array.
	if (same_pipeline) {
		vg_emit_candidate(index, vg_candidate_pipeline(index, first_pipeline, first_fused));
		return;
	}
	for (uint entry = 0u; entry < count; ++entry) {
		bool entry_depth;
		bool entry_fused;
		uint pipeline = vg_palette_pipeline(reference.x, palette, entry, triangles, entry_depth, entry_fused);
		pipeline = vg_candidate_pipeline(index, pipeline, entry_fused);
		bool duplicate = false;
		for (uint previous = 0u; previous < entry; ++previous) {
			bool previous_depth, previous_fused;
			uint previous_pipeline = vg_palette_pipeline(reference.x, palette, previous, triangles, previous_depth, previous_fused);
			if (vg_candidate_pipeline(index, previous_pipeline, previous_fused) == pipeline) {
				duplicate = true;
				break;
			}
		}
		if (!duplicate) {
			vg_emit_candidate(index, pipeline);
		}
	}
}
