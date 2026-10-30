#include "runtime_fixups.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace VirtualGeometry {

GpuFixupError resolve_geometry_gpu_root_paths(const GeometryModelRoot &p_root, uint32_t p_metadata_words,
		std::vector<GeometryGpuFixup> &r_commands, GeometryFineLodBounds &r_fine_bounds) {
	const auto &hierarchy = p_root.hierarchy;
	const uint64_t nodes_end = MODEL_ROOT_HEADER_BYTES / 4 + uint64_t(hierarchy.nodes.size()) * (MODEL_NODE_BYTES / 4);
	if (hierarchy.nodes.empty() || nodes_end > p_metadata_words) {
		return GpuFixupError::INVALID_LAYOUT;
	}
	try {
		std::vector<uint8_t> contains_root(hierarchy.nodes.size(), 0);
		std::vector<GeometryGpuFixup> commands;
		GeometryFineLodBounds fine_bounds;
		std::array<double, 3> lower = { INFINITY, INFINITY, INFINITY };
		std::array<double, 3> upper = { -INFINITY, -INFINITY, -INFINITY };
		bool has_fine_branch = false;
		// The admitted spatial tree has forward child indices, one incoming
		// edge per node and one visit per part. DAG links are not tree edges.
		for (size_t end = hierarchy.nodes.size(); end > 0; --end) {
			const uint32_t node = uint32_t(end - 1);
			uint32_t root_count = 0;
			size_t single_internal = 0;
			bool has_internal = false;
			for (uint32_t child = 0; child < HIERARCHY_FANOUT; ++child) {
				const auto &branch = hierarchy.nodes[node].children[child];
				const bool root = branch.node != UINT32_MAX ? contains_root[branch.node] != 0 : branch.part != UINT32_MAX && hierarchy.parts[branch.part].group == p_root.root_group;
				if (root) {
					contains_root[node] = 1;
					++root_count;
					if (branch.node != UINT32_MAX) {
						single_internal = commands.size();
						has_internal = true;
					}
					commands.push_back({ 0, MODEL_ROOT_HEADER_BYTES / 4 + node * (MODEL_NODE_BYTES / 4) + child * 16 + 15, GPU_ROOT_PATH, 0 });
				} else if (branch.node != UINT32_MAX || branch.part != UINT32_MAX) {
					has_fine_branch = true;
					fine_bounds.max_error = std::max(fine_bounds.max_error, branch.max_error);
					for (uint32_t axis = 0; axis < 3; ++axis) {
						lower[axis] = std::min(lower[axis], std::nextafter(double(branch.lod_bounds[axis]) - double(branch.lod_bounds[3]), -std::numeric_limits<double>::infinity()));
						upper[axis] = std::max(upper[axis], std::nextafter(double(branch.lod_bounds[axis]) + double(branch.lod_bounds[3]), std::numeric_limits<double>::infinity()));
					}
				}
			}
			if (root_count == 1 && has_internal) {
				commands[single_internal].value |= GPU_ROOT_PATH_SINGLE_INTERNAL;
			}
		}
		if (has_fine_branch) {
			// Use the rounded float center when deriving the outward radius.
			// This covers descendant errors and LOD balls independently of any
			// parent monotonicity or geometric AABB containment assumption.
			double radius_squared = 0;
			for (uint32_t axis = 0; axis < 3; ++axis) {
				fine_bounds.sphere[axis] = float((lower[axis] + upper[axis]) * 0.5);
				const double extent = std::max(upper[axis] - double(fine_bounds.sphere[axis]), double(fine_bounds.sphere[axis]) - lower[axis]);
				radius_squared += extent * extent;
			}
			fine_bounds.sphere[3] = std::nextafter(float(std::sqrt(radius_squared)), std::numeric_limits<float>::infinity());
			fine_bounds.valid = std::isfinite(fine_bounds.sphere[3]);
		}
		r_commands = std::move(commands);
		r_fine_bounds = fine_bounds;
		return GpuFixupError::OK;
	} catch (const std::bad_alloc &) {
		return GpuFixupError::MEMORY;
	}
}

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
