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
}
occlusion;
layout(push_constant, std430) uniform Parameters {
	uint references;
	uint buckets;
	uint entries_word;
	uint capacity;
	uint scatter;
	uint phase;
}
params;
shared uint pipeline_ids[64];
shared bool draw_cluster;

bool vg_cluster_visible(uvec2 reference) {
	if (occlusion.flags.x == 0u) {
		return true;
	}
	uint record = vg_record(reference.y);
	vec3 lower = uintBitsToFloat(uvec3(geometry.data[record + 18u], geometry.data[record + 19u], geometry.data[record + 20u]));
	vec3 upper = uintBitsToFloat(uvec3(geometry.data[record + 21u], geometry.data[record + 22u], geometry.data[record + 23u]));
	mat4 world = vg_source_matrix(reference.x, 0u);
	float displacement = uintBitsToFloat(scene.data[vg_source_word(reference.x) + 48u]);
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

void main() {
	uint index = gl_WorkGroupID.x + gl_WorkGroupID.y * 65535u;
	if (index >= params.references) {
		return;
	}
	uvec2 reference = vg_visible_reference(index);
	if (reference.x == 0xffffffffu || reference.y == 0xffffffffu) {
		return;
	}
	uint lane = gl_LocalInvocationIndex;
	if (lane == 0u) {
		if (params.phase == 0u) {
			draw_cluster = vg_cluster_visible(reference);
			if (params.scatter == 0u) {
				pending.data[index] = draw_cluster ? 0u : 1u;
			}
		} else {
			draw_cluster = pending.data[index] != 0u && vg_cluster_visible(reference);
		}
	}
	barrier();
	if (!draw_cluster) {
		return;
	}
	uint record = vg_record(reference.y);
	uint count = geometry.data[record + 6u];
	uint pipeline = 0xffffffffu;
	if (count == 0u || count > 64u || reference.x >= (scene.data[1] - scene.data[0]) / 64u) {
		atomicOr(status.data[0], 4u);
		return; // Same admitted reference and condition for the complete group.
	}
	if (lane < count) {
		uint palette = (reference.y >> 8u) * 8192u + (geometry.data[record + 68u] >> 2u);
		uint slot = geometry.data[palette + lane];
		uint source = vg_source_word(reference.x);
		if (slot >= scene.data[source + 52u]) {
			atomicOr(status.data[0], 4u);
		} else {
			uint material = scene.data[scene.data[2] + scene.data[source + 47u] + slot];
			if (material >= scene.data[3]) {
				atomicOr(status.data[0], 4u);
			} else {
				pipeline = materials.data[material * 4u + 2u] == 0u ? 0u : material + 1u;
			}
		}
	}
	pipeline_ids[lane] = pipeline;
	barrier();
	if (pipeline == 0xffffffffu || pipeline >= params.buckets) {
		return;
	}
	for (uint previous = 0u; previous < lane; ++previous) {
		if (pipeline_ids[previous] == pipeline) {
			return;
		}
	}
	if (params.scatter == 0u) {
		atomicAdd(candidates.data[2u + pipeline], 1u);
	} else {
		uint offset = candidates.data[2u + params.buckets + pipeline] + atomicAdd(candidates.data[2u + params.buckets * 2u + pipeline], 1u);
		if (offset < params.capacity) {
			candidates.data[params.entries_word + offset] = index;
		} else {
			atomicOr(candidates.data[1], 1u);
			atomicOr(status.data[0], 4u);
		}
	}
}
