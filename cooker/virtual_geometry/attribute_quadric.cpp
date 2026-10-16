#include "attribute_quadric.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <new>
#include <utility>

namespace VirtualGeometry {
namespace {

QuadricPoint add(const QuadricPoint &p_left, const QuadricPoint &p_right) {
	return { p_left[0] + p_right[0], p_left[1] + p_right[1], p_left[2] + p_right[2] };
}

QuadricPoint subtract(const QuadricPoint &p_left, const QuadricPoint &p_right) {
	return { p_left[0] - p_right[0], p_left[1] - p_right[1], p_left[2] - p_right[2] };
}

QuadricPoint scale(const QuadricPoint &p_value, double p_scale) {
	return { p_value[0] * p_scale, p_value[1] * p_scale, p_value[2] * p_scale };
}

double dot(const QuadricPoint &p_left, const QuadricPoint &p_right) {
	return p_left[0] * p_right[0] + p_left[1] * p_right[1] + p_left[2] * p_right[2];
}

QuadricPoint cross(const QuadricPoint &p_left, const QuadricPoint &p_right) {
	return { p_left[1] * p_right[2] - p_left[2] * p_right[1], p_left[2] * p_right[0] - p_left[0] * p_right[2], p_left[0] * p_right[1] - p_left[1] * p_right[0] };
}

QuadricPoint multiply(const QuadricMatrix &p_matrix, const QuadricPoint &p_value) {
	QuadricPoint result = {};
	for (size_t row = 0; row < 3; ++row) {
		for (size_t column = 0; column < 3; ++column) {
			result[row] += p_matrix[row * 3 + column] * p_value[column];
		}
	}
	return result;
}

template <size_t N>
bool finite(const std::array<double, N> &p_values) {
	return std::all_of(p_values.begin(), p_values.end(), [](double p_value) { return std::isfinite(p_value); });
}

bool valid(const PositionQuadric &p_quadric) {
	return finite(p_quadric.origin) && finite(p_quadric.matrix) && finite(p_quadric.linear) && std::isfinite(p_quadric.constant) && p_quadric.matrix[1] == p_quadric.matrix[3] && p_quadric.matrix[2] == p_quadric.matrix[6] && p_quadric.matrix[5] == p_quadric.matrix[7];
}

bool valid(const AttributeQuadric &p_quadric) {
	if (!valid(p_quadric.position) || !std::isfinite(p_quadric.area) || p_quadric.area < 0 || !finite(p_quadric.volume_normal) || !std::isfinite(p_quadric.volume_offset)) {
		return false;
	}
	for (const auto &attribute : p_quadric.attributes) {
		if (!finite(attribute.gradient) || !std::isfinite(attribute.offset) || !std::isfinite(attribute.weight) || attribute.weight < 0) {
			return false;
		}
	}
	return true;
}

bool valid(const QuadricOptimizer &p_optimizer) {
	return valid(p_optimizer.position) && finite(p_optimizer.gradient_products) && finite(p_optimizer.gradient_offsets) && std::isfinite(p_optimizer.area) && p_optimizer.area >= 0 && finite(p_optimizer.volume_normal) && std::isfinite(p_optimizer.volume_offset);
}

PositionQuadric rebase(const PositionQuadric &p_quadric, const QuadricPoint &p_origin) {
	PositionQuadric result = p_quadric;
	const QuadricPoint delta = subtract(p_origin, p_quadric.origin);
	const QuadricPoint transformed = multiply(p_quadric.matrix, delta);
	result.origin = p_origin;
	result.linear = add(p_quadric.linear, transformed);
	result.constant += 2 * dot(p_quadric.linear, delta) + dot(delta, transformed);
	return result;
}

void accumulate(const PositionQuadric &p_quadric, PositionQuadric &r_sum) {
	for (size_t coefficient = 0; coefficient < 9; ++coefficient) {
		r_sum.matrix[coefficient] += p_quadric.matrix[coefficient];
	}
	r_sum.linear = add(r_sum.linear, p_quadric.linear);
	r_sum.constant += p_quadric.constant;
}

double evaluate(const PositionQuadric &p_quadric, const QuadricPoint &p_local) {
	return dot(p_local, multiply(p_quadric.matrix, p_local)) + 2 * dot(p_quadric.linear, p_local) + p_quadric.constant;
}

template <size_t N>
class LinearSystem {
	std::array<double, N * N> original;
	std::array<double, N * N> factor;
	std::array<size_t, N> permutation = {};

public:
	explicit LinearSystem(const std::array<double, N * N> &p_matrix) :
			original(p_matrix), factor(p_matrix) {}

	bool factorize() {
		for (size_t row = 0; row < N; ++row) {
			permutation[row] = row;
		}
		for (size_t column = 0; column < N; ++column) {
			size_t pivot = column;
			for (size_t row = column + 1; row < N; ++row) {
				if (std::abs(factor[row * N + column]) > std::abs(factor[pivot * N + column])) {
					pivot = row;
				}
			}
			if (!std::isfinite(factor[pivot * N + column]) || std::abs(factor[pivot * N + column]) < 1e-12) {
				return false;
			}
			if (pivot != column) {
				for (size_t entry = 0; entry < N; ++entry) {
					std::swap(factor[column * N + entry], factor[pivot * N + entry]);
				}
				std::swap(permutation[column], permutation[pivot]);
			}
			for (size_t row = column + 1; row < N; ++row) {
				factor[row * N + column] /= factor[column * N + column];
				for (size_t entry = column + 1; entry < N; ++entry) {
					factor[row * N + entry] -= factor[row * N + column] * factor[column * N + entry];
				}
			}
		}
		return true;
	}

	std::array<double, N> solve(const std::array<double, N> &p_rhs) const {
		std::array<double, N> result = {};
		for (size_t row = 0; row < N; ++row) {
			result[row] = p_rhs[permutation[row]];
			for (size_t column = 0; column < row; ++column) {
				result[row] -= factor[row * N + column] * result[column];
			}
		}
		for (size_t back = N; back > 0; --back) {
			const size_t row = back - 1;
			for (size_t column = row + 1; column < N; ++column) {
				result[row] -= factor[row * N + column] * result[column];
			}
			result[row] /= factor[row * N + row];
		}
		return result;
	}

	bool refine(const std::array<double, N> &p_rhs, std::array<double, N> &r_result, size_t p_iterations) const {
		auto result = solve(p_rhs);
		for (size_t iteration = 0; iteration < p_iterations; ++iteration) {
			auto residual = p_rhs;
			for (size_t row = 0; row < N; ++row) {
				for (size_t column = 0; column < N; ++column) {
					residual[row] -= original[row * N + column] * result[column];
				}
			}
			const auto correction = solve(residual);
			double squared = 0;
			for (size_t row = 0; row < N; ++row) {
				result[row] += correction[row];
				squared += correction[row] * correction[row];
			}
			if (!finite(result)) {
				return false;
			}
			if (p_iterations == 1 || squared < 1e-4) {
				r_result = result;
				return true;
			}
		}
		return false;
	}
};

QuadricError make_triangle(const std::array<QuadricPoint, 3> &p_positions, const std::array<std::vector<double>, 3> &p_attributes, const std::vector<double> &p_weights, AttributeQuadric &r_quadric) {
	AttributeQuadric candidate;
	candidate.position.origin = p_positions[0];
	candidate.attributes.resize(p_weights.size());
	for (size_t channel = 0; channel < p_weights.size(); ++channel) {
		candidate.attributes[channel].weight = p_weights[channel];
	}
	const QuadricPoint edge1 = subtract(p_positions[1], p_positions[0]);
	const QuadricPoint edge2 = subtract(p_positions[2], p_positions[0]);
	const QuadricPoint area_normal = cross(edge2, edge1);
	const double length = std::hypot(area_normal[0], area_normal[1], area_normal[2]);
	if (!std::isfinite(length)) {
		return QuadricError::NON_FINITE;
	}
	if (length * 0.5 < 1e-12) {
		r_quadric = std::move(candidate);
		return QuadricError::OK;
	}
	candidate.area = length * 0.5;
	candidate.volume_normal = area_normal;
	const QuadricPoint normal = scale(area_normal, 1 / length);
	for (size_t row = 0; row < 3; ++row) {
		for (size_t column = 0; column < 3; ++column) {
			candidate.position.matrix[row * 3 + column] = normal[row] * normal[column] * candidate.area;
		}
	}
	LinearSystem<3> gradient_solver({ edge1[0], edge1[1], edge1[2], edge2[0], edge2[1], edge2[2], normal[0], normal[1], normal[2] });
	const bool invertible = gradient_solver.factorize();
	for (size_t channel = 0; channel < p_weights.size(); ++channel) {
		if (p_weights[channel] == 0) {
			continue;
		}
		const double value = p_weights[channel] * p_attributes[0][channel];
		QuadricPoint gradient = {};
		if (invertible) {
			const QuadricPoint rhs = { p_weights[channel] * p_attributes[1][channel] - value, p_weights[channel] * p_attributes[2][channel] - value, 0 };
			if (!gradient_solver.refine(rhs, gradient, 1)) {
				return QuadricError::NON_FINITE;
			}
		}
		for (size_t row = 0; row < 3; ++row) {
			for (size_t column = 0; column < 3; ++column) {
				candidate.position.matrix[row * 3 + column] += gradient[row] * gradient[column] * candidate.area;
			}
		}
		candidate.position.linear = add(candidate.position.linear, scale(gradient, value * candidate.area));
		candidate.position.constant += value * value * candidate.area;
		candidate.attributes[channel].gradient = scale(gradient, candidate.area);
		candidate.attributes[channel].offset = value * candidate.area;
	}
	if (!valid(candidate)) {
		return QuadricError::NON_FINITE;
	}
	r_quadric = std::move(candidate);
	return QuadricError::OK;
}

QuadricError reduced_system(const QuadricOptimizer &p_optimizer, QuadricMatrix &r_matrix, QuadricPoint &r_rhs) {
	if (!valid(p_optimizer)) {
		return QuadricError::INVALID_INPUT;
	}
	if (!p_optimizer.has_origin || p_optimizer.area < 1e-12) {
		return QuadricError::SINGULAR;
	}
	for (size_t entry = 0; entry < 9; ++entry) {
		r_matrix[entry] = p_optimizer.position.matrix[entry] - p_optimizer.gradient_products[entry] / p_optimizer.area;
	}
	r_rhs = subtract(scale(p_optimizer.gradient_offsets, 1 / p_optimizer.area), p_optimizer.position.linear);
	return finite(r_matrix) && finite(r_rhs) ? QuadricError::OK : QuadricError::NON_FINITE;
}

} // namespace

QuadricError make_attribute_quadric(const std::array<QuadricPoint, 3> &p_positions, const std::array<std::vector<double>, 3> &p_attributes, const std::vector<double> &p_weights, AttributeQuadric &r_quadric) {
	for (size_t vertex = 0; vertex < 3; ++vertex) {
		if (!finite(p_positions[vertex]) || p_attributes[vertex].size() != p_weights.size()) {
			return QuadricError::INVALID_INPUT;
		}
		for (double value : p_attributes[vertex]) {
			if (!std::isfinite(value)) {
				return QuadricError::INVALID_INPUT;
			}
		}
	}
	for (double weight : p_weights) {
		if (!std::isfinite(weight) || weight < 0) {
			return QuadricError::INVALID_INPUT;
		}
	}
	try {
		return make_triangle(p_positions, p_attributes, p_weights, r_quadric);
	} catch (const std::bad_alloc &) {
		return QuadricError::MEMORY;
	}
}

QuadricError rebase_attribute_quadric(const AttributeQuadric &p_quadric, const QuadricPoint &p_origin, AttributeQuadric &r_quadric) {
	if (!valid(p_quadric) || !finite(p_origin)) {
		return QuadricError::INVALID_INPUT;
	}
	try {
		AttributeQuadric candidate = p_quadric;
		candidate.position = rebase(p_quadric.position, p_origin);
		const QuadricPoint delta = subtract(p_origin, p_quadric.position.origin);
		for (auto &attribute : candidate.attributes) {
			attribute.offset += dot(attribute.gradient, delta);
		}
		candidate.volume_offset += dot(candidate.volume_normal, delta);
		if (!valid(candidate)) {
			return QuadricError::NON_FINITE;
		}
		r_quadric = std::move(candidate);
		return QuadricError::OK;
	} catch (const std::bad_alloc &) {
		return QuadricError::MEMORY;
	}
}

QuadricError accumulate_attribute_quadric(const AttributeQuadric &p_quadric, AttributeQuadric &r_sum) {
	if (!valid(p_quadric) || !valid(r_sum)) {
		return QuadricError::INVALID_INPUT;
	}
	if (p_quadric.attributes.size() != r_sum.attributes.size()) {
		return QuadricError::LAYOUT_MISMATCH;
	}
	for (size_t channel = 0; channel < p_quadric.attributes.size(); ++channel) {
		if (p_quadric.attributes[channel].weight != r_sum.attributes[channel].weight) {
			return QuadricError::LAYOUT_MISMATCH;
		}
	}
	AttributeQuadric candidate;
	const QuadricError transformed = rebase_attribute_quadric(p_quadric, r_sum.position.origin, candidate);
	if (transformed != QuadricError::OK) {
		return transformed;
	}
	accumulate(r_sum.position, candidate.position);
	candidate.area += r_sum.area;
	candidate.volume_normal = add(candidate.volume_normal, r_sum.volume_normal);
	candidate.volume_offset += r_sum.volume_offset;
	for (size_t channel = 0; channel < candidate.attributes.size(); ++channel) {
		candidate.attributes[channel].gradient = add(candidate.attributes[channel].gradient, r_sum.attributes[channel].gradient);
		candidate.attributes[channel].offset += r_sum.attributes[channel].offset;
	}
	if (!valid(candidate)) {
		return QuadricError::NON_FINITE;
	}
	r_sum = std::move(candidate);
	return QuadricError::OK;
}

QuadricError evaluate_attribute_quadric(const AttributeQuadric &p_quadric, const QuadricPoint &p_position, const std::vector<double> &p_values, double &r_error) {
	if (!valid(p_quadric) || !finite(p_position)) {
		return QuadricError::INVALID_INPUT;
	}
	if (p_values.size() != p_quadric.attributes.size()) {
		return QuadricError::LAYOUT_MISMATCH;
	}
	const QuadricPoint point = subtract(p_position, p_quadric.position.origin);
	double error = evaluate(p_quadric.position, point);
	for (size_t channel = 0; channel < p_values.size(); ++channel) {
		if (!std::isfinite(p_values[channel])) {
			return QuadricError::INVALID_INPUT;
		}
		const auto &attribute = p_quadric.attributes[channel];
		const double value = attribute.weight * p_values[channel];
		error += value * (p_quadric.area * value - 2 * (dot(attribute.gradient, point) + attribute.offset));
	}
	if (!std::isfinite(error)) {
		return QuadricError::NON_FINITE;
	}
	r_error = std::max(0.0, error);
	return QuadricError::OK;
}

QuadricError fit_quadric_attributes(const AttributeQuadric &p_quadric, const QuadricPoint &p_position, std::vector<double> &r_values, double &r_error) {
	if (!valid(p_quadric) || !finite(p_position)) {
		return QuadricError::INVALID_INPUT;
	}
	if (r_values.size() != p_quadric.attributes.size()) {
		return QuadricError::LAYOUT_MISMATCH;
	}
	if (p_quadric.area <= 0) {
		return QuadricError::SINGULAR;
	}
	try {
		auto values = r_values;
		const QuadricPoint point = subtract(p_position, p_quadric.position.origin);
		double error = evaluate(p_quadric.position, point);
		for (size_t channel = 0; channel < values.size(); ++channel) {
			const auto &attribute = p_quadric.attributes[channel];
			if (!std::isfinite(values[channel])) {
				return QuadricError::INVALID_INPUT;
			}
			if (attribute.weight != 0) {
				const double projected = dot(attribute.gradient, point) + attribute.offset;
				const double weighted = projected / p_quadric.area;
				values[channel] = weighted / attribute.weight;
				error -= projected * weighted;
				if (!std::isfinite(values[channel])) {
					return QuadricError::NON_FINITE;
				}
			}
		}
		if (!std::isfinite(error)) {
			return QuadricError::NON_FINITE;
		}
		r_values = std::move(values);
		r_error = std::max(0.0, error);
		return QuadricError::OK;
	} catch (const std::bad_alloc &) {
		return QuadricError::MEMORY;
	}
}

QuadricError make_edge_quadric(const QuadricPoint &p_start, const QuadricPoint &p_end, double p_weight, PositionQuadric &r_quadric) {
	if (!finite(p_start) || !finite(p_end) || !std::isfinite(p_weight) || p_weight < 0) {
		return QuadricError::INVALID_INPUT;
	}
	PositionQuadric candidate;
	candidate.origin = p_start;
	const QuadricPoint edge = subtract(p_end, p_start);
	const double length = std::hypot(edge[0], edge[1], edge[2]);
	if (!std::isfinite(length)) {
		return QuadricError::NON_FINITE;
	}
	if (length >= 1e-8) {
		const QuadricPoint direction = scale(edge, 1 / length);
		const double weight = p_weight * length;
		for (size_t row = 0; row < 3; ++row) {
			for (size_t column = 0; column < 3; ++column) {
				candidate.matrix[row * 3 + column] = weight * ((row == column ? 1.0 : 0.0) - direction[row] * direction[column]);
			}
		}
	}
	if (!valid(candidate)) {
		return QuadricError::NON_FINITE;
	}
	r_quadric = candidate;
	return QuadricError::OK;
}

QuadricError evaluate_position_quadric(const PositionQuadric &p_quadric, const QuadricPoint &p_position, double &r_error) {
	if (!valid(p_quadric) || !finite(p_position)) {
		return QuadricError::INVALID_INPUT;
	}
	const double error = evaluate(p_quadric, subtract(p_position, p_quadric.origin));
	if (!std::isfinite(error)) {
		return QuadricError::NON_FINITE;
	}
	r_error = std::max(0.0, error);
	return QuadricError::OK;
}

QuadricError accumulate_optimizer_wedge(const AttributeQuadric &p_quadric, QuadricOptimizer &r_optimizer) {
	if (!valid(p_quadric) || !valid(r_optimizer)) {
		return QuadricError::INVALID_INPUT;
	}
	if (p_quadric.area < 1e-8) {
		return QuadricError::OK;
	}
	QuadricOptimizer candidate = r_optimizer;
	if (!candidate.has_origin) {
		candidate.position.origin = p_quadric.position.origin;
		candidate.has_origin = true;
	}
	const QuadricPoint delta = subtract(candidate.position.origin, p_quadric.position.origin);
	accumulate(rebase(p_quadric.position, candidate.position.origin), candidate.position);
	candidate.area += p_quadric.area;
	candidate.volume_normal = add(candidate.volume_normal, p_quadric.volume_normal);
	candidate.volume_offset += p_quadric.volume_offset + dot(p_quadric.volume_normal, delta);
	for (const auto &attribute : p_quadric.attributes) {
		const double offset = attribute.offset + dot(attribute.gradient, delta);
		for (size_t row = 0; row < 3; ++row) {
			for (size_t column = 0; column < 3; ++column) {
				candidate.gradient_products[row * 3 + column] += attribute.gradient[row] * attribute.gradient[column];
			}
		}
		candidate.gradient_offsets = add(candidate.gradient_offsets, scale(attribute.gradient, offset));
	}
	if (!valid(candidate)) {
		return QuadricError::NON_FINITE;
	}
	r_optimizer = candidate;
	return QuadricError::OK;
}

QuadricError accumulate_optimizer_edge(const PositionQuadric &p_quadric, QuadricOptimizer &r_optimizer) {
	if (!valid(p_quadric) || !valid(r_optimizer)) {
		return QuadricError::INVALID_INPUT;
	}
	QuadricOptimizer candidate = r_optimizer;
	if (!candidate.has_origin) {
		candidate.position.origin = p_quadric.origin;
		candidate.has_origin = true;
	}
	accumulate(rebase(p_quadric, candidate.position.origin), candidate.position);
	if (!valid(candidate)) {
		return QuadricError::NON_FINITE;
	}
	r_optimizer = candidate;
	return QuadricError::OK;
}

QuadricError solve_quadric_position(const QuadricOptimizer &p_optimizer, QuadricPoint &r_position) {
	QuadricMatrix matrix;
	QuadricPoint rhs;
	const QuadricError reduced = reduced_system(p_optimizer, matrix, rhs);
	if (reduced != QuadricError::OK) {
		return reduced;
	}
	LinearSystem<3> solver(matrix);
	QuadricPoint local;
	if (!solver.factorize() || !solver.refine(rhs, local, 4)) {
		return QuadricError::SINGULAR;
	}
	const QuadricPoint position = add(local, p_optimizer.position.origin);
	if (!finite(position)) {
		return QuadricError::NON_FINITE;
	}
	r_position = position;
	return QuadricError::OK;
}

QuadricError solve_quadric_volume(const QuadricOptimizer &p_optimizer, QuadricPoint &r_position) {
	QuadricMatrix matrix;
	QuadricPoint rhs;
	const QuadricError reduced = reduced_system(p_optimizer, matrix, rhs);
	if (reduced != QuadricError::OK) {
		return reduced;
	}
	const QuadricPoint &normal = p_optimizer.volume_normal;
	if (dot(normal, normal) <= 1e-12) {
		return QuadricError::SINGULAR;
	}
	LinearSystem<4> solver({ matrix[0], matrix[1], matrix[2], normal[0], matrix[3], matrix[4], matrix[5], normal[1], matrix[6], matrix[7], matrix[8], normal[2], normal[0], normal[1], normal[2], 0 });
	std::array<double, 4> solution;
	if (!solver.factorize() || !solver.refine({ rhs[0], rhs[1], rhs[2], -p_optimizer.volume_offset }, solution, 4)) {
		return QuadricError::SINGULAR;
	}
	const QuadricPoint position = add({ solution[0], solution[1], solution[2] }, p_optimizer.position.origin);
	if (!finite(position)) {
		return QuadricError::NON_FINITE;
	}
	r_position = position;
	return QuadricError::OK;
}

QuadricError solve_quadric_edge(const QuadricOptimizer &p_optimizer, const QuadricPoint &p_start, const QuadricPoint &p_end, QuadricPoint &r_position) {
	if (!finite(p_start) || !finite(p_end)) {
		return QuadricError::INVALID_INPUT;
	}
	QuadricMatrix matrix;
	QuadricPoint rhs;
	const QuadricError reduced = reduced_system(p_optimizer, matrix, rhs);
	if (reduced != QuadricError::OK) {
		return reduced;
	}
	const QuadricPoint start = subtract(p_start, p_optimizer.position.origin);
	const QuadricPoint delta = subtract(p_end, p_start);
	const QuadricPoint direction = multiply(matrix, delta);
	const QuadricPoint residual = subtract(rhs, multiply(matrix, start));
	const double squared = dot(direction, direction);
	if (!std::isfinite(squared)) {
		return QuadricError::NON_FINITE;
	}
	if (squared < 1e-16) {
		return QuadricError::SINGULAR;
	}
	double parameter = dot(direction, residual) / squared;
	const QuadricPoint &normal = p_optimizer.volume_normal;
	const double normal_squared = dot(normal, normal);
	if (normal_squared > 1e-12) {
		const double normal_step = dot(normal, delta);
		const double diagonal = squared + normal_step * normal_step;
		const double off_diagonal = dot(direction, normal);
		const double determinant = diagonal * normal_squared - off_diagonal * off_diagonal;
		if (std::abs(determinant) > 1e-16) {
			const double first = dot(direction, residual) - (p_optimizer.volume_offset + dot(normal, start)) * normal_step;
			parameter = (normal_squared * first - off_diagonal * dot(normal, residual)) / determinant;
		}
	}
	if (!std::isfinite(parameter)) {
		return QuadricError::NON_FINITE;
	}
	const QuadricPoint position = add(p_start, scale(delta, std::clamp(parameter, 0.0, 1.0)));
	if (!finite(position)) {
		return QuadricError::NON_FINITE;
	}
	r_position = position;
	return QuadricError::OK;
}

} // namespace VirtualGeometry
