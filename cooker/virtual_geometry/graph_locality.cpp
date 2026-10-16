#include "graph_locality.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <numeric>

namespace VirtualGeometry {
namespace {

uint32_t morton_bits(uint32_t p_value) {
	uint32_t result = 0;
	for (uint32_t bit = 0; bit < 10; ++bit) {
		result |= ((p_value >> bit) & 1u) << (bit * 3);
	}
	return result;
}

bool build(const std::vector<std::array<double, 3>> &p_centers, const std::vector<uint32_t> &p_islands,
		const std::vector<uint32_t> &p_labels, const std::array<double, 3> &p_lower,
		const std::array<double, 3> &p_upper, GraphLocality &r_locality) {
	const uint32_t count = static_cast<uint32_t>(p_centers.size());
	const double extent = std::max({ p_upper[0] - p_lower[0], p_upper[1] - p_lower[1], p_upper[2] - p_lower[2] });
	std::vector<uint32_t> keys(count, 0);
	for (uint32_t node = 0; node < count; ++node) {
		for (uint32_t axis = 0; axis < 3; ++axis) {
			const double scaled = extent > 0 ? (p_centers[node][axis] - p_lower[axis]) / extent * 1023.0 : 0;
			keys[node] |= morton_bits(static_cast<uint32_t>(std::clamp(scaled, 0.0, 1023.0))) << axis;
		}
	}
	r_locality.order.resize(count);
	std::iota(r_locality.order.begin(), r_locality.order.end(), 0u);
	std::sort(r_locality.order.begin(), r_locality.order.end(), [&](uint32_t p_left, uint32_t p_right) {
		return keys[p_left] != keys[p_right] ? keys[p_left] < keys[p_right] : p_left < p_right;
	});
	const auto &order = r_locality.order;
	std::vector<uint32_t> run_begin(count);
	std::vector<uint32_t> run_end(count);
	for (uint32_t begin = 0; begin < count;) {
		uint32_t end = begin + 1;
		while (end < count && p_islands[order[end]] == p_islands[order[begin]]) {
			++end;
		}
		for (uint32_t index = begin; index < end; ++index) {
			run_begin[index] = begin;
			run_end[index] = end;
		}
		begin = end;
	}
	for (uint32_t index = 0; index < count; ++index) {
		if (run_end[index] - run_begin[index] >= 128) {
			continue;
		}
		const uint32_t node = order[index];
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
				const uint32_t other = order[static_cast<size_t>(cursor)];
				if (p_islands[node] == p_islands[other] || (!p_labels.empty() && p_labels[node] != p_labels[other])) {
					cursor = direction < 0 ? run_begin[static_cast<size_t>(cursor)] : run_end[static_cast<size_t>(cursor)] - 1;
					continue;
				}
				double distance = 0;
				for (size_t axis = 0; axis < 3; ++axis) {
					const double delta = p_centers[node][axis] - p_centers[other][axis];
					distance += delta * delta;
				}
				if (!std::isfinite(distance)) {
					return false;
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
				r_locality.links.emplace_back(std::min(node, candidate.second), std::max(node, candidate.second));
			}
		}
	}
	std::sort(r_locality.links.begin(), r_locality.links.end());
	r_locality.links.erase(std::unique(r_locality.links.begin(), r_locality.links.end()), r_locality.links.end());
	return true;
}

} // namespace

LocalityError build_graph_locality(const std::vector<std::array<double, 3>> &p_centers,
		const std::vector<uint32_t> &p_islands, const std::vector<uint32_t> &p_labels,
		const std::array<double, 3> &p_lower, const std::array<double, 3> &p_upper, GraphLocality &r_locality) {
	if (p_centers.size() > INT32_MAX || p_centers.size() != p_islands.size() || (!p_labels.empty() && p_labels.size() != p_centers.size())) {
		return LocalityError::INVALID_INPUT;
	}
	for (size_t axis = 0; axis < 3; ++axis) {
		if (!std::isfinite(p_lower[axis]) || !std::isfinite(p_upper[axis]) || p_upper[axis] < p_lower[axis] || !std::isfinite(p_upper[axis] - p_lower[axis])) {
			return LocalityError::INVALID_INPUT;
		}
		for (const auto &center : p_centers) {
			if (!std::isfinite(center[axis]) || center[axis] < p_lower[axis] || center[axis] > p_upper[axis]) {
				return LocalityError::INVALID_INPUT;
			}
		}
	}
	try {
		GraphLocality candidate;
		if (!build(p_centers, p_islands, p_labels, p_lower, p_upper, candidate)) {
			return LocalityError::INVALID_INPUT;
		}
		r_locality = std::move(candidate);
		return LocalityError::OK;
	} catch (const std::bad_alloc &) {
		return LocalityError::MEMORY;
	}
}

} // namespace VirtualGeometry
