#include "cluster_dag.h"

#include "graph_locality.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <new>
#include <numeric>
#include <utility>

namespace VirtualGeometry {
namespace {

using Point = std::array<double, 3>;
using Position = std::array<float, 3>;
using Edge = std::pair<Position, Position>;
using EdgeOwners = std::map<Edge, std::map<uint32_t, uint32_t>>;

double distance(const Point &p_left, const Point &p_right) {
	return std::hypot(p_left[0] - p_right[0], p_left[1] - p_right[1], p_left[2] - p_right[2]);
}

void include_sphere(const GeometrySphere &p_other, GeometrySphere &r_sphere) {
	const double separation = distance(r_sphere.center, p_other.center);
	if (separation + p_other.radius <= r_sphere.radius) {
		return;
	}
	if (separation + r_sphere.radius <= p_other.radius) {
		r_sphere = p_other;
		return;
	}
	const double radius = 0.5 * (separation + r_sphere.radius + p_other.radius);
	if (separation > 0) {
		const double shift = (radius - r_sphere.radius) / separation;
		for (size_t axis = 0; axis < 3; ++axis) {
			r_sphere.center[axis] += (p_other.center[axis] - r_sphere.center[axis]) * shift;
		}
	}
	r_sphere.radius = radius;
}

GeometrySphere sphere_union(const std::vector<GeometrySphere> &p_spheres) {
	std::array<size_t, 3> lower = {};
	std::array<size_t, 3> upper = {};
	for (size_t index = 1; index < p_spheres.size(); ++index) {
		for (size_t axis = 0; axis < 3; ++axis) {
			if (p_spheres[index].center[axis] - p_spheres[index].radius < p_spheres[lower[axis]].center[axis] - p_spheres[lower[axis]].radius) {
				lower[axis] = index;
			}
			if (p_spheres[index].center[axis] + p_spheres[index].radius > p_spheres[upper[axis]].center[axis] + p_spheres[upper[axis]].radius) {
				upper[axis] = index;
			}
		}
	}
	size_t largest_axis = 0;
	double largest_distance = -1;
	for (size_t axis = 0; axis < 3; ++axis) {
		const auto &first = p_spheres[lower[axis]];
		const auto &second = p_spheres[upper[axis]];
		const double span = distance(first.center, second.center) + first.radius + second.radius;
		if (span > largest_distance) {
			largest_axis = axis;
			largest_distance = span;
		}
	}
	GeometrySphere result = p_spheres[lower[largest_axis]];
	include_sphere(p_spheres[upper[largest_axis]], result);
	for (const auto &sphere : p_spheres) {
		include_sphere(sphere, result);
	}
	// Recompute at the final center; rounding during incremental expansion must
	// never make a previous child fall outside its parent's LOD bound.
	for (const auto &sphere : p_spheres) {
		result.radius = std::max(result.radius, distance(result.center, sphere.center) + sphere.radius);
	}
	if (result.radius > 0) {
		result.radius = std::nextafter(result.radius, std::numeric_limits<double>::infinity());
	}
	return result;
}

void bound_cluster(GeometryCluster &r_cluster) {
	r_cluster.lower.fill(std::numeric_limits<float>::infinity());
	r_cluster.upper.fill(-std::numeric_limits<float>::infinity());
	std::vector<GeometrySphere> points;
	points.reserve(r_cluster.mesh.positions.size());
	for (const auto &position : r_cluster.mesh.positions) {
		points.push_back({ { position[0], position[1], position[2] }, 0 });
		for (size_t axis = 0; axis < 3; ++axis) {
			r_cluster.lower[axis] = std::min(r_cluster.lower[axis], position[axis]);
			r_cluster.upper[axis] = std::max(r_cluster.upper[axis], position[axis]);
		}
	}
	r_cluster.bounds = sphere_union(points);
}

GeometryCluster materialize_cluster(const TriangleMeshInput &p_mesh, const std::vector<float> &p_attributes, size_t p_stride, const LeafCluster &p_leaf) {
	GeometryCluster result;
	result.mesh.indices = p_leaf.indices;
	result.mesh.materials = p_leaf.materials;
	result.external_edges = p_leaf.external_edges;
	result.mesh.positions.reserve(p_leaf.source_vertices.size());
	result.attributes.reserve(p_leaf.source_vertices.size() * p_stride);
	for (uint32_t vertex : p_leaf.source_vertices) {
		result.mesh.positions.push_back(p_mesh.positions[vertex]);
		result.attributes.insert(result.attributes.end(), p_attributes.begin() + vertex * p_stride, p_attributes.begin() + (vertex + 1) * p_stride);
	}
	bound_cluster(result);
	result.lod_bounds = result.bounds;
	return result;
}

Edge edge_at(const TriangleMeshInput &p_mesh, uint32_t p_corner) {
	const uint32_t next = p_corner / 3 * 3 + (p_corner + 1) % 3;
	return { p_mesh.positions[p_mesh.indices[p_corner]], p_mesh.positions[p_mesh.indices[next]] };
}

uint32_t find_root(std::vector<uint32_t> &r_roots, uint32_t p_node) {
	while (r_roots[p_node] != p_node) {
		p_node = r_roots[p_node] = r_roots[r_roots[p_node]];
	}
	return p_node;
}

DagBuildResult partition_level(const GeometryDag &p_dag, const std::vector<uint32_t> &p_level, std::vector<std::vector<uint32_t>> &r_groups) {
	if (p_level.size() <= 32) {
		r_groups.push_back(p_level);
		return {};
	}
	const uint32_t count = static_cast<uint32_t>(p_level.size());
	EdgeOwners owners;
	for (uint32_t local = 0; local < count; ++local) {
		const auto &cluster = p_dag.clusters[p_level[local]];
		for (uint32_t corner = 0; corner < cluster.external_edges.size(); ++corner) {
			if (cluster.external_edges[corner] != 0) {
				++owners[edge_at(cluster.mesh, corner)][local];
			}
		}
	}
	std::vector<std::map<uint32_t, uint64_t>> shared(count);
	uint64_t directed_matches = 0;
	for (const auto &entry : owners) {
		const auto opposite = owners.find({ entry.first.second, entry.first.first });
		if (opposite == owners.end()) {
			continue;
		}
		for (const auto &first : entry.second) {
			for (const auto &second : opposite->second) {
				if (first.first != second.first) {
					const uint64_t matches = static_cast<uint64_t>(first.second) * second.second;
					directed_matches += matches;
					if (directed_matches > INT32_MAX) {
						return { DagBuildError::LIMIT };
					}
					shared[first.first][second.first] += matches;
				}
			}
		}
	}
	std::vector<uint32_t> roots(count);
	std::iota(roots.begin(), roots.end(), 0u);
	std::vector<Point> centers(count);
	for (uint32_t local = 0; local < count; ++local) {
		for (const auto &neighbor : shared[local]) {
			const uint32_t first = find_root(roots, local);
			const uint32_t second = find_root(roots, neighbor.first);
			roots[std::max(first, second)] = std::min(first, second);
		}
		const auto &cluster = p_dag.clusters[p_level[local]];
		for (size_t axis = 0; axis < 3; ++axis) {
			centers[local][axis] = 0.5 * (static_cast<double>(cluster.lower[axis]) + cluster.upper[axis]);
		}
	}
	for (uint32_t local = 0; local < count; ++local) {
		roots[local] = find_root(roots, local);
	}
	GraphLocality locality;
	const LocalityError localized = build_graph_locality(centers, roots, {}, p_dag.lower, p_dag.upper, locality);
	if (localized != LocalityError::OK) {
		return { localized == LocalityError::MEMORY ? DagBuildError::MEMORY : DagBuildError::INVALID_INPUT };
	}
	std::vector<uint32_t> inverse(count);
	for (uint32_t index = 0; index < count; ++index) {
		inverse[locality.order[index]] = index;
	}
	std::vector<std::map<uint32_t, uint64_t>> rows(count);
	for (uint32_t local = 0; local < count; ++local) {
		for (const auto &neighbor : shared[local]) {
			const auto &first = p_dag.clusters[p_level[local]];
			const auto &second = p_dag.clusters[p_level[neighbor.first]];
			const bool siblings = first.group != UINT32_MAX && first.group == second.group;
			rows[inverse[local]][inverse[neighbor.first]] += neighbor.second * (siblings ? 1u : 16u) + 4;
		}
	}
	for (const auto &link : locality.links) {
		++rows[inverse[link.first]][inverse[link.second]];
		++rows[inverse[link.second]][inverse[link.first]];
	}
	PartitionGraph graph;
	graph.offsets.push_back(0);
	uint64_t cost = 0;
	for (const auto &row : rows) {
		for (const auto &neighbor : row) {
			cost += neighbor.second;
			if (cost > INT32_MAX) {
				return { DagBuildError::LIMIT };
			}
			graph.neighbors.push_back(static_cast<int32_t>(neighbor.first));
			graph.costs.push_back(static_cast<int32_t>(neighbor.second));
		}
		graph.offsets.push_back(static_cast<int32_t>(graph.neighbors.size()));
	}
	GraphPartition partition;
	const PartitionResult result = partition_strict(graph, 8, 32, partition);
	if (result.error != PartitionError::OK) {
		DagBuildResult error;
		error.error = DagBuildError::PARTITION;
		error.partition_result = result;
		return error;
	}
	for (const auto &range : partition.ranges) {
		std::vector<uint32_t> children;
		for (uint32_t index = range.begin; index < range.end; ++index) {
			children.push_back(p_level[locality.order[partition.vertices[index]]]);
		}
		std::sort(children.begin(), children.end());
		r_groups.push_back(std::move(children));
	}
	return {};
}

SimplificationInput merge_children(const GeometryDag &p_dag, const std::vector<uint32_t> &p_children, uint32_t &r_clamped) {
	SimplificationInput merged;
	merged.layout = p_dag.layout;
	merged.validation = TriangleValidation::BUILD_INTERMEDIATE;
	const size_t stride = p_dag.layout.scalar_count();
	std::map<std::vector<float>, uint32_t> vertex_ids;
	std::vector<uint32_t> corner_owners;
	for (uint32_t owner = 0; owner < p_children.size(); ++owner) {
		const auto &child = p_dag.clusters[p_children[owner]];
		std::vector<uint32_t> remap(child.mesh.positions.size(), UINT32_MAX);
		for (uint32_t vertex : child.mesh.indices) {
			if (remap[vertex] == UINT32_MAX) {
				const auto &position = child.mesh.positions[vertex];
				std::vector<float> key{ position[0], position[1], position[2] };
				key.insert(key.end(), child.attributes.begin() + vertex * stride, child.attributes.begin() + (vertex + 1) * stride);
				const auto inserted = vertex_ids.emplace(std::move(key), static_cast<uint32_t>(merged.mesh.positions.size()));
				if (inserted.second) {
					merged.mesh.positions.push_back(position);
					merged.attributes.insert(merged.attributes.end(), child.attributes.begin() + vertex * stride, child.attributes.begin() + (vertex + 1) * stride);
				}
				remap[vertex] = inserted.first->second;
			}
			merged.mesh.indices.push_back(remap[vertex]);
			corner_owners.push_back(owner);
		}
		merged.mesh.materials.insert(merged.mesh.materials.end(), child.mesh.materials.begin(), child.mesh.materials.end());
		merged.external_edges.insert(merged.external_edges.end(), child.external_edges.begin(), child.external_edges.end());
	}
	EdgeOwners owners;
	for (uint32_t corner = 0; corner < merged.mesh.indices.size(); ++corner) {
		++owners[edge_at(merged.mesh, corner)][corner_owners[corner]];
	}
	for (uint32_t corner = 0; corner < merged.mesh.indices.size(); ++corner) {
		const Edge edge = edge_at(merged.mesh, corner);
		const auto opposite = owners.find({ edge.second, edge.first });
		uint64_t inside = 0;
		if (opposite != owners.end()) {
			for (const auto &entry : opposite->second) {
				if (entry.first != corner_owners[corner]) {
					inside += entry.second;
				}
			}
		}
		uint32_t &external = merged.external_edges[corner];
		if (inside > external) {
			++r_clamped;
			external = 0;
		} else {
			external -= static_cast<uint32_t>(inside);
		}
	}
	return merged;
}

void include_bounds(const GeometryCluster &p_cluster, GeometryDag &r_dag) {
	for (size_t axis = 0; axis < 3; ++axis) {
		r_dag.lower[axis] = std::min(r_dag.lower[axis], static_cast<double>(p_cluster.lower[axis]));
		r_dag.upper[axis] = std::max(r_dag.upper[axis], static_cast<double>(p_cluster.upper[axis]));
	}
}

DagBuildResult reduce_group(const DagBuildSettings &p_settings, const std::vector<uint32_t> &p_children, GeometryDag &r_dag, std::vector<uint32_t> &r_next) {
	const uint32_t group_id = static_cast<uint32_t>(r_dag.groups.size());
	GeometryGroup group;
	group.children = p_children;
	const SimplificationInput merged = merge_children(r_dag, p_children, group.clamped_external_edges);
	const uint32_t wanted = static_cast<uint32_t>((merged.mesh.materials.size() + 255) / 256);
	group.target_parents = wanted;
	std::vector<GeometryCluster> parents;
	for (uint32_t target_size = 126; target_size > 64; target_size -= 2) {
		++group.attempts;
		SimplifiedMesh simplified;
		if (static_cast<uint64_t>(wanted) * target_size >= merged.mesh.materials.size()) {
			simplified.mesh = merged.mesh;
			simplified.attributes = merged.attributes;
			simplified.external_edges = merged.external_edges;
			simplified.report.reached_target = true;
		} else {
			SimplificationSettings settings;
			settings.target_triangles = wanted * target_size;
			settings.max_edge_length_factor = p_settings.max_edge_length_factor;
			settings.preserve_area = p_settings.preserve_area;
			const SimplificationResult result = simplify_mesh(merged, settings, simplified);
			if (result.error != SimplificationError::OK) {
				DagBuildResult error;
				error.error = DagBuildError::SIMPLIFICATION;
				error.group = group_id;
				error.simplification_result = result;
				return error;
			}
		}
		if (simplified.report.locked_position_moves != 0 || simplified.report.fallback_removed_triangles != 0) {
			DagBuildResult error;
			error.error = simplified.report.locked_position_moves != 0 ? DagBuildError::BOUNDARY_CHANGE : DagBuildError::FALLBACK_REMOVAL;
			error.group = group_id;
			error.simplification_report = simplified.report;
			return error;
		}
		if (wanted == 1) {
			GeometryCluster parent;
			parent.mesh = std::move(simplified.mesh);
			parent.attributes = std::move(simplified.attributes);
			parent.external_edges = std::move(simplified.external_edges);
			bound_cluster(parent);
			parents.push_back(std::move(parent));
		} else {
			MeshLeafClusters split;
			const LeafResult result = build_leaf_clusters(simplified.mesh, split, TriangleValidation::BUILD_INTERMEDIATE, simplified.external_edges);
			if (result.error != LeafError::OK) {
				DagBuildResult error;
				error.error = DagBuildError::LEAF_BUILD;
				error.group = group_id;
				error.leaf_result = result;
				return error;
			}
			if (split.clusters.size() > wanted) {
				continue;
			}
			for (const auto &leaf : split.clusters) {
				parents.push_back(materialize_cluster(simplified.mesh, simplified.attributes, r_dag.layout.scalar_count(), leaf));
			}
		}
		group.simplification = simplified.report;
		break;
	}
	if (parents.empty()) {
		DagBuildResult error;
		error.error = DagBuildError::RETRY_EXHAUSTED;
		error.group = group_id;
		return error;
	}
	std::vector<GeometrySphere> draw_spheres;
	std::vector<GeometrySphere> lod_spheres;
	group.min_child_error = std::numeric_limits<double>::infinity();
	group.max_parent_error = group.simplification.lod_error;
	for (uint32_t child : p_children) {
		auto &cluster = r_dag.clusters[child];
		draw_spheres.push_back(cluster.bounds);
		lod_spheres.push_back(cluster.lod_bounds);
		group.min_child_error = std::min(group.min_child_error, cluster.generating_group == UINT32_MAX ? -1.0 : cluster.lod_error);
		group.max_parent_error = std::max(group.max_parent_error, cluster.lod_error);
		group.level = std::max(group.level, cluster.level);
		cluster.group = group_id;
	}
	group.bounds = sphere_union(draw_spheres);
	group.lod_bounds = sphere_union(lod_spheres);
	for (auto &parent : parents) {
		if (r_dag.clusters.size() >= UINT32_MAX) {
			return { DagBuildError::LIMIT };
		}
		parent.lod_bounds = group.lod_bounds;
		parent.lod_error = group.max_parent_error;
		parent.level = group.level + 1;
		parent.generating_group = group_id;
		include_bounds(parent, r_dag);
		const uint32_t id = static_cast<uint32_t>(r_dag.clusters.size());
		group.parents.push_back(id);
		r_next.push_back(id);
		r_dag.clusters.push_back(std::move(parent));
	}
	r_dag.groups.push_back(std::move(group));
	return {};
}

DagBuildResult build(const GeometryBuildInput &p_input, const DagBuildSettings &p_settings, GeometryDag &r_dag) {
	PreparedAttributePolicy policy;
	if (prepare_attribute_policy(p_input.mesh, p_input.layout, p_input.attributes, policy) != AttributePolicyError::OK) {
		return { DagBuildError::INVALID_INPUT };
	}
	const size_t stride = p_input.layout.scalar_count();
	std::vector<float> corrected(p_input.attributes.size());
	std::vector<double> vertex(stride);
	for (size_t index = 0; index < p_input.mesh.positions.size(); ++index) {
		std::copy_n(p_input.attributes.begin() + index * stride, stride, vertex.begin());
		if (correct_vertex_attributes(p_input.layout, vertex) != AttributePolicyError::OK) {
			return { DagBuildError::INVALID_INPUT };
		}
		for (size_t channel = 0; channel < stride; ++channel) {
			corrected[index * stride + channel] = static_cast<float>(vertex[channel]);
		}
	}
	MeshLeafClusters leaves;
	const LeafResult initial = build_leaf_clusters(p_input.mesh, leaves);
	if (initial.error != LeafError::OK) {
		DagBuildResult error;
		error.error = DagBuildError::LEAF_BUILD;
		error.leaf_result = initial;
		return error;
	}
	GeometryDag candidate;
	candidate.layout = p_input.layout;
	candidate.lower.fill(std::numeric_limits<double>::infinity());
	candidate.upper.fill(-std::numeric_limits<double>::infinity());
	candidate.source_triangle_clusters = std::move(leaves.triangle_cluster);
	candidate.leaf_count = static_cast<uint32_t>(leaves.clusters.size());
	std::vector<uint32_t> level;
	for (const auto &leaf : leaves.clusters) {
		GeometryCluster cluster = materialize_cluster(p_input.mesh, corrected, stride, leaf);
		include_bounds(cluster, candidate);
		level.push_back(static_cast<uint32_t>(candidate.clusters.size()));
		candidate.clusters.push_back(std::move(cluster));
	}
	while (level.size() > 1) {
		std::vector<std::vector<uint32_t>> groups;
		const DagBuildResult grouped = partition_level(candidate, level, groups);
		if (grouped.error != DagBuildError::OK) {
			return grouped;
		}
		std::vector<uint32_t> next;
		for (const auto &children : groups) {
			const DagBuildResult reduced = reduce_group(p_settings, children, candidate, next);
			if (reduced.error != DagBuildError::OK) {
				return reduced;
			}
		}
		if (next.empty() || next.size() >= level.size()) {
			return { DagBuildError::NO_PROGRESS };
		}
		level = std::move(next);
	}
	candidate.root_cluster = level.front();
	candidate.root_group = static_cast<uint32_t>(candidate.groups.size());
	auto &root = candidate.clusters[candidate.root_cluster];
	root.group = candidate.root_group;
	GeometryGroup root_group;
	root_group.root = true;
	root_group.children.push_back(candidate.root_cluster);
	root_group.bounds = root.bounds;
	root_group.max_parent_error = std::max(1e10, root.lod_error);
	root_group.level = root.level + 1;
	candidate.groups.push_back(std::move(root_group));
	r_dag = std::move(candidate);
	return {};
}

} // namespace

DagBuildResult build_cluster_dag(const GeometryBuildInput &p_input, const DagBuildSettings &p_settings, GeometryDag &r_dag) {
	const double factor_squared = p_settings.max_edge_length_factor * p_settings.max_edge_length_factor;
	if (!std::isfinite(p_settings.max_edge_length_factor) || p_settings.max_edge_length_factor < 0 || (p_settings.max_edge_length_factor > 0 && (!std::isfinite(factor_squared) || factor_squared == 0))) {
		return { DagBuildError::INVALID_INPUT };
	}
	try {
		return build(p_input, p_settings, r_dag);
	} catch (const std::bad_alloc &) {
		return { DagBuildError::MEMORY };
	}
}

} // namespace VirtualGeometry
