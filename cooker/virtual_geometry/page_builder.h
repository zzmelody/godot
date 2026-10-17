#ifndef VIRTUAL_GEOMETRY_PAGE_BUILDER_H
#define VIRTUAL_GEOMETRY_PAGE_BUILDER_H

#include "cluster_encoding.h"

#include "core/io/virtual_geometry/page_codec.h"
#include "core/io/virtual_geometry_page_directory.h"

namespace VirtualGeometry {

struct ClusterPageLocation {
	uint32_t page = INVALID_PAGE;
	uint32_t cluster = UINT32_MAX;
};

struct GroupPagePart {
	uint32_t group = UINT32_MAX;
	uint32_t page = INVALID_PAGE;
	uint32_t cluster_start = 0;
	uint32_t cluster_count = 0;
};

struct GeometryPageSet {
	PageDirectory directory;
	std::vector<uint8_t> directory_bytes;
	std::vector<uint8_t> container;
	std::vector<ClusterPageLocation> clusters;
	std::vector<GroupPagePart> parts;
};

enum class PageBuildError {
	OK,
	INVALID_GEOMETRY,
	METADATA_RANGE,
	PAGE_CODEC,
	DIRECTORY,
	OUTPUT_LIMIT,
	MEMORY,
};

struct PageBuildResult {
	PageBuildError error = PageBuildError::OK;
	uint32_t cluster = UINT32_MAX;
	PageCodecResult codec_result;
	PageDirectoryResult directory_result;
};

// Coarse-to-fine, alternating Morton group order, then diagonal cluster order.
// Accounts for the actual GPU triangle output when splitting pages. Root pages
// contain the complete root cluster set and fill spare capacity with finer
// groups, matching the reference's default zero-KiB minimum-residency profile.
// Output limit covers container+serialized directory; caller separately owns
// input, maps, candidate metadata and one bounded page's transient storage.
// Dependencies conservatively retain each fine group's parent-cluster pages.
// Hierarchy fixups/BVH and cross-cluster references are not built by this step.
// No file publication. Input and complete old output survive refusal.
PageBuildResult build_geometry_pages(const EncodableGeometry &p_geometry, uint64_t p_max_output_bytes, GeometryPageSet &r_pages);

} // namespace VirtualGeometry

#endif // VIRTUAL_GEOMETRY_PAGE_BUILDER_H
