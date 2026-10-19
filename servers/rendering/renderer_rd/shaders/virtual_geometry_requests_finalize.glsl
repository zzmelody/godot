#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 1, std430) buffer Requests { uint data[]; } requests;
layout(set = 0, binding = 2, std430) readonly buffer PageState { uint data[]; } page_state;
layout(push_constant, std430) uniform Parameters {
	uint capacity;
	uint missing_capacity;
	uint pages;
} params;
void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= min(requests.data[0], min(params.capacity, params.missing_capacity))) { return; }
	uint page = requests.data[4u + index * 2u];
	if (page >= params.pages) { atomicOr(requests.data[3], 32u); return; }
	requests.data[5u + index * 2u] = page_state.data[page * 2u + 1u];
}
