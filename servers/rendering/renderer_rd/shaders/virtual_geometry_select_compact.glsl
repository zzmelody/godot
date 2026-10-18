#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 3, std140) uniform Parameters { uvec4 data[16]; } params;
layout(set = 0, binding = 4, std430) readonly buffer Work { uint data[]; } work;
layout(set = 0, binding = 6, std430) buffer Result { uint data[]; } result;
layout(push_constant, std430) uniform Phase { uint root_only; uint result_word; } phase;

void main() {
	uint id = (gl_WorkGroupID.x + gl_WorkGroupID.y * 65535u) * 64u + gl_LocalInvocationIndex;
	uint instances = params.data[13].x;
	uvec4 limits = params.data[14];
	uint fine = 16u + instances * 16u;
	uint roots = fine + limits.y * 2u;
	if (phase.root_only != 0u) {
		if (id == 0u) {
			result.data[phase.result_word + 4u] |= 1u;
			result.data[phase.result_word + 7u] = (result.data[phase.result_word + 4u] & 24u) != 0u ? 0u : result.data[phase.result_word + 6u];
			result.data[phase.result_word + 11u] = roots;
		}
		return;
	}
	uint source_fine = fine + limits.x * 4u + limits.y * 2u;
	uint source_roots = source_fine + limits.y * 2u;
	if (id < 16u) {
		result.data[phase.result_word + id] = id == 11u ? (work.data[11] == source_roots ? roots : fine) : work.data[id];
	}
	if (id < instances * 16u) {
		result.data[phase.result_word + 16u + id] = work.data[16u + id];
	}
	if (id < min(work.data[3], limits.y) * 2u) {
		result.data[phase.result_word + fine + id] = work.data[source_fine + id];
	}
	if (id < min(work.data[6], limits.z) * 2u) {
		result.data[phase.result_word + roots + id] = work.data[source_roots + id];
	}
}
