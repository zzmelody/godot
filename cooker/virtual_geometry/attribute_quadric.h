#ifndef VIRTUAL_GEOMETRY_ATTRIBUTE_QUADRIC_H
#define VIRTUAL_GEOMETRY_ATTRIBUTE_QUADRIC_H

#include <array>
#include <vector>

namespace VirtualGeometry {

using QuadricPoint = std::array<double, 3>;
using QuadricMatrix = std::array<double, 9>;

// Q(x) = x^T C x + 2 b^T x + c, where x = position - origin.
struct PositionQuadric {
	QuadricPoint origin = {};
	QuadricMatrix matrix = {};
	QuadricPoint linear = {};
	double constant = 0;
};

struct QuadricAttribute {
	QuadricPoint gradient = {};
	double offset = 0;
	double weight = 0;
};

// Adds area*s_i^2 - 2*s_i*(dot(g_i, x)+d_i) for each weighted attribute
// s_i = weight_i * attribute_i. Different wedges must stay separate until
// position optimization; only quadrics from the same wedge are accumulated.
struct AttributeQuadric {
	PositionQuadric position;
	double area = 0;
	std::vector<QuadricAttribute> attributes;
	QuadricPoint volume_normal = {};
	double volume_offset = 0;
};

struct QuadricOptimizer {
	PositionQuadric position;
	QuadricMatrix gradient_products = {};
	QuadricPoint gradient_offsets = {};
	double area = 0;
	QuadricPoint volume_normal = {};
	double volume_offset = 0;
	bool has_origin = false;
};

enum class QuadricError {
	OK,
	INVALID_INPUT,
	LAYOUT_MISMATCH,
	NON_FINITE,
	SINGULAR,
	MEMORY,
};

// Value-only offline math. All functions preserve outputs on refusal. Positions,
// attributes, weights and coefficients must be finite; weights are nonnegative.
// Triangle area <1e-12 produces a zero quadric, matching the selected UE baseline.
QuadricError make_attribute_quadric(const std::array<QuadricPoint, 3> &p_positions,
		const std::array<std::vector<double>, 3> &p_attributes,
		const std::vector<double> &p_weights, AttributeQuadric &r_quadric);
QuadricError rebase_attribute_quadric(const AttributeQuadric &p_quadric, const QuadricPoint &p_origin, AttributeQuadric &r_quadric);
QuadricError accumulate_attribute_quadric(const AttributeQuadric &p_quadric, AttributeQuadric &r_sum);
QuadricError evaluate_attribute_quadric(const AttributeQuadric &p_quadric, const QuadricPoint &p_position, const std::vector<double> &p_values, double &r_error);
// r_values must already have the layout's size. Zero-weight channels retain their
// previous value. Error is evaluated before normal/tangent/color correction.
QuadricError fit_quadric_attributes(const AttributeQuadric &p_quadric, const QuadricPoint &p_position, std::vector<double> &r_values, double &r_error);

// Edge constraint is distance to the infinite line, scaled by weight*length.
QuadricError make_edge_quadric(const QuadricPoint &p_start, const QuadricPoint &p_end, double p_weight, PositionQuadric &r_quadric);
QuadricError evaluate_position_quadric(const PositionQuadric &p_quadric, const QuadricPoint &p_position, double &r_error);
QuadricError accumulate_optimizer_wedge(const AttributeQuadric &p_quadric, QuadricOptimizer &r_optimizer);
QuadricError accumulate_optimizer_edge(const PositionQuadric &p_quadric, QuadricOptimizer &r_optimizer);

// The optimizer deliberately uses the selected UE implementation's total-area
// denominator across wedges. It does not claim exact independent-wedge Schur
// minimization. Singular results let the collapse caller try the next candidate.
QuadricError solve_quadric_position(const QuadricOptimizer &p_optimizer, QuadricPoint &r_position);
QuadricError solve_quadric_volume(const QuadricOptimizer &p_optimizer, QuadricPoint &r_position);
QuadricError solve_quadric_edge(const QuadricOptimizer &p_optimizer, const QuadricPoint &p_start, const QuadricPoint &p_end, QuadricPoint &r_position);

} // namespace VirtualGeometry

#endif // VIRTUAL_GEOMETRY_ATTRIBUTE_QUADRIC_H
