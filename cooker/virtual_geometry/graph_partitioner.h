#ifndef VIRTUAL_GEOMETRY_GRAPH_PARTITIONER_H
#define VIRTUAL_GEOMETRY_GRAPH_PARTITIONER_H

#include <cstdint>
#include <vector>

namespace VirtualGeometry {

// A symmetric weighted graph in CSR form. Each row has strictly increasing
// neighbors. The caller owns mesh/cluster identities and their spatial order.
struct PartitionGraph {
	std::vector<int32_t> offsets;
	std::vector<int32_t> neighbors;
	std::vector<int32_t> costs;
};

struct PartitionRange {
	uint32_t begin = 0;
	uint32_t end = 0;
};

struct GraphPartition {
	std::vector<uint32_t> vertices;
	std::vector<PartitionRange> ranges;
};

enum class PartitionError {
	OK,
	INVALID_GRAPH,
	INVALID_LIMITS,
	MEMORY,
	METIS_FAILURE,
	NO_PROGRESS,
};

struct PartitionResult {
	PartitionError error = PartitionError::OK;
	int metis_status = 0;
};

// Cooker-only. Uses recursive weighted METIS bisection until every range is at
// most p_max_size. p_target_min_size controls balancing; it is not a guaranteed
// lower bound (a small final graph may need two smaller ranges).
// Input and previous output survive refusal. Concurrent calls are supported;
// only METIS's global random/signal state is serialized.
PartitionResult partition_strict(const PartitionGraph &p_graph, uint32_t p_target_min_size, uint32_t p_max_size, GraphPartition &r_partition);

} // namespace VirtualGeometry

#endif // VIRTUAL_GEOMETRY_GRAPH_PARTITIONER_H
