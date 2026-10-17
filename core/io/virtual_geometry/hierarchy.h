#ifndef VIRTUAL_GEOMETRY_HIERARCHY_H
#define VIRTUAL_GEOMETRY_HIERARCHY_H

#include <array>
#include <cstdint>
#include <vector>

namespace VirtualGeometry {

constexpr uint32_t HIERARCHY_FANOUT = 4;
constexpr uint32_t MAX_GROUP_PAGE_PARTS = 7;
constexpr uint32_t CLUSTER_FLAG_ROOT_LEAF = 1;
constexpr uint32_t CLUSTER_FLAG_STREAMING_LEAF = 2;
constexpr uint32_t CLUSTER_FLAG_FULL_LEAF = 4;
constexpr uint32_t CLUSTER_FLAG_ROOT_GROUP = 8;

// Logical shared records, not a native-struct disk or GPU ABI. Exactly one of
// node/part is valid in an occupied branch; both invalid means an empty slot.
struct HierarchyBranch {
	std::array<float, 3> lower = {};
	std::array<float, 3> upper = {};
	std::array<float, 4> lod_bounds = {};
	float min_error = 0;
	float max_error = 0;
	uint32_t node = UINT32_MAX;
	uint32_t part = UINT32_MAX;
};

struct HierarchyNode {
	std::array<HierarchyBranch, HIERARCHY_FANOUT> children;
};

struct HierarchyPart {
	uint32_t group = UINT32_MAX;
	uint32_t page = UINT32_MAX;
	uint32_t cluster_start = 0;
	uint32_t cluster_count = 0;
	uint32_t node = UINT32_MAX;
	uint32_t child = UINT32_MAX;
	uint32_t dependency_first = 0;
	uint32_t dependency_count = 0;
};

struct HierarchyFixup {
	uint32_t page = UINT32_MAX;
	uint32_t node = UINT32_MAX;
	uint32_t child = UINT32_MAX;
	uint32_t cluster_start = 0;
	uint32_t dependency_first = 0;
	uint32_t dependency_count = 0;
};

struct ClusterFixup {
	uint32_t page = UINT32_MAX;
	uint32_t cluster = UINT32_MAX;
	uint32_t dependency_first = 0;
	uint32_t dependency_count = 0;
};

struct HierarchyPage {
	uint32_t clusters = 0;
	uint32_t depth = 0;
	uint32_t first_part = 0;
	uint32_t part_count = 0;
	std::vector<HierarchyFixup> hierarchy_fixups;
	std::vector<ClusterFixup> cluster_fixups;
	// Coarser target pages, excluding this page, sorted and unique.
	std::vector<uint32_t> dependencies;
};

struct GeometryHierarchy {
	// Root node is always zero, including a single-part model.
	std::vector<HierarchyNode> nodes;
	std::vector<HierarchyPart> parts;
	std::vector<HierarchyPage> pages;
	uint32_t root_pages = 0;
	// Global cluster order; copied into a newly transcoded page before fixups.
	std::vector<uint32_t> cluster_flags;
};

struct HierarchyReferenceUpdate {
	uint32_t node = UINT32_MAX;
	uint32_t child = UINT32_MAX;
	// Invalid on uninstall. Physical GPU slot/address resolution is the host's.
	uint32_t page = UINT32_MAX;
	uint32_t cluster_start = 0;
};

struct ClusterLeafUpdate {
	uint32_t page = UINT32_MAX;
	uint32_t cluster = UINT32_MAX;
	bool streaming_leaf = true;
};

struct GeometryFixupUpdates {
	std::vector<HierarchyReferenceUpdate> hierarchy;
	std::vector<ClusterLeafUpdate> clusters;
};

enum class FixupPlanError {
	OK,
	INVALID_INPUT,
	INVALID_FIXUP,
	MISSING_TARGET,
	MEMORY,
};

// Value-only installation planning, matching dependency-committed gating.
// The host owns the validated model and committed-page snapshot (zero absent,
// nonzero committed). Only this chunk and its dependency ranges are visited.
// The resident page owner lends p_chunk only for this call; root metadata need
// not retain fixup payloads. Installation runs after its transcode completes.
// Uninstall invalidates its group's
// hierarchy references unconditionally and restores resident parent leaves.
// Root pages remain committed until whole-resource destruction. Old output
// survives refusal. This function does no GPU work or per-frame traversal.
FixupPlanError plan_geometry_fixups(const GeometryHierarchy &p_hierarchy, uint32_t p_page, const HierarchyPage &p_chunk,
		bool p_uninstall, const std::vector<uint8_t> &p_committed, GeometryFixupUpdates &r_updates);

} // namespace VirtualGeometry

#endif // VIRTUAL_GEOMETRY_HIERARCHY_H
