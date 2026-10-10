#include "character_actions.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "core/templates/hash_map.h"
#include "scene/3d/bone_attachment_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/resources/3d/skin.h"
#include "scene/resources/mesh.h"

namespace CookerCharacter {
namespace {
struct RigBone {
  String name;
  int parent = -1;
  int source = -1;
  int twist_source = -1;
  Vector3 segment_start;
  Vector3 segment_end;
  Transform3D rest;
};

struct Influence {
  int bone = 0;
  float weight = 0;
};

Skeleton3D* FindSkeleton(Node* node) {
  if (auto* skeleton = Object::cast_to<Skeleton3D>(node)) {
    return skeleton;
  }
  for (int i = 0; i < node->get_child_count(); ++i) {
    if (auto* skeleton = FindSkeleton(node->get_child(i))) {
      return skeleton;
    }
  }
  return nullptr;
}

Error CollectMeshes(Node* node, Vector<MeshInstance3D*>& meshes,
                    int& visited) {
  ERR_FAIL_COND_V(++visited > 512, ERR_PARAMETER_RANGE_ERROR);
  if (auto* mesh = Object::cast_to<MeshInstance3D>(node)) {
    if (mesh->get_skin().is_valid()) {
      ERR_FAIL_COND_V(meshes.size() >= 32, ERR_PARAMETER_RANGE_ERROR);
      meshes.push_back(mesh);
    }
  }
  // Numeric attachment indexes cannot survive a source-bone prune.
  ERR_FAIL_COND_V(Object::cast_to<BoneAttachment3D>(node), ERR_UNAVAILABLE);
  for (int i = 0; i < node->get_child_count(); ++i) {
    const Error error = CollectMeshes(node->get_child(i), meshes, visited);
    ERR_FAIL_COND_V(error != OK, error);
  }
  return OK;
}

Error ReadBones(Skeleton3D* skeleton, Skeleton3D* reference,
                const Array& declarations,
                Vector<RigBone>& bones, Vector<int>& remap) {
  ERR_FAIL_COND_V(declarations.is_empty() || declarations.size() > 128,
                  ERR_PARAMETER_RANGE_ERROR);
  HashMap<String, int> indexes;
  for (const Variant& value : declarations) {
    ERR_FAIL_COND_V(value.get_type() != Variant::DICTIONARY,
                    ERR_INVALID_PARAMETER);
    const Dictionary row = value;
    RigBone bone;
    bone.name = row.get("name", "");
    ERR_FAIL_COND_V(bone.name.is_empty() || bone.name.length() > 96 ||
                        bone.name.contains(":") || indexes.has(bone.name),
                    ERR_INVALID_DATA);
    const String parent = row.get("parent", "");
    if (!parent.is_empty()) {
      ERR_FAIL_COND_V(!indexes.has(parent), ERR_INVALID_DATA);
      bone.parent = indexes[parent];
    }
    bone.source = skeleton->find_bone(bone.name);
    if (bone.source >= 0) {
      bone.rest = skeleton->get_bone_global_rest(bone.source);
    } else if (row.has("copy_from")) {
      const String from = row.get("copy_from", "");
      const String toward = row.get("toward", "");
      ERR_FAIL_COND_V(!indexes.has(from) || !indexes.has(toward),
                      ERR_INVALID_DATA);
      const double fraction = row.get("fraction", 0.5);
      ERR_FAIL_COND_V(!std::isfinite(fraction) || fraction <= 0 || fraction >= 1,
                      ERR_INVALID_PARAMETER);
      bone.twist_source = indexes[from];
      bone.rest = bones[bone.twist_source].rest;
      bone.segment_start = bone.rest.origin;
      bone.segment_end = bones[indexes[toward]].rest.origin;
      ERR_FAIL_COND_V(bone.segment_start.distance_to(bone.segment_end) < 0.001,
                      ERR_INVALID_DATA);
      bone.rest.origin = bone.segment_start.lerp(bone.segment_end, fraction);
    } else if (!row.has("offset") && reference &&
               reference->find_bone(bone.name) >= 0) {
      // Missing fingers inherit the target hand's rest space, not the source
      // model's world-space position. Limb proportions scale their offsets.
      int anchor = bone.parent;
      while (anchor >= 0 && bones[anchor].source < 0) {
        anchor = bones[anchor].parent;
      }
      ERR_FAIL_COND_V(anchor < 0, ERR_INVALID_DATA);
      const int reference_anchor = reference->find_bone(bones[anchor].name);
      const int reference_bone = reference->find_bone(bone.name);
      ERR_FAIL_COND_V(reference_anchor < 0, ERR_INVALID_DATA);
      Transform3D relative =
          reference->get_bone_global_rest(reference_anchor).affine_inverse() *
          reference->get_bone_global_rest(reference_bone);
      const int target_parent = skeleton->get_bone_parent(bones[anchor].source);
      const int reference_parent = reference->get_bone_parent(reference_anchor);
      double ratio = 1;
      if (target_parent >= 0 && reference_parent >= 0) {
        const double target_length = bones[anchor].rest.origin.distance_to(
            skeleton->get_bone_global_rest(target_parent).origin);
        const double reference_length =
            reference->get_bone_global_rest(reference_anchor).origin.distance_to(
                reference->get_bone_global_rest(reference_parent).origin);
        ERR_FAIL_COND_V(reference_length < 0.001, ERR_INVALID_DATA);
        ratio = target_length / reference_length;
      }
      ERR_FAIL_COND_V(!std::isfinite(ratio) || ratio < 0.25 || ratio > 4,
                      ERR_INVALID_DATA);
      relative.origin *= ratio;
      bone.rest = bones[anchor].rest * relative;
    } else {
      const Array offset = row.get("offset", Array());
      ERR_FAIL_COND_V(offset.size() != 3, ERR_INVALID_DATA);
      const Vector3 position(offset[0], offset[1], offset[2]);
      ERR_FAIL_COND_V(!position.is_finite(), ERR_INVALID_DATA);
      bone.rest = bone.parent < 0 ? Transform3D() : bones[bone.parent].rest;
      bone.rest.origin = bone.rest.xform(position);
    }
    indexes.insert(bone.name, bones.size());
    bones.push_back(bone);
  }
  remap.resize(skeleton->get_bone_count());
  for (int source = 0; source < remap.size(); ++source) {
    int ancestor = source;
    while (ancestor >= 0 && !indexes.has(skeleton->get_bone_name(ancestor))) {
      ancestor = skeleton->get_bone_parent(ancestor);
    }
    ERR_FAIL_COND_V(ancestor < 0, ERR_INVALID_DATA);
    remap.write[source] = indexes[skeleton->get_bone_name(ancestor)];
  }
  return OK;
}

void AddInfluence(std::array<Influence, 16>& entries, int& count, int bone,
                  float weight) {
  if (weight <= 0) {
    return;
  }
  for (int i = 0; i < count; ++i) {
    if (entries[i].bone == bone) {
      entries[i].weight += weight;
      return;
    }
  }
  entries[count++] = {bone, weight};
}

Error RebindMesh(MeshInstance3D* instance, Skeleton3D* skeleton,
                 const Vector<RigBone>& bones, const Vector<int>& remap,
                 uint64_t& vertex_count, uint64_t& twist_vertices,
                 uint64_t& finger_vertices) {
  const Ref<ArrayMesh> source = instance->get_mesh();
  const Ref<Skin> source_skin = instance->get_skin();
  ERR_FAIL_COND_V(source.is_null() || source_skin.is_null() ||
                      source_skin->get_bind_count() == 0 ||
                      source->get_shadow_mesh().is_valid(),
                  ERR_INVALID_DATA);
  Vector<int> bind_bones;
  bind_bones.resize(source_skin->get_bind_count());
  Transform3D mesh_to_skeleton;
  for (int bind = 0; bind < bind_bones.size(); ++bind) {
    const StringName name = source_skin->get_bind_name(bind);
    const int source_bone = name == StringName()
                                ? source_skin->get_bind_bone(bind)
                                : skeleton->find_bone(name);
    ERR_FAIL_COND_V(source_bone < 0 || source_bone >= remap.size(),
                    ERR_INVALID_DATA);
    bind_bones.write[bind] = remap[source_bone];
    const Transform3D bind_space = skeleton->get_bone_global_rest(source_bone) *
                                   source_skin->get_bind_pose(bind);
    if (bind == 0) {
      mesh_to_skeleton = bind_space;
    } else {
      ERR_FAIL_COND_V(!bind_space.is_equal_approx(mesh_to_skeleton),
                      ERR_INVALID_DATA);
    }
  }
  Ref<ArrayMesh> mesh;
  mesh.instantiate();
  mesh->set_blend_shape_mode(source->get_blend_shape_mode());
  for (int i = 0; i < source->get_blend_shape_count(); ++i) {
    mesh->add_blend_shape(source->get_blend_shape_name(i));
  }
  for (int surface = 0; surface < source->get_surface_count(); ++surface) {
    Array arrays = source->surface_get_arrays(surface);
    const PackedVector3Array positions = arrays[Mesh::ARRAY_VERTEX];
    PackedInt32Array skin_bones = arrays[Mesh::ARRAY_BONES];
    PackedFloat32Array skin_weights = arrays[Mesh::ARRAY_WEIGHTS];
    ERR_FAIL_COND_V(positions.is_empty() || skin_bones.size() != skin_weights.size(),
                    ERR_INVALID_DATA);
    const int stride = skin_bones.size() / positions.size();
    ERR_FAIL_COND_V((stride != 4 && stride != 8) ||
                        skin_bones.size() != stride * positions.size() ||
                        vertex_count + positions.size() > 1000000,
                    ERR_PARAMETER_RANGE_ERROR);
    vertex_count += positions.size();
    for (int vertex = 0; vertex < positions.size(); ++vertex) {
      std::array<Influence, 16> influences{};
      int count = 0;
      bool has_twist = false;
      bool has_fingers = false;
      const Vector3 point = mesh_to_skeleton.xform(positions[vertex]);
      for (int slot = 0; slot < stride; ++slot) {
        const int bind = skin_bones[vertex * stride + slot];
        const float weight = skin_weights[vertex * stride + slot];
        ERR_FAIL_COND_V(bind < 0 || bind >= bind_bones.size() ||
                            !std::isfinite(weight) || weight < 0,
                        ERR_INVALID_DATA);
        const int mapped = bind_bones[bind];
        const String& mapped_name = bones[mapped].name;
        if (mapped_name == "LeftHand" || mapped_name == "RightHand") {
          int closest = -1;
          double closest_distance = 1e20;
          double closest_phase = 0;
          for (int finger = 0; finger < bones.size(); ++finger) {
            const RigBone& candidate = bones[finger];
            if (candidate.source >= 0 || candidate.parent < 0 ||
                !candidate.name.begins_with(mapped_name.trim_suffix("Hand")) ||
                (!candidate.name.ends_with("Metacarpal") &&
                 !candidate.name.ends_with("Proximal") &&
                 !candidate.name.ends_with("Intermediate") &&
                 !candidate.name.ends_with("Distal"))) {
              continue;
            }
            Vector3 end = candidate.rest.origin +
                          (candidate.rest.origin -
                           bones[candidate.parent].rest.origin) * 0.7;
            for (const RigBone& child : bones) {
              if (child.parent == finger) {
                end = child.rest.origin;
                break;
              }
            }
            const Vector3 segment = end - candidate.rest.origin;
            if (segment.length_squared() < 1e-8) {
              continue;
            }
            const double phase = CLAMP(
                (point - candidate.rest.origin).dot(segment) /
                    segment.length_squared(), 0.0, 1.0);
            const double distance = point.distance_squared_to(
                candidate.rest.origin + segment * phase);
            if (distance < closest_distance) {
              closest = finger;
              closest_distance = distance;
              closest_phase = phase;
            }
          }
          const double hand_distance = point.distance_to(bones[mapped].rest.origin);
          if (closest >= 0 && hand_distance > 0.001) {
            double amount = CLAMP(
                1 - Math::sqrt(closest_distance) / hand_distance, 0.0, 1.0);
            // Keep the palm on Hand. Only the knuckle band transitions to
            // finger joints; a nearby folded finger must not pull the wrist.
            if (!bones[closest].name.contains("Thumb")) {
              Vector3 knuckles;
              int knuckle_count = 0;
              for (const RigBone& candidate : bones) {
                if (candidate.parent == mapped &&
                    candidate.name.ends_with("Proximal") &&
                    !candidate.name.contains("Thumb")) {
                  knuckles += candidate.rest.origin;
                  ++knuckle_count;
                }
              }
              if (knuckle_count > 0) {
                const Vector3 palm = knuckles / knuckle_count -
                                     bones[mapped].rest.origin;
                const double length_squared = palm.length_squared();
                if (length_squared > 1e-8) {
                  const double phase =
                      (point - bones[mapped].rest.origin).dot(palm) /
                      length_squared;
                  amount *= CLAMP((phase - .65) / .35, 0.0, 1.0);
                }
              }
            }
            if (bones[closest].parent == mapped) {
              amount *= closest_phase;
            }
            const float finger_weight = weight * float(amount * 0.9);
            const float parent_weight = bones[closest].parent == mapped
                ? 0 : finger_weight * float(CLAMP(.25 - closest_phase, 0.0, .25));
            AddInfluence(influences, count, closest, finger_weight - parent_weight);
            AddInfluence(influences, count, bones[closest].parent, parent_weight);
            AddInfluence(influences, count, mapped, weight - finger_weight);
            has_fingers |= finger_weight > 0.0001;
            continue;
          }
        }
        float twist_weight = 0;
        for (int twist = 0; twist < bones.size(); ++twist) {
          const RigBone& candidate = bones[twist];
          if (candidate.twist_source != mapped) {
            continue;
          }
          const Vector3 segment = candidate.segment_end - candidate.segment_start;
          const double along = (point - candidate.segment_start).dot(segment) /
                               segment.length_squared();
          twist_weight = weight * 0.5f * float(CLAMP(along, 0.0, 1.0));
          AddInfluence(influences, count, twist, twist_weight);
          has_twist |= twist_weight > 0;
          break;
        }
        AddInfluence(influences, count, mapped, weight - twist_weight);
      }
      std::sort(influences.begin(), influences.begin() + count,
                [](const Influence& left, const Influence& right) {
                  return left.weight == right.weight ? left.bone < right.bone
                                                      : left.weight > right.weight;
                });
      float sum = 0;
      for (int slot = 0; slot < MIN(stride, count); ++slot) {
        sum += influences[slot].weight;
      }
      ERR_FAIL_COND_V(sum <= 0, ERR_INVALID_DATA);
      for (int slot = 0; slot < stride; ++slot) {
        skin_bones.set(vertex * stride + slot,
                       slot < count ? influences[slot].bone : 0);
        skin_weights.set(vertex * stride + slot,
                         slot < count ? influences[slot].weight / sum : 0);
      }
      twist_vertices += has_twist;
      finger_vertices += has_fingers;
    }
    arrays[Mesh::ARRAY_BONES] = skin_bones;
    arrays[Mesh::ARRAY_WEIGHTS] = skin_weights;
    const uint64_t flags = stride == 8 ? Mesh::ARRAY_FLAG_USE_8_BONE_WEIGHTS : 0;
    mesh->add_surface_from_arrays(source->surface_get_primitive_type(surface),
                                 arrays, source->surface_get_blend_shape_arrays(surface),
                                 source->surface_get_lods(surface), flags);
    mesh->surface_set_material(surface, source->surface_get_material(surface));
    mesh->surface_set_name(surface, source->surface_get_name(surface));
  }
  Ref<Skin> skin;
  skin.instantiate();
  for (int bone = 0; bone < bones.size(); ++bone) {
    skin->add_named_bind(bones[bone].name,
                         bones[bone].rest.affine_inverse() * mesh_to_skeleton);
  }
  instance->set_mesh(mesh);
  instance->set_skin(skin);
  return OK;
}
}  // namespace

Error PrepareRig(Node* root, const Array& declarations, Dictionary& report,
                 Node* reference_root) {
  Skeleton3D* skeleton = FindSkeleton(root);
  ERR_FAIL_NULL_V(skeleton, ERR_INVALID_DATA);
  const int source_count = skeleton->get_bone_count();
  ERR_FAIL_COND_V(source_count <= 0 || source_count > 256, ERR_INVALID_DATA);
  Vector<RigBone> bones;
  Vector<int> remap;
  Skeleton3D* reference = reference_root ? FindSkeleton(reference_root) : nullptr;
  ERR_FAIL_COND_V(reference_root && !reference, ERR_INVALID_DATA);
  Error error = ReadBones(skeleton, reference, declarations, bones, remap);
  ERR_FAIL_COND_V(error != OK, error);
  Vector<MeshInstance3D*> meshes;
  int visited = 0;
  error = CollectMeshes(root, meshes, visited);
  ERR_FAIL_COND_V(error != OK || meshes.is_empty(), ERR_INVALID_DATA);
  uint64_t vertices = 0;
  uint64_t twist_vertices = 0;
  uint64_t finger_vertices = 0;
  for (auto* mesh : meshes) {
    error = RebindMesh(mesh, skeleton, bones, remap, vertices, twist_vertices,
                       finger_vertices);
    ERR_FAIL_COND_V(error != OK, error);
  }
  skeleton->clear_bones();
  Array names;
  for (int bone = 0; bone < bones.size(); ++bone) {
    const RigBone& row = bones[bone];
    skeleton->add_bone(row.name);
    skeleton->set_bone_parent(bone, row.parent);
    skeleton->set_bone_rest(bone, row.parent < 0
                                    ? row.rest
                                    : bones[row.parent].rest.affine_inverse() * row.rest);
    names.push_back(row.name);
  }
  skeleton->reset_bone_poses();
  report["source_bones"] = source_count;
  report["runtime_bones"] = bones.size();
  report["meshes"] = meshes.size();
  report["vertices"] = vertices;
  report["twist_weighted_vertices"] = twist_vertices;
  report["finger_weighted_vertices"] = finger_vertices;
  report["bone_names"] = names;
  root->set_meta("character_rig_bones", names);
  root->set_meta("character_rig_report", report);
  return OK;
}
}  // namespace CookerCharacter
