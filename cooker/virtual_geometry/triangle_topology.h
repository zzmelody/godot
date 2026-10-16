#ifndef VIRTUAL_GEOMETRY_TRIANGLE_TOPOLOGY_H
#define VIRTUAL_GEOMETRY_TRIANGLE_TOPOLOGY_H

#include "graph_partitioner.h"

#include <array>
#include <cstdint>
#include <vector>

namespace VirtualGeometry {

// Positions are used only to identify reverse edges and spatial neighbors.
// Source vertex identities stay distinct, including UV/normal/attribute seams.
struct TriangleMeshInput {
	std::vector<std::array<float, 3>> positions;
	std::vector<uint32_t> indices;
	std::vector<uint32_t> materials;
};

struct TriangleTopology {
	// Source corner -> all reverse source corners, including nonmanifold edges.
	std::vector<uint32_t> edge_offsets;
	std::vector<uint32_t> adjacent_edges;
	// Weighted graph indices are spatially ordered; this maps back to source tris.
	std::vector<uint32_t> graph_to_triangle;
	PartitionGraph graph;
};

enum class TopologyError {
	OK,
	INVALID_INPUT,
	INDEX_LIMIT,
	MEMORY,
};

enum class TriangleValidation {
	SOURCE,
	BUILD_INTERMEDIATE,
	INVALID,
};

// Both modes require three distinct finite positions per triangle. Source input
// also requires nonzero area. Build intermediates retain collinear triangles to
// preserve boundary identities, as in the reference simplifier and encoder.
TopologyError validate_triangle_mesh(const TriangleMeshInput &p_input, TriangleValidation p_validation = TriangleValidation::SOURCE);

// Offline, value-only. Requires valid indices and one material per triangle.
// Source/intermediate validation is explicit; refusal preserves output.
// Geometry edges cost 260; weak spatial links cost 1 and cross only disconnected
// islands with the same material. Geometry adjacency can cross material seams.
TopologyError build_triangle_topology(const TriangleMeshInput &p_input, TriangleTopology &r_topology, TriangleValidation p_validation = TriangleValidation::SOURCE);

} // namespace VirtualGeometry

#endif // VIRTUAL_GEOMETRY_TRIANGLE_TOPOLOGY_H
