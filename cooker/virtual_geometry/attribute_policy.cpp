#include "attribute_policy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <new>
#include <utility>

namespace VirtualGeometry {
namespace {

AttributePolicyError prepare(const TriangleMeshInput &p_mesh, const AttributeLayout &p_layout, const std::vector<float> &p_values, PreparedAttributePolicy &r_policy) {
	PreparedAttributePolicy candidate;
	const size_t stride = p_layout.scalar_count();
	candidate.weights.resize(stride, 0);
	std::fill_n(candidate.weights.begin(), 3, 1.0);
	size_t channel = 3;
	if (p_layout.has_tangents) {
		std::fill_n(candidate.weights.begin() + channel, 3, 0.0625);
		candidate.weights[channel + 3] = 0.5;
		channel += 4;
	}
	if (p_layout.has_colors) {
		std::fill_n(candidate.weights.begin() + channel, 4, 0.0625);
	}
	std::array<double, 4> uv_areas = {};
	candidate.uv_orientation_masks.resize(p_mesh.materials.size(), 0);
	for (size_t triangle = 0; triangle < p_mesh.materials.size(); ++triangle) {
		const uint32_t vertex0 = p_mesh.indices[triangle * 3];
		const uint32_t vertex1 = p_mesh.indices[triangle * 3 + 1];
		const uint32_t vertex2 = p_mesh.indices[triangle * 3 + 2];
		std::array<double, 3> edge1;
		std::array<double, 3> edge2;
		for (size_t axis = 0; axis < 3; ++axis) {
			edge1[axis] = static_cast<double>(p_mesh.positions[vertex1][axis]) - p_mesh.positions[vertex0][axis];
			edge2[axis] = static_cast<double>(p_mesh.positions[vertex2][axis]) - p_mesh.positions[vertex0][axis];
		}
		candidate.surface_area += 0.5 * std::hypot(edge1[1] * edge2[2] - edge1[2] * edge2[1], edge1[2] * edge2[0] - edge1[0] * edge2[2], edge1[0] * edge2[1] - edge1[1] * edge2[0]);
		for (uint32_t uv = 0; uv < p_layout.uv_sets; ++uv) {
			const size_t offset = p_layout.uv_offset() + uv * 2;
			const double u0 = p_values[vertex0 * stride + offset];
			const double v0 = p_values[vertex0 * stride + offset + 1];
			const double u1 = p_values[vertex1 * stride + offset];
			const double v1 = p_values[vertex1 * stride + offset + 1];
			const double u2 = p_values[vertex2 * stride + offset];
			const double v2 = p_values[vertex2 * stride + offset + 1];
			const double signed_area = 0.5 * ((u1 - u0) * (v2 - v0) - (v1 - v0) * (u2 - u0));
			uv_areas[uv] += std::abs(signed_area);
			if (signed_area >= 0) {
				candidate.uv_orientation_masks[triangle] |= static_cast<uint8_t>(1u << uv);
			}
		}
	}
	const double triangles = static_cast<double>(p_mesh.materials.size());
	const float triangle_size = static_cast<float>(std::max(std::sqrt(candidate.surface_area / triangles), 0.00002));
	if (!std::isfinite(candidate.surface_area) || !std::isfinite(triangle_size)) {
		return AttributePolicyError::NON_FINITE;
	}
	candidate.position_scale = std::ldexp(1.0, std::clamp(-2 - std::ilogb(triangle_size), -126, 127));
	for (uint32_t uv = 0; uv < p_layout.uv_sets; ++uv) {
		if (!std::isfinite(uv_areas[uv])) {
			return AttributePolicyError::NON_FINITE;
		}
		const double weight = p_layout.interpolate_uvs ? 1 / (128 * std::max(std::sqrt(uv_areas[uv] / triangles), 1.0 / 1024)) : 0;
		candidate.weights[p_layout.uv_offset() + uv * 2] = weight;
		candidate.weights[p_layout.uv_offset() + uv * 2 + 1] = weight;
	}
	r_policy = std::move(candidate);
	return AttributePolicyError::OK;
}

bool normalize(std::array<double, 3> &r_vector) {
	const double length = std::hypot(r_vector[0], r_vector[1], r_vector[2]);
	if (!std::isfinite(length)) {
		return false;
	}
	if (length > 1e-4) {
		for (double &coordinate : r_vector) {
			coordinate /= length;
		}
	}
	return true;
}

} // namespace

AttributePolicyError prepare_attribute_policy(const TriangleMeshInput &p_mesh, const AttributeLayout &p_layout, const std::vector<float> &p_values, PreparedAttributePolicy &r_policy, TriangleValidation p_validation) {
	if (p_layout.uv_sets > 4) {
		return AttributePolicyError::INVALID_LAYOUT;
	}
	if (validate_triangle_mesh(p_mesh, p_validation) != TopologyError::OK) {
		return AttributePolicyError::INVALID_MESH;
	}
	const size_t stride = p_layout.scalar_count();
	if (p_values.size() % stride != 0 || p_values.size() / stride != p_mesh.positions.size() || !std::all_of(p_values.begin(), p_values.end(), [](float p_value) { return std::isfinite(p_value); })) {
		return AttributePolicyError::INVALID_ATTRIBUTES;
	}
	try {
		return prepare(p_mesh, p_layout, p_values, r_policy);
	} catch (const std::bad_alloc &) {
		return AttributePolicyError::MEMORY;
	}
}

AttributePolicyError correct_vertex_attributes(const AttributeLayout &p_layout, std::vector<double> &r_values) {
	if (p_layout.uv_sets > 4) {
		return AttributePolicyError::INVALID_LAYOUT;
	}
	if (r_values.size() != p_layout.scalar_count() || !std::all_of(r_values.begin(), r_values.end(), [](double p_value) { return std::isfinite(p_value); })) {
		return AttributePolicyError::INVALID_ATTRIBUTES;
	}
	std::array<double, 11> corrected = {};
	std::copy_n(r_values.begin(), p_layout.uv_offset(), corrected.begin());
	std::array<double, 3> normal = { corrected[0], corrected[1], corrected[2] };
	if (!normalize(normal)) {
		return AttributePolicyError::NON_FINITE;
	}
	std::copy(normal.begin(), normal.end(), corrected.begin());
	size_t channel = 3;
	if (p_layout.has_tangents) {
		std::array<double, 3> tangent = { corrected[3], corrected[4], corrected[5] };
		const double projection = normal[0] * tangent[0] + normal[1] * tangent[1] + normal[2] * tangent[2];
		for (size_t axis = 0; axis < 3; ++axis) {
			tangent[axis] -= normal[axis] * projection;
		}
		if (!std::all_of(tangent.begin(), tangent.end(), [](double p_value) { return std::isfinite(p_value); })) {
			return AttributePolicyError::NON_FINITE;
		}
		if (!normalize(tangent)) {
			return AttributePolicyError::NON_FINITE;
		}
		std::copy(tangent.begin(), tangent.end(), corrected.begin() + 3);
		corrected[6] = corrected[6] < 0 ? -1 : 1;
		channel += 4;
	}
	if (p_layout.has_colors) {
		for (size_t color = 0; color < 4; ++color) {
			corrected[channel + color] = std::clamp(corrected[channel + color], 0.0, 1.0);
		}
	}
	std::copy_n(corrected.begin(), p_layout.uv_offset(), r_values.begin());
	return AttributePolicyError::OK;
}

} // namespace VirtualGeometry
