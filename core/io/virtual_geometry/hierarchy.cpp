#include "hierarchy.h"

#include <new>
#include <utility>

namespace VirtualGeometry {
namespace {

bool valid_dependencies(uint32_t p_first, uint32_t p_count, const GeometryHierarchy &p_hierarchy) {
	return p_count == 0 ? p_first == 0 : p_count <= MAX_GROUP_PAGE_PARTS && p_first >= p_hierarchy.root_pages && p_first <= p_hierarchy.pages.size() && p_count <= p_hierarchy.pages.size() - p_first;
}

bool dependencies_ready(uint32_t p_first, uint32_t p_count, const std::vector<uint8_t> &p_committed) {
	for (uint32_t page = p_first; page < p_first + p_count; ++page) {
		if (p_committed[page] == 0) {
			return false;
		}
	}
	return true;
}

} // namespace

FixupPlanError plan_geometry_fixups(const GeometryHierarchy &p_hierarchy, uint32_t p_page, const HierarchyPage &p_chunk,
		bool p_uninstall, const std::vector<uint8_t> &p_committed, GeometryFixupUpdates &r_updates) {
	if (p_hierarchy.root_pages == 0 || p_hierarchy.root_pages > p_hierarchy.pages.size() || p_page >= p_hierarchy.pages.size() ||
			p_committed.size() != p_hierarchy.pages.size() || (p_uninstall && p_page < p_hierarchy.root_pages) ||
			(!p_uninstall && p_committed[p_page] == 0) || p_chunk.clusters != p_hierarchy.pages[p_page].clusters) {
		return FixupPlanError::INVALID_INPUT;
	}
	for (uint32_t root = 0; root < p_hierarchy.root_pages; ++root) {
		if (p_committed[root] == 0) {
			return FixupPlanError::INVALID_INPUT;
		}
	}
	try {
		GeometryFixupUpdates candidate;
		const auto &page = p_chunk;
		for (const auto &fixup : page.cluster_fixups) {
			if (fixup.page >= p_hierarchy.pages.size() || fixup.cluster >= p_hierarchy.pages[fixup.page].clusters ||
					!valid_dependencies(fixup.dependency_first, fixup.dependency_count, p_hierarchy)) {
				return FixupPlanError::INVALID_FIXUP;
			}
			if (!p_uninstall && !dependencies_ready(fixup.dependency_first, fixup.dependency_count, p_committed)) {
				continue;
			}
			if (p_committed[fixup.page] == 0) {
				if (!p_uninstall) {
					return FixupPlanError::MISSING_TARGET;
				}
				continue;
			}
			candidate.clusters.push_back({ fixup.page, fixup.cluster, p_uninstall });
		}
		for (const auto &fixup : page.hierarchy_fixups) {
			if (fixup.page >= p_hierarchy.pages.size() || fixup.node >= p_hierarchy.nodes.size() || fixup.child >= HIERARCHY_FANOUT ||
					fixup.cluster_start >= p_hierarchy.pages[fixup.page].clusters ||
					!valid_dependencies(fixup.dependency_first, fixup.dependency_count, p_hierarchy)) {
				return FixupPlanError::INVALID_FIXUP;
			}
			const auto &branch = p_hierarchy.nodes[fixup.node].children[fixup.child];
			if (branch.part >= p_hierarchy.parts.size()) {
				return FixupPlanError::INVALID_FIXUP;
			}
			const auto &part = p_hierarchy.parts[branch.part];
			if (part.page != fixup.page || part.cluster_start != fixup.cluster_start || part.node != fixup.node || part.child != fixup.child ||
					part.dependency_first != fixup.dependency_first || part.dependency_count != fixup.dependency_count) {
				return FixupPlanError::INVALID_FIXUP;
			}
			if (!p_uninstall && !dependencies_ready(fixup.dependency_first, fixup.dependency_count, p_committed)) {
				continue;
			}
			if (!p_uninstall && p_committed[fixup.page] == 0) {
				return FixupPlanError::MISSING_TARGET;
			}
			candidate.hierarchy.push_back({ fixup.node, fixup.child, p_uninstall ? UINT32_MAX : fixup.page, p_uninstall ? 0u : fixup.cluster_start });
		}
		r_updates = std::move(candidate);
		return FixupPlanError::OK;
	} catch (const std::bad_alloc &) {
		return FixupPlanError::MEMORY;
	}
}

} // namespace VirtualGeometry
