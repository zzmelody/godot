#include "metadata_bounds.h"

#include <cmath>
#include <limits>

namespace VirtualGeometry {

bool round_metadata_error(double p_value, bool p_upper, float &r_value) {
	if (!std::isfinite(p_value) || std::abs(p_value) > std::numeric_limits<float>::max()) {
		return false;
	}
	float value = static_cast<float>(p_value);
	if (p_upper ? value < p_value : value > p_value) {
		value = std::nextafter(value, p_upper ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity());
	}
	if (!std::isfinite(value)) {
		return false;
	}
	r_value = value;
	return true;
}

bool round_metadata_sphere(const GeometrySphere &p_sphere, std::array<float, 4> &r_sphere) {
	std::array<float, 4> candidate;
	for (uint32_t axis = 0; axis < 3; ++axis) {
		if (!std::isfinite(p_sphere.center[axis]) || std::abs(p_sphere.center[axis]) > std::numeric_limits<float>::max()) {
			return false;
		}
		candidate[axis] = static_cast<float>(p_sphere.center[axis]);
	}
	const double offset = std::hypot(p_sphere.center[0] - candidate[0], p_sphere.center[1] - candidate[1], p_sphere.center[2] - candidate[2]);
	const double radius = offset > 0 ? std::nextafter(p_sphere.radius + offset, std::numeric_limits<double>::infinity()) : p_sphere.radius;
	if (!std::isfinite(p_sphere.radius) || p_sphere.radius < 0 || !round_metadata_error(radius, true, candidate[3])) {
		return false;
	}
	r_sphere = candidate;
	return true;
}

} // namespace VirtualGeometry
