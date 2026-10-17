#include "runtime_fixups.h"

#include <new>
#include <utility>

namespace VirtualGeometry {

GpuFixupError resolve_geometry_gpu_fixups(const GeometryModelRoot &p_root, uint32_t p_page, bool p_uninstall,
		const GeometryFixupUpdates &p_updates, const std::vector<uint32_t> &p_slots,
		const GeometryGpuLayout &p_layout, std::vector<GeometryGpuFixup> &r_commands) {
	const auto &hierarchy = p_root.hierarchy;
	const uint64_t nodes_end = MODEL_ROOT_HEADER_BYTES / 4 + uint64_t(hierarchy.nodes.size()) * (MODEL_NODE_BYTES / 4);
	if (p_slots.size() != hierarchy.pages.size() || p_page >= p_slots.size() ||
			p_layout.page_table_word < nodes_end || p_layout.page_table_word > p_layout.metadata_words ||
			p_slots.size() > p_layout.metadata_words - p_layout.page_table_word) {
		return GpuFixupError::INVALID_LAYOUT;
	}
	const auto valid_slot = [&](uint32_t p_index) {
		if (p_index >= p_slots.size() || p_slots[p_index] == UINT32_MAX) {
			return false;
		}
		const uint64_t start = uint64_t(p_slots[p_index]) * (ROOT_PAGE_GPU_CAPACITY / 4);
		const uint32_t capacity = p_index < hierarchy.root_pages ? ROOT_PAGE_GPU_CAPACITY : STREAMING_PAGE_GPU_CAPACITY;
		return (p_index < hierarchy.root_pages || p_slots[p_index] % 4 == 0) &&
				start <= p_layout.pool_words && capacity / 4 <= p_layout.pool_words - start;
	};
	if (!p_uninstall && !valid_slot(p_page)) {
		return GpuFixupError::INVALID_REFERENCE;
	}
	try {
		std::vector<GeometryGpuFixup> commands;
		commands.reserve(p_updates.hierarchy.size() + p_updates.clusters.size() + 1);
		for (const auto &update : p_updates.hierarchy) {
			if (update.node >= hierarchy.nodes.size() || update.child >= HIERARCHY_FANOUT) {
				return GpuFixupError::INVALID_REFERENCE;
			}
			uint32_t reference = UINT32_MAX;
			if (update.page != UINT32_MAX) {
				if (p_uninstall || !valid_slot(update.page) || update.cluster_start >= hierarchy.pages[update.page].clusters ||
						update.cluster_start >= 256 || p_slots[update.page] > (UINT32_MAX >> 8)) {
					return GpuFixupError::INVALID_REFERENCE;
				}
				reference = (p_slots[update.page] << 8) | update.cluster_start;
			} else if (!p_uninstall) {
				return GpuFixupError::INVALID_REFERENCE;
			}
			const uint32_t word = MODEL_ROOT_HEADER_BYTES / 4 + update.node * (MODEL_NODE_BYTES / 4) + update.child * 16 + 14;
			commands.push_back({ 0, word, reference, 0 });
		}
		for (const auto &update : p_updates.clusters) {
			if (!valid_slot(update.page) || update.cluster >= hierarchy.pages[update.page].clusters) {
				return GpuFixupError::INVALID_REFERENCE;
			}
			const uint64_t word = uint64_t(p_slots[update.page]) * (ROOT_PAGE_GPU_CAPACITY / 4) + GEOMETRY_PAGE_HEADER_BYTES / 4 +
					uint64_t(update.cluster) * (GEOMETRY_PAGE_CLUSTER_BYTES / 4) + 7;
			if (word >= p_layout.pool_words) {
				return GpuFixupError::INVALID_REFERENCE;
			}
			commands.push_back({ 1, uint32_t(word), CLUSTER_FLAG_STREAMING_LEAF, update.streaming_leaf ? 1u : 2u });
		}
		commands.push_back({ 0, p_layout.page_table_word + p_page, p_uninstall ? UINT32_MAX : p_slots[p_page], 0 });
		r_commands = std::move(commands);
		return GpuFixupError::OK;
	} catch (const std::bad_alloc &) {
		return GpuFixupError::MEMORY;
	}
}

} // namespace VirtualGeometry
