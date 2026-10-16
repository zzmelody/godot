#include "leaf_clusters.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace VirtualGeometry {
namespace {

bool materialize_leaves(const TriangleMeshInput &p_input, const TriangleTopology &p_topology, const GraphPartition &p_partition, const std::vector<uint32_t> &p_inherited_edges, MeshLeafClusters &r_leaves) {
	r_leaves.clusters.resize(p_partition.ranges.size());
	r_leaves.triangle_cluster.resize(p_input.materials.size());
	for (uint32_t cluster = 0; cluster < p_partition.ranges.size(); ++cluster) {
		const PartitionRange &range = p_partition.ranges[cluster];
		for (uint32_t index = range.begin; index < range.end; ++index) {
			const uint32_t triangle = p_topology.graph_to_triangle[p_partition.vertices[index]];
			r_leaves.triangle_cluster[triangle] = cluster;
		}
	}
	// Reuse one dense source->local map, clearing only the vertices just visited.
	std::vector<uint32_t> remap(p_input.positions.size(), UINT32_MAX);
	for (uint32_t cluster = 0; cluster < p_partition.ranges.size(); ++cluster) {
		const PartitionRange &range = p_partition.ranges[cluster];
		LeafCluster &leaf = r_leaves.clusters[cluster];
		const uint32_t triangles = range.end - range.begin;
		leaf.source_triangles.reserve(triangles);
		leaf.source_vertices.reserve(triangles * 3);
		leaf.indices.reserve(triangles * 3);
		leaf.materials.reserve(triangles);
		leaf.external_edges.reserve(triangles * 3);
		leaf.lower.fill(std::numeric_limits<float>::infinity());
		leaf.upper.fill(-std::numeric_limits<float>::infinity());
		for (uint32_t index = range.begin; index < range.end; ++index) {
			const uint32_t triangle = p_topology.graph_to_triangle[p_partition.vertices[index]];
			leaf.source_triangles.push_back(triangle);
			leaf.materials.push_back(p_input.materials[triangle]);
			for (uint32_t side = 0; side < 3; ++side) {
				const uint32_t corner = triangle * 3 + side;
				const uint32_t source_vertex = p_input.indices[corner];
				if (remap[source_vertex] == UINT32_MAX) {
					remap[source_vertex] = static_cast<uint32_t>(leaf.source_vertices.size());
					leaf.source_vertices.push_back(source_vertex);
					for (size_t axis = 0; axis < 3; ++axis) {
						const float coordinate = p_input.positions[source_vertex][axis];
						leaf.lower[axis] = std::min(leaf.lower[axis], coordinate);
						leaf.upper[axis] = std::max(leaf.upper[axis], coordinate);
					}
				}
				leaf.indices.push_back(remap[source_vertex]);
				uint64_t external = p_inherited_edges.empty() ? 0 : p_inherited_edges[corner];
				for (uint32_t edge = p_topology.edge_offsets[corner]; edge < p_topology.edge_offsets[corner + 1]; ++edge) {
					const uint32_t other = p_topology.adjacent_edges[edge] / 3;
					external += r_leaves.triangle_cluster[other] != cluster ? 1u : 0u;
				}
				if (external > UINT32_MAX) {
					return false;
				}
				leaf.external_edges.push_back(static_cast<uint32_t>(external));
			}
		}
		for (uint32_t source_vertex : leaf.source_vertices) {
			remap[source_vertex] = UINT32_MAX;
		}
	}
	return true;
}

} // namespace

LeafResult build_leaf_clusters(const TriangleMeshInput &p_input, MeshLeafClusters &r_leaves, TriangleValidation p_validation, const std::vector<uint32_t> &p_inherited_edges) {
	if (!p_inherited_edges.empty() && p_inherited_edges.size() != p_input.indices.size()) {
		return { LeafError::INVALID_EXTERNAL_EDGES };
	}
	try {
		TriangleTopology topology;
		const TopologyError topology_result = build_triangle_topology(p_input, topology, p_validation);
		if (topology_result != TopologyError::OK) {
			return { LeafError::TOPOLOGY, topology_result };
		}
		GraphPartition partition;
		const PartitionResult partition_result = partition_strict(topology.graph, 124, 128, partition);
		if (partition_result.error != PartitionError::OK) {
			return { LeafError::PARTITION, TopologyError::OK, partition_result };
		}
		MeshLeafClusters candidate;
		if (!materialize_leaves(p_input, topology, partition, p_inherited_edges, candidate)) {
			return { LeafError::EDGE_COUNT_LIMIT };
		}
		r_leaves = std::move(candidate);
		return {};
	} catch (const std::bad_alloc &) {
		return { LeafError::MEMORY };
	}
}

} // namespace VirtualGeometry
