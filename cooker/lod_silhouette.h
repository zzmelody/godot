#pragma once

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "core/variant/variant.h"

namespace CookerLod {
// Offline, double-sided orthographic coverage. Each candidate uses exactly the
// source projection, pixel centres and bounds, including views above the crown.
class Silhouette {
 public:
  Silhouette(const PackedVector3Array& vertices,
             const PackedInt32Array& indices, double pixel_scale) {
    for (int elevation = 0; elevation < 2; ++elevation) {
      for (int direction = 0; direction < 8; ++direction) {
        View view;
        const double angle = direction * Math::PI / 4;
        const double tilt = elevation * Math::PI / 6;
        Vector2 minimum(INFINITY, INFINITY), maximum(-INFINITY, -INFINITY);
        for (const Vector3& vertex : vertices) {
          const double depth =
              -vertex.x * std::sin(angle) + vertex.z * std::cos(angle);
          const Vector2 point(
              vertex.x * std::cos(angle) + vertex.z * std::sin(angle),
              vertex.y * std::cos(tilt) - depth * std::sin(tilt));
          minimum = minimum.min(point);
          maximum = maximum.max(point);
          view.points.push_back(point);
        }
        const Vector2 extent = maximum - minimum;
        const double scale = MIN(
            pixel_scale, (kSize - 4) / MAX(MAX(extent.x, extent.y), 0.00001));
        view.width = static_cast<int>(std::ceil(extent.x * scale)) + 4;
        view.height = static_cast<int>(std::ceil(extent.y * scale)) + 4;
        for (Vector2& point : view.points) {
          point = (point - minimum) * scale + Vector2(2, 2);
        }
        view.source = Raster(view.points, indices, view.width, view.height);
        views_.push_back(std::move(view));
      }
    }
  }

  bool Accepts(const PackedInt32Array& indices, double minimum_coverage) const {
    for (const View& view : views_) {
      const auto candidate =
          Raster(view.points, indices, view.width, view.height);
      int source_pixels = 0, changed_pixels = 0;
      for (size_t pixel = 0; pixel < view.source.size(); ++pixel) {
        source_pixels += view.source[pixel];
        changed_pixels += view.source[pixel] != candidate[pixel];
      }
      if (source_pixels &&
          changed_pixels > source_pixels * (1 - minimum_coverage)) {
        return false;
      }
    }
    return true;
  }

 private:
  static constexpr int kSize = 256;
  struct View {
    int width = 0, height = 0;
    std::vector<Vector2> points;
    std::vector<unsigned char> source;
  };

  static double Edge(const Vector2& first, const Vector2& second,
                     const Vector2& point) {
    return (second.x - first.x) * (point.y - first.y) -
           (second.y - first.y) * (point.x - first.x);
  }

  static std::vector<unsigned char> Raster(const std::vector<Vector2>& points,
                                           const PackedInt32Array& indices,
                                           int width, int height) {
    std::vector<unsigned char> mask(width * height, 0);
    for (int index = 0; index < indices.size(); index += 3) {
      const Vector2 a = points[indices[index]];
      const Vector2 b = points[indices[index + 1]];
      const Vector2 c = points[indices[index + 2]];
      const double orientation = Edge(a, b, c);
      if (std::abs(orientation) < 0.000001) {
        continue;
      }
      const Vector2 minimum = a.min(b).min(c), maximum = a.max(b).max(c);
      const int left = MAX(0, static_cast<int>(std::floor(minimum.x)));
      const int right = MIN(width - 1, static_cast<int>(std::ceil(maximum.x)));
      const int bottom = MAX(0, static_cast<int>(std::floor(minimum.y)));
      const int top = MIN(height - 1, static_cast<int>(std::ceil(maximum.y)));
      for (int y = bottom; y <= top; ++y) {
        for (int x = left; x <= right; ++x) {
          const Vector2 point(x + 0.5, y + 0.5);
          const double ab = Edge(a, b, point), bc = Edge(b, c, point);
          const double ca = Edge(c, a, point);
          if ((orientation > 0 && ab >= 0 && bc >= 0 && ca >= 0) ||
              (orientation < 0 && ab <= 0 && bc <= 0 && ca <= 0)) {
            mask[y * width + x] = 1;
          }
        }
      }
    }
    return mask;
  }

  std::vector<View> views_;
};
}  // namespace CookerLod
