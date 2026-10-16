#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

#include "core/variant/variant.h"

namespace CookerLod {
// Preserve a tall connected component that starts at the source's lowest Y.
// Position welding includes UV/normal seams without joining nearby branches.
class RootedSpan {
 public:
  RootedSpan(const PackedVector3Array& vertices,
             const PackedInt32Array& indices) {
    std::map<std::array<real_t, 3>, int> positions;
    double minimum = INFINITY, maximum = -INFINITY;
    for (const Vector3& vertex : vertices) {
      const std::array<real_t, 3> key = {vertex.x, vertex.y, vertex.z};
      auto found = positions.find(key);
      if (found == positions.end()) {
        found = positions.emplace(key, heights_.size()).first;
        heights_.push_back(vertex.y);
      }
      welded_.push_back(found->second);
      minimum = MIN(minimum, vertex.y);
      maximum = MAX(maximum, vertex.y);
    }
    tolerance_ = (maximum - minimum) * 0.01;
    for (const auto& span : Spans(indices)) {
      if (span.first <= minimum + tolerance_ &&
          span.second - span.first >= (maximum - minimum) * 0.5 &&
          span.second - span.first > required_.second - required_.first) {
        required_ = span;
      }
    }
  }

  bool Accepts(const PackedInt32Array& indices) const {
    if (required_.second <= required_.first) {
      return true;
    }
    for (const auto& span : Spans(indices)) {
      if (span.first <= required_.first + tolerance_ &&
          span.second >= required_.second - tolerance_ * 3) {
        return true;
      }
    }
    return false;
  }

 private:
  static int Find(std::vector<int>& parent, int index) {
    while (parent[index] != index) {
      parent[index] = parent[parent[index]];
      index = parent[index];
    }
    return index;
  }

  std::vector<std::pair<double, double>> Spans(
      const PackedInt32Array& indices) const {
    std::vector<int> parent(heights_.size());
    for (size_t i = 0; i < parent.size(); ++i) {
      parent[i] = i;
    }
    for (int i = 0; i < indices.size(); i += 3) {
      const int root = Find(parent, welded_[indices[i]]);
      parent[Find(parent, welded_[indices[i + 1]])] = root;
      parent[Find(parent, welded_[indices[i + 2]])] = root;
    }
    std::map<int, std::pair<double, double>> spans;
    for (const int index : indices) {
      const int vertex = welded_[index];
      const int root = Find(parent, vertex);
      auto found = spans.find(root);
      if (found == spans.end()) {
        spans.emplace(root, std::make_pair(heights_[vertex], heights_[vertex]));
      } else {
        found->second.first = MIN(found->second.first, heights_[vertex]);
        found->second.second = MAX(found->second.second, heights_[vertex]);
      }
    }
    std::vector<std::pair<double, double>> result;
    for (const auto& span : spans) {
      result.push_back(span.second);
    }
    return result;
  }

  std::vector<int> welded_;
  std::vector<double> heights_;
  std::pair<double, double> required_ = {0, 0};
  double tolerance_ = 0;
};

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
        view.source =
            Outline(Raster(view.points, indices, view.width, view.height),
                    view.width, view.height);
        view.expanded = Expand(view.source, view.width, view.height);
        views_.push_back(std::move(view));
      }
    }
  }

  bool Accepts(const PackedInt32Array& indices, double minimum_coverage) const {
    for (const View& view : views_) {
      const auto candidate =
          Outline(Raster(view.points, indices, view.width, view.height),
                  view.width, view.height);
      const auto expanded = Expand(candidate, view.width, view.height);
      int source_pixels = 0, changed_pixels = 0;
      for (size_t pixel = 0; pixel < view.source.size(); ++pixel) {
        source_pixels += view.source[pixel];
        changed_pixels += (view.source[pixel] && !expanded[pixel]) ||
                          (candidate[pixel] && !view.expanded[pixel]);
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
    std::vector<unsigned char> expanded;
  };

  // Samples are twice the expected screen density. Two samples permit one
  // screen pixel of contour movement when thinning individual leaves.
  static std::vector<unsigned char> Expand(
      const std::vector<unsigned char>& mask, int width, int height) {
    std::vector<unsigned char> horizontal(mask.size(), 0),
        result(mask.size(), 0);
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        for (int offset = -2; offset <= 2; ++offset) {
          if (x + offset >= 0 && x + offset < width) {
            horizontal[y * width + x] |= mask[y * width + x + offset];
          }
        }
      }
    }
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        for (int offset = -2; offset <= 2; ++offset) {
          if (y + offset >= 0 && y + offset < height) {
            result[y * width + x] |= horizontal[(y + offset) * width + x];
          }
        }
      }
    }
    return result;
  }

  // Compare each scanline's outer envelope. Interior leaf holes may change
  // when reducing disconnected leaves; their individual pixels are not shape.
  static std::vector<unsigned char> Outline(std::vector<unsigned char> mask,
                                            int width, int height) {
    for (int y = 0; y < height; ++y) {
      int left = width, right = -1;
      for (int x = 0; x < width; ++x) {
        if (mask[y * width + x]) {
          left = MIN(left, x);
          right = MAX(right, x);
        }
      }
      for (int x = left; x <= right; ++x) {
        mask[y * width + x] = 1;
      }
    }
    return mask;
  }

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
