#pragma once

#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/io/resource_loader.h"
#include "core/object/script_language.h"
#include "core/templates/hash_set.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/resources/packed_scene.h"
#include "scene/resources/texture.h"

namespace CookerStreaming {
inline Array VectorValue(const Vector3& value) {
  Array result;
  result.push_back(value.x);
  result.push_back(value.y);
  result.push_back(value.z);
  return result;
}

struct Measure {
  AABB bounds;
  bool geometry = false;
  uint64_t nodes = 0, geometry_bytes = 0, texture_bytes = 0;
  HashSet<ObjectID> resources;
};

inline Error MeasureValue(const Variant& value, Measure& result, int depth) {
  ERR_FAIL_COND_V(depth > 64, ERR_INVALID_DATA);
  if (value.get_type() == Variant::OBJECT) {
    Ref<Resource> resource = value;
    if (resource.is_null() || result.resources.has(resource->get_instance_id())) {
      return OK;
    }
    ERR_FAIL_COND_V(result.resources.size() >= 65536, ERR_INVALID_DATA);
    result.resources.insert(resource->get_instance_id());
    Ref<Mesh> mesh = resource;
    if (mesh.is_valid()) {
      for (int surface = 0; surface < mesh->get_surface_count(); ++surface) {
        result.geometry_bytes +=
            uint64_t(mesh->surface_get_array_len(surface)) * 64 +
            uint64_t(mesh->surface_get_array_index_len(surface)) * 4;
      }
    }
    Ref<Texture2D> texture = resource;
    if (texture.is_valid()) {
      const Ref<Image> image = texture->get_image();
      if (image.is_valid()) {
        result.texture_bytes += image->get_data().size();
      }
    }
    List<PropertyInfo> properties;
    resource->get_property_list(&properties);
    for (const auto& property : properties) {
      if (property.usage & PROPERTY_USAGE_STORAGE) {
        const auto error = MeasureValue(resource->get(property.name), result,
                                        depth + 1);
        ERR_FAIL_COND_V(error != OK, error);
      }
    }
  } else if (value.get_type() == Variant::ARRAY) {
    const Array values = value;
    ERR_FAIL_COND_V(values.size() > 1048576, ERR_INVALID_DATA);
    for (const Variant& child : values) {
      const auto error = MeasureValue(child, result, depth + 1);
      ERR_FAIL_COND_V(error != OK, error);
    }
  } else if (value.get_type() == Variant::DICTIONARY) {
    const Dictionary values = value;
    ERR_FAIL_COND_V(values.size() > 65536, ERR_INVALID_DATA);
    for (const Variant* key = values.next(); key; key = values.next(key)) {
      const auto error = MeasureValue(values[*key], result, depth + 1);
      ERR_FAIL_COND_V(error != OK, error);
    }
  }
  return OK;
}

inline Error MeasureTree(Node* node, const Transform3D& parent,
                         Measure& result, int depth = 0) {
  ERR_FAIL_COND_V(!node || depth > 64 || ++result.nodes > 65536,
                 ERR_INVALID_DATA);
  const Ref<Script> script = node->get_script();
  ERR_FAIL_COND_V(script.is_valid(), ERR_INVALID_DATA);
  Transform3D transform = parent;
  if (const auto* spatial = Object::cast_to<Node3D>(node)) {
    transform *= spatial->get_transform();
  }
  const auto add = [&](const AABB& box) {
    result.bounds = result.geometry ? result.bounds.merge(box) : box;
    result.geometry = true;
  };
  if (const auto* mesh = Object::cast_to<MeshInstance3D>(node);
      mesh && mesh->get_mesh().is_valid()) {
    add(transform.xform(mesh->get_mesh()->get_aabb()));
  }
  if (const auto* multi = Object::cast_to<MultiMeshInstance3D>(node);
      multi && multi->get_multimesh().is_valid()) {
    const Ref<MultiMesh> batch = multi->get_multimesh();
    ERR_FAIL_COND_V(batch->get_mesh().is_null() ||
                       batch->get_instance_count() > 1048576,
                   ERR_INVALID_DATA);
    const AABB box = batch->get_mesh()->get_aabb();
    for (int i = 0; i < batch->get_instance_count(); ++i) {
      add((transform * batch->get_instance_transform(i)).xform(box));
    }
  }
  List<PropertyInfo> properties;
  node->get_property_list(&properties);
  for (const auto& property : properties) {
    if (property.usage & PROPERTY_USAGE_STORAGE) {
      const auto error = MeasureValue(node->get(property.name), result, 0);
      ERR_FAIL_COND_V(error != OK, error);
    }
  }
  for (int i = 0; i < node->get_child_count(); ++i) {
    const auto error = MeasureTree(node->get_child(i), transform, result, depth + 1);
    ERR_FAIL_COND_V(error != OK, error);
  }
  return OK;
}

inline Error Record(const String& path, Dictionary& output) {
  Error error = OK;
  const Ref<Resource> resource = ResourceLoader::load(
      path, "", ResourceFormatLoader::CACHE_MODE_REUSE, &error);
  ERR_FAIL_COND_V(resource.is_null() || error != OK, ERR_INVALID_DATA);
  Measure measure;
  error = MeasureValue(resource, measure, 0);
  ERR_FAIL_COND_V(error != OK, error);
  Ref<Mesh> mesh = resource;
  if (mesh.is_valid()) {
    measure.bounds = mesh->get_aabb();
    measure.geometry = true;
  }
  Array layouts;
  Ref<PackedScene> scene = resource;
  if (scene.is_valid()) {
    Node* root = scene->instantiate();
    error = MeasureTree(root, Transform3D(), measure);
    if (error == OK) {
      for (int i = 0; i < root->get_child_count(); ++i) {
        auto* child = Object::cast_to<Node3D>(root->get_child(i));
        if (!child || child->get_scene_file_path().is_empty()) {
          layouts.clear();
          break;
        }
        Measure placement;
        error = MeasureTree(child, Transform3D(), placement);
        if (error != OK || !placement.geometry) {
          layouts.clear();
          break;
        }
        Dictionary entry;
        entry["name"] = String(child->get_name());
        entry["resource"] = path.get_base_dir().path_to_file(child->get_scene_file_path());
        entry["position"] = VectorValue(child->get_transform().origin);
        Array basis;
        for (int column = 0; column < 3; ++column) {
          basis.push_back(VectorValue(child->get_transform().basis.get_column(column)));
        }
        entry["basis"] = basis;
        entry["minimum"] = VectorValue(placement.bounds.position);
        entry["maximum"] = VectorValue(placement.bounds.get_end());
        layouts.push_back(entry);
      }
      // Animated/equipped actors need a conservative envelope; the exact
      // gameplay pose remains in the separate simulation support resource.
      if (root->has_meta("character_simulation")) {
        measure.bounds = measure.bounds.grow(3);
      }
    }
    if (root) {
      memdelete(root);
    }
    ERR_FAIL_COND_V(error != OK, error);
  }
  Array closure;
  HashSet<String> visited;
  Vector<String> pending;
  pending.push_back(path);
  while (!pending.is_empty()) {
    const String current = pending[pending.size() - 1];
    pending.resize(pending.size() - 1);
    if (visited.has(current)) {
      continue;
    }
    ERR_FAIL_COND_V(visited.size() >= 65536, ERR_INVALID_DATA);
    visited.insert(current);
    Dictionary dependency;
    dependency["path"] = path.get_base_dir().path_to_file(current);
    dependency["sha256"] = FileAccess::get_sha256(current);
    const Ref<FileAccess> file = FileAccess::open(current, FileAccess::READ);
    ERR_FAIL_COND_V(file.is_null(), ERR_FILE_CANT_READ);
    dependency["bytes"] = int64_t(file->get_length());
    closure.push_back(dependency);
    List<String> dependencies;
    ResourceLoader::get_dependencies(current, &dependencies, true);
    for (const String& reference : dependencies) {
      const PackedStringArray parts = reference.split("::");
      String next = parts[0];
      if (next.begins_with("uid://")) {
        ERR_FAIL_COND_V(parts.size() < 3, ERR_INVALID_DATA);
        next = parts[2];
      }
      if (next.is_relative_path()) {
        next = current.get_base_dir().path_join(next);
      }
      pending.push_back(next.simplify_path());
    }
  }
  output["version"] = 1;
  output["sha256"] = FileAccess::get_sha256(path);
  output["resource_type"] = String(resource->get_class());
  output["has_geometry"] = measure.geometry;
  output["minimum"] = VectorValue(measure.bounds.position);
  output["maximum"] = VectorValue(measure.bounds.get_end());
  output["nodes"] = int64_t(measure.nodes);
  output["geometry_bytes"] = int64_t(measure.geometry_bytes);
  output["texture_bytes"] = int64_t(measure.texture_bytes);
  output["dependencies"] = closure;
  output["layouts"] = layouts;
  return OK;
}
}  // namespace CookerStreaming
