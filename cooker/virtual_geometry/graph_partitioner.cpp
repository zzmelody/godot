#include "graph_partitioner.h"

#include <metis.h>

#include <algorithm>
#include <limits>
#include <mutex>
#include <new>
#include <numeric>
#include <type_traits>
#include <utility>

namespace VirtualGeometry {
namespace {

static_assert(std::is_same<idx_t, int32_t>::value, "Geometry uses METIS's 32-bit index ABI.");
static_assert(std::is_same<real_t, float>::value, "Geometry uses METIS's float32 ABI.");
std::mutex metis_mutex;

bool validate_graph(const PartitionGraph &p_graph) {
	if (p_graph.offsets.empty() || p_graph.offsets.size() - 1 > INT32_MAX || p_graph.neighbors.size() > INT32_MAX || p_graph.costs.size() != p_graph.neighbors.size() || p_graph.offsets.front() != 0 || p_graph.offsets.back() < 0 || static_cast<size_t>(p_graph.offsets.back()) != p_graph.neighbors.size()) {
		return false;
	}
	const size_t count = p_graph.offsets.size() - 1;
	for (size_t vertex = 0; vertex < count; ++vertex) {
		if (p_graph.offsets[vertex] < 0 || p_graph.offsets[vertex] > p_graph.offsets[vertex + 1] || static_cast<size_t>(p_graph.offsets[vertex + 1]) > p_graph.neighbors.size()) {
			return false;
		}
	}
	int64_t total_cost = 0;
	for (size_t vertex = 0; vertex < count; ++vertex) {
		const int32_t begin = p_graph.offsets[vertex];
		const int32_t end = p_graph.offsets[vertex + 1];
		for (int32_t edge = begin; edge < end; ++edge) {
			const int32_t neighbor = p_graph.neighbors[edge];
			if (neighbor < 0 || static_cast<size_t>(neighbor) >= count || static_cast<size_t>(neighbor) == vertex || p_graph.costs[edge] <= 0 || (edge != begin && neighbor <= p_graph.neighbors[edge - 1])) {
				return false;
			}
			total_cost += p_graph.costs[edge];
			if (total_cost > INT32_MAX) {
				return false;
			}
		}
	}
	for (size_t vertex = 0; vertex < count; ++vertex) {
		for (int32_t edge = p_graph.offsets[vertex]; edge < p_graph.offsets[vertex + 1]; ++edge) {
			const int32_t neighbor = p_graph.neighbors[edge];
			const auto begin = p_graph.neighbors.begin() + p_graph.offsets[neighbor];
			const auto end = p_graph.neighbors.begin() + p_graph.offsets[neighbor + 1];
			const auto reverse = std::lower_bound(begin, end, static_cast<int32_t>(vertex));
			if (reverse == end || *reverse != static_cast<int32_t>(vertex) || p_graph.costs[reverse - p_graph.neighbors.begin()] != p_graph.costs[edge]) {
				return false;
			}
		}
	}
	return true;
}

struct PendingGraph {
	PartitionGraph graph;
	std::vector<uint32_t> source_vertices;
};

PartitionResult bisect(PendingGraph &p_node, uint32_t p_min_size, uint32_t p_max_size, PendingGraph (&r_children)[2]) {
	idx_t count = static_cast<idx_t>(p_node.source_vertices.size());
	const uint64_t target_size = (static_cast<uint64_t>(p_min_size) + p_max_size) / 2;
	const uint64_t target_parts = std::max<uint64_t>(2, (count + target_size / 2) / target_size);
	real_t weights[] = { static_cast<real_t>(target_parts / 2) / static_cast<real_t>(target_parts), 0 };
	weights[1] = 1.0f - weights[0];
	idx_t constraints = 1;
	idx_t parts = 2;
	idx_t cut = 0;
	idx_t options[METIS_NOPTIONS];
	std::vector<idx_t> labels(count);
	int status = 0;
	{
		// GKlib's RNG and signal registration are process-global, even though its
		// allocation bookkeeping is TLS. No other work holds this mutex.
		const std::lock_guard<std::mutex> guard(metis_mutex);
		METIS_SetDefaultOptions(options);
		options[METIS_OPTION_SEED] = 4321;
		options[METIS_OPTION_NUMBERING] = 0;
		options[METIS_OPTION_DBGLVL] = 0;
		options[METIS_OPTION_ONDISK] = 0;
		options[METIS_OPTION_UFACTOR] = (target_parts >= 128 || p_max_size / p_min_size > 1) ? 200 : 1;
		status = METIS_PartGraphRecursive(&count, &constraints,
				p_node.graph.offsets.data(), p_node.graph.neighbors.data(),
				nullptr, nullptr, p_node.graph.costs.data(), &parts, weights,
				nullptr, options, &cut, labels.data());
	}
	if (status != METIS_OK) {
		return { status == METIS_ERROR_MEMORY ? PartitionError::MEMORY : PartitionError::METIS_FAILURE, status };
	}
	for (idx_t label : labels) {
		if (label != 0 && label != 1) {
			return { PartitionError::METIS_FAILURE, status };
		}
	}
	std::vector<uint32_t> order(count);
	std::iota(order.begin(), order.end(), 0u);
	uint32_t front = 0;
	uint32_t back = static_cast<uint32_t>(count);
	while (front < back) {
		while (front < back && labels[order[front]] == 0) {
			++front;
		}
		while (front < back && labels[order[back - 1]] == 1) {
			--back;
		}
		if (front < back) {
			std::swap(order[front++], order[--back]);
		}
	}
	if (front == 0 || front == static_cast<uint32_t>(count)) {
		return { PartitionError::NO_PROGRESS, status };
	}
	std::vector<uint32_t> remap(count);
	for (uint32_t vertex = 0; vertex < static_cast<uint32_t>(count); ++vertex) {
		remap[order[vertex]] = vertex;
	}
	for (uint32_t side = 0; side < 2; ++side) {
		const uint32_t begin = side == 0 ? 0 : front;
		const uint32_t end = side == 0 ? front : static_cast<uint32_t>(count);
		PendingGraph &child = r_children[side];
		child.source_vertices.reserve(end - begin);
		child.graph.offsets.reserve(end - begin + 1);
		child.graph.offsets.push_back(0);
		for (uint32_t vertex = begin; vertex < end; ++vertex) {
			const uint32_t original = order[vertex];
			child.source_vertices.push_back(p_node.source_vertices[original]);
			for (int32_t edge = p_node.graph.offsets[original]; edge < p_node.graph.offsets[original + 1]; ++edge) {
				const uint32_t neighbor = remap[p_node.graph.neighbors[edge]];
				if (neighbor >= begin && neighbor < end) {
					child.graph.neighbors.push_back(static_cast<int32_t>(neighbor - begin));
					child.graph.costs.push_back(p_node.graph.costs[edge]);
				}
			}
			child.graph.offsets.push_back(static_cast<int32_t>(child.graph.neighbors.size()));
		}
	}
	return {};
}

PartitionResult partition_graph(const PartitionGraph &p_graph, uint32_t p_min_size, uint32_t p_max_size, GraphPartition &r_partition) {
	const size_t count = p_graph.offsets.size() - 1;
	PendingGraph root;
	root.graph = p_graph;
	root.source_vertices.resize(count);
	std::iota(root.source_vertices.begin(), root.source_vertices.end(), 0u);
	std::vector<PendingGraph> pending;
	pending.push_back(std::move(root));
	GraphPartition candidate;
	candidate.vertices.reserve(count);
	while (!pending.empty()) {
		PendingGraph node = std::move(pending.back());
		pending.pop_back();
		if (node.source_vertices.empty()) {
			continue;
		}
		if (node.source_vertices.size() <= p_max_size) {
			const uint32_t begin = static_cast<uint32_t>(candidate.vertices.size());
			candidate.vertices.insert(candidate.vertices.end(), node.source_vertices.begin(), node.source_vertices.end());
			candidate.ranges.push_back({ begin, static_cast<uint32_t>(candidate.vertices.size()) });
			continue;
		}
		PendingGraph children[2];
		const PartitionResult result = bisect(node, p_min_size, p_max_size, children);
		if (result.error != PartitionError::OK) {
			return result;
		}
		pending.push_back(std::move(children[1]));
		pending.push_back(std::move(children[0]));
	}
	r_partition = std::move(candidate);
	return {};
}

} // namespace

PartitionResult partition_strict(const PartitionGraph &p_graph, uint32_t p_target_min_size, uint32_t p_max_size, GraphPartition &r_partition) {
	if (p_target_min_size == 0 || p_max_size < p_target_min_size || p_max_size > INT32_MAX) {
		return { PartitionError::INVALID_LIMITS };
	}
	if (!validate_graph(p_graph)) {
		return { PartitionError::INVALID_GRAPH };
	}
	try {
		return partition_graph(p_graph, p_target_min_size, p_max_size, r_partition);
	} catch (const std::bad_alloc &) {
		return { PartitionError::MEMORY };
	}
}

} // namespace VirtualGeometry
