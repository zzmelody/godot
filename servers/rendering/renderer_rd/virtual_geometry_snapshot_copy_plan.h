// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace RendererRD {

struct VirtualGeometrySnapshotCopyPlan {
	static constexpr uint32_t TILE_BYTES = 4096;
	static constexpr uint32_t MAX_JOBS = 16384;
	static constexpr uint32_t JOB_BYTES = 24;
	static constexpr uint32_t MAX_BUFFER_BYTES = 512 * 1024;
	// Cap both dispatch axes so the shader's flattened group ID cannot wrap.
	static constexpr uint32_t MAX_DISPATCH_AXIS = 65535;
	static_assert(uint64_t(MAX_JOBS) * JOB_BYTES <= MAX_BUFFER_BYTES);

	static bool valid_range(uint32_t p_offset, uint32_t p_bytes, uint32_t p_capacity) {
		return uint64_t(p_offset) + p_bytes <= p_capacity;
	}
	static uint32_t tiles(uint32_t p_bytes) {
		return uint32_t((uint64_t(p_bytes) + TILE_BYTES - 1) / TILE_BYTES);
	}
	static bool gatherable(uint32_t p_source_offset, uint32_t p_destination_offset, uint32_t p_bytes, uint32_t p_jobs, uint32_t p_tiles, uint32_t p_max_tiles) {
		return p_bytes > 0 && p_jobs < MAX_JOBS && uint64_t(p_tiles) + tiles(p_bytes) <= p_max_tiles &&
				((p_source_offset | p_destination_offset | p_bytes) & 3u) == 0;
	}
};

} // namespace RendererRD
