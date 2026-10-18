#pragma once
/*<<----- VEYA_COOKER: Value-only admission profile with conservative runtime accounting. */

#include "character_actions.h"
#include "virtual_geometry_validation.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/io/resource_loader.h"
#include "core/object/script_language.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "scene/3d/gpu_particles_3d.h"
#include "scene/3d/lightmap_gi.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/animation/animation_player.h"
#include "scene/resources/3d/box_shape_3d.h"
#include "scene/resources/3d/capsule_shape_3d.h"
#include "scene/resources/3d/concave_polygon_shape_3d.h"
#include "scene/resources/3d/convex_polygon_shape_3d.h"
#include "scene/resources/3d/height_map_shape_3d.h"
#include "scene/resources/3d/sphere_shape_3d.h"
#include "scene/resources/animation.h"
#include "scene/resources/packed_scene.h"
#include "scene/resources/shader.h"
#include "scene/resources/texture.h"

#include <initializer_list>

namespace CookerStreaming {
inline String PackageRoot(const String &path) {
	if (path.begins_with("res://assets/runtime/")) {
		return "res://assets/runtime/";
	}
	for (const char *prefix : { "res://content/worlds/",
				 "res://content/mod-worlds/" }) {
		if (path.begins_with(prefix)) {
			const String id = path.trim_prefix(prefix).get_slicec('/', 0);
			return id.is_empty() ? String() : String(prefix) + id + "/";
		}
	}
	return {};
}

inline String ReferencePath(const String &source, const String &target) {
	// A world can move to user://. Dependencies outside its package cannot
	// follow that move; this matches the binary resource saver contract.
	const String package = PackageRoot(source);
	return !package.is_empty() && package == PackageRoot(target)
			? source.get_base_dir().path_to_file(target)
			: target;
}

inline Array VectorValue(const Vector3 &value) {
	Array result;
	result.push_back(value.x);
	result.push_back(value.y);
	result.push_back(value.z);
	return result;
}

struct Measure {
	AABB bounds, collision_bounds;
	bool geometry = false, collision = false, effect_valid = true;
	uint64_t nodes = 0, geometry_bytes = 0, texture_bytes = 0;
	uint64_t instances = 0, particle_systems = 0, declared_particles = 0;
	HashSet<ObjectID> resources;
	const HashSet<ObjectID> *validated_resources = nullptr;
	String path;
	Array costs;
	HashMap<String, HashSet<String>> page_containers;
};

inline Error MeasureValue(const Variant &value, Measure &result, int depth) {
	ERR_FAIL_COND_V(depth >= 32, ERR_INVALID_DATA);
	if (value.get_type() == Variant::OBJECT) {
		Ref<Resource> resource = value;
		if (resource.is_null() || result.resources.has(resource->get_instance_id())) {
			return OK;
		}
		ERR_FAIL_COND_V(result.resources.size() >= 65536, ERR_INVALID_DATA);
		result.resources.insert(resource->get_instance_id());
		uint64_t geometry_bytes = 0, texture_bytes = 0;
		Ref<Mesh> mesh = resource;
		const Ref<VirtualGeometryMesh> virtual_mesh = resource;
		if (virtual_mesh.is_valid()) {
			const String owner = resource->get_path().get_slice("::", 0);
			ERR_FAIL_COND_V(owner.is_empty(), ERR_INVALID_DATA);
			if (result.validated_resources == nullptr || !result.validated_resources->has(resource->get_instance_id())) {
				const Error error = CookerVirtualGeometry::validate_container(virtual_mesh, owner);
				ERR_FAIL_COND_V(error != OK, error);
			}
			result.page_containers[owner].insert(owner.get_base_dir().path_join(virtual_mesh->get_container_file()));
			geometry_bytes = CookerVirtualGeometry::metadata_bytes(virtual_mesh) +
					uint64_t(virtual_mesh->get_model().hierarchy.root_pages) * VirtualGeometry::ROOT_PAGE_GPU_CAPACITY;
		} else if (mesh.is_valid()) {
			for (int surface = 0; surface < mesh->get_surface_count(); ++surface) {
				geometry_bytes +=
						uint64_t(mesh->surface_get_array_len(surface)) * 128 +
						uint64_t(mesh->surface_get_array_index_len(surface)) * 8;
			}
		}
		if (mesh.is_valid() && virtual_mesh.is_null()) {
			geometry_bytes += 1024;
		}
		Ref<Texture2D> texture = resource;
		if (texture.is_valid()) {
			texture_bytes = uint64_t(texture->get_width()) *
					uint64_t(texture->get_height()) * 16 / 3;
		}
		Ref<TextureLayered> layered = resource;
		if (layered.is_valid()) {
			texture_bytes = uint64_t(layered->get_width()) *
					uint64_t(layered->get_height()) *
					uint64_t(layered->get_layers()) * 16 * 4 / 3;
		}
		Ref<Animation> animation = resource;
		if (animation.is_valid()) {
			geometry_bytes = 1024;
			for (int i = 0; i < animation->get_track_count(); ++i) {
				ERR_FAIL_COND_V(animation->track_get_key_count(i) > 1000000, ERR_INVALID_DATA);
				geometry_bytes += uint64_t(animation->track_get_key_count(i)) * 96 + 512;
			}
		}
		Ref<Shape3D> shape = resource;
		if (shape.is_valid()) {
			geometry_bytes = 1024;
		}
		Ref<ConcavePolygonShape3D> concave = resource;
		Ref<ConvexPolygonShape3D> convex = resource;
    Ref<HeightMapShape3D> heightfield = resource;
		if (concave.is_valid()) {
      const PackedVector3Array faces = concave->get_faces();
      ERR_FAIL_COND_V(faces.is_empty() || faces.size() % 3 != 0,
                      ERR_INVALID_DATA);
      for (const Vector3& vertex : faces) {
        ERR_FAIL_COND_V(!vertex.is_finite(), ERR_INVALID_DATA);
      }
      geometry_bytes = uint64_t(faces.size()) * 64 + 1024;
    } else if (heightfield.is_valid()) {
      const Vector<real_t> data = heightfield->get_map_data();
      const int width = heightfield->get_map_width();
      ERR_FAIL_COND_V(width < 4 || width != heightfield->get_map_depth() ||
                          data.size() != width * width, ERR_INVALID_DATA);
      for (real_t value : data) {
        ERR_FAIL_COND_V(!Math::is_finite(value), ERR_INVALID_DATA);
      }
      // Conservative data/backend accounting, not measured physics memory.
      geometry_bytes = uint64_t(data.size()) * 16 + 1024;
    } else if (convex.is_valid()) {
			geometry_bytes = uint64_t(convex->get_points().size()) * 64 + 1024;
		}
		Ref<LightmapGIData> lightmap = resource;
		if (lightmap.is_valid()) {
			const Dictionary probes = lightmap->get("probe_data");
			const PackedVector3Array points = probes.get("points", PackedVector3Array());
			const PackedColorArray sh = probes.get("sh", PackedColorArray());
			const PackedInt32Array tetrahedra = probes.get("tetrahedra", PackedInt32Array());
			const PackedInt32Array bsp = probes.get("bsp", PackedInt32Array());
			geometry_bytes = 1024 + uint64_t(points.size()) * 16 + uint64_t(sh.size()) * 16 +
					uint64_t(tetrahedra.size()) * 4 + uint64_t(bsp.size()) * 4;
		}
		Ref<MultiMesh> multi = resource;
		if (multi.is_valid()) {
			geometry_bytes = uint64_t(multi->get_instance_count()) * 80 + 1024;
		}
		if (resource->has_meta("veya_resource_index_bytes")) {
			const int64_t bytes = resource->get_meta("veya_resource_index_bytes");
			ERR_FAIL_COND_V(bytes < 0 || bytes > 51200000, ERR_INVALID_DATA);
			geometry_bytes += uint64_t(bytes);
      const Variant encoded =
          resource->get_meta("veya_resource_index", Variant());
      ERR_FAIL_COND_V(encoded.get_type() != Variant::DICTIONARY,
                      ERR_INVALID_DATA);
      const Dictionary block = encoded;
      ERR_FAIL_COND_V(int64_t(block.get("format", 0)) != 1 ||
                          block.get("ids", Variant()).get_type() !=
                              Variant::PACKED_STRING_ARRAY ||
                          block.get("kinds", Variant()).get_type() !=
                              Variant::PACKED_STRING_ARRAY ||
                          block.get("values", Variant()).get_type() !=
                              Variant::PACKED_FLOAT64_ARRAY ||
                          block.get("blocking", Variant()).get_type() !=
                              Variant::PACKED_BYTE_ARRAY,
                      ERR_INVALID_DATA);
      const PackedStringArray ids = block["ids"], kinds = block["kinds"];
      const PackedFloat64Array values = block["values"];
      const PackedByteArray blocking = block["blocking"];
      ERR_FAIL_COND_V(ids.size() > 100000 || kinds.size() != ids.size() ||
                          blocking.size() != ids.size() ||
                          values.size() != ids.size() * 18,
                      ERR_INVALID_DATA);
      HashSet<String> identities;
      for (int64_t row = 0; row < ids.size(); ++row) {
        ERR_FAIL_COND_V(
            ids[row].is_empty() || ids[row].length() > 96 ||
                !ids[row].is_valid_identifier() || identities.has(ids[row]) ||
                kinds[row].is_empty() || kinds[row].length() > 48 ||
                !kinds[row].is_valid_identifier() || blocking[row] > 1,
            ERR_INVALID_DATA);
        identities.insert(ids[row]);
        Vector3 vectors[6];
        for (int vector = 0; vector < 6; ++vector) {
          for (int axis = 0; axis < 3; ++axis) {
            vectors[vector][axis] = values[row * 18 + vector * 3 + axis];
          }
          ERR_FAIL_COND_V(!vectors[vector].is_finite(), ERR_INVALID_DATA);
        }
        ERR_FAIL_COND_V(
            Math::abs(Basis(vectors[1], vectors[2], vectors[3]).determinant()) <
                    1e-9 ||
                vectors[4].x <= 0 || vectors[4].y <= 0 || vectors[4].z <= 0,
            ERR_INVALID_DATA);
      }
    }
		result.geometry_bytes += geometry_bytes;
		result.texture_bytes += texture_bytes;
		if (geometry_bytes || texture_bytes) {
			Dictionary cost;
			const String identity = resource->get_path().is_empty()
					? result.path + "#embedded"
					: resource->get_path();
			cost["identity"] = ReferencePath(result.path, identity);
			cost["geometry_bytes"] = int64_t(geometry_bytes);
			cost["texture_bytes"] = int64_t(texture_bytes);
			result.costs.push_back(cost);
		}
		Ref<Shader> shader = resource;
		if (shader.is_valid()) {
			List<StringName> names;
			shader->get_default_texture_parameter_list(&names);
			for (const auto &name : names) {
				for (int index = 0; index < 32; ++index) {
					const Ref<Texture> texture =
							shader->get_default_texture_parameter(name, index);
					if (texture.is_null()) {
						break;
					}
					const auto error = MeasureValue(texture, result, depth + 1);
					ERR_FAIL_COND_V(error != OK, error);
				}
			}
		}
		if (mesh.is_valid()) {
			for (int surface = 0; surface < mesh->get_surface_count(); ++surface) {
				const Error material_error = MeasureValue(mesh->surface_get_material(surface), result, depth + 1);
				ERR_FAIL_COND_V(material_error != OK, material_error);
			}
		}
		List<PropertyInfo> properties;
		resource->get_property_list(&properties);
		for (const auto &property : properties) {
			if ((property.usage & PROPERTY_USAGE_STORAGE) &&
					(property.type == Variant::OBJECT || property.type == Variant::ARRAY || property.type == Variant::DICTIONARY)) {
				if ((mesh.is_valid() && property.name == "_surfaces") ||
						((texture.is_valid() || layered.is_valid()) &&
								(property.name == "image" || property.name == "images"))) {
					continue;
				}
				const auto error = MeasureValue(resource->get(property.name), result,
						depth + 1);
				ERR_FAIL_COND_V(error != OK, error);
			}
		}
	} else if (value.get_type() == Variant::ARRAY) {
		const Array values = value;
		// PackedScene storage arrays grow with the number of spatial batches.
		// Keep them bounded without rejecting valid, finely clustered scenes.
		ERR_FAIL_COND_V(values.size() > 262144, ERR_INVALID_DATA);
		for (const Variant &child : values) {
			const auto error = MeasureValue(child, result, depth + 1);
			ERR_FAIL_COND_V(error != OK, error);
		}
	} else if (value.get_type() == Variant::DICTIONARY) {
		const Dictionary values = value;
		ERR_FAIL_COND_V(values.size() > 16384, ERR_INVALID_DATA);
		for (const Variant *key = values.next(); key; key = values.next(key)) {
			const auto error = MeasureValue(values[*key], result, depth + 1);
			ERR_FAIL_COND_V(error != OK, error);
		}
	}
	return OK;
}

inline Error MeasureTree(Node *node, const Transform3D &parent,
		Measure &result, int depth = 0) {
	ERR_FAIL_COND_V(!node || depth >= 64 || ++result.nodes > 100000,
			ERR_INVALID_DATA);
	result.effect_valid = result.effect_valid &&
			(node->is_class("Node3D") || node->is_class("GPUParticles3D") ||
					node->is_class("MeshInstance3D") || node->is_class("MultiMeshInstance3D") ||
					node->is_class("AnimationPlayer"));
	if (const auto *particles = Object::cast_to<GPUParticles3D>(node)) {
		result.effect_valid = result.effect_valid && particles->get_amount() <= 512;
	}
	const Ref<Script> script = node->get_script();
	ERR_FAIL_COND_V(script.is_valid(), ERR_INVALID_DATA);
	Transform3D transform = parent;
	if (const auto *spatial = Object::cast_to<Node3D>(node)) {
		ERR_FAIL_COND_V(spatial->is_set_as_top_level() || !spatial->get_transform().is_finite(), ERR_INVALID_DATA);
		transform *= spatial->get_transform();
	}
	const auto add = [&](const AABB &box) {
		result.bounds = result.geometry ? result.bounds.merge(box) : box;
		result.geometry = true;
	};
	if (const auto *mesh = Object::cast_to<MeshInstance3D>(node);
			mesh && mesh->get_mesh().is_valid()) {
		++result.instances;
		add(transform.xform(mesh->get_mesh()->get_aabb().grow(mesh->get_extra_cull_margin())));
	}
	if (const auto *multi = Object::cast_to<MultiMeshInstance3D>(node);
			multi && multi->get_multimesh().is_valid()) {
		const Ref<MultiMesh> batch = multi->get_multimesh();
		ERR_FAIL_COND_V(batch->get_mesh().is_null() ||
						batch->get_instance_count() > 100000,
				ERR_INVALID_DATA);
		const AABB box = batch->get_mesh()->get_aabb();
		result.instances += batch->get_instance_count();
		const PackedFloat32Array buffer = batch->get("buffer");
		const int stride = 12 + (batch->is_using_colors() ? 4 : 0) +
				(batch->is_using_custom_data() ? 4 : 0);
		ERR_FAIL_COND_V(batch->get_transform_format() != MultiMesh::TRANSFORM_3D ||
						buffer.size() != int64_t(batch->get_instance_count()) * stride,
				ERR_INVALID_DATA);
		AABB local;
		for (int i = 0; i < batch->get_instance_count(); ++i) {
			const float *values = buffer.ptr() + int64_t(i) * stride;
			const Transform3D pose(Basis(Vector3(values[0], values[4], values[8]),
										   Vector3(values[1], values[5], values[9]),
										   Vector3(values[2], values[6], values[10])),
					Vector3(values[3], values[7], values[11]));
			ERR_FAIL_COND_V(!pose.is_finite(), ERR_INVALID_DATA);
			const AABB instance = pose.xform(box);
			local = i == 0 ? instance : local.merge(instance);
		}
		if (batch->get_instance_count()) {
			add(transform.xform(local.grow(multi->get_extra_cull_margin())));
		}
	}
	if (const auto *particles = Object::cast_to<GPUParticles3D>(node)) {
		++result.particle_systems;
		result.declared_particles += particles->get_amount();
	}
	if (const auto *shape_node = Object::cast_to<CollisionShape3D>(node);
			shape_node && shape_node->get_shape().is_valid()) {
		const Ref<Shape3D> shape = shape_node->get_shape();
		AABB box;
		const Ref<BoxShape3D> cube = shape;
		const Ref<SphereShape3D> sphere = shape;
		const Ref<CapsuleShape3D> capsule = shape;
		const Ref<ConvexPolygonShape3D> convex = shape;
		const Ref<ConcavePolygonShape3D> concave = shape;
		const Ref<HeightMapShape3D> heightfield = shape;
		if (cube.is_valid()) {
			box = AABB(-cube->get_size() / 2, cube->get_size());
		} else if (sphere.is_valid()) {
			const Vector3 radius(sphere->get_radius(), sphere->get_radius(), sphere->get_radius());
			box = AABB(-radius, radius * 2);
		} else if (capsule.is_valid()) {
			const Vector3 size(capsule->get_radius() * 2, capsule->get_height(), capsule->get_radius() * 2);
			box = AABB(-size / 2, size);
		} else if (heightfield.is_valid()) {
      const double half = (heightfield->get_map_width() - 1) * 0.5;
      box = AABB(Vector3(-half, heightfield->get_min_height(), -half),
                 Vector3(half * 2,
                         heightfield->get_max_height() - heightfield->get_min_height(),
                         half * 2));
		} else {
			const PackedVector3Array points = convex.is_valid() ? convex->get_points() : concave.is_valid() ? concave->get_faces()
																											: PackedVector3Array();
			ERR_FAIL_COND_V(points.is_empty(), ERR_INVALID_DATA);
			box.position = points[0];
			for (int i = 1; i < points.size(); ++i) {
				box.expand_to(points[i]);
			}
		}
		const AABB world = transform.xform(box);
		result.collision_bounds = result.collision ? result.collision_bounds.merge(world) : world;
		result.collision = true;
	}
	List<PropertyInfo> properties;
	node->get_property_list(&properties);
	for (const auto &property : properties) {
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

inline String Digest(const String &path, HashMap<String, String> &digests) {
	const String key = path.simplify_path();
	if (const String *cached = digests.getptr(key)) {
		return *cached;
	}
	const String actual = FileAccess::get_md5(key);
	digests.insert(key, actual);
	return actual;
}

inline Skeleton3D *Skeleton(Node *root) {
	Vector<Node *> pending;
	if (root) {
		pending.push_back(root);
	}
	int nodes = 0;
	while (!pending.is_empty() && ++nodes <= 4096) {
		Node *node = pending[pending.size() - 1];
		pending.resize(pending.size() - 1);
		if (auto *skeleton = Object::cast_to<Skeleton3D>(node)) {
			return skeleton;
		}
		for (int i = 0; i < node->get_child_count(); ++i) {
			pending.push_back(node->get_child(i));
		}
	}
	return nullptr;
}
inline Transform3D Axes(Skeleton3D *skeleton) {
	Transform3D result;
	for (Node3D *node = skeleton; node; node = node->get_parent_node_3d()) {
		result = node->get_transform() * result;
	}
	return result;
}
inline bool SupportValuesValid(const Variant& value, HashSet<ObjectID>& seen,
                               int depth) {
  if (depth >= 32) {
    return false;
  }
  if (value.get_type() == Variant::OBJECT) {
    const Ref<Resource> resource = value;
    if (resource.is_null()) {
      return static_cast<Object*>(value) == nullptr;
    }
    const ObjectID id = resource->get_instance_id();
    if (seen.has(id)) {
      return true;
    }
    seen.insert(id);
    const StringName type = resource->get_class_name();
    if (seen.size() > 4096 || (type != "PackedScene" && type != "Animation" &&
                               type != "AnimationLibrary" && type != "Curve")) {
      return false;
    }
    List<PropertyInfo> properties;
    resource->get_property_list(&properties);
    for (const PropertyInfo& property : properties) {
      if ((property.usage & PROPERTY_USAGE_STORAGE) &&
          !SupportValuesValid(resource->get(property.name), seen, depth + 1)) {
        return false;
      }
    }
  } else if (value.get_type() == Variant::ARRAY) {
    const Array values = value;
    if (values.size() > 16384) {
      return false;
    }
    for (int index = 0; index < values.size(); ++index) {
      if (!SupportValuesValid(values[index], seen, depth + 1)) {
        return false;
      }
    }
  } else if (value.get_type() == Variant::DICTIONARY) {
    const Dictionary values = value;
    if (values.size() > 16384) {
      return false;
    }
    for (const Variant& key : values.keys()) {
      if (!SupportValuesValid(values[key], seen, depth + 1)) {
        return false;
      }
    }
  }
  return true;
}
inline bool SupportTreeValid(Node* root, int& nodes) {
  const StringName type = root->get_class_name();
  if (root->get_script_instance() || ++nodes > 256 ||
      (type != "Node3D" && type != "Skeleton3D" && type != "BoneAttachment3D" &&
       type != "AnimationPlayer")) {
    return false;
  }
  for (int index = 0; index < root->get_child_count(); ++index) {
    if (!SupportTreeValid(root->get_child(index), nodes)) {
      return false;
    }
  }
  return true;
}
inline AnimationPlayer* SupportPlayer(Node* root) {
  if (auto* player = Object::cast_to<AnimationPlayer>(root)) {
    return player;
  }
  for (int index = 0; index < root->get_child_count(); ++index) {
    if (auto* player = SupportPlayer(root->get_child(index))) {
      return player;
    }
  }
  return nullptr;
}
inline Error CheckSupportResource(const Ref<PackedScene>& resource,
                                  Node* root) {
  int nodes = 0;
  HashSet<ObjectID> seen;
  ERR_FAIL_COND_V(!root || !SupportValuesValid(resource, seen, 0) ||
                      !SupportTreeValid(root, nodes),
                  ERR_INVALID_DATA);
  Skeleton3D* skeleton = Skeleton(root);
  AnimationPlayer* player = SupportPlayer(root);
  ERR_FAIL_COND_V(!skeleton || !player ||
                      !bool(root->get_meta("veya_simulation_support", false)) ||
                      skeleton->get_bone_count() <= 0 ||
                      skeleton->get_bone_count() > 128 ||
                      !Math::is_finite(skeleton->get_motion_scale()) ||
                      skeleton->get_motion_scale() <= 0 ||
                      !player->has_animation("character/move.idle"),
                  ERR_INVALID_DATA);
  LocalVector<StringName> animations;
  player->get_animation_list(&animations);
  for (const StringName& name : animations) {
    const Ref<Animation> animation = player->get_animation(name);
    ERR_FAIL_COND_V(animation.is_null() || animation->get_track_count() > 512,
                    ERR_INVALID_DATA);
    for (int track = 0; track < animation->get_track_count(); ++track) {
      if (CookerCharacter::IsGroundSignalTrack(animation, track)) {
        continue;
      }
      const auto type = animation->track_get_type(track);
      const NodePath path = animation->track_get_path(track);
      Node* target = player->get_node_or_null(player->get_root_node());
      if (target && path.get_name_count() > 0) {
        target =
            target->get_node_or_null(NodePath(path.get_concatenated_names()));
      }
      ERR_FAIL_COND_V((type != Animation::TYPE_POSITION_3D &&
                       type != Animation::TYPE_ROTATION_3D &&
                       type != Animation::TYPE_SCALE_3D) ||
                          path.get_subname_count() != 1 || target != skeleton ||
                          skeleton->find_bone(path.get_subname(0)) < 0 ||
                          animation->track_get_key_count(track) > 1000000,
                      ERR_INVALID_DATA);
    }
  }
  return OK;
}
inline Error CheckSupport(Node* root, const String& path,
                          String& support_path) {
  const Ref<PackedScene> support =
      root->get_meta("character_simulation", Variant());
  const String relative = root->get_meta("character_simulation_path", "");
  ERR_FAIL_COND_V(
      support.is_null() || relative.is_empty() ||
          relative != relative.get_file() || !relative.ends_with(".scn") ||
          support->get_path() != path.get_base_dir().path_join(relative),
      ERR_INVALID_DATA);
  Node* support_root = support->instantiate();
  Skeleton3D* detailed = Skeleton(root);
  Skeleton3D* simulation = Skeleton(support_root);
  bool matching = CheckSupportResource(support, support_root) == OK &&
                  detailed && simulation && detailed->get_bone_count() > 0 &&
                  detailed->get_bone_count() <= 128 &&
                  detailed->get_bone_count() == simulation->get_bone_count() &&
                  Math::is_equal_approx(detailed->get_motion_scale(),
                                        simulation->get_motion_scale()) &&
                  Axes(detailed).is_equal_approx(Axes(simulation));
  for (int bone = 0; matching && bone < detailed->get_bone_count(); ++bone) {
    matching =
        detailed->get_bone_name(bone) == simulation->get_bone_name(bone) &&
        detailed->get_bone_parent(bone) == simulation->get_bone_parent(bone) &&
        detailed->get_bone_rest(bone).is_equal_approx(
            simulation->get_bone_rest(bone));
  }
  if (support_root) {
    memdelete(support_root);
  }
  ERR_FAIL_COND_V(!matching, ERR_INVALID_DATA);
	support_path = support->get_path();
	return OK;
}

inline Error Record(const String &path, Dictionary &output,
		HashMap<String, String> &digests) {
	Error error = OK;
	const Ref<Resource> resource = ResourceLoader::load(
			path, "", ResourceFormatLoader::CACHE_MODE_REUSE, &error);
	ERR_FAIL_COND_V(resource.is_null() || error != OK, ERR_INVALID_DATA);
	Measure measure;
	measure.path = path;
	error = MeasureValue(resource, measure, 0);
	ERR_FAIL_COND_V(error != OK, error);
	Ref<Mesh> mesh = resource;
	if (mesh.is_valid()) {
		measure.bounds = mesh->get_aabb();
		measure.geometry = true;
	}
  if (Ref<HeightMapShape3D> heightfield = resource; heightfield.is_valid()) {
    const double half = (heightfield->get_map_width() - 1) * 0.5;
    measure.collision_bounds = AABB(
        Vector3(-half, heightfield->get_min_height(), -half),
        Vector3(half * 2, heightfield->get_max_height() - heightfield->get_min_height(), half * 2));
    measure.collision = true;
  }
	Array layouts;
	bool layout_valid = false;
	Ref<PackedScene> scene = resource;
	if (scene.is_valid()) {
		Node *root = scene->instantiate();
		error = MeasureTree(root, Transform3D(), measure);
		if (error == OK && bool(root->get_meta("veya_simulation_support", false))) {
			error = CheckSupportResource(scene, root);
			if (error == OK) {
				output["simulation_support_valid"] = true;
			}
		}
		if (error == OK) {
			layout_valid = true;
			for (int i = 0; i < root->get_child_count(); ++i) {
				auto *child = Object::cast_to<Node3D>(root->get_child(i));
        if (!child || child->get_scene_file_path().is_empty()) {
          layout_valid = false;
          layouts.clear();
          break;
        }
        Transform3D parent;
        if (const auto* spatial = Object::cast_to<Node3D>(root)) {
          parent = spatial->get_transform();
        }
		const Transform3D pose = parent * child->get_transform();
		Measure placement;
		// The parent record already validated these retained, immutable Resources.
		// Placement bounds and costs still use an independent measurement.
		placement.validated_resources = &measure.resources;
		error = MeasureTree(child, parent, placement);
		if (error != OK || !placement.geometry) {
			layout_valid = false;
			layouts.clear();
			break;
		}
				Dictionary entry;
				entry["name"] = String(child->get_name());
				entry["resource"] = ReferencePath(path, child->get_scene_file_path());
				entry["position"] = VectorValue(pose.origin);
				Array basis;
				for (int column = 0; column < 3; ++column) {
					basis.push_back(VectorValue(pose.basis.get_column(column)));
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
				String support_path;
				error = CheckSupport(root, path, support_path);
				if (error == OK) {
					output["simulation_resource"] = ReferencePath(path, support_path);
					output["simulation_valid"] = true;
				}
			}
		}
		if (root) {
			memdelete(root);
		}
		ERR_FAIL_COND_V(error != OK, error);
	}
	Array closure;
	Array graph;
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
		dependency["path"] = ReferencePath(path, current);
		dependency["md5"] = Digest(current, digests);
		const bool deferred = current.ends_with(".vgp");
		if (deferred) {
			dependency["deferred"] = true;
		}
		const Ref<FileAccess> file = FileAccess::open(current, FileAccess::READ);
		ERR_FAIL_COND_V(file.is_null(), ERR_FILE_CANT_READ);
		dependency["bytes"] = int64_t(file->get_length());
		closure.push_back(dependency);
		List<String> dependencies;
		Array direct;
		if (!deferred) {
			ResourceLoader::get_dependencies(current, &dependencies, true);
		}
		for (const String &reference : dependencies) {
			const PackedStringArray parts = reference.split("::");
			String next = parts[0];
			if (next.begins_with("uid://")) {
				ERR_FAIL_COND_V(parts.size() < 3, ERR_INVALID_DATA);
				next = parts[2];
			}
			if (next.is_relative_path()) {
				next = current.get_base_dir().path_join(next);
			}
			next = next.simplify_path();
			direct.push_back(ReferencePath(path, next));
			pending.push_back(next);
		}
		if (const auto *containers = measure.page_containers.getptr(current)) {
			for (const String &container : *containers) {
				direct.push_back(ReferencePath(path, container));
				pending.push_back(container);
			}
		}
		Dictionary entry;
		entry["path"] = ReferencePath(path, current);
		entry["dependencies"] = direct;
		graph.push_back(entry);
	}
	output["version"] = 2;
	output["admission_profile_version"] = 1;
	output["measurement_version"] = 1;
	output["hash_algorithm"] = "md5";
	output["md5"] = Digest(path, digests);
	output["resource_type"] = String(resource->get_class());
	output["effect_valid"] = scene.is_valid() && measure.effect_valid && measure.nodes <= 32 &&
			measure.particle_systems <= 4 && measure.declared_particles <= 1024;
	output["has_geometry"] = measure.geometry;
	output["minimum"] = VectorValue(measure.bounds.position);
	output["maximum"] = VectorValue(measure.bounds.get_end());
	output["has_collision"] = measure.collision;
	output["collision_minimum"] = VectorValue(measure.collision_bounds.position);
	output["collision_maximum"] = VectorValue(measure.collision_bounds.get_end());
	output["nodes"] = int64_t(measure.nodes);
	output["visual_instances"] = int64_t(measure.instances);
	output["particle_systems"] = int64_t(measure.particle_systems);
	output["declared_particles"] = int64_t(measure.declared_particles);
	output["geometry_bytes"] = int64_t(measure.geometry_bytes);
	output["texture_bytes"] = int64_t(measure.texture_bytes);
	Dictionary combined_costs;
	for (int i = 0; i < measure.costs.size(); ++i) {
		const Dictionary cost = measure.costs[i];
		const String identity = cost["identity"];
		Dictionary sum = combined_costs.get(identity, Dictionary());
		sum["identity"] = identity;
		sum["geometry_bytes"] = int64_t(sum.get("geometry_bytes", 0)) + int64_t(cost["geometry_bytes"]);
		sum["texture_bytes"] = int64_t(sum.get("texture_bytes", 0)) + int64_t(cost["texture_bytes"]);
		combined_costs[identity] = sum;
	}
	output["costs"] = combined_costs.values();
	output["dependencies"] = closure;
	output["dependency_graph_version"] = 1;
	output["dependency_graph"] = graph;
	output["layouts"] = layouts;
	output["layout_valid"] = layout_valid;
	return OK;
}
} // namespace CookerStreaming
/*>>----- VEYA_COOKER */
