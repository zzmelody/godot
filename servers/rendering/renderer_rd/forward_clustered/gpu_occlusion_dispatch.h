// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>

namespace RendererRD::GpuOcclusionDispatch {

constexpr uint32_t WORKGROUP_SIZE = 64;
constexpr uint32_t PARAMETER_BYTES = 128;
struct Address {
	uint32_t low = 0;
	uint32_t high = 0;
};
static_assert(sizeof(Address) == 8);
constexpr uint64_t MAX_METADATA_BYTES = 8 * 1024 * 1024;

struct Source {
	uint32_t group;
	uint32_t count;
};

struct Range {
	uint32_t offset = 0;
	uint32_t count = 0;
	uint32_t cursor = 0;
};

struct Task {
	uint32_t batch;
	uint32_t first_instance;
};
static_assert(sizeof(Task) == 8);

inline uint32_t task_count(uint32_t p_instances) {
	return p_instances / WORKGROUP_SIZE + (p_instances % WORKGROUP_SIZE != 0);
}

// Counts are bounded by GpuOcclusion::MAX_CANDIDATES before reaching this helper.
inline uint32_t capacity_for(uint32_t p_count) {
	uint32_t capacity = p_count == 0 ? 0 : p_count - 1;
	capacity |= capacity >> 1;
	capacity |= capacity >> 2;
	capacity |= capacity >> 4;
	capacity |= capacity >> 8;
	capacity |= capacity >> 16;
	return capacity + 1;
}

inline uint64_t metadata_bytes(uint32_t p_parameter_capacity, uint32_t p_task_capacity, bool p_addresses) {
	return uint64_t(p_parameter_capacity) * (PARAMETER_BYTES * 2 + (p_addresses ? sizeof(Address) : 0)) + uint64_t(p_task_capacity) * sizeof(Task);
}

inline bool fits_budget(uint32_t p_batches, uint32_t p_tasks, uint32_t p_parameter_capacity, uint32_t p_task_capacity, bool p_addresses) {
	uint32_t parameters = capacity_for(p_batches);
	uint32_t tasks = capacity_for(p_tasks);
	parameters = parameters > p_parameter_capacity ? parameters : p_parameter_capacity;
	tasks = tasks > p_task_capacity ? tasks : p_task_capacity;
	return metadata_bytes(parameters, tasks, p_addresses) <= MAX_METADATA_BYTES;
}

// Caller owns all arrays. Sources retain original batch order; only tasks move.
// Every group occupies one contiguous range, including non-adjacent sources.
inline uint32_t build_tasks(const Source *p_sources, uint32_t p_source_count, Range *r_ranges, uint32_t p_group_count, Task *r_tasks) {
	for (uint32_t group = 0; group < p_group_count; ++group) {
		r_ranges[group] = {};
	}
	for (uint32_t batch = 0; batch < p_source_count; ++batch) {
		r_ranges[p_sources[batch].group].count += task_count(p_sources[batch].count);
	}
	uint32_t total = 0;
	for (uint32_t group = 0; group < p_group_count; ++group) {
		r_ranges[group].offset = total;
		r_ranges[group].cursor = total;
		total += r_ranges[group].count;
	}
	for (uint32_t batch = 0; batch < p_source_count; ++batch) {
		Range &range = r_ranges[p_sources[batch].group];
		for (uint32_t first = 0; first < p_sources[batch].count; first += WORKGROUP_SIZE) {
			r_tasks[range.cursor++] = { batch, first };
		}
	}
	return total;
}

inline uint32_t dispatch_count(uint32_t p_remaining, uint32_t p_limit) {
	return p_remaining < p_limit ? p_remaining : p_limit;
}

} // namespace RendererRD::GpuOcclusionDispatch
