#include "mesh_simplifier.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <numeric>
#include <set>
#include <utility>
#include <vector>

namespace VirtualGeometry {
namespace {

using Point = QuadricPoint;
using PairKey = std::pair<uint32_t, uint32_t>;
using EdgeKey = std::pair<Point, Point>;

static_assert(sizeof(float) == sizeof(uint32_t) && std::numeric_limits<float>::is_iec559, "Area dilation requires IEEE float32 random fractions.");

Point add(const Point &p_left, const Point &p_right) {
	return { p_left[0] + p_right[0], p_left[1] + p_right[1], p_left[2] + p_right[2] };
}
Point subtract(const Point &p_left, const Point &p_right) {
	return { p_left[0] - p_right[0], p_left[1] - p_right[1], p_left[2] - p_right[2] };
}
Point scale(const Point &p_value, double p_factor) {
	return { p_value[0] * p_factor, p_value[1] * p_factor, p_value[2] * p_factor };
}
double dot(const Point &p_left, const Point &p_right) {
	return p_left[0] * p_right[0] + p_left[1] * p_right[1] + p_left[2] * p_right[2];
}
Point cross(const Point &p_left, const Point &p_right) {
	return { p_left[1] * p_right[2] - p_left[2] * p_right[1], p_left[2] * p_right[0] - p_left[0] * p_right[2], p_left[0] * p_right[1] - p_left[1] * p_right[0] };
}
bool round_point(Point &r_point) {
	for (double &coordinate : r_point) {
		const float value = static_cast<float>(coordinate);
		if (!std::isfinite(value)) {
			return false;
		}
		coordinate = value;
	}
	return true;
}
uint32_t next_corner(uint32_t p_corner) {
	return p_corner / 3 * 3 + (p_corner + 1) % 3;
}
PairKey pair_key(uint32_t p_left, uint32_t p_right) {
	return { std::min(p_left, p_right), std::max(p_left, p_right) };
}

struct Vertex {
	uint32_t group = 0;
	uint32_t references = 0;
	std::vector<double> attributes;
};
struct PositionGroup {
	Point position = {};
	std::set<uint32_t> corners;
	std::set<uint32_t> pairs;
	bool locked = false;
};
struct Triangle {
	std::array<uint32_t, 3> vertices = {};
	uint32_t material = 0;
	uint8_t uv_orientation = 0;
	bool alive = true;
	AttributeQuadric quadric;
};
struct Pair {
	PairKey groups;
	double priority = 0;
	uint32_t heap_index = UINT32_MAX;
	bool alive = true;
};

// One live heap entry per pair. Removed slots are reused; stale candidates do not
// accumulate in an append-only priority queue as simplification proceeds.
class PairHeap {
	std::vector<uint32_t> heap;

	static bool less(uint32_t p_left, uint32_t p_right, const std::vector<Pair> &p_pairs) {
		return p_pairs[p_left].priority != p_pairs[p_right].priority ? p_pairs[p_left].priority < p_pairs[p_right].priority : p_left < p_right;
	}
	void swap_entries(uint32_t p_left, uint32_t p_right, std::vector<Pair> &r_pairs) {
		std::swap(heap[p_left], heap[p_right]);
		r_pairs[heap[p_left]].heap_index = p_left;
		r_pairs[heap[p_right]].heap_index = p_right;
	}
	void repair(uint32_t p_index, std::vector<Pair> &r_pairs) {
		while (p_index > 0 && less(heap[p_index], heap[(p_index - 1) / 2], r_pairs)) {
			const uint32_t parent = (p_index - 1) / 2;
			swap_entries(p_index, parent, r_pairs);
			p_index = parent;
		}
		while (static_cast<size_t>(p_index) * 2 + 1 < heap.size()) {
			uint32_t child = p_index * 2 + 1;
			if (child + 1 < heap.size() && less(heap[child + 1], heap[child], r_pairs)) {
				++child;
			}
			if (!less(heap[child], heap[p_index], r_pairs)) {
				break;
			}
			swap_entries(child, p_index, r_pairs);
			p_index = child;
		}
	}

public:
	bool empty() const { return heap.empty(); }
	uint32_t top() const { return heap.front(); }
	void update(uint32_t p_pair, double p_priority, std::vector<Pair> &r_pairs) {
		Pair &pair = r_pairs[p_pair];
		pair.priority = p_priority;
		if (pair.heap_index == UINT32_MAX) {
			pair.heap_index = static_cast<uint32_t>(heap.size());
			heap.push_back(p_pair);
		}
		repair(pair.heap_index, r_pairs);
	}
	void remove(uint32_t p_pair, std::vector<Pair> &r_pairs) {
		const uint32_t index = r_pairs[p_pair].heap_index;
		if (index == UINT32_MAX) {
			return;
		}
		r_pairs[p_pair].heap_index = UINT32_MAX;
		const uint32_t last = heap.back();
		heap.pop_back();
		if (index < heap.size()) {
			heap[index] = last;
			r_pairs[last].heap_index = index;
			repair(index, r_pairs);
		}
	}
};

struct MergeCandidate {
	Point position = {};
	std::vector<uint32_t> triangles;
	std::vector<uint32_t> triangle_wedges;
	std::vector<std::vector<double>> attributes;
	std::vector<uint8_t> attribute_updates;
	double error = 0;
	double penalty = 0;
	bool disjoint = false;
	bool removes_all = false;
};

class Simplifier {
	const SimplificationInput &input;
	const SimplificationSettings &settings;
	PreparedAttributePolicy policy;
	std::vector<Vertex> vertices;
	std::vector<PositionGroup> groups;
	std::vector<Triangle> triangles;
	std::map<Point, uint32_t> position_groups;
	std::map<EdgeKey, uint32_t> locked_edges;
	std::vector<Pair> pairs;
	std::vector<uint32_t> free_pairs;
	std::map<PairKey, uint32_t> pair_ids;
	PairHeap heap;
	uint32_t remaining = 0;
	double max_error = 0;
	SimplificationReport report;
	QuadricError numeric_error = QuadricError::OK;

	Point position(uint32_t p_vertex) const { return groups[vertices[p_vertex].group].position; }
	uint32_t corner_vertex(uint32_t p_corner) const { return triangles[p_corner / 3].vertices[p_corner % 3]; }
	uint32_t corner_group(uint32_t p_corner) const { return vertices[corner_vertex(p_corner)].group; }
	bool same_surface(uint32_t p_left, uint32_t p_right) const {
		return triangles[p_left].material == triangles[p_right].material && triangles[p_left].uv_orientation == triangles[p_right].uv_orientation;
	}
	bool checked(QuadricError p_error) {
		if (p_error != QuadricError::OK) {
			numeric_error = p_error;
			return false;
		}
		return true;
	}
	bool recompute(uint32_t p_triangle) {
		Triangle &triangle = triangles[p_triangle];
		std::array<Point, 3> points;
		std::array<std::vector<double>, 3> attributes;
		for (size_t side = 0; side < 3; ++side) {
			points[side] = position(triangle.vertices[side]);
			attributes[side] = vertices[triangle.vertices[side]].attributes;
		}
		++report.recomputed_triangles;
		return checked(make_attribute_quadric(points, attributes, policy.weights, triangle.quadric));
	}
	void remove_triangle(uint32_t p_triangle) {
		Triangle &triangle = triangles[p_triangle];
		for (uint32_t side = 0; side < 3; ++side) {
			const uint32_t vertex = triangle.vertices[side];
			groups[vertices[vertex].group].corners.erase(p_triangle * 3 + side);
			--vertices[vertex].references;
		}
		triangle.alive = false;
		--remaining;
	}
	bool fix_triangle(uint32_t p_triangle) {
		Triangle &triangle = triangles[p_triangle];
		const Point p0 = position(triangle.vertices[0]);
		const Point p1 = position(triangle.vertices[1]);
		const Point p2 = position(triangle.vertices[2]);
		if (p0 == p1 || p1 == p2 || p2 == p0) {
			remove_triangle(p_triangle);
			return true;
		}
		for (uint32_t side = 0; side < 3; ++side) {
			uint32_t &vertex = triangle.vertices[side];
			uint32_t canonical = vertex;
			for (uint32_t corner : groups[vertices[vertex].group].corners) {
				const uint32_t other = corner_vertex(corner);
				if (other < canonical && vertices[other].attributes == vertices[vertex].attributes) {
					canonical = other;
				}
			}
			if (canonical != vertex) {
				--vertices[vertex].references;
				++vertices[canonical].references;
				vertex = canonical;
			}
		}
		for (uint32_t corner : groups[vertices[triangle.vertices[0]].group].corners) {
			const uint32_t other = corner / 3;
			if (other >= p_triangle || !same_surface(other, p_triangle)) {
				continue;
			}
			if (corner_vertex(corner) == triangle.vertices[0] && corner_vertex(next_corner(corner)) == triangle.vertices[1] && corner_vertex(next_corner(next_corner(corner))) == triangle.vertices[2]) {
				remove_triangle(p_triangle);
				return true;
			}
		}
		return recompute(p_triangle);
	}
	uint32_t register_pair(uint32_t p_left, uint32_t p_right) {
		const PairKey key = pair_key(p_left, p_right);
		const auto found = pair_ids.find(key);
		if (found != pair_ids.end()) {
			return found->second;
		}
		uint32_t id;
		if (free_pairs.empty()) {
			id = static_cast<uint32_t>(pairs.size());
			pairs.push_back({ key });
		} else {
			id = free_pairs.back();
			free_pairs.pop_back();
			pairs[id] = { key };
		}
		pair_ids.emplace(key, id);
		groups[key.first].pairs.insert(id);
		groups[key.second].pairs.insert(id);
		return id;
	}
	void remove_pair(uint32_t p_id) {
		Pair &pair = pairs[p_id];
		heap.remove(p_id, pairs);
		groups[pair.groups.first].pairs.erase(p_id);
		groups[pair.groups.second].pairs.erase(p_id);
		pair_ids.erase(pair.groups);
		pair.alive = false;
		free_pairs.push_back(p_id);
	}
	void register_triangle_pairs(uint32_t p_triangle) {
		for (uint32_t side = 0; side < 3; ++side) {
			const uint32_t first = vertices[triangles[p_triangle].vertices[side]].group;
			const uint32_t second = vertices[triangles[p_triangle].vertices[(side + 1) % 3]].group;
			if (first != second) {
				register_pair(first, second);
			}
		}
	}
	bool boundary_constraint(uint32_t p_corner, PositionQuadric &r_quadric, bool &r_needed, bool &r_open) {
		const uint32_t start = corner_vertex(p_corner);
		const uint32_t end = corner_vertex(next_corner(p_corner));
		bool reverse_position = false;
		for (uint32_t other : groups[vertices[end].group].corners) {
			if (corner_group(next_corner(other)) != vertices[start].group) {
				continue;
			}
			reverse_position = true;
			if (corner_vertex(other) == end && corner_vertex(next_corner(other)) == start && same_surface(p_corner / 3, other / 3)) {
				r_needed = false;
				r_open = false;
				return true;
			}
		}
		r_needed = true;
		r_open = !reverse_position;
		return checked(make_edge_quadric(position(start), position(end), reverse_position ? 1.0 : 2.0, r_quadric));
	}
	bool valid_position(const MergeCandidate &p_candidate, const PairKey &p_pair, const Point &p_position, const Point &p_lower, const Point &p_upper) const {
		double outside = 0;
		for (size_t axis = 0; axis < 3; ++axis) {
			const double delta = p_position[axis] - std::clamp(p_position[axis], p_lower[axis], p_upper[axis]);
			outside += delta * delta;
		}
		const Point diagonal = subtract(p_upper, p_lower);
		if (outside > dot(diagonal, diagonal) * 4) {
			return false;
		}
		for (uint32_t triangle : p_candidate.triangles) {
			uint32_t moved = 0;
			uint32_t moved_side = 0;
			for (uint32_t side = 0; side < 3; ++side) {
				const uint32_t group = vertices[triangles[triangle].vertices[side]].group;
				if (group == p_pair.first || group == p_pair.second) {
					++moved;
					moved_side = side;
				}
			}
			if (moved != 1) {
				continue;
			}
			const auto &verts = triangles[triangle].vertices;
			const Point old = position(verts[moved_side]);
			const Point first = position(verts[(moved_side + 1) % 3]);
			const Point second = position(verts[(moved_side + 2) % 3]);
			const Point edge = subtract(second, first);
			if (dot(cross(subtract(old, first), edge), cross(subtract(p_position, first), edge)) < 0) {
				return false;
			}
		}
		return true;
	}
	bool evaluate_pair(uint32_t p_id, MergeCandidate &r_candidate) {
		++report.evaluated_pairs;
		const PairKey key = pairs[p_id].groups;
		std::set<uint32_t> adjacent;
		uint32_t degree = 0;
		for (uint32_t group : { key.first, key.second }) {
			degree += static_cast<uint32_t>(groups[group].corners.size());
			for (uint32_t corner : groups[group].corners) {
				adjacent.insert(corner / 3);
			}
		}
		r_candidate.triangles.assign(adjacent.begin(), adjacent.end());
		if (degree == 0 || degree == remaining * 2) {
			r_candidate.removes_all = true;
			return true;
		}
		std::vector<uint32_t> roots(adjacent.size());
		std::iota(roots.begin(), roots.end(), 0u);
		auto find = [&](uint32_t p_index) {
			while (roots[p_index] != p_index) {
				p_index = roots[p_index] = roots[roots[p_index]];
			}
			return p_index;
		};
		std::map<uint32_t, uint32_t> vertex_triangle;
		std::vector<std::pair<uint32_t, uint32_t>> endpoint_vertices[2];
		Point lower;
		Point upper;
		lower.fill(std::numeric_limits<double>::infinity());
		upper.fill(-std::numeric_limits<double>::infinity());
		for (uint32_t local = 0; local < r_candidate.triangles.size(); ++local) {
			const uint32_t triangle = r_candidate.triangles[local];
			for (uint32_t vertex : triangles[triangle].vertices) {
				const Point point = position(vertex);
				for (size_t axis = 0; axis < 3; ++axis) {
					lower[axis] = std::min(lower[axis], point[axis]);
					upper[axis] = std::max(upper[axis], point[axis]);
				}
				const uint32_t group = vertices[vertex].group;
				if (group != key.first && group != key.second) {
					continue;
				}
				const auto inserted = vertex_triangle.emplace(vertex, local);
				if (!inserted.second) {
					const uint32_t first = find(local);
					const uint32_t second = find(inserted.first->second);
					roots[std::max(first, second)] = std::min(first, second);
				} else {
					endpoint_vertices[group == key.first ? 0 : 1].emplace_back(vertex, local);
				}
			}
		}
		std::map<uint32_t, uint32_t> wedge_ids;
		std::vector<AttributeQuadric> quadrics;
		for (uint32_t local = 0; local < r_candidate.triangles.size(); ++local) {
			const uint32_t root = find(local);
			const auto inserted = wedge_ids.emplace(root, static_cast<uint32_t>(quadrics.size()));
			const auto &quadric = triangles[r_candidate.triangles[local]].quadric;
			if (inserted.second) {
				quadrics.push_back(quadric);
			} else if (!checked(accumulate_attribute_quadric(quadric, quadrics[inserted.first->second]))) {
				return false;
			}
			r_candidate.triangle_wedges.push_back(inserted.first->second);
		}
		QuadricOptimizer optimizer;
		for (const auto &quadric : quadrics) {
			if (!checked(accumulate_optimizer_wedge(quadric, optimizer))) {
				return false;
			}
		}
		std::vector<PositionQuadric> edge_quadrics;
		for (uint32_t triangle : r_candidate.triangles) {
			for (uint32_t side = 0; side < 3; ++side) {
				const uint32_t corner = triangle * 3 + side;
				const uint32_t start = corner_group(corner);
				const uint32_t end = corner_group(next_corner(corner));
				if (start != key.first && start != key.second && end != key.first && end != key.second) {
					continue;
				}
				PositionQuadric edge;
				bool needed;
				bool open;
				if (!boundary_constraint(corner, edge, needed, open)) {
					return false;
				}
				if (needed) {
					if (!checked(accumulate_optimizer_edge(edge, optimizer))) {
						return false;
					}
					edge_quadrics.push_back(edge);
				}
			}
		}
		const Point first = groups[key.first].position;
		const Point second = groups[key.second].position;
		const bool locked0 = groups[key.first].locked;
		const bool locked1 = groups[key.second].locked;
		r_candidate.penalty = degree > 24 ? 0.5 * (degree - 24) : 0;
		if (locked0 && locked1) {
			r_candidate.penalty += 1e8;
		}
		auto acceptable = [&]() { return round_point(r_candidate.position) && valid_position(r_candidate, key, r_candidate.position, lower, upper); };
		if (locked0 != locked1) {
			r_candidate.position = locked0 ? first : second;
			if (!acceptable()) {
				r_candidate.penalty += 100;
			}
		} else {
			bool solved = solve_quadric_volume(optimizer, r_candidate.position) == QuadricError::OK && acceptable();
			if (!solved) {
				solved = solve_quadric_position(optimizer, r_candidate.position) == QuadricError::OK && acceptable();
			}
			if (!solved) {
				solved = solve_quadric_edge(optimizer, first, second, r_candidate.position) == QuadricError::OK && acceptable();
			}
			if (!solved) {
				r_candidate.position = scale(add(first, second), 0.5);
				if (!acceptable()) {
					r_candidate.penalty += 100;
				}
			}
		}
		r_candidate.attributes.resize(quadrics.size(), std::vector<double>(policy.weights.size(), 0));
		r_candidate.attribute_updates.assign(quadrics.size(), 0);
		const Point delta0 = subtract(r_candidate.position, first);
		const Point delta1 = subtract(r_candidate.position, second);
		const uint32_t farthest = dot(delta0, delta0) > dot(delta1, delta1) ? 0 : 1;
		for (uint32_t pass = 0; pass < 2; ++pass) {
			for (const auto &record : endpoint_vertices[(farthest + pass) & 1]) {
				r_candidate.attributes[r_candidate.triangle_wedges[record.second]] = vertices[record.first].attributes;
			}
		}
		double surface_area = 0;
		for (size_t wedge = 0; wedge < quadrics.size(); ++wedge) {
			const auto &quadric = quadrics[wedge];
			auto &attributes = r_candidate.attributes[wedge];
			if (quadric.area > 1e-8) {
				r_candidate.attribute_updates[wedge] = 1;
				double error;
				if (locked0 != locked1) {
					if (!checked(evaluate_attribute_quadric(quadric, r_candidate.position, attributes, error))) {
						return false;
					}
				} else {
					if (!checked(fit_quadric_attributes(quadric, r_candidate.position, attributes, error))) {
						return false;
					}
					if (correct_vertex_attributes(input.layout, attributes) != AttributePolicyError::OK) {
						numeric_error = QuadricError::NON_FINITE;
						return false;
					}
				}
				for (double &value : attributes) {
					const float rounded = static_cast<float>(value);
					if (!std::isfinite(rounded)) {
						numeric_error = QuadricError::NON_FINITE;
						return false;
					}
					value = rounded;
				}
				r_candidate.error += std::min(error, quadric.area);
			}
			surface_area += quadric.area;
		}
		for (const auto &edge : edge_quadrics) {
			double error;
			if (!checked(evaluate_position_quadric(edge, r_candidate.position, error))) {
				return false;
			}
			r_candidate.error += error;
		}
		r_candidate.disjoint = adjacent.size() == 1 || (adjacent.size() == 2 && degree == 4);
		r_candidate.error = r_candidate.disjoint ? surface_area : std::min(r_candidate.error, surface_area);
		if (settings.max_edge_length_factor > 0) {
			const double denominator = settings.max_edge_length_factor * settings.max_edge_length_factor;
			for (uint32_t triangle : r_candidate.triangles) {
				uint32_t moved = 0;
				for (uint32_t vertex : triangles[triangle].vertices) {
					const uint32_t group = vertices[vertex].group;
					moved += group == key.first || group == key.second ? 1u : 0u;
				}
				if (moved == 1) {
					for (uint32_t vertex : triangles[triangle].vertices) {
						const uint32_t group = vertices[vertex].group;
						if (group != key.first && group != key.second) {
							const Point delta = subtract(r_candidate.position, position(vertex));
							r_candidate.error = std::max(r_candidate.error, dot(delta, delta) / denominator);
						}
					}
				}
			}
		}
		if (!std::isfinite(r_candidate.error) || !std::isfinite(r_candidate.penalty)) {
			numeric_error = QuadricError::NON_FINITE;
			return false;
		}
		return true;
	}
	bool update_pair(uint32_t p_pair) {
		if (!pairs[p_pair].alive) {
			return true;
		}
		const PairKey key = pairs[p_pair].groups;
		bool supported = false;
		for (uint32_t corner : groups[key.first].corners) {
			if (corner_group(next_corner(corner)) == key.second || corner_group(next_corner(next_corner(corner))) == key.second) {
				supported = true;
				break;
			}
		}
		if (!supported) {
			remove_pair(p_pair);
			return true;
		}
		MergeCandidate candidate;
		if (!evaluate_pair(p_pair, candidate)) {
			return false;
		}
		if (candidate.removes_all) {
			heap.remove(p_pair, pairs);
		} else {
			heap.update(p_pair, candidate.error + candidate.penalty, pairs);
		}
		return true;
	}
	bool apply_pair(uint32_t p_id, const MergeCandidate &p_candidate, std::map<uint32_t, double> &r_area_deltas) {
		const PairKey key = pairs[p_id].groups;
		std::set<uint32_t> moved_groups{ key.first, key.second };
		const auto collision = position_groups.find(p_candidate.position);
		if (collision != position_groups.end()) {
			moved_groups.insert(collision->second);
		}
		const uint32_t target = *moved_groups.begin();
		std::set<uint32_t> touched(p_candidate.triangles.begin(), p_candidate.triangles.end());
		std::set<uint32_t> removed_pairs;
		for (uint32_t group : moved_groups) {
			removed_pairs.insert(groups[group].pairs.begin(), groups[group].pairs.end());
			for (uint32_t corner : groups[group].corners) {
				touched.insert(corner / 3);
			}
		}
		for (uint32_t pair : removed_pairs) {
			remove_pair(pair);
		}
		for (size_t local = 0; local < p_candidate.triangles.size(); ++local) {
			const uint32_t wedge = p_candidate.triangle_wedges[local];
			// As in MeshSimplify, an area-degenerate wedge has no fitted values.
			// Keep each surviving vertex's own attributes instead of writing zeros.
			if (p_candidate.attribute_updates[wedge] == 0) {
				continue;
			}
			for (uint32_t vertex : triangles[p_candidate.triangles[local]].vertices) {
				const uint32_t group = vertices[vertex].group;
				if (group == key.first || group == key.second) {
					vertices[vertex].attributes = p_candidate.attributes[wedge];
				}
			}
		}
		bool locked = false;
		std::set<uint32_t> merged_corners;
		for (uint32_t group : moved_groups) {
			locked = locked || groups[group].locked;
			if (groups[group].locked && groups[group].position != p_candidate.position) {
				++report.locked_position_moves;
			}
			position_groups.erase(groups[group].position);
			merged_corners.insert(groups[group].corners.begin(), groups[group].corners.end());
			groups[group].corners.clear();
		}
		groups[target].position = p_candidate.position;
		groups[target].locked = locked;
		groups[target].corners = std::move(merged_corners);
		position_groups[p_candidate.position] = target;
		for (uint32_t corner : groups[target].corners) {
			vertices[corner_vertex(corner)].group = target;
		}
		std::set<uint32_t> dirty_groups;
		for (uint32_t triangle : touched) {
			for (uint32_t vertex : triangles[triangle].vertices) {
				dirty_groups.insert(vertices[vertex].group);
			}
			r_area_deltas[triangles[triangle].material] -= triangles[triangle].quadric.area;
		}
		for (uint32_t triangle : touched) {
			if (!fix_triangle(triangle)) {
				return false;
			}
			if (triangles[triangle].alive) {
				r_area_deltas[triangles[triangle].material] += triangles[triangle].quadric.area;
				register_triangle_pairs(triangle);
			}
		}
		std::set<uint32_t> dirty_pairs;
		for (uint32_t group : dirty_groups) {
			dirty_pairs.insert(groups[group].pairs.begin(), groups[group].pairs.end());
		}
		for (uint32_t pair : dirty_pairs) {
			if (!update_pair(pair)) {
				return false;
			}
		}
		max_error = std::max(max_error, p_candidate.error);
		++report.collapsed_pairs;
		return true;
	}
	bool dilate(const std::map<uint32_t, double> &p_deltas, std::vector<Point> &r_positions) {
		uint32_t material = 0;
		double lost = 0;
		for (const auto &entry : p_deltas) {
			if (entry.second < lost) {
				material = entry.first;
				lost = entry.second;
			}
		}
		if (lost == 0) {
			return true;
		}
		std::vector<std::array<double, 4>> normals(vertices.size(), { 0, 0, 0, 0 });
		double perimeter = 0;
		double area = 0;
		uint32_t edge_count = 0;
		for (uint32_t triangle = 0; triangle < triangles.size(); ++triangle) {
			if (!triangles[triangle].alive || triangles[triangle].material != material) {
				continue;
			}
			area += triangles[triangle].quadric.area;
			const auto &verts = triangles[triangle].vertices;
			const Point face = cross(subtract(position(verts[2]), position(verts[0])), subtract(position(verts[1]), position(verts[0])));
			for (uint32_t side = 0; side < 3; ++side) {
				PositionQuadric constraint;
				bool needed;
				bool open;
				if (!boundary_constraint(triangle * 3 + side, constraint, needed, open)) {
					return false;
				}
				if (!needed || !open) {
					continue;
				}
				++edge_count;
				const Point edge = subtract(position(verts[(side + 1) % 3]), position(verts[side]));
				Point normal = cross(face, edge);
				const double length = std::sqrt(dot(edge, edge));
				const double normal_length = std::sqrt(dot(normal, normal));
				if (normal_length > 1e-4) {
					normal = scale(normal, 1 / normal_length);
				}
				perimeter += length;
				normal = scale(normal, length);
				for (uint32_t endpoint : { verts[side], verts[(side + 1) % 3] }) {
					const uint32_t group = vertices[endpoint].group;
					if (groups[group].locked) {
						continue;
					}
					std::set<uint32_t> group_vertices;
					for (uint32_t corner : groups[group].corners) {
						group_vertices.insert(corner_vertex(corner));
					}
					for (uint32_t vertex : group_vertices) {
						for (size_t axis = 0; axis < 3; ++axis) {
							normals[vertex][axis] += normal[axis];
						}
						normals[vertex][3] += length;
					}
				}
			}
		}
		if (-lost > 4 * area || perimeter <= 0) {
			return true;
		}
		const double distance = -lost / perimeter;
		uint32_t seed = edge_count;
		for (uint32_t vertex = 0; vertex < vertices.size(); ++vertex) {
			const double weight = normals[vertex][3];
			if (weight <= 1e-6 || vertices[vertex].references == 0) {
				continue;
			}
			seed = seed * 196314165u + 907633515u;
			const uint32_t bits = 0x3f800000u | (seed >> 9);
			float random;
			std::memcpy(&random, &bits, sizeof(random));
			const double factor = (random - 1) * 0.5 + 0.75;
			const Point normal = { normals[vertex][0] / weight, normals[vertex][1] / weight, normals[vertex][2] / weight };
			const double squared = dot(normal, normal);
			if (squared > 0.1) {
				r_positions[vertex] = add(r_positions[vertex], scale(normal, factor * distance / squared));
				if (!round_point(r_positions[vertex])) {
					numeric_error = QuadricError::NON_FINITE;
					return false;
				}
				report.area_dilation_applied = true;
			}
		}
		return true;
	}
	SimplificationResult export_mesh(const std::map<uint32_t, double> &p_deltas, SimplifiedMesh &r_output) {
		if (remaining == 0) {
			return { SimplificationError::EMPTY };
		}
		if (remaining < settings.minimum_triangles) {
			return { SimplificationError::MINIMUM_VIOLATION };
		}
		std::vector<Point> positions(vertices.size());
		for (uint32_t vertex = 0; vertex < vertices.size(); ++vertex) {
			positions[vertex] = position(vertex);
		}
		if (settings.preserve_area && !dilate(p_deltas, positions)) {
			return { SimplificationError::NUMERIC, AttributePolicyError::OK, numeric_error };
		}
		SimplifiedMesh candidate;
		std::vector<uint32_t> remap(vertices.size(), UINT32_MAX);
		for (uint32_t vertex = 0; vertex < vertices.size(); ++vertex) {
			if (vertices[vertex].references == 0) {
				continue;
			}
			remap[vertex] = static_cast<uint32_t>(candidate.mesh.positions.size());
			std::array<float, 3> point;
			for (size_t axis = 0; axis < 3; ++axis) {
				point[axis] = static_cast<float>(positions[vertex][axis] / policy.position_scale);
				if (!std::isfinite(point[axis])) {
					return { SimplificationError::NUMERIC, AttributePolicyError::OK, QuadricError::NON_FINITE };
				}
			}
			candidate.mesh.positions.push_back(point);
			for (double attribute : vertices[vertex].attributes) {
				candidate.attributes.push_back(static_cast<float>(attribute));
			}
		}
		for (const Triangle &triangle : triangles) {
			if (!triangle.alive) {
				continue;
			}
			candidate.mesh.materials.push_back(triangle.material);
			for (uint32_t side = 0; side < 3; ++side) {
				const uint32_t first = triangle.vertices[side];
				const uint32_t second = triangle.vertices[(side + 1) % 3];
				candidate.mesh.indices.push_back(remap[first]);
				const auto locked = locked_edges.find({ positions[first], positions[second] });
				candidate.external_edges.push_back(locked != locked_edges.end() ? locked->second : 0);
			}
		}
		report.reached_target = remaining <= settings.target_triangles;
		report.lod_error = std::sqrt(max_error) / policy.position_scale;
		candidate.report = report;
		if (validate_triangle_mesh(candidate.mesh, TriangleValidation::BUILD_INTERMEDIATE) != TopologyError::OK) {
			return { SimplificationError::NUMERIC, AttributePolicyError::OK, QuadricError::NON_FINITE };
		}
		r_output = std::move(candidate);
		return {};
	}

public:
	Simplifier(const SimplificationInput &p_input, const SimplificationSettings &p_settings) :
			input(p_input), settings(p_settings) {}

	SimplificationResult run(SimplifiedMesh &r_output) {
		const AttributePolicyError prepared = prepare_attribute_policy(input.mesh, input.layout, input.attributes, policy, input.validation);
		if (prepared != AttributePolicyError::OK) {
			return { SimplificationError::ATTRIBUTE_POLICY, prepared };
		}
		if (!input.external_edges.empty() && input.external_edges.size() != input.mesh.indices.size()) {
			return { SimplificationError::INVALID_EXTERNAL_EDGES };
		}
		vertices.resize(input.mesh.positions.size());
		const size_t stride = input.layout.scalar_count();
		for (uint32_t vertex = 0; vertex < vertices.size(); ++vertex) {
			Point point;
			for (size_t axis = 0; axis < 3; ++axis) {
				point[axis] = input.mesh.positions[vertex][axis] * policy.position_scale;
			}
			if (!round_point(point)) {
				return { SimplificationError::NUMERIC, AttributePolicyError::OK, QuadricError::NON_FINITE };
			}
			const auto inserted = position_groups.emplace(point, static_cast<uint32_t>(groups.size()));
			if (inserted.second) {
				PositionGroup group;
				group.position = point;
				groups.push_back(std::move(group));
			}
			vertices[vertex].group = inserted.first->second;
			vertices[vertex].attributes.assign(input.attributes.begin() + vertex * stride, input.attributes.begin() + (vertex + 1) * stride);
			if (correct_vertex_attributes(input.layout, vertices[vertex].attributes) != AttributePolicyError::OK) {
				return { SimplificationError::NUMERIC };
			}
			for (double &value : vertices[vertex].attributes) {
				value = static_cast<float>(value);
			}
		}
		triangles.resize(input.mesh.materials.size());
		remaining = static_cast<uint32_t>(triangles.size());
		for (uint32_t triangle = 0; triangle < triangles.size(); ++triangle) {
			Triangle &record = triangles[triangle];
			record.material = input.mesh.materials[triangle];
			record.uv_orientation = policy.uv_orientation_masks[triangle];
			for (uint32_t side = 0; side < 3; ++side) {
				const uint32_t corner = triangle * 3 + side;
				record.vertices[side] = input.mesh.indices[corner];
				++vertices[record.vertices[side]].references;
				groups[vertices[record.vertices[side]].group].corners.insert(corner);
			}
		}
		for (uint32_t corner = 0; corner < input.external_edges.size(); ++corner) {
			const uint32_t count = input.external_edges[corner];
			if (count == 0) {
				continue;
			}
			const uint32_t first = corner_group(corner);
			const uint32_t second = corner_group(next_corner(corner));
			groups[first].locked = true;
			groups[second].locked = true;
			const auto inserted = locked_edges.emplace(EdgeKey{ groups[first].position, groups[second].position }, count);
			if (!inserted.second && inserted.first->second != count) {
				return { SimplificationError::INVALID_EXTERNAL_EDGES };
			}
		}
		for (uint32_t triangle = 0; triangle < triangles.size(); ++triangle) {
			if (!fix_triangle(triangle)) {
				return { SimplificationError::NUMERIC, AttributePolicyError::OK, numeric_error };
			}
		}
		for (uint32_t triangle = 0; triangle < triangles.size(); ++triangle) {
			if (triangles[triangle].alive) {
				register_triangle_pairs(triangle);
			}
		}
		for (uint32_t pair = 0; pair < pairs.size(); ++pair) {
			if (!update_pair(pair)) {
				return { SimplificationError::NUMERIC, AttributePolicyError::OK, numeric_error };
			}
		}
		std::map<uint32_t, double> area_deltas;
		while (remaining > settings.target_triangles && !heap.empty()) {
			const uint32_t pair = heap.top();
			heap.remove(pair, pairs);
			MergeCandidate candidate;
			if (!evaluate_pair(pair, candidate)) {
				return { SimplificationError::NUMERIC, AttributePolicyError::OK, numeric_error };
			}
			if (candidate.removes_all) {
				continue;
			}
			uint32_t removed = 0;
			const PairKey key = pairs[pair].groups;
			for (uint32_t triangle : candidate.triangles) {
				uint32_t moved = 0;
				for (uint32_t vertex : triangles[triangle].vertices) {
					const uint32_t group = vertices[vertex].group;
					moved += group == key.first || group == key.second ? 1u : 0u;
				}
				removed += moved > 1 ? 1u : 0u;
			}
			if (removed >= remaining || remaining - removed < settings.minimum_triangles) {
				continue;
			}
			if (!apply_pair(pair, candidate, area_deltas)) {
				return { SimplificationError::NUMERIC, AttributePolicyError::OK, numeric_error };
			}
		}
		// The selected reference also has this final face-removal path. Record it
		// separately: neither its heuristic error nor dilation proves image quality.
		for (uint32_t triangle = 0; remaining > settings.target_triangles && remaining > settings.minimum_triangles && triangle < triangles.size(); ++triangle) {
			if (triangles[triangle].alive) {
				remove_triangle(triangle);
				++report.fallback_removed_triangles;
			}
		}
		return export_mesh(area_deltas, r_output);
	}
};

} // namespace

SimplificationResult simplify_mesh(const SimplificationInput &p_input, const SimplificationSettings &p_settings, SimplifiedMesh &r_output) {
	if (p_settings.target_triangles == 0 || p_settings.minimum_triangles == 0 || p_settings.minimum_triangles > p_settings.target_triangles || !std::isfinite(p_settings.max_edge_length_factor) || p_settings.max_edge_length_factor < 0 || (p_settings.max_edge_length_factor > 0 && (!std::isfinite(p_settings.max_edge_length_factor * p_settings.max_edge_length_factor) || p_settings.max_edge_length_factor * p_settings.max_edge_length_factor == 0))) {
		return { SimplificationError::INVALID_SETTINGS };
	}
	try {
		Simplifier simplifier(p_input, p_settings);
		return simplifier.run(r_output);
	} catch (const std::bad_alloc &) {
		return { SimplificationError::MEMORY };
	}
}

} // namespace VirtualGeometry
