#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0, std430) readonly buffer Work { uint data[]; } work;
// Two fixed sections reserve missing-page capacity against use-hit traffic.
// Header: missing count, hit count, overflow, source diagnostics.
layout(set = 0, binding = 1, std430) buffer Requests { uint data[]; } requests;
// Per logical page: observed-kind bits and maximum missing-page priority.
layout(set = 0, binding = 2, std430) buffer PageState { uint data[]; } page_state;
layout(push_constant, std430) uniform Parameters {
	uint source_word;
	uint source_capacity;
	uint capacity;
	uint header_word;
	uint pages;
} params;

uint reserve_record(uint word, uint capacity) {
	uint previous = atomicOr(requests.data[word], 0u);
	for (;;) {
		if (previous >= capacity) { atomicOr(requests.data[2], 1u); return 0xffffffffu; }
		uint observed = atomicCompSwap(requests.data[word], previous, previous + 1u);
		if (observed == previous) { return previous; }
		previous = observed;
	}
}

void append_page(uint page, uint priority, bool hit) {
	float value = uintBitsToFloat(priority);
	if (page >= params.pages || isnan(value) || isinf(value) || (!hit && value < 0.0)) {
		atomicOr(requests.data[3], 32u);
		return;
	}
	uint bit = hit ? 2u : 1u;
	if (!hit) { atomicMax(page_state.data[page * 2u + 1u], value == 0.0 ? 0u : priority); }
	if ((atomicOr(page_state.data[page * 2u], bit) & bit) != 0u) { return; }
	uint missing_capacity = params.capacity * 3u / 4u;
	uint record = reserve_record(hit ? 1u : 0u, hit ? params.capacity - missing_capacity : missing_capacity);
	if (record == 0xffffffffu) { return; }
	if (hit) { record += missing_capacity; }
	requests.data[4u + record * 2u] = page;
	// Finalization reads the maximum after every view has contributed.
	requests.data[5u + record * 2u] = hit ? floatBitsToUint(-1.0) : 0u;
}

void main() {
	uint index = gl_GlobalInvocationID.x;
	uint flags = work.data[params.header_word + 4u];
	uint observed_missing = work.data[params.header_word + 5u];
	if (index == 0u) {
		atomicOr(requests.data[3], flags);
		if ((flags & 4u) != 0u || observed_missing > params.source_capacity) { atomicOr(requests.data[2], 1u); }
	}
	if ((flags & 16u) != 0u) { return; }
	if (index < min(observed_missing, params.source_capacity)) {
		uint word = params.source_word + index * 2u;
		append_page(work.data[word], work.data[word + 1u], false);
	}
	if (index < min(work.data[params.header_word + 12u], 128u)) {
		uint word = params.source_word + params.source_capacity * 2u + index * 2u;
		append_page(work.data[word], floatBitsToUint(-1.0), true);
	}
}
