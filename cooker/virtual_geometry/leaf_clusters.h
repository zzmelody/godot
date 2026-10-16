#ifndef VIRTUAL_GEOMETRY_LEAF_CLUSTERS_H
#define VIRTUAL_GEOMETRY_LEAF_CLUSTERS_H

#include "triangle_topology.h"

namespace VirtualGeometry {

// Raw build data before DAG reduction and encoding constraints. Vertex IDs map
// every source attribute without welding seams; they are not GPU vertex indices.
struct LeafCluster {
	std::vector<uint32_t> source_triangles;
	std::vector<uint32_t> source_vertices;
	std::vector<uint32_t> indices;
	std::vector<uint32_t> materials;
	std::vector<uint32_t> external_edges;
	std::array<float, 3> lower = {};
	std::array<float, 3> upper = {};
};

struct MeshLeafClusters {
	std::vector<LeafCluster> clusters;
	std::vector<uint32_t> triangle_cluster;
};

enum class LeafError {
	OK,
	TOPOLOGY,
	PARTITION,
	MEMORY,
	INVALID_EXTERNAL_EDGES,
	EDGE_COUNT_LIMIT,
};

struct LeafResult {
	LeafError error = LeafError::OK;
	TopologyError topology_error = TopologyError::OK;
	PartitionResult partition_result;
};

// Builds <=128-triangle raw leaves using the 124/128 strict partition target.
// No simplification, attribute modification or encoded 256-vertex/64-material
// constraint occurs here. Input/old output survive refusal; no borrowed state
// escapes. The output owns source identity maps, not references to source arrays.
// Inherited counts are added to the new split boundaries. Empty means none.
LeafResult build_leaf_clusters(const TriangleMeshInput &p_input, MeshLeafClusters &r_leaves, TriangleValidation p_validation = TriangleValidation::SOURCE, const std::vector<uint32_t> &p_inherited_edges = {});

} // namespace VirtualGeometry

#endif // VIRTUAL_GEOMETRY_LEAF_CLUSTERS_H
