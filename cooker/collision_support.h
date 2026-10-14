#pragma once

#include "../thirdparty/meshoptimizer/meshoptimizer.h"

#include "core/math/vector3.h"
#include "core/templates/vector.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace cooker_collision {

// Simplify positions only. Locked borders keep adjacent terrain chunks joined.
// The error is measured in the mesh's local metres; hosts retain its transform.
inline float Simplify(Vector<Vector3> &faces, double ratio, double max_error) {
	const size_t count = faces.size();
	std::vector<float> positions(count * 3);
	for (size_t i = 0; i < count; ++i) {
		positions[i * 3] = faces[i].x;
		positions[i * 3 + 1] = faces[i].y;
		positions[i * 3 + 2] = faces[i].z;
	}
	std::vector<unsigned int> remap(count);
	const size_t vertices = meshopt_generateVertexRemap(
			remap.data(), nullptr, count, positions.data(), count, 3 * sizeof(float));
	std::vector<float> welded(vertices * 3);
	meshopt_remapVertexBuffer(welded.data(), positions.data(), count,
			3 * sizeof(float), remap.data());
	const float scale =
			meshopt_simplifyScale(welded.data(), vertices, 3 * sizeof(float));
	if (scale <= 0) {
		return 0;
	}
	std::vector<unsigned int> indices(count);
	meshopt_remapIndexBuffer(indices.data(), nullptr, count, remap.data());
	std::vector<unsigned int> reduced(count);
	float error = 0;
	const size_t target = std::max<size_t>(3, size_t(count * ratio) / 3 * 3);
	const size_t size = meshopt_simplify(
			reduced.data(), indices.data(), count, welded.data(), vertices,
			3 * sizeof(float), target, max_error / scale, meshopt_SimplifyLockBorder,
			&error);
	if (size < 3 || size >= count || error * scale > max_error) {
		return 0;
	}
	// Keep the source positions rather than introducing quantized coordinates.
	std::vector<Vector3> source(vertices);
	for (size_t i = 0; i < count; ++i) {
		source[remap[i]] = faces[i];
	}
	faces.resize(size);
	for (size_t i = 0; i < size; ++i) {
		faces.write[i] = source[reduced[i]];
	}
	return error * scale;
}

} // namespace cooker_collision
