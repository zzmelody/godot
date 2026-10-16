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

// Checks the shared mesh input contract without constructing adjacency or a graph.
TopologyError validate_triangle_mesh(const TriangleMeshInput &p_input);

// Offline, value-only. Input must contain finite positions, valid nondegenerate
// triangles and one material identity per triangle. Refusal preserves output.
// Geometry edges cost 260; weak spatial links cost 1 and cross only disconnected
// islands with the same material. Geometry adjacency can cross material seams.
TopologyError build_triangle_topology(const TriangleMeshInput &p_input, TriangleTopology &r_topology);

} // namespace VirtualGeometry

#endif // VIRTUAL_GEOMETRY_TRIANGLE_TOPOLOGY_H
