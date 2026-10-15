#pragma once

#include <cmath>
#include <limits>
#include <vector>

#include "core/math/face3.h"
#include "scene/resources/3d/height_map_shape_3d.h"
#include "scene/resources/mesh.h"

namespace cooker_heightfield {
// A square, centered regular grid can retain every source triangle exactly.
// Rotating the sampled grid by a quarter turn selects the other diagonal.
inline Error Build(const Ref<Mesh>& mesh, int width, double cell_size,
                   double maximum_error, Ref<HeightMapShape3D>& output) {
  ERR_FAIL_COND_V(width < 4 || width > 1025 || !std::isfinite(cell_size) ||
                      cell_size <= 0 || !std::isfinite(maximum_error) ||
                      maximum_error <= 0 || maximum_error > 0.2,
                  ERR_INVALID_PARAMETER);
  const Vector<Face3> faces = mesh->get_faces();
  const int divisions = width - 1;
  ERR_FAIL_COND_V(faces.size() != divisions * divisions * 2, ERR_INVALID_DATA);
  const double half = divisions * cell_size * 0.5;
  std::vector<real_t> heights(width * width,
                              std::numeric_limits<real_t>::quiet_NaN());
  std::vector<unsigned int> cells(divisions * divisions, 0);
  int diagonal = -1;
  double achieved_error = 0;
  for (const Face3& face : faces) {
    int xs[3], zs[3];
    int cell_x = divisions, cell_z = divisions;
    for (int corner = 0; corner < 3; ++corner) {
      const Vector3& point = face.vertex[corner];
      ERR_FAIL_COND_V(!point.is_finite(), ERR_INVALID_DATA);
      const int x = int(std::round((point.x + half) / cell_size));
      const int z = int(std::round((point.z + half) / cell_size));
      ERR_FAIL_COND_V(x < 0 || x >= width || z < 0 || z >= width ||
                          point.x != real_t(x * cell_size - half) ||
                          point.z != real_t(z * cell_size - half),
                      ERR_INVALID_DATA);
      real_t& height = heights[z * width + x];
      ERR_FAIL_COND_V(std::isfinite(height) && height != point.y,
                      ERR_INVALID_DATA);
      height = point.y;
      xs[corner] = x;
      zs[corner] = z;
      cell_x = MIN(cell_x, x);
      cell_z = MIN(cell_z, z);
      achieved_error =
          MAX(achieved_error, std::abs(point.x - (x * cell_size - half)));
      achieved_error =
          MAX(achieved_error, std::abs(point.z - (z * cell_size - half)));
    }
    ERR_FAIL_COND_V(cell_x >= divisions || cell_z >= divisions,
                    ERR_INVALID_DATA);
    unsigned int mask = 0;
    for (int corner = 0; corner < 3; ++corner) {
      const int x = xs[corner] - cell_x, z = zs[corner] - cell_z;
      ERR_FAIL_COND_V(x > 1 || z > 1, ERR_INVALID_DATA);
      mask |= 1u << (z * 2 + x);
    }
    // Corner numbers: 0--1, 2--3. Opposite corner pairs choose the diagonal.
    const int current = mask == 11 || mask == 13  ? 1
                        : mask == 7 || mask == 14 ? 0
                                                  : -1;
    ERR_FAIL_COND_V(current < 0 || (diagonal >= 0 && diagonal != current),
                    ERR_INVALID_DATA);
    diagonal = current;
    const unsigned int bit = 1u << mask;
    unsigned int& seen = cells[cell_z * divisions + cell_x];
    ERR_FAIL_COND_V(seen & bit, ERR_INVALID_DATA);
    seen |= bit;
  }
  const unsigned int expected =
      diagonal == 1 ? (1u << 11) | (1u << 13) : (1u << 7) | (1u << 14);
  for (unsigned int cell : cells) {
    ERR_FAIL_COND_V(cell != expected, ERR_INVALID_DATA);
  }
  Vector<real_t> data;
  data.resize(width * width);
  for (int z = 0; z < width; ++z) {
    for (int x = 0; x < width; ++x) {
      const real_t height = diagonal == 1 ? heights[(width - 1 - x) * width + z]
                                          : heights[z * width + x];
      ERR_FAIL_COND_V(!std::isfinite(height), ERR_INVALID_DATA);
      data.write[z * width + x] = height;
    }
  }
  ERR_FAIL_COND_V(achieved_error > maximum_error, ERR_INVALID_DATA);
  output.instantiate();
  output->set_map_width(width);
  output->set_map_depth(width);
  output->set_map_data(data);
  Dictionary support;
  support["version"] = 1;
  support["cell_size"] = cell_size;
  support["quarter_turn"] = diagonal == 1;
  support["maximum_error_metres"] = maximum_error;
  support["achieved_error_metres"] = achieved_error;
  support["source_triangles"] = faces.size();
  support["locked_borders"] = true;
  output->set_meta("veya_collision_support", support);
  return OK;
}
}  // namespace cooker_heightfield
