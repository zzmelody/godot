#include "triangle_topology.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <numeric>
#include <tuple>
#include <utility>

namespace VirtualGeometry {
namespace {

using Center = std::array<double, 3>;
constexpr int32_t GEOMETRY_COST = 4 * 65;

struct Edge {
	uint32_t low;
	uint32_t high;
	bool forward;
	uint32_t corner;
};

struct Arc {
	uint32_t from;
	uint32_t to;
	int32_t cost;
};

class Islands {
	std::vector<uint32_t> roots;

public:
	explicit Islands(uint32_t p_count) :
			roots(p_count) {
		std::iota(roots.begin(), roots.end(), 0u);
	}

	uint32_t find(uint32_t p_index) {
		uint32_t root = p_index;
		while (root != roots[root]) {
			root = roots[root];
		}
		while (p_index != root) {
			const uint32_t next = roots[p_index];
			roots[p_index] = root;
			p_index = next;
		}
		return root;
	}

	void join(uint32_t p_left, uint32_t p_right) {
		const uint32_t left = find(p_left);
		const uint32_t right = find(p_right);
		roots[std::max(left, right)] = std::min(left, right);
	}
};

TopologyError validate_input(const TriangleMeshInput &p_input) {
	if (p_input.indices.empty() || p_input.indices.size() % 3 != 0 || p_input.materials.size() != p_input.indices.size() / 3) {
		return TopologyError::INVALID_INPUT;
	}
	if (p_input.positions.size() > INT32_MAX || p_input.indices.size() > INT32_MAX) {
		return TopologyError::INDEX_LIMIT;
	}
	for (const auto &position : p_input.positions) {
		for (float coordinate : position) {
			if (!std::isfinite(coordinate)) {
				return TopologyError::INVALID_INPUT;
			}
		}
	}
	for (size_t corner = 0; corner < p_input.indices.size(); corner += 3) {
		for (size_t side = 0; side < 3; ++side) {
			if (p_input.indices[corner + side] >= p_input.positions.size()) {
				return TopologyError::INVALID_INPUT;
			}
		}
		Center edges[2];
		for (size_t axis = 0; axis < 3; ++axis) {
			const double origin = p_input.positions[p_input.indices[corner]][axis];
			edges[0][axis] = static_cast<double>(p_input.positions[p_input.indices[corner + 1]][axis]) - origin;
			edges[1][axis] = static_cast<double>(p_input.positions[p_input.indices[corner + 2]][axis]) - origin;
		}
		const Center cross = {
			edges[0][1] * edges[1][2] - edges[0][2] * edges[1][1],
			edges[0][2] * edges[1][0] - edges[0][0] * edges[1][2],
			edges[0][0] * edges[1][1] - edges[0][1] * edges[1][0]
		};
		if (cross[0] == 0 && cross[1] == 0 && cross[2] == 0) {
			return TopologyError::INVALID_INPUT;
		}
	}
	return TopologyError::OK;
}

std::vector<Edge> make_edges(const TriangleMeshInput &p_input) {
	std::vector<uint32_t> order(p_input.positions.size());
	std::iota(order.begin(), order.end(), 0u);
	std::sort(order.begin(), order.end(), [&](uint32_t p_left, uint32_t p_right) {
		return p_input.positions[p_left] != p_input.positions[p_right] ? p_input.positions[p_left] < p_input.positions[p_right] : p_left < p_right;
	});
	std::vector<uint32_t> position_ids(order.size());
	uint32_t position_id = 0;
	for (size_t index = 0; index < order.size(); ++index) {
		if (index != 0 && p_input.positions[order[index]] != p_input.positions[order[index - 1]]) {
			++position_id;
		}
		position_ids[order[index]] = position_id;
	}
	std::vector<Edge> edges;
	edges.reserve(p_input.indices.size());
	for (uint32_t corner = 0; corner < p_input.indices.size(); ++corner) {
		const uint32_t next = corner / 3 * 3 + (corner + 1) % 3;
		const uint32_t from = position_ids[p_input.indices[corner]];
		const uint32_t to = position_ids[p_input.indices[next]];
		edges.push_back({ std::min(from, to), std::max(from, to), from < to, corner });
	}
	std::sort(edges.begin(), edges.end(), [](const Edge &p_left, const Edge &p_right) {
		return std::tie(p_left.low, p_left.high, p_left.forward, p_left.corner) < std::tie(p_right.low, p_right.high, p_right.forward, p_right.corner);
	});
	return edges;
}

TopologyError build_adjacency(const TriangleMeshInput &p_input, TriangleTopology &r_topology, Islands &r_islands) {
	const std::vector<Edge> edges = make_edges(p_input);
	r_topology.edge_offsets.resize(edges.size() + 1, 0);
	uint64_t adjacency_count = 0;
	for (size_t begin = 0; begin < edges.size();) {
		size_t end = begin + 1;
		while (end < edges.size() && edges[end].low == edges[begin].low && edges[end].high == edges[begin].high) {
			++end;
		}
		size_t split = begin;
		while (split < end && !edges[split].forward) {
			++split;
		}
		adjacency_count += 2 * static_cast<uint64_t>(split - begin) * (end - split);
		// Reject before materializing a potentially quadratic nonmanifold bucket.
		if (adjacency_count > INT32_MAX / GEOMETRY_COST) {
			return TopologyError::INDEX_LIMIT;
		}
		for (size_t edge = begin; edge < end; ++edge) {
			r_topology.edge_offsets[edges[edge].corner + 1] = static_cast<uint32_t>(edge < split ? end - split : split - begin);
		}
		begin = end;
	}
	std::partial_sum(r_topology.edge_offsets.begin(), r_topology.edge_offsets.end(), r_topology.edge_offsets.begin());
	r_topology.adjacent_edges.resize(static_cast<size_t>(adjacency_count));
	for (size_t begin = 0; begin < edges.size();) {
		size_t end = begin + 1;
		while (end < edges.size() && edges[end].low == edges[begin].low && edges[end].high == edges[begin].high) {
			++end;
		}
		size_t split = begin;
		while (split < end && !edges[split].forward) {
			++split;
		}
		for (size_t edge = begin; edge < end; ++edge) {
			uint32_t output = r_topology.edge_offsets[edges[edge].corner];
			const size_t other_begin = edge < split ? split : begin;
			const size_t other_end = edge < split ? end : split;
			for (size_t other = other_begin; other < other_end; ++other) {
				const uint32_t corner = edges[other].corner;
				r_topology.adjacent_edges[output++] = corner;
				r_islands.join(edges[edge].corner / 3, corner / 3);
			}
		}
		begin = end;
	}
	return TopologyError::OK;
}

uint32_t morton_bits(uint32_t p_value) {
	uint32_t result = 0;
	for (uint32_t bit = 0; bit < 10; ++bit) {
		result |= ((p_value >> bit) & 1u) << (bit * 3);
	}
	return result;
}

void spatial_order(const TriangleMeshInput &p_input, std::vector<Center> &r_centers, std::vector<uint32_t> &r_order) {
	const uint32_t count = static_cast<uint32_t>(p_input.materials.size());
	Center lower = { INFINITY, INFINITY, INFINITY };
	Center upper = { -INFINITY, -INFINITY, -INFINITY };
	r_centers.resize(count);
	for (uint32_t triangle = 0; triangle < count; ++triangle) {
		for (size_t axis = 0; axis < 3; ++axis) {
			double sum = 0;
			for (size_t side = 0; side < 3; ++side) {
				const double value = p_input.positions[p_input.indices[triangle * 3 + side]][axis];
				sum += value;
				lower[axis] = std::min(lower[axis], value);
				upper[axis] = std::max(upper[axis], value);
			}
			r_centers[triangle][axis] = sum / 3.0;
		}
	}
	const double extent = std::max({ upper[0] - lower[0], upper[1] - lower[1], upper[2] - lower[2] });
	std::vector<uint32_t> keys(count, 0);
	for (uint32_t triangle = 0; triangle < count; ++triangle) {
		for (uint32_t axis = 0; axis < 3; ++axis) {
			const double scaled = (r_centers[triangle][axis] - lower[axis]) / extent * 1023.0;
			keys[triangle] |= morton_bits(static_cast<uint32_t>(std::clamp(scaled, 0.0, 1023.0))) << axis;
		}
	}
	r_order.resize(count);
	std::iota(r_order.begin(), r_order.end(), 0u);
	std::sort(r_order.begin(), r_order.end(), [&](uint32_t p_left, uint32_t p_right) {
		return keys[p_left] != keys[p_right] ? keys[p_left] < keys[p_right] : p_left < p_right;
	});
}

std::vector<std::pair<uint32_t, uint32_t>> locality_links(const std::vector<Center> &p_centers, const std::vector<uint32_t> &p_materials, const std::vector<uint32_t> &p_order, Islands &r_islands) {
	const uint32_t count = static_cast<uint32_t>(p_order.size());
	std::vector<uint32_t> island_ids(count);
	std::vector<uint32_t> run_begin(count);
	std::vector<uint32_t> run_end(count);
	for (uint32_t triangle = 0; triangle < count; ++triangle) {
		island_ids[triangle] = r_islands.find(triangle);
	}
	for (uint32_t begin = 0; begin < count;) {
		uint32_t end = begin + 1;
		while (end < count && island_ids[p_order[end]] == island_ids[p_order[begin]]) {
			++end;
		}
		for (uint32_t index = begin; index < end; ++index) {
			run_begin[index] = begin;
			run_end[index] = end;
		}
		begin = end;
	}
	std::vector<std::pair<uint32_t, uint32_t>> links;
	for (uint32_t index = 0; index < count; ++index) {
		if (run_end[index] - run_begin[index] >= 128) {
			continue;
		}
		const uint32_t triangle = p_order[index];
		std::pair<double, uint32_t> closest[5];
		for (auto &candidate : closest) {
			candidate = { std::numeric_limits<double>::infinity(), UINT32_MAX };
		}
		for (int direction : { -1, 1 }) {
			int64_t cursor = index;
			for (uint32_t iteration = 0; iteration < 16; ++iteration) {
				cursor += direction;
				if (cursor < 0 || cursor >= count) {
					break;
				}
				const uint32_t other = p_order[static_cast<size_t>(cursor)];
				if (island_ids[triangle] == island_ids[other] || p_materials[triangle] != p_materials[other]) {
					cursor = direction < 0 ? run_begin[static_cast<size_t>(cursor)] : run_end[static_cast<size_t>(cursor)] - 1;
					continue;
				}
				double distance = 0;
				for (size_t axis = 0; axis < 3; ++axis) {
					const double delta = p_centers[triangle][axis] - p_centers[other][axis];
					distance += delta * delta;
				}
				std::pair<double, uint32_t> candidate = { distance, other };
				for (auto &slot : closest) {
					if (candidate.first < slot.first) {
						std::swap(candidate, slot);
					}
				}
			}
		}
		for (const auto &candidate : closest) {
			if (candidate.second != UINT32_MAX) {
				links.emplace_back(std::min(triangle, candidate.second), std::max(triangle, candidate.second));
			}
		}
	}
	std::sort(links.begin(), links.end());
	links.erase(std::unique(links.begin(), links.end()), links.end());
	return links;
}

TopologyError build_graph(const TriangleMeshInput &p_input, TriangleTopology &r_topology, Islands &r_islands) {
	std::vector<Center> centers;
	spatial_order(p_input, centers, r_topology.graph_to_triangle);
	const auto links = locality_links(centers, p_input.materials, r_topology.graph_to_triangle, r_islands);
	const uint32_t count = static_cast<uint32_t>(p_input.materials.size());
	std::vector<uint32_t> inverse(count);
	for (uint32_t index = 0; index < count; ++index) {
		inverse[r_topology.graph_to_triangle[index]] = index;
	}
	const uint64_t total_cost = static_cast<uint64_t>(r_topology.adjacent_edges.size()) * GEOMETRY_COST + static_cast<uint64_t>(links.size()) * 2;
	if (total_cost > INT32_MAX) {
		return TopologyError::INDEX_LIMIT;
	}
	std::vector<Arc> arcs;
	arcs.reserve(r_topology.adjacent_edges.size() + links.size() * 2);
	for (uint32_t corner = 0; corner < p_input.indices.size(); ++corner) {
		for (uint32_t edge = r_topology.edge_offsets[corner]; edge < r_topology.edge_offsets[corner + 1]; ++edge) {
			arcs.push_back({ inverse[corner / 3], inverse[r_topology.adjacent_edges[edge] / 3], GEOMETRY_COST });
		}
	}
	for (const auto &link : links) {
		arcs.push_back({ inverse[link.first], inverse[link.second], 1 });
		arcs.push_back({ inverse[link.second], inverse[link.first], 1 });
	}
	std::sort(arcs.begin(), arcs.end(), [](const Arc &p_left, const Arc &p_right) {
		return std::tie(p_left.from, p_left.to) < std::tie(p_right.from, p_right.to);
	});
	PartitionGraph &graph = r_topology.graph;
	graph.offsets.reserve(count + 1);
	graph.offsets.push_back(0);
	size_t arc = 0;
	for (uint32_t row = 0; row < count; ++row) {
		while (arc < arcs.size() && arcs[arc].from == row) {
			const uint32_t neighbor = arcs[arc].to;
			int32_t cost = 0;
			do {
				cost += arcs[arc++].cost;
			} while (arc < arcs.size() && arcs[arc].from == row && arcs[arc].to == neighbor);
			graph.neighbors.push_back(static_cast<int32_t>(neighbor));
			graph.costs.push_back(cost);
		}
		graph.offsets.push_back(static_cast<int32_t>(graph.neighbors.size()));
	}
	return TopologyError::OK;
}

} // namespace

TopologyError build_triangle_topology(const TriangleMeshInput &p_input, TriangleTopology &r_topology) {
	const TopologyError valid = validate_input(p_input);
	if (valid != TopologyError::OK) {
		return valid;
	}
	try {
		TriangleTopology candidate;
		Islands islands(static_cast<uint32_t>(p_input.materials.size()));
		const TopologyError adjacency = build_adjacency(p_input, candidate, islands);
		if (adjacency != TopologyError::OK) {
			return adjacency;
		}
		const TopologyError graph = build_graph(p_input, candidate, islands);
		if (graph != TopologyError::OK) {
			return graph;
		}
		r_topology = std::move(candidate);
		return TopologyError::OK;
	} catch (const std::bad_alloc &) {
		return TopologyError::MEMORY;
	}
}

} // namespace VirtualGeometry
