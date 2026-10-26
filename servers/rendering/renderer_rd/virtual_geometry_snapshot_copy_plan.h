// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace RendererRD {

struct VirtualGeometrySnapshotCopyPlan {
	static constexpr uint32_t MAX_COPY_BYTES = 4096;
	static constexpr uint32_t MAX_JOBS = 8192;
	static constexpr uint32_t JOB_BYTES = 24;
	static constexpr uint32_t MAX_BUFFER_BYTES = 256 * 1024;
	static_assert(uint64_t(MAX_JOBS) * JOB_BYTES <= MAX_BUFFER_BYTES);

	static bool valid_range(uint32_t p_offset, uint32_t p_bytes, uint32_t p_capacity) {
		return uint64_t(p_offset) + p_bytes <= p_capacity;
	}
	static bool gatherable(uint32_t p_source_offset, uint32_t p_destination_offset, uint32_t p_bytes, uint32_t p_jobs) {
		return p_bytes > 0 && p_bytes <= MAX_COPY_BYTES && p_jobs < MAX_JOBS &&
				((p_source_offset | p_destination_offset | p_bytes) & 3u) == 0;
	}
};

} // namespace RendererRD
