#ifndef VIRTUAL_GEOMETRY_HIERARCHY_BUILDER_H
#define VIRTUAL_GEOMETRY_HIERARCHY_BUILDER_H

#include "page_builder.h"

#include "core/io/virtual_geometry/hierarchy.h"

namespace VirtualGeometry {

enum class HierarchyBuildError {
	OK,
	INVALID_GEOMETRY,
	INVALID_PAGES,
	GROUP_PAGE_LIMIT,
	METADATA_RANGE,
	DEPTH_LIMIT,
	MEMORY,
};

struct HierarchyBuildResult {
	HierarchyBuildError error = HierarchyBuildError::OK;
	uint32_t part = UINT32_MAX;
};

// Four-way, complete top-down trees per LOD, then a tree of the level roots.
// Two binary splits choose the axis with lowest summed child surface area.
// Incomplete level roots are flattened as in the UE 5.4.3 build structure.
// Each leaf is exactly one group/page part, never a replacement-DAG edge.
// Builds shared logical nodes, part locations, page depths, initial flags and
// both kinds of install/uninstall fixups. Groups span at most seven pages.
// Input/old output survive refusal. This does not serialize a model root,
// mutate page bytes, execute GPU traversal or publish runtime residency.
HierarchyBuildResult build_geometry_hierarchy(const EncodableGeometry &p_geometry,
		const GeometryPageSet &p_pages, GeometryHierarchy &r_hierarchy);

} // namespace VirtualGeometry

#endif // VIRTUAL_GEOMETRY_HIERARCHY_BUILDER_H
