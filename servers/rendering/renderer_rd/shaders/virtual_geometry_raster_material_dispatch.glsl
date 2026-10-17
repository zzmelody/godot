#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 4, std430) readonly buffer Candidates {
	uint data[];
}
candidates;
layout(set = 0, binding = 5, std430) readonly buffer Bins {
	uint data[];
}
bins;
layout(set = 0, binding = 7, std430) writeonly buffer Arguments {
	uint data[];
}
arguments;
layout(set = 0, binding = 15, std430) readonly buffer Status {
	uint data[];
}
status;
layout(push_constant, std430) uniform Parameters {
	uint mode;
	uint buckets;
	uint entries_word;
	uint capacity;
}
params;

void main() {
	uint item = (gl_WorkGroupID.x + gl_WorkGroupID.y * 65535u) * 64u + gl_LocalInvocationIndex;
	if (params.mode != 0u && item != 0u) {
		return;
	}
	uint bucket = params.mode == 0u ? item : params.entries_word;
	if (bucket >= params.buckets) {
		return;
	}
	uint count = candidates.data[2u + bucket];
	uint output_word = bucket * 12u;
	bool valid = status.data[0] == 0u && candidates.data[1] == 0u && candidates.data[0] <= params.capacity;
	if (params.mode == 0u) {
		arguments.data[output_word] = valid ? min(65535u, count) : 0u;
		arguments.data[output_word + 1u] = max(1u, (count + 65534u) / 65535u);
		arguments.data[output_word + 2u] = 1u;
		arguments.data[output_word + 3u] = 0u;
	} else {
		uint hardware = bins.data[bucket * 4u];
		uint software = bins.data[bucket * 4u + 1u];
		valid = valid && bins.data[bucket * 4u + 2u] == 0u && hardware <= count && software <= count - hardware;
		arguments.data[output_word + 4u] = valid ? min(65535u, software) : 0u;
		arguments.data[output_word + 5u] = max(1u, (software + 65534u) / 65535u);
		arguments.data[output_word + 6u] = 1u;
		arguments.data[output_word + 7u] = 0u;
		arguments.data[output_word + 8u] = valid ? 384u : 0u;
		arguments.data[output_word + 9u] = valid ? hardware : 0u;
		arguments.data[output_word + 10u] = 0u;
		arguments.data[output_word + 11u] = 0u;
	}
}
