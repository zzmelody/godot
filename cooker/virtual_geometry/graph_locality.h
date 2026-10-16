#ifndef VIRTUAL_GEOMETRY_GRAPH_LOCALITY_H
#define VIRTUAL_GEOMETRY_GRAPH_LOCALITY_H

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace VirtualGeometry {

struct GraphLocality {
	std::vector<uint32_t> order;
	// Unique undirected links in original node identities, sorted by endpoints.
	std::vector<std::pair<uint32_t, uint32_t>> links;
};

enum class LocalityError {
	OK,
	INVALID_INPUT,
	MEMORY,
};

// Shared by triangle and cluster grouping. Input order supplies stable tie IDs.
// Empty labels allow links between all islands; otherwise labels must match.
// Uses 10-bit Morton keys, runs <128, 16 visits each way and five nearest links.
// Positions must lie within finite bounds; evaluated squared distances must be
// representable as doubles. Refusal preserves the old output.
LocalityError build_graph_locality(const std::vector<std::array<double, 3>> &p_centers,
		const std::vector<uint32_t> &p_islands, const std::vector<uint32_t> &p_labels,
		const std::array<double, 3> &p_lower, const std::array<double, 3> &p_upper,
		GraphLocality &r_locality);

} // namespace VirtualGeometry

#endif // VIRTUAL_GEOMETRY_GRAPH_LOCALITY_H
