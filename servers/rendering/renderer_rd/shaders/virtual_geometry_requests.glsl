#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0, std430) readonly buffer Work {
	uint data[];
}
work;
layout(set = 0, binding = 1, std430) writeonly buffer Requests {
	uint data[];
}
requests;
layout(push_constant, std430) uniform Parameters {
	uint source_word;
	uint source_capacity;
	uint capacity;
}
params;

void main() {
	uint index = gl_GlobalInvocationID.x;
	uint available = (work.data[4] & 16u) == 0u ? min(work.data[5], params.source_capacity) : 0u;
	uint missing = min(available, params.capacity * 3u / 4u);
	uint hits = (work.data[4] & 16u) == 0u ? min(work.data[12], min(128u, params.capacity - missing)) : 0u;
	uint count = missing + hits;
	if (index == 0u) {
		requests.data[0] = count;
		requests.data[1] = work.data[5] - missing;
		requests.data[2] = work.data[4];
		requests.data[3] = 0u;
	}
	if (index < count) {
		uint source = index < missing ? params.source_word + index * 2u : params.source_word + params.source_capacity * 2u + (index - missing) * 2u;
		requests.data[4u + index * 2u] = work.data[source];
		requests.data[5u + index * 2u] = work.data[source + 1u];
	}
}
