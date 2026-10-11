#pragma once

#include <map>
#include <vector>

#include "core/io/image.h"
#include "core/object/script_language.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"
#include "scene/resources/packed_scene.h"
#include "scene/resources/surface_tool.h"

namespace CookerMinimum {
struct Options {
  double triangle_ratio = 0.25, minimum_height = 0;
  int texture_size = 256;
  bool lock_border = true;
};

class Builder {
 public:
  explicit Builder(const Options& options) : options_(options) {}
  Ref<Resource> Build(const Ref<Resource>& source) {
    Ref<Mesh> mesh = source;
    if (mesh.is_valid()) {
      return MeshValue(mesh);
    }
    Ref<Material> material = source;
    if (material.is_valid()) {
      return MaterialValue(material);
    }
    Ref<PackedScene> packed = source;
    if (packed.is_null()) {
      return Ref<Resource>();
    }
    Node* original = packed->instantiate();
    if (!original) {
      return Ref<Resource>();
    }
    auto* root = memnew(Node3D);
    root->set_name("MinimumVisual");
    const bool valid = CopyTree(original, root, Transform3D(), 0);
    memdelete(original);
    Ref<PackedScene> result;
    if (valid && root->get_child_count()) {
      result.instantiate();
      root->set_meta("veya_minimum_visual", true);
      if (result->pack(root) != OK) {
        result.unref();
      }
    }
    memdelete(root);
    return result;
  }

 private:
  Ref<Texture2D> TextureValue(const Ref<Texture2D>& source) {
    const auto id = source->get_instance_id();
    if (textures_.count(id)) {
      return textures_.at(id);
    }
    Ref<Image> image = source->get_image();
    if (image.is_null() || image->is_empty()) {
      return Ref<Texture2D>();
    }
    image = image->duplicate();
    if (image->is_compressed() && image->decompress() != OK) {
      return Ref<Texture2D>();
    }
    const double scale = MIN(1.0, double(options_.texture_size) /
                                    MAX(image->get_width(), image->get_height()));
    image->resize(MAX(1, int(image->get_width() * scale)),
                  MAX(1, int(image->get_height() * scale)), Image::INTERPOLATE_LANCZOS);
    image->generate_mipmaps();
    Ref<Texture2D> result = ImageTexture::create_from_image(image);
    textures_.emplace(id, result);
    return result;
  }
  Ref<Material> MaterialValue(const Ref<Material>& source) {
    if (source.is_null()) {
      return Ref<Material>();
    }
    const auto id = source->get_instance_id();
    if (materials_.count(id)) {
      return materials_.at(id);
    }
    Ref<Material> result = source->duplicate(false);
    if (result.is_null()) {
      return Ref<Material>();
    }
    materials_.emplace(id, result);
    List<PropertyInfo> properties;
    source->get_property_list(&properties);
    for (const auto& property : properties) {
      if (!(property.usage & PROPERTY_USAGE_STORAGE)) {
        continue;
      }
      const Variant value = source->get(property.name);
      if (value.get_type() != Variant::OBJECT) {
        continue;
      }
      Ref<Texture2D> texture = value;
      if (texture.is_valid()) {
        const Ref<Texture2D> minimum = TextureValue(texture);
        if (minimum.is_null()) {
          return Ref<Material>();
        }
        result->set(property.name, minimum);
      }
      Ref<Material> next = value;
      if (next.is_valid()) {
        result->set(property.name, MaterialValue(next));
      }
    }
    return result;
  }
  Ref<Mesh> MeshValue(const Ref<Mesh>& source) {
    const auto id = source->get_instance_id();
    if (meshes_.count(id)) {
      return meshes_.at(id);
    }
    Ref<ArrayMesh> result;
    result.instantiate();
    for (int surface = 0; surface < source->get_surface_count(); ++surface) {
      if (source->surface_get_primitive_type(surface) != Mesh::PRIMITIVE_TRIANGLES) {
        return Ref<Mesh>();
      }
      Array arrays = source->surface_get_arrays(surface).duplicate();
      PackedVector3Array vertices = arrays[Mesh::ARRAY_VERTEX];
      PackedInt32Array indices = arrays[Mesh::ARRAY_INDEX];
      if (vertices.is_empty() || vertices.size() > 1048576) {
        return Ref<Mesh>();
      }
      if (indices.is_empty()) {
        indices.resize(vertices.size());
        for (int i = 0; i < indices.size(); ++i) {
          indices.set(i, i);
        }
      }
      if (indices.size() % 3 || !SurfaceTool::simplify_func) {
        return Ref<Mesh>();
      }
      std::vector<float> positions(size_t(vertices.size()) * 3);
      for (int i = 0; i < vertices.size(); ++i) {
        for (int axis = 0; axis < 3; ++axis) {
          positions[size_t(i) * 3 + axis] = vertices[i][axis];
        }
      }
      PackedInt32Array reduced;
      reduced.resize(indices.size());
      const size_t target = MAX(size_t(3), size_t(indices.size() * options_.triangle_ratio) / 3 * 3);
      const unsigned int flags = options_.lock_border ? SurfaceTool::SIMPLIFY_LOCK_BORDER : 0;
      const size_t count = SurfaceTool::simplify_func(
          reinterpret_cast<unsigned int*>(reduced.ptrw()),
          reinterpret_cast<const unsigned int*>(indices.ptr()), indices.size(),
          positions.data(), vertices.size(), sizeof(float) * 3, target, 0.02f,
          flags, nullptr);
      if (count < 3 || count % 3) {
        return Ref<Mesh>();
      }
      reduced.resize(count);
      arrays[Mesh::ARRAY_INDEX] = reduced;
      // Preserve normals, UVs, colors and tangent semantics. These static
      // outputs own new vertex data and never retain a source Mesh or Skin.
      arrays[Mesh::ARRAY_BONES] = Variant();
      arrays[Mesh::ARRAY_WEIGHTS] = Variant();
      result->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
      const Ref<Material> material = source->surface_get_material(surface);
      const Ref<Material> minimum = MaterialValue(material);
      if (material.is_valid() && minimum.is_null()) {
        return Ref<Mesh>();
      }
      result->surface_set_material(result->get_surface_count() - 1, minimum);
    }
    if (!result->get_surface_count()) {
      return Ref<Mesh>();
    }
    meshes_.emplace(id, result);
    return result;
  }
  bool CopyTree(Node* source, Node3D* destination, const Transform3D& parent,
                 int depth) {
    const Ref<Script> script = source->get_script();
    if (depth > 64 || ++nodes_ > 65536 || script.is_valid() ||
        Object::cast_to<Skeleton3D>(source)) {
      return false;
    }
    Transform3D transform = parent;
    if (const auto* spatial = Object::cast_to<Node3D>(source)) {
      transform *= spatial->get_transform();
    }
    Node3D* node = nullptr;
    if (auto* mesh = Object::cast_to<MeshInstance3D>(source);
        mesh && mesh->get_mesh().is_valid()) {
      if (mesh->get_skin().is_valid()) {
        return false;
      }
      if (mesh->get_mesh()->get_aabb().size.y >= options_.minimum_height) {
        auto* copy = memnew(MeshInstance3D);
        copy->set_mesh(MeshValue(mesh->get_mesh()));
        copy->set_material_override(MaterialValue(mesh->get_material_override()));
        if (copy->get_mesh().is_null()) {
          memdelete(copy);
          return false;
        }
        for (int surface = 0; surface < mesh->get_mesh()->get_surface_count(); ++surface) {
          copy->set_surface_override_material(surface,
              MaterialValue(mesh->get_surface_override_material(surface)));
        }
        node = copy;
      }
    } else if (auto* multi = Object::cast_to<MultiMeshInstance3D>(source);
               multi && multi->get_multimesh().is_valid()) {
      const Ref<MultiMesh> original = multi->get_multimesh();
      if (original->get_mesh().is_null()) {
        return false;
      }
      if (original->get_mesh()->get_aabb().size.y >= options_.minimum_height) {
        Ref<MultiMesh> batch = original->duplicate(false);
        batch->set_mesh(MeshValue(original->get_mesh()));
        auto* copy = memnew(MultiMeshInstance3D);
        copy->set_multimesh(batch);
        copy->set_material_override(MaterialValue(multi->get_material_override()));
        if (batch->get_mesh().is_null()) {
          memdelete(copy);
          return false;
        }
        node = copy;
      }
    }
    if (node) {
      node->set_name("Minimum_" + itos(nodes_));
      node->set_transform(transform);
      destination->add_child(node);
      node->set_owner(destination);
    }
    for (int i = 0; i < source->get_child_count(); ++i) {
      if (!CopyTree(source->get_child(i), destination, transform, depth + 1)) {
        return false;
      }
    }
    return true;
  }
  Options options_;
  size_t nodes_ = 0;
  std::map<ObjectID, Ref<Mesh>> meshes_;
  std::map<ObjectID, Ref<Material>> materials_;
  std::map<ObjectID, Ref<Texture2D>> textures_;
};
}  // namespace CookerMinimum
