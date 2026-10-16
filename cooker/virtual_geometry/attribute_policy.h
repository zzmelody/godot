#ifndef VIRTUAL_GEOMETRY_ATTRIBUTE_POLICY_H
#define VIRTUAL_GEOMETRY_ATTRIBUTE_POLICY_H

#include "triangle_topology.h"

#include <cstddef>

namespace VirtualGeometry {

// Vertex-major scalar order: normal3, optional tangent3/sign1, optional RGBA4,
// then UV pairs. More than four UV sets or undeclared channels are refused.
struct AttributeLayout {
	bool has_tangents = false;
	bool has_colors = false;
	uint32_t uv_sets = 0;
	bool interpolate_uvs = true;

	size_t uv_offset() const { return 3 + (has_tangents ? 4 : 0) + (has_colors ? 4 : 0); }
	size_t scalar_count() const { return uv_offset() + static_cast<size_t>(uv_sets) * 2; }
};

struct PreparedAttributePolicy {
	std::vector<double> weights;
	std::vector<uint8_t> uv_orientation_masks;
	double surface_area = 0;
	double position_scale = 1;
};

enum class AttributePolicyError {
	OK,
	INVALID_LAYOUT,
	INVALID_MESH,
	INVALID_ATTRIBUTES,
	NON_FINITE,
	MEMORY,
};

// Prepares the selected UE cluster simplification weights and power-of-two
// position normalization. UV orientation stays separate from material identity.
// Does not scale or modify source data. Old output survives every refusal.
AttributePolicyError prepare_attribute_policy(const TriangleMeshInput &p_mesh,
		const AttributeLayout &p_layout, const std::vector<float> &p_values,
		PreparedAttributePolicy &r_policy, TriangleValidation p_validation = TriangleValidation::SOURCE);

// Corrects one fitted vertex after its error has been evaluated: normalize normal,
// orthogonalize/normalize tangent, restore sign and clamp all RGBA channels.
// UVs remain unchanged. Refusal preserves the whole input/output array.
AttributePolicyError correct_vertex_attributes(const AttributeLayout &p_layout, std::vector<double> &r_values);

} // namespace VirtualGeometry

#endif // VIRTUAL_GEOMETRY_ATTRIBUTE_POLICY_H
