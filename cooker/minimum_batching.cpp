#include "minimum_batching.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <tuple>
#include <vector>

#include "scene/3d/multimesh_instance_3d.h"
#include "scene/resources/3d/virtual_geometry_mesh.h"
#include "scene/resources/material.h"
#include "scene/resources/multimesh.h"
#include "scene/resources/shader.h"
#include "servers/rendering/rendering_server.h"

namespace CookerMinimum {
namespace {

struct Source {
  MultiMeshInstance3D* node = nullptr;
  Ref<MultiMesh> mesh;
  Transform3D transform;
  Array resources;
  bool has_resources = false;
};

struct Group {
  std::vector<Source> sources;
  uint64_t instances = 0;
  real_t cull_margin = 0;
};

using GroupKey =
    std::tuple<uint64_t, uint64_t, real_t, bool, bool, bool>;

class Coalescer {
 public:
  Error Run(Node3D& root) {
    std::vector<Group> groups;
    std::map<GroupKey, size_t> indexes;
    uint64_t total_batches = 0;
    for (int child = 0; child < root.get_child_count(); ++child) {
      auto* node = Object::cast_to<MultiMeshInstance3D>(root.get_child(child));
      total_batches += node != nullptr;
      Source source;
      real_t cull_margin = 0;
      if (!node || !ReadSource(*node, source, cull_margin)) {
        continue;
      }
      const Ref<Mesh> mesh = source.mesh->get_mesh();
      const Ref<Material> material = node->get_material_override();
      const GroupKey key{
          static_cast<uint64_t>(mesh->get_instance_id()),
          material.is_valid()
              ? static_cast<uint64_t>(material->get_instance_id())
              : 0,
          cull_margin, source.mesh->is_using_colors(),
          source.mesh->is_using_custom_data(), source.mesh->is_local_to_scene()};
      auto found = indexes.find(key);
      if (found == indexes.end()) {
        found = indexes.emplace(key, groups.size()).first;
        groups.push_back(Group{});
        groups.back().cull_margin = cull_margin;
      }
      Group& group = groups[found->second];
      group.instances += source.mesh->get_instance_count();
      group.sources.push_back(std::move(source));
    }
    uint64_t merged_sources = 0;
    uint64_t merged_groups = 0;
    for (Group& group : groups) {
      if (group.sources.size() < 2) {
        continue;
      }
      const Ref<MultiMesh> first = group.sources.front().mesh;
      const uint64_t stride =
          12 + (first->is_using_colors() ? 4 : 0) +
          (first->is_using_custom_data() ? 4 : 0);
      // A group that cannot fit Godot's actual array representation keeps its
      // original batches. It does not refuse or truncate the input scene.
      if (group.instances >
          static_cast<uint64_t>(std::numeric_limits<int>::max()) / stride) {
        continue;
      }
      const Error error = Merge(root, group, static_cast<int>(stride));
      if (error != OK) {
        return error;
      }
      merged_sources += group.sources.size();
      ++merged_groups;
    }
    if (merged_groups != 0) {
      root.set_meta("veya_minimum_batches_before",
                    static_cast<int64_t>(total_batches));
      root.set_meta("veya_minimum_batches_after",
                    static_cast<int64_t>(total_batches - merged_sources +
                                         merged_groups));
    }
    return OK;
  }

 private:
  bool CanReindexMaterial(const Ref<Material>& material) {
    if (material.is_null()) {
      return true;
    }
    const auto id = material->get_instance_id();
    const auto found = materials_.find(id);
    if (found != materials_.end()) {
      return found->second;
    }
    bool eligible = false;
    if (material->get_next_pass().is_null()) {
      const Ref<BaseMaterial3D> standard = material;
      if (standard.is_valid()) {
        eligible = standard->get_billboard_mode() ==
                   BaseMaterial3D::BILLBOARD_DISABLED;
      } else if (const Ref<ShaderMaterial> custom = material;
                 custom.is_valid() && custom->get_shader().is_valid()) {
        // Baking preserves VGM's per-instance MODEL_MATRIX and custom values.
        // Index-, node- or preprocessor-dependent programs need their original
        // instance numbering and source boundary.
        const String code = custom->get_shader()->get_code();
        const String identifiers = code.replace("INSTANCE_CUSTOM", "");
        eligible = !code.is_empty() && !code.contains("#") &&
                   !identifiers.contains("INSTANCE_") &&
                   !identifiers.contains("NODE_");
      }
    }
    materials_.emplace(id, eligible);
    return eligible;
  }

  bool CanReindexMesh(const Ref<Mesh>& mesh) {
    const auto id = mesh->get_instance_id();
    const auto found = meshes_.find(id);
    if (found != meshes_.end()) {
      return found->second;
    }
    bool eligible = true;
    for (int slot = 0; slot < mesh->get_surface_count(); ++slot) {
      eligible &= CanReindexMaterial(mesh->surface_get_material(slot));
    }
    meshes_.emplace(id, eligible);
    return eligible;
  }

  bool ReadSource(MultiMeshInstance3D& node, Source& source,
                  real_t& cull_margin) {
    source.node = &node;
    source.mesh = node.get_multimesh();
    if (source.mesh.is_null() || source.mesh->get_instance_count() == 0 ||
        source.mesh->get_transform_format() != MultiMesh::TRANSFORM_3D ||
        source.mesh->get_custom_aabb() != AABB()) {
      return false;
    }
    const int visible = source.mesh->get_visible_instance_count();
    if (visible >= 0 && visible != source.mesh->get_instance_count()) {
      return false;
    }
    List<StringName> metadata;
    source.mesh->get_meta_list(&metadata);
    if (!metadata.is_empty()) {
      return false;
    }
    const Ref<VirtualGeometryMesh> geometry = source.mesh->get_mesh();
    if (geometry.is_null()) {
      return false;
    }
    const Ref<Material> override = node.get_material_override();
    if (!(override.is_valid() ? CanReindexMaterial(override)
                             : CanReindexMesh(geometry))) {
      return false;
    }
    source.transform = node.get_transform();
    if (!source.transform.is_finite() ||
        source.transform.basis.determinant() == 0) {
      return false;
    }
    double maximum_row = 0;
    for (int row = 0; row < 3; ++row) {
      const Vector3 values = source.transform.basis.rows[row];
      maximum_row = std::max(
          maximum_row, std::abs(double(values.x)) +
                           std::abs(double(values.y)) +
                           std::abs(double(values.z)));
    }
    const double expanded = double(node.get_extra_cull_margin()) * maximum_row;
    if (!std::isfinite(expanded) || expanded < 0 ||
        expanded > std::numeric_limits<real_t>::max()) {
      return false;
    }
    cull_margin = static_cast<real_t>(expanded);
    if (double(cull_margin) < expanded) {
      cull_margin = std::nextafter(
          cull_margin, std::numeric_limits<real_t>::infinity());
    }
    source.has_resources = node.has_meta("veya_resources");
    if (source.has_resources) {
      const Variant value = node.get_meta("veya_resources");
      if (value.get_type() != Variant::ARRAY) {
        return false;
      }
      source.resources = value;
      for (const Variant& value : source.resources) {
        if (value.get_type() != Variant::DICTIONARY) {
          return false;
        }
        const Dictionary entry = value;
        const Variant index = entry.get("index", Variant());
        if (index.get_type() != Variant::INT || int64_t(index) < 0 ||
            int64_t(index) >= source.mesh->get_instance_count() ||
            entry.get("pose", Variant()).get_type() != Variant::TRANSFORM3D ||
            entry.get("member_pose", Variant()).get_type() !=
                Variant::TRANSFORM3D) {
          return false;
        }
      }
    }
    return true;
  }

  Error Merge(Node3D& root, Group& group, int stride) {
    PackedFloat32Array output;
    const int instances = static_cast<int>(group.instances);
    const Error allocation = output.resize(instances * stride);
    if (allocation != OK) {
      return allocation;
    }
    float* destination = output.ptrw();
    Array resources;
    bool has_resources = false;
    int first = 0;
    for (const Source& source : group.sources) {
      const PackedFloat32Array input =
          RenderingServer::get_singleton()->multimesh_get_buffer(
              source.mesh->get_rid());
      const int count = source.mesh->get_instance_count();
      if (input.size() != int64_t(count) * stride) {
        return ERR_INVALID_DATA;
      }
      const float* data = input.ptr();
      for (int instance = 0; instance < count; ++instance) {
        const float* values = data + int64_t(instance) * stride;
        Transform3D local;
        for (int row = 0; row < 3; ++row) {
          for (int column = 0; column < 3; ++column) {
            local.basis.rows[row][column] = values[row * 4 + column];
          }
          local.origin[row] = values[row * 4 + 3];
        }
        const Transform3D baked = source.transform * local;
        if (!baked.is_finite()) {
          return ERR_INVALID_DATA;
        }
        float* target = destination + int64_t(first + instance) * stride;
        for (int row = 0; row < 3; ++row) {
          for (int column = 0; column < 3; ++column) {
            target[row * 4 + column] = baked.basis.rows[row][column];
          }
          target[row * 4 + 3] = baked.origin[row];
        }
        std::memcpy(target + 12, values + 12,
                    static_cast<size_t>(stride - 12) * sizeof(float));
      }
      for (const Variant& value : source.resources) {
        Dictionary entry = Dictionary(value).duplicate(false);
        entry["index"] = first + int64_t(entry["index"]);
        entry["pose"] = source.transform * Transform3D(entry["pose"]);
        entry["member_pose"] =
            source.transform * Transform3D(entry["member_pose"]);
        resources.push_back(entry);
      }
      has_resources |= source.has_resources;
      first += count;
    }
    const Source& original = group.sources.front();
    Ref<MultiMesh> mesh;
    mesh.instantiate();
    mesh->set_transform_format(MultiMesh::TRANSFORM_3D);
    mesh->set_use_colors(original.mesh->is_using_colors());
    mesh->set_use_custom_data(original.mesh->is_using_custom_data());
    mesh->set_local_to_scene(original.mesh->is_local_to_scene());
    mesh->set_instance_count(instances);
    mesh->set_mesh(original.mesh->get_mesh());
    RenderingServer::get_singleton()->multimesh_set_buffer(mesh->get_rid(),
                                                         output);
    auto* replacement = memnew(MultiMeshInstance3D);
    replacement->set_multimesh(mesh);
    replacement->set_material_override(original.node->get_material_override());
    replacement->set_extra_cull_margin(group.cull_margin);
    replacement->set_name("MinimumBatch_" + String(original.node->get_name()));
    if (has_resources) {
      replacement->set_meta("veya_resources", resources);
    }
    const int index = original.node->get_index();
    root.add_child(replacement);
    replacement->set_owner(&root);
    root.move_child(replacement, index);
    for (Source& source : group.sources) {
      root.remove_child(source.node);
      memdelete(source.node);
      source.node = nullptr;
    }
    return OK;
  }

  std::map<ObjectID, bool> materials_;
  std::map<ObjectID, bool> meshes_;
};

}  // namespace

Error CoalesceMinimumBatches(Node3D& root) {
  try {
    Coalescer coalescer;
    return coalescer.Run(root);
  } catch (const std::bad_alloc&) {
    return ERR_OUT_OF_MEMORY;
  }
}

}  // namespace CookerMinimum
