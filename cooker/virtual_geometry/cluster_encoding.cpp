#include "cluster_encoding.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <new>
#include <numeric>
#include <utility>

namespace VirtualGeometry {
namespace {

bool finite_sphere(const GeometrySphere &p_sphere) {
	return std::isfinite(p_sphere.radius) && p_sphere.radius >= 0 &&
			std::all_of(p_sphere.center.begin(), p_sphere.center.end(), [](double p_value) { return std::isfinite(p_value); });
}

bool validate_dag(const GeometryDag &p_dag) {
	if (p_dag.clusters.empty() || p_dag.clusters.size() > UINT32_MAX / 2 || p_dag.groups.size() > UINT32_MAX ||
			p_dag.leaf_count == 0 || p_dag.leaf_count > p_dag.clusters.size() ||
			p_dag.root_cluster >= p_dag.clusters.size() || p_dag.root_group >= p_dag.groups.size()) {
		return false;
	}
	std::vector<uint8_t> child_owners(p_dag.clusters.size(), 0);
	std::vector<uint8_t> parent_owners(p_dag.clusters.size(), 0);
	for (uint32_t id = 0; id < p_dag.clusters.size(); ++id) {
		const auto &cluster = p_dag.clusters[id];
		PreparedAttributePolicy policy;
		if (cluster.mesh.materials.size() > MAX_CLUSTER_TRIANGLES || cluster.group >= p_dag.groups.size() ||
				cluster.external_edges.size() != cluster.mesh.indices.size() ||
				!std::isfinite(cluster.lod_error) || cluster.lod_error < 0 ||
				!finite_sphere(cluster.bounds) || !finite_sphere(cluster.lod_bounds) ||
				prepare_attribute_policy(cluster.mesh, p_dag.layout, cluster.attributes, policy, TriangleValidation::BUILD_INTERMEDIATE) != AttributePolicyError::OK) {
			return false;
		}
		if (id < p_dag.leaf_count) {
			if (cluster.generating_group != UINT32_MAX || cluster.level != 0) {
				return false;
			}
		} else if (cluster.generating_group >= cluster.group) {
			return false;
		}
	}
	for (uint32_t id = 0; id < p_dag.groups.size(); ++id) {
		const auto &group = p_dag.groups[id];
		if (group.children.empty() || group.children.size() > 32 || !finite_sphere(group.bounds) ||
				!finite_sphere(group.lod_bounds) || !std::isfinite(group.min_child_error) ||
				!std::isfinite(group.max_parent_error) || group.max_parent_error < 0 || group.min_child_error > group.max_parent_error ||
				group.root != (id == p_dag.root_group) || (!group.root && group.parents.empty())) {
			return false;
		}
		if (group.root && (group.children != std::vector<uint32_t>{ p_dag.root_cluster } || !group.parents.empty())) {
			return false;
		}
		for (uint32_t child : group.children) {
			if (child >= p_dag.clusters.size() || p_dag.clusters[child].group != id || child_owners[child]++ != 0) {
				return false;
			}
			for (uint32_t parent : group.parents) {
				if (parent >= p_dag.clusters.size() || parent <= child || p_dag.clusters[parent].level <= p_dag.clusters[child].level) {
					return false;
				}
			}
		}
		for (uint32_t parent : group.parents) {
			if (parent < p_dag.leaf_count || parent >= p_dag.clusters.size() || parent_owners[parent]++ != 0) {
				return false;
			}
			const auto &cluster = p_dag.clusters[parent];
			if (cluster.generating_group != id || cluster.lod_error != group.max_parent_error ||
					cluster.lod_bounds.center != group.lod_bounds.center || cluster.lod_bounds.radius != group.lod_bounds.radius) {
				return false;
			}
		}
	}
	for (uint32_t id = 0; id < p_dag.clusters.size(); ++id) {
		if (child_owners[id] != 1 || parent_owners[id] != (id < p_dag.leaf_count ? 0 : 1)) {
			return false;
		}
	}
	return true;
}

std::vector<uint32_t> sort_materials(const GeometryCluster &p_source, std::vector<uint32_t> p_triangles) {
	std::map<uint32_t, uint32_t> counts;
	for (uint32_t triangle : p_triangles) {
		++counts[p_source.mesh.materials[triangle]];
	}
	std::sort(p_triangles.begin(), p_triangles.end(), [&](uint32_t p_left, uint32_t p_right) {
		const uint32_t left = p_source.mesh.materials[p_left];
		const uint32_t right = p_source.mesh.materials[p_right];
		if (counts.at(left) != counts.at(right)) {
			return counts.at(left) > counts.at(right);
		}
		return left != right ? left < right : p_left < p_right;
	});
	return p_triangles;
}

EncodableCluster materialize(const GeometryCluster &p_source, const AttributeLayout &p_layout,
		uint32_t p_source_id, std::vector<uint32_t> p_triangles) {
	EncodableCluster result;
	result.source_cluster = p_source_id;
	result.source_triangles = sort_materials(p_source, std::move(p_triangles));
	result.geometry = p_source;
	result.geometry.mesh = {};
	result.geometry.attributes.clear();
	result.geometry.external_edges.clear();
	std::map<uint32_t, uint32_t> vertices;
	std::map<uint32_t, uint32_t> palette;
	for (uint32_t triangle : result.source_triangles) {
		palette.emplace(p_source.mesh.materials[triangle], 0);
	}
	for (auto &entry : palette) {
		entry.second = static_cast<uint32_t>(result.material_palette.size());
		result.material_palette.push_back(entry.first);
	}
	const size_t stride = p_layout.scalar_count();
	for (uint32_t triangle : result.source_triangles) {
		const uint32_t material = p_source.mesh.materials[triangle];
		const uint32_t local_material = palette.at(material);
		if (result.material_ranges.empty() || result.material_ranges.back().material != local_material) {
			result.material_ranges.push_back({ static_cast<uint32_t>(result.geometry.mesh.materials.size()), 0, local_material });
		}
		++result.material_ranges.back().count;
		result.geometry.mesh.materials.push_back(material);
		for (uint32_t side = 0; side < 3; ++side) {
			const uint32_t corner = triangle * 3 + side;
			const uint32_t vertex = p_source.mesh.indices[corner];
			const auto inserted = vertices.emplace(vertex, static_cast<uint32_t>(vertices.size()));
			if (inserted.second) {
				result.source_vertices.push_back(vertex);
				result.geometry.mesh.positions.push_back(p_source.mesh.positions[vertex]);
				result.geometry.attributes.insert(result.geometry.attributes.end(), p_source.attributes.begin() + vertex * stride,
						p_source.attributes.begin() + (vertex + 1) * stride);
			}
			result.geometry.mesh.indices.push_back(inserted.first->second);
			result.geometry.external_edges.push_back(p_source.external_edges[corner]);
		}
	}
	return result;
}

void apply_strip(StripifiedCluster p_strip, size_t p_stride, EncodableCluster &r_cluster) {
	const auto positions = std::move(r_cluster.geometry.mesh.positions);
	const auto attributes = std::move(r_cluster.geometry.attributes);
	const auto materials = std::move(r_cluster.geometry.mesh.materials);
	const auto external = std::move(r_cluster.geometry.external_edges);
	const auto vertices = std::move(r_cluster.source_vertices);
	const auto triangles = std::move(r_cluster.source_triangles);
	r_cluster.geometry.mesh.positions.clear();
	r_cluster.geometry.attributes.clear();
	r_cluster.geometry.mesh.materials.clear();
	r_cluster.geometry.external_edges.clear();
	r_cluster.source_vertices.clear();
	r_cluster.source_triangles.clear();
	for (uint32_t vertex : p_strip.source_vertices) {
		r_cluster.geometry.mesh.positions.push_back(positions[vertex]);
		r_cluster.geometry.attributes.insert(r_cluster.geometry.attributes.end(), attributes.begin() + vertex * p_stride,
				attributes.begin() + (vertex + 1) * p_stride);
		r_cluster.source_vertices.push_back(vertices[vertex]);
	}
	for (uint32_t local = 0; local < p_strip.source_triangles.size(); ++local) {
		const uint32_t triangle = p_strip.source_triangles[local];
		const uint8_t rotation = p_strip.corner_rotations[local];
		r_cluster.source_triangles.push_back(triangles[triangle]);
		r_cluster.source_corner_rotations.push_back(rotation);
		r_cluster.geometry.mesh.materials.push_back(materials[triangle]);
		for (uint32_t side = 0; side < 3; ++side) {
			r_cluster.geometry.external_edges.push_back(external[triangle * 3 + (rotation + side) % 3]);
		}
	}
	r_cluster.geometry.mesh.indices = std::move(p_strip.indices);
	r_cluster.topology = std::move(p_strip.topology);
}

void build_reuse_batches(EncodableCluster &r_cluster) {
	for (const auto &range : r_cluster.material_ranges) {
		std::array<bool, MAX_CLUSTER_VERTICES> used = {};
		uint32_t unique = 0;
		uint32_t count = 0;
		std::vector<uint8_t> batches;
		for (uint32_t triangle = range.start; triangle < range.start + range.count; ++triangle) {
			uint32_t additional = 0;
			for (uint32_t side = 0; side < 3; ++side) {
				additional += !used[r_cluster.geometry.mesh.indices[triangle * 3 + side]] ? 1u : 0u;
			}
			if (count == 32 || unique + additional > 32) {
				batches.push_back(static_cast<uint8_t>(count));
				used.fill(false);
				unique = 0;
				count = 0;
			}
			for (uint32_t side = 0; side < 3; ++side) {
				const uint32_t vertex = r_cluster.geometry.mesh.indices[triangle * 3 + side];
				unique += !used[vertex] ? 1u : 0u;
				used[vertex] = true;
			}
			++count;
		}
		if (count != 0) {
			batches.push_back(static_cast<uint8_t>(count));
		}
		r_cluster.reuse_batches.push_back(std::move(batches));
	}
}

ClusterEncodingResult split_cluster(const GeometryDag &p_dag, uint32_t p_id, EncodableGeometry &r_output) {
	const auto &source = p_dag.clusters[p_id];
	std::vector<uint32_t> all(source.mesh.materials.size());
	std::iota(all.begin(), all.end(), 0u);
	std::vector<std::vector<uint32_t>> pending;
	pending.push_back(std::move(all));
	const uint32_t begin = static_cast<uint32_t>(r_output.clusters.size());
	while (!pending.empty()) {
		auto part = materialize(source, p_dag.layout, p_id, std::move(pending.back()));
		pending.pop_back();
		StripifiedCluster strip;
		const auto stripped = stripify_cluster(part.geometry.mesh, strip);
		if (stripped.error != StripBuildError::OK) {
			ClusterEncodingResult error;
			error.error = stripped.error == StripBuildError::MEMORY ? ClusterEncodingError::MEMORY : ClusterEncodingError::TOPOLOGY;
			error.cluster = p_id;
			error.topology_error = stripped.codec_error;
			return error;
		}
		apply_strip(std::move(strip), p_dag.layout.scalar_count(), part);
		if (part.geometry.mesh.positions.size() > MAX_CLUSTER_VERTICES || part.material_palette.size() > MAX_CLUSTER_MATERIALS) {
			const auto middle = part.source_triangles.begin() + part.source_triangles.size() / 2;
			pending.emplace_back(middle, part.source_triangles.end());
			pending.emplace_back(part.source_triangles.begin(), middle);
			continue;
		}
		const auto packed = pack_cluster_materials(part.material_ranges, static_cast<uint32_t>(part.geometry.mesh.materials.size()), 0, part.materials);
		if (packed != MaterialCodecError::OK) {
			return { ClusterEncodingError::MATERIALS, p_id, packed };
		}
		build_reuse_batches(part);
		const auto batches = pack_vertex_reuse_batches(part.material_ranges, part.reuse_batches, part.reuse_batch_words);
		if (batches != TopologyCodecError::OK) {
			ClusterEncodingResult error;
			error.error = ClusterEncodingError::TOPOLOGY;
			error.cluster = p_id;
			error.topology_error = batches;
			return error;
		}
		r_output.clusters.push_back(std::move(part));
	}
	const uint32_t end = static_cast<uint32_t>(r_output.clusters.size());
	r_output.source_cluster_parts.push_back({ begin, end });
	if (end - begin > 1) {
		TriangleTopology topology;
		if (build_triangle_topology(source.mesh, topology, TriangleValidation::BUILD_INTERMEDIATE) != TopologyError::OK) {
			return { ClusterEncodingError::INVALID_DAG, p_id };
		}
		std::vector<uint32_t> owner(source.mesh.materials.size());
		for (uint32_t id = begin; id < end; ++id) {
			for (uint32_t triangle : r_output.clusters[id].source_triangles) {
				owner[triangle] = id;
			}
		}
		for (uint32_t id = begin; id < end; ++id) {
			auto &part = r_output.clusters[id];
			for (uint32_t local = 0; local < part.source_triangles.size(); ++local) {
				for (uint32_t side = 0; side < 3; ++side) {
					const uint32_t corner = part.source_triangles[local] * 3 + (part.source_corner_rotations[local] + side) % 3;
					uint64_t external = part.geometry.external_edges[local * 3 + side];
					for (uint32_t edge = topology.edge_offsets[corner]; edge < topology.edge_offsets[corner + 1]; ++edge) {
						external += owner[topology.adjacent_edges[edge] / 3] != id ? 1u : 0u;
					}
					if (external > UINT32_MAX) {
						return { ClusterEncodingError::LIMIT, p_id };
					}
					part.geometry.external_edges[local * 3 + side] = static_cast<uint32_t>(external);
				}
			}
		}
	}
	return {};
}

std::vector<uint32_t> expand(const std::vector<uint32_t> &p_ids, const EncodableGeometry &p_geometry) {
	std::vector<uint32_t> result;
	for (uint32_t id : p_ids) {
		const auto range = p_geometry.source_cluster_parts[id];
		for (uint32_t part = range.begin; part < range.end; ++part) {
			result.push_back(part);
		}
	}
	return result;
}

void inflate(double p_error, GeometrySphere &r_sphere) {
	if (p_error > 0) {
		r_sphere.radius = std::nextafter(r_sphere.radius + p_error, std::numeric_limits<double>::infinity());
	}
}

ClusterEncodingResult quantize(const ClusterEncodingSettings &p_settings, EncodableGeometry &r_geometry) {
	int32_t precision = p_settings.position_precision;
	if (precision == INT32_MIN) {
		double total_log_size = 0;
		uint32_t count = 0;
		for (uint32_t id = 0; id < r_geometry.leaf_count; ++id) {
			const auto &positions = r_geometry.clusters[id].geometry.mesh.positions;
			std::array<double, 3> low{ INFINITY, INFINITY, INFINITY };
			std::array<double, 3> high{ -INFINITY, -INFINITY, -INFINITY };
			for (const auto &point : positions) {
				for (size_t axis = 0; axis < 3; ++axis) {
					low[axis] = std::min(low[axis], static_cast<double>(point[axis]));
					high[axis] = std::max(high[axis], static_cast<double>(point[axis]));
				}
			}
			const double size = 0.5 * std::hypot(high[0] - low[0], high[1] - low[1], high[2] - low[2]);
			if (size > 0) {
				total_log_size += std::log2(size);
				++count;
			}
		}
		// UE's automatic floor is 1/16 cm. In Veya's meter units the nearest
		// finer power-of-two floor is 1/2048 m. Density still selects finer grids.
		precision = std::clamp(7 - static_cast<int32_t>(std::floor(total_log_size / std::max(1u, count) + 0.5)), 11, MAX_POSITION_PRECISION);
	}
	for (;;) {
		bool fits = true;
		for (auto &cluster : r_geometry.clusters) {
			const auto result = quantize_cluster_positions(cluster.geometry.mesh.positions, precision, cluster.positions);
			if (result != PositionCodecError::OK) {
				if (result == PositionCodecError::COORDINATE_RANGE && precision > MIN_POSITION_PRECISION) {
					fits = false;
					break;
				}
				return { ClusterEncodingError::POSITIONS, cluster.source_cluster, MaterialCodecError::OK, result };
			}
		}
		if (fits) {
			break;
		}
		--precision;
	}
	r_geometry.position_precision = precision;
	r_geometry.lower.fill(INFINITY);
	r_geometry.upper.fill(-INFINITY);
	for (auto &cluster : r_geometry.clusters) {
		std::vector<std::array<float, 3>> restored;
		const auto reconstructed = reconstruct_cluster_positions(cluster.positions, restored);
		if (reconstructed != PositionCodecError::OK) {
			return { ClusterEncodingError::POSITIONS, cluster.source_cluster, MaterialCodecError::OK, reconstructed };
		}
		for (size_t vertex = 0; vertex < restored.size(); ++vertex) {
			const auto &old = cluster.geometry.mesh.positions[vertex];
			const auto &point = restored[vertex];
			const double error = std::hypot(static_cast<double>(point[0]) - old[0], static_cast<double>(point[1]) - old[1], static_cast<double>(point[2]) - old[2]);
			r_geometry.max_position_error = std::max(r_geometry.max_position_error, error);
		}
		cluster.geometry.mesh.positions = std::move(restored);
		if (validate_triangle_mesh(cluster.geometry.mesh, TriangleValidation::BUILD_INTERMEDIATE) != TopologyError::OK) {
			return { ClusterEncodingError::QUANTIZATION_COLLAPSE, cluster.source_cluster };
		}
		const auto packed = pack_cluster_positions(cluster.positions, cluster.position_bytes);
		if (packed != PositionCodecError::OK) {
			return { ClusterEncodingError::POSITIONS, cluster.source_cluster, MaterialCodecError::OK, packed };
		}
		cluster.geometry.lower.fill(INFINITY);
		cluster.geometry.upper.fill(-INFINITY);
		for (const auto &point : cluster.geometry.mesh.positions) {
			for (size_t axis = 0; axis < 3; ++axis) {
				cluster.geometry.lower[axis] = std::min(cluster.geometry.lower[axis], point[axis]);
				cluster.geometry.upper[axis] = std::max(cluster.geometry.upper[axis], point[axis]);
				r_geometry.lower[axis] = std::min(r_geometry.lower[axis], static_cast<double>(point[axis]));
				r_geometry.upper[axis] = std::max(r_geometry.upper[axis], static_cast<double>(point[axis]));
			}
		}
	}
	if (r_geometry.max_position_error > 0) {
		r_geometry.max_position_error = std::nextafter(r_geometry.max_position_error, std::numeric_limits<double>::infinity());
	}
	for (auto &cluster : r_geometry.clusters) {
		inflate(r_geometry.max_position_error, cluster.geometry.bounds);
		inflate(r_geometry.max_position_error, cluster.geometry.lod_bounds);
	}
	for (auto &group : r_geometry.groups) {
		inflate(r_geometry.max_position_error, group.bounds);
		inflate(r_geometry.max_position_error, group.lod_bounds);
	}
	return {};
}

} // namespace

ClusterEncodingResult prepare_cluster_encoding(const GeometryDag &p_dag, const ClusterEncodingSettings &p_settings,
		EncodableGeometry &r_geometry) {
	if ((p_settings.position_precision != INT32_MIN &&
				(p_settings.position_precision < MIN_POSITION_PRECISION || p_settings.position_precision > MAX_POSITION_PRECISION)) ||
			p_settings.normal_bits == 0 || p_settings.normal_bits > 15 || p_settings.tangent_bits > 12 ||
			(p_dag.layout.has_tangents && p_settings.tangent_bits == 0)) {
		return { ClusterEncodingError::INVALID_SETTINGS };
	}
	try {
		if (!validate_dag(p_dag)) {
			return { ClusterEncodingError::INVALID_DAG };
		}
		EncodableGeometry candidate;
		candidate.layout = p_dag.layout;
		candidate.groups = p_dag.groups;
		candidate.root_group = p_dag.root_group;
		for (uint32_t id = 0; id < p_dag.clusters.size(); ++id) {
			const auto result = split_cluster(p_dag, id, candidate);
			if (result.error != ClusterEncodingError::OK) {
				return result;
			}
			if (id + 1 == p_dag.leaf_count) {
				candidate.leaf_count = static_cast<uint32_t>(candidate.clusters.size());
			}
		}
		for (auto &group : candidate.groups) {
			group.children = expand(group.children, candidate);
			group.parents = expand(group.parents, candidate);
		}
		candidate.root_clusters = candidate.groups[candidate.root_group].children;
		const auto quantized = quantize(p_settings, candidate);
		if (quantized.error != ClusterEncodingError::OK) {
			return quantized;
		}
		ClusterAttributeFormat format;
		format.has_tangents = p_dag.layout.has_tangents;
		format.has_colors = p_dag.layout.has_colors;
		format.uv_sets = p_dag.layout.uv_sets;
		format.lossless_uvs = !p_dag.layout.interpolate_uvs;
		format.normal_bits = p_settings.normal_bits;
		format.tangent_bits = p_settings.tangent_bits;
		for (auto &cluster : candidate.clusters) {
			const auto result = pack_cluster_attributes(format, cluster.geometry.attributes,
					static_cast<uint32_t>(cluster.geometry.mesh.positions.size()), cluster.encoded_attributes);
			if (result.error != AttributeCodecError::OK) {
				ClusterEncodingResult error;
				error.error = ClusterEncodingError::ATTRIBUTES;
				error.cluster = cluster.source_cluster;
				error.attribute_result = result;
				return error;
			}
			const auto &measured = cluster.encoded_attributes.errors;
			candidate.attribute_errors.normal_radians = std::max(candidate.attribute_errors.normal_radians, measured.normal_radians);
			candidate.attribute_errors.tangent_radians = std::max(candidate.attribute_errors.tangent_radians, measured.tangent_radians);
			candidate.attribute_errors.color_absolute = std::max(candidate.attribute_errors.color_absolute, measured.color_absolute);
			candidate.attribute_errors.uv_absolute = std::max(candidate.attribute_errors.uv_absolute, measured.uv_absolute);
			candidate.attribute_errors.uv_relative = std::max(candidate.attribute_errors.uv_relative, measured.uv_relative);
		}
		r_geometry = std::move(candidate);
		return {};
	} catch (const std::bad_alloc &) {
		return { ClusterEncodingError::MEMORY };
	}
}

} // namespace VirtualGeometry
