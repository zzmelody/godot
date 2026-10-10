#include "character_actions.h"

#include "asset_files.h"
#include "atomic_save.h"
#include "core/io/file_access.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "scene/3d/bone_attachment_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/resources/3d/skin.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/resources/3d/primitive_meshes.h"

namespace CookerCharacter {
namespace {
constexpr int kMaximumSourceVertices = 100000;
constexpr int kMaximumHeadVertices = 24000;
constexpr int kMaximumChannels = 64;

struct FaceCorner {
  int source = 0;
  int uv = -1;
};

struct FaceSurface {
  String group;
  double minimum_y = -100;
  Color color;
  HashMap<uint64_t, int> indexes;
  Vector<FaceCorner> corners;
  PackedInt32Array triangles;
};

struct FaceSource {
  Vector<Vector3> positions;
  Vector<Vector2> uvs;
  Vector<FaceSurface> surfaces;
  HashSet<int> left_eye;
  HashSet<int> right_eye;
};

Error ReadVector(const Variant& value, Vector3& result) {
  ERR_FAIL_COND_V(value.get_type() != Variant::ARRAY, ERR_INVALID_PARAMETER);
  const Array row = value;
  ERR_FAIL_COND_V(row.size() != 3, ERR_INVALID_PARAMETER);
  for (int i = 0; i < 3; ++i) {
    ERR_FAIL_COND_V(row[i].get_type() != Variant::INT &&
                        row[i].get_type() != Variant::FLOAT,
                    ERR_INVALID_PARAMETER);
    result[i] = row[i];
  }
  ERR_FAIL_COND_V(!result.is_finite(), ERR_INVALID_PARAMETER);
  return OK;
}

Error ReadSurfaces(const Array& rows, FaceSource& source) {
  ERR_FAIL_COND_V(rows.is_empty() || rows.size() > 8, ERR_PARAMETER_RANGE_ERROR);
  HashSet<String> groups;
  for (const Variant& value : rows) {
    ERR_FAIL_COND_V(value.get_type() != Variant::DICTIONARY,
                    ERR_INVALID_PARAMETER);
    const Dictionary row = value;
    FaceSurface surface;
    surface.group = row.get("group", "");
    ERR_FAIL_COND_V(surface.group.is_empty() || groups.has(surface.group),
                    ERR_INVALID_DATA);
    groups.insert(surface.group);
    surface.minimum_y = row.get("minimum_y", -100.0);
    ERR_FAIL_COND_V(!Math::is_finite(surface.minimum_y), ERR_INVALID_PARAMETER);
    Vector3 color;
    const Error error = ReadVector(row.get("color", Variant()), color);
    ERR_FAIL_COND_V(error != OK, error);
    for (int axis = 0; axis < 3; ++axis) {
      ERR_FAIL_COND_V(color[axis] < 0 || color[axis] > 1, ERR_INVALID_PARAMETER);
    }
    surface.color = Color(color.x, color.y, color.z);
    source.surfaces.push_back(surface);
  }
  return OK;
}

Error ReadCorner(const String& token, const FaceSource& source,
                 FaceCorner& corner) {
  const Vector<String> pieces = token.split("/", true);
  ERR_FAIL_COND_V(pieces.is_empty() || !pieces[0].is_valid_int(),
                  ERR_FILE_CORRUPT);
  corner.source = pieces[0].to_int() - 1;
  ERR_FAIL_COND_V(corner.source < 0 || corner.source >= source.positions.size(),
                  ERR_FILE_CORRUPT);
  if (pieces.size() > 1 && !pieces[1].is_empty()) {
    ERR_FAIL_COND_V(!pieces[1].is_valid_int(), ERR_FILE_CORRUPT);
    corner.uv = pieces[1].to_int() - 1;
    ERR_FAIL_COND_V(corner.uv < 0 || corner.uv >= source.uvs.size(),
                    ERR_FILE_CORRUPT);
  }
  return OK;
}

int IndexCorner(FaceSurface& surface, const FaceCorner& corner) {
  const uint64_t key = (uint64_t(corner.source) << 32) |
                       uint32_t(corner.uv + 1);
  if (const int* index = surface.indexes.getptr(key)) {
    return *index;
  }
  const int index = surface.corners.size();
  surface.indexes.insert(key, index);
  surface.corners.push_back(corner);
  return index;
}

Error ReadSource(const String& path, FaceSource& source) {
  Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
  ERR_FAIL_COND_V(file.is_null() || file->get_length() > 16 * 1024 * 1024,
                  ERR_FILE_CANT_READ);
  int selected = -1;
  int landmark = -1;
  while (!file->eof_reached()) {
    const String line = file->get_line().strip_edges();
    if (line.is_empty() || line.begins_with("#")) {
      continue;
    }
    const Vector<String> fields = line.split(" ", false);
    if (fields[0] == "v") {
      ERR_FAIL_COND_V(fields.size() != 4 ||
                          source.positions.size() >= kMaximumSourceVertices,
                      ERR_FILE_CORRUPT);
      Vector3 point;
      for (int axis = 0; axis < 3; ++axis) {
        ERR_FAIL_COND_V(!fields[axis + 1].is_valid_float(), ERR_FILE_CORRUPT);
        point[axis] = fields[axis + 1].to_float();
      }
      ERR_FAIL_COND_V(!point.is_finite(), ERR_FILE_CORRUPT);
      source.positions.push_back(point);
    } else if (fields[0] == "vt") {
      ERR_FAIL_COND_V(fields.size() < 3 || fields.size() > 4 ||
                          source.uvs.size() >= 200000 ||
                          !fields[1].is_valid_float() ||
                          !fields[2].is_valid_float(), ERR_FILE_CORRUPT);
      const Vector2 uv(fields[1].to_float(), 1 - fields[2].to_float());
      ERR_FAIL_COND_V(!uv.is_finite(), ERR_FILE_CORRUPT);
      source.uvs.push_back(uv);
    } else if (fields[0] == "g") {
      selected = -1;
      landmark = -1;
      if (fields.size() == 2 && fields[1] == "joint-l-eye") {
        landmark = 0;
      } else if (fields.size() == 2 && fields[1] == "joint-r-eye") {
        landmark = 1;
      }
      for (int surface = 0; surface < source.surfaces.size(); ++surface) {
        if (fields.size() == 2 && fields[1] == source.surfaces[surface].group) {
          selected = surface;
          break;
        }
      }
    } else if (fields[0] == "f" && (selected >= 0 || landmark >= 0)) {
      ERR_FAIL_COND_V(fields.size() < 4 || fields.size() > 5, ERR_FILE_CORRUPT);
      FaceCorner corners[4];
      bool inside = true;
      for (int i = 1; i < fields.size(); ++i) {
        const Error error = ReadCorner(fields[i], source, corners[i - 1]);
        ERR_FAIL_COND_V(error != OK, error);
        if (landmark >= 0) {
          HashSet<int>& points = landmark == 0 ? source.left_eye : source.right_eye;
          points.insert(corners[i - 1].source);
        } else {
          inside &= source.positions[corners[i - 1].source].y >=
                    source.surfaces[selected].minimum_y;
        }
      }
      if (landmark >= 0 || !inside) {
        continue;
      }
      FaceSurface& surface = source.surfaces.write[selected];
      for (int triangle = 1; triangle + 1 < fields.size() - 1; ++triangle) {
        // OBJ fronts are counter-clockwise; Godot fronts are clockwise.
        surface.triangles.push_back(IndexCorner(surface, corners[0]));
        surface.triangles.push_back(IndexCorner(surface, corners[triangle + 1]));
        surface.triangles.push_back(IndexCorner(surface, corners[triangle]));
      }
      ERR_FAIL_COND_V(surface.corners.size() > kMaximumHeadVertices ||
                          surface.triangles.size() > 120000,
                      ERR_PARAMETER_RANGE_ERROR);
    }
  }
  ERR_FAIL_COND_V(source.positions.is_empty(), ERR_INVALID_DATA);
  for (const FaceSurface& surface : source.surfaces) {
    ERR_FAIL_COND_V(surface.triangles.is_empty(), ERR_INVALID_DATA);
  }
  return OK;
}

Error ReadTarget(const String& path, int vertices,
                 PackedVector3Array& deltas) {
  Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
  ERR_FAIL_COND_V(file.is_null() || file->get_length() > 4 * 1024 * 1024,
                  ERR_FILE_CANT_READ);
  deltas.resize(vertices);
  HashSet<int> seen;
  while (!file->eof_reached()) {
    const String line = file->get_line().strip_edges();
    if (line.is_empty() || line.begins_with("#")) {
      continue;
    }
    const Vector<String> fields = line.split(" ", false);
    ERR_FAIL_COND_V(fields.size() != 4 || !fields[0].is_valid_int(),
                    ERR_FILE_CORRUPT);
    const int index = fields[0].to_int();
    ERR_FAIL_COND_V(index < 0 || index >= vertices || seen.has(index),
                    ERR_FILE_CORRUPT);
    seen.insert(index);
    Vector3 delta;
    for (int axis = 0; axis < 3; ++axis) {
      ERR_FAIL_COND_V(!fields[axis + 1].is_valid_float(), ERR_FILE_CORRUPT);
      delta[axis] = fields[axis + 1].to_float();
    }
    ERR_FAIL_COND_V(!delta.is_finite() || delta.length() > 2, ERR_INVALID_DATA);
    deltas.set(index, delta);
  }
  ERR_FAIL_COND_V(seen.is_empty(), ERR_INVALID_DATA);
  return OK;
}

PackedVector3Array Normals(const PackedVector3Array& points,
                          const PackedInt32Array& indices) {
  PackedVector3Array normals;
  normals.resize(points.size());
  for (int triangle = 0; triangle < indices.size(); triangle += 3) {
    const int a = indices[triangle];
    const int b = indices[triangle + 1];
    const int c = indices[triangle + 2];
    const Vector3 normal = (points[c] - points[a]).cross(points[b] - points[a]);
    normals.set(a, normals[a] + normal);
    normals.set(b, normals[b] + normal);
    normals.set(c, normals[c] + normal);
  }
  for (int i = 0; i < normals.size(); ++i) {
    normals.set(i, normals[i].normalized());
  }
  return normals;
}

Array SurfaceArrays(const FaceSource& source, const FaceSurface& surface,
                    const Vector3& origin, const Vector3& scale) {
  PackedVector3Array points;
  PackedVector2Array uvs;
  points.resize(surface.corners.size());
  uvs.resize(surface.corners.size());
  for (int vertex = 0; vertex < surface.corners.size(); ++vertex) {
    const FaceCorner& corner = surface.corners[vertex];
    points.set(vertex, (source.positions[corner.source] - origin) * scale);
    if (corner.uv >= 0) {
      uvs.set(vertex, source.uvs[corner.uv]);
    }
  }
  Array arrays;
  arrays.resize(Mesh::ARRAY_MAX);
  arrays[Mesh::ARRAY_VERTEX] = points;
  arrays[Mesh::ARRAY_NORMAL] = Normals(points, surface.triangles);
  arrays[Mesh::ARRAY_TEX_UV] = uvs;
  arrays[Mesh::ARRAY_INDEX] = surface.triangles;
  return arrays;
}

Vector3 EyeCenter(const FaceSource& source, const HashSet<int>& vertices,
                  const Vector3& origin, const Vector3& scale) {
  Vector3 center;
  for (const int vertex : vertices) {
    center += source.positions[vertex];
  }
  return (center / vertices.size() - origin) * scale;
}

Error CollectNodes(Node* node, Vector<MeshInstance3D*>& meshes,
                   Skeleton3D*& skeleton, int& visited) {
  ERR_FAIL_COND_V(++visited > 512, ERR_PARAMETER_RANGE_ERROR);
  if (auto* found = Object::cast_to<Skeleton3D>(node)) {
    ERR_FAIL_COND_V(skeleton && found != skeleton, ERR_INVALID_DATA);
    skeleton = found;
  }
  if (auto* mesh = Object::cast_to<MeshInstance3D>(node)) {
    if (mesh->get_skin().is_valid()) {
      ERR_FAIL_COND_V(meshes.size() >= 32, ERR_PARAMETER_RANGE_ERROR);
      meshes.push_back(mesh);
    }
  }
  for (int i = 0; i < node->get_child_count(); ++i) {
    const Error error = CollectNodes(node->get_child(i), meshes, skeleton, visited);
    ERR_FAIL_COND_V(error != OK, error);
  }
  return OK;
}

Error RemoveHead(MeshInstance3D* instance, Skeleton3D* skeleton,
                 int head, double neck_offset, int& removed) {
  const Ref<ArrayMesh> source = instance->get_mesh();
  const Ref<Skin> skin = instance->get_skin();
  ERR_FAIL_COND_V(source.is_null() || skin.is_null(), ERR_INVALID_DATA);
  Vector<bool> head_binds;
  head_binds.resize(skin->get_bind_count());
  Transform3D mesh_to_skeleton;
  for (int bind = 0; bind < head_binds.size(); ++bind) {
    int bone = skin->get_bind_name(bind) == StringName()
                   ? skin->get_bind_bone(bind)
                   : skeleton->find_bone(skin->get_bind_name(bind));
    ERR_FAIL_COND_V(bone < 0 || bone >= skeleton->get_bone_count(), ERR_INVALID_DATA);
    if (bind == 0) {
      mesh_to_skeleton = skeleton->get_bone_global_rest(bone) *
                          skin->get_bind_pose(bind);
    }
    while (bone >= 0 && bone != head) {
      bone = skeleton->get_bone_parent(bone);
    }
    head_binds.write[bind] = bone == head;
  }
  const Transform3D head_space =
      skeleton->get_bone_global_rest(head).affine_inverse() * mesh_to_skeleton;
  Ref<ArrayMesh> mesh;
  mesh.instantiate();
  mesh->set_blend_shape_mode(source->get_blend_shape_mode());
  for (int shape = 0; shape < source->get_blend_shape_count(); ++shape) {
    mesh->add_blend_shape(source->get_blend_shape_name(shape));
  }
  for (int surface = 0; surface < source->get_surface_count(); ++surface) {
    ERR_FAIL_COND_V(source->surface_get_primitive_type(surface) !=
                        Mesh::PRIMITIVE_TRIANGLES, ERR_INVALID_DATA);
    Array arrays = source->surface_get_arrays(surface);
    const PackedVector3Array vertices = arrays[Mesh::ARRAY_VERTEX];
    const PackedInt32Array binds = arrays[Mesh::ARRAY_BONES];
    const PackedFloat32Array weights = arrays[Mesh::ARRAY_WEIGHTS];
    const PackedInt32Array indices = arrays[Mesh::ARRAY_INDEX];
    ERR_FAIL_COND_V(vertices.is_empty() || binds.size() != weights.size() ||
                        binds.size() % vertices.size() != 0 ||
                        indices.is_empty() || indices.size() % 3 != 0,
                    ERR_INVALID_DATA);
    const int stride = binds.size() / vertices.size();
    ERR_FAIL_COND_V(stride != 4 && stride != 8, ERR_INVALID_DATA);
    Vector<bool> discard;
    discard.resize(vertices.size());
    for (int vertex = 0; vertex < vertices.size(); ++vertex) {
      double amount = 0;
      for (int slot = 0; slot < stride; ++slot) {
        const int bind = binds[vertex * stride + slot];
        ERR_FAIL_COND_V(bind < 0 || bind >= head_binds.size(), ERR_INVALID_DATA);
        if (head_binds[bind]) {
          amount += weights[vertex * stride + slot];
        }
      }
      discard.write[vertex] = amount > 0.5 &&
                              head_space.xform(vertices[vertex]).y >= neck_offset;
    }
    PackedInt32Array kept;
    for (int triangle = 0; triangle < indices.size(); triangle += 3) {
      bool erase = true;
      for (int corner = 0; corner < 3; ++corner) {
        const int vertex = indices[triangle + corner];
        ERR_FAIL_COND_V(vertex < 0 || vertex >= discard.size(), ERR_INVALID_DATA);
        erase &= discard[vertex];
      }
      if (erase) {
        ++removed;
      } else {
        for (int corner = 0; corner < 3; ++corner) {
          kept.push_back(indices[triangle + corner]);
        }
      }
    }
    if (kept.is_empty()) {
      continue;
    }
    arrays[Mesh::ARRAY_INDEX] = kept;
    const int output_surface = mesh->get_surface_count();
    mesh->add_surface_from_arrays(
        Mesh::PRIMITIVE_TRIANGLES, arrays,
        source->surface_get_blend_shape_arrays(surface), Dictionary(),
        stride == 8 ? Mesh::ARRAY_FLAG_USE_8_BONE_WEIGHTS : 0);
    mesh->surface_set_material(output_surface, source->surface_get_material(surface));
    mesh->surface_set_name(output_surface, source->surface_get_name(surface));
  }
  instance->set_mesh(mesh);
  return OK;
}

void AddEyePart(Node* owner, Node* attachment, const String& name,
                double radius, double depth, double forward, const Color& color) {
  Ref<SphereMesh> sphere;
  sphere.instantiate();
  sphere->set_radius(radius);
  sphere->set_height(radius * 2);
  sphere->set_radial_segments(24);
  sphere->set_rings(12);
  Ref<StandardMaterial3D> material;
  material.instantiate();
  material->set_albedo(color);
  material->set_roughness(0.35);
  sphere->set_material(material);
  auto* mesh = memnew(MeshInstance3D);
  mesh->set_name(name);
  mesh->set_mesh(sphere);
  mesh->set_position(Vector3(0, 0, forward));
  mesh->set_scale(Vector3(1, 1, depth));
  attachment->add_child(mesh);
  mesh->set_owner(owner);
}
}  // namespace

Error ComposeFace(const Dictionary& job, Dictionary& result) {
  const String source_path = job.get("source", "");
  const String output = job.get("output", "");
  Error error = CookerFiles::check_path(source_path);
  ERR_FAIL_COND_V(error != OK, error);
  error = CookerFiles::check_path(output, true);
  ERR_FAIL_COND_V(error != OK || output.get_extension() != "res",
                  ERR_INVALID_PARAMETER);
  ERR_FAIL_COND_V(job.get("parts", Variant()).get_type() != Variant::ARRAY ||
                      job.get("channels", Variant()).get_type() != Variant::ARRAY,
                  ERR_INVALID_PARAMETER);
  Vector3 origin;
  Vector3 scale;
  error = ReadVector(job.get("origin", Variant()), origin);
  ERR_FAIL_COND_V(error != OK, error);
  error = ReadVector(job.get("scale", Variant()), scale);
  ERR_FAIL_COND_V(error != OK, error);
  for (int axis = 0; axis < 3; ++axis) {
    ERR_FAIL_COND_V(scale[axis] <= 0 || scale[axis] > 2, ERR_INVALID_PARAMETER);
  }
  FaceSource source;
  error = ReadSurfaces(job["parts"], source);
  ERR_FAIL_COND_V(error != OK, error);
  error = ReadSource(source_path, source);
  ERR_FAIL_COND_V(error != OK, error);
  const Array channels = job["channels"];
  ERR_FAIL_COND_V(channels.is_empty() || channels.size() > kMaximumChannels,
                  ERR_PARAMETER_RANGE_ERROR);
  Ref<ArrayMesh> mesh;
  mesh.instantiate();
  mesh->set_blend_shape_mode(Mesh::BLEND_SHAPE_MODE_RELATIVE);
  Vector<Array> arrays;
  Vector<Array> shapes;
  for (const FaceSurface& surface : source.surfaces) {
    arrays.push_back(SurfaceArrays(source, surface, origin, scale));
    shapes.push_back(Array());
  }
  HashSet<String> names;
  Array coverage;
  for (const Variant& value : channels) {
    ERR_FAIL_COND_V(value.get_type() != Variant::DICTIONARY,
                    ERR_INVALID_PARAMETER);
    const Dictionary row = value;
    const String name = row.get("name", "");
    const String target_path = row.get("source", "");
    ERR_FAIL_COND_V(name.is_empty() || name.length() > 64 || names.has(name),
                    ERR_INVALID_DATA);
    names.insert(name);
    error = CookerFiles::check_path(target_path);
    ERR_FAIL_COND_V(error != OK, error);
    PackedVector3Array deltas;
    error = ReadTarget(target_path, source.positions.size(), deltas);
    ERR_FAIL_COND_V(error != OK, error);
    int affected = 0;
    double maximum_delta = 0;
    for (int part = 0; part < source.surfaces.size(); ++part) {
      const FaceSurface& surface = source.surfaces[part];
      const PackedVector3Array base = arrays[part][Mesh::ARRAY_VERTEX];
      const PackedVector3Array base_normals = arrays[part][Mesh::ARRAY_NORMAL];
      PackedVector3Array offsets;
      PackedVector3Array moved = base;
      offsets.resize(base.size());
      for (int vertex = 0; vertex < base.size(); ++vertex) {
        const Vector3 delta = deltas[surface.corners[vertex].source] * scale;
        offsets.set(vertex, delta);
        moved.set(vertex, base[vertex] + delta);
        affected += delta.length_squared() > 1e-12;
        maximum_delta = MAX(maximum_delta, delta.length());
      }
      PackedVector3Array normals = Normals(moved, surface.triangles);
      for (int vertex = 0; vertex < normals.size(); ++vertex) {
        normals.set(vertex, normals[vertex] - base_normals[vertex]);
      }
      Array shape;
      shape.resize(Mesh::ARRAY_MAX);
      shape[Mesh::ARRAY_VERTEX] = offsets;
      shape[Mesh::ARRAY_NORMAL] = normals;
      shapes.write[part].push_back(shape);
    }
    ERR_FAIL_COND_V_MSG(affected == 0, ERR_INVALID_DATA,
                        "Face channel has no selected vertices: " + name);
    mesh->add_blend_shape(name);
    Dictionary channel;
    channel["name"] = name;
    channel["affected_vertices"] = affected;
    channel["maximum_delta_meters"] = maximum_delta;
    coverage.push_back(channel);
  }
  int vertices = 0;
  for (int part = 0; part < source.surfaces.size(); ++part) {
    mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays[part],
                                 shapes[part]);
    Ref<StandardMaterial3D> material;
    material.instantiate();
    material->set_albedo(source.surfaces[part].color);
    material->set_roughness(0.85);
    mesh->surface_set_material(part, material);
    mesh->surface_set_name(part, source.surfaces[part].group);
    vertices += source.surfaces[part].corners.size();
  }
  mesh->set_meta("face_channels", coverage);
  mesh->set_meta("face_source", source_path);
  ERR_FAIL_COND_V(source.left_eye.is_empty() || source.right_eye.is_empty(),
                  ERR_INVALID_DATA);
  Array eyes;
  eyes.push_back(EyeCenter(source, source.left_eye, origin, scale));
  eyes.push_back(EyeCenter(source, source.right_eye, origin, scale));
  mesh->set_meta("eye_centers", eyes);
  error = DirAccess::make_dir_recursive_absolute(
      ProjectSettings::get_singleton()->globalize_path(output.get_base_dir()));
  ERR_FAIL_COND_V(error != OK, error);
  error = CookerAtomicSave::save_resource(
      mesh, output, ResourceSaver::FLAG_COMPRESS | ResourceSaver::FLAG_RELATIVE_PATHS);
  ERR_FAIL_COND_V(error != OK, error);
  result["output"] = output;
  result["vertices"] = vertices;
  result["surfaces"] = source.surfaces.size();
  result["channels"] = coverage;
  return OK;
}

Error AttachFace(Node* root, const Dictionary& configuration,
                  Dictionary& result) {
  const String path = configuration.get("mesh", "");
  Error error = CookerFiles::check_path(path);
  ERR_FAIL_COND_V(error != OK, error);
  const Ref<ArrayMesh> face = ResourceLoader::load(path, "ArrayMesh");
  ERR_FAIL_COND_V(face.is_null() || face->get_blend_shape_count() != 52 ||
                      !face->has_meta("eye_centers"), ERR_INVALID_DATA);
  Vector<MeshInstance3D*> meshes;
  Skeleton3D* skeleton = nullptr;
  int visited = 0;
  error = CollectNodes(root, meshes, skeleton, visited);
  ERR_FAIL_COND_V(error != OK || !skeleton, ERR_INVALID_DATA);
  const int head = skeleton->find_bone("Head");
  ERR_FAIL_COND_V(head < 0, ERR_INVALID_DATA);
  const double neck_offset = configuration.get("neck_offset", -0.07);
  const double eye_radius = configuration.get("eye_radius", 0.0125);
  ERR_FAIL_COND_V(!Math::is_finite(neck_offset) || neck_offset < -0.2 ||
                      neck_offset > 0.1 || !Math::is_finite(eye_radius) ||
                      eye_radius < 0.005 || eye_radius > 0.025,
                  ERR_INVALID_PARAMETER);
  int removed = 0;
  for (auto* mesh : meshes) {
    error = RemoveHead(mesh, skeleton, head, neck_offset, removed);
    ERR_FAIL_COND_V(error != OK, error);
  }
  ERR_FAIL_COND_V(removed == 0, ERR_INVALID_DATA);
  auto* attachment = memnew(BoneAttachment3D);
  attachment->set_name("FaceAttachment");
  skeleton->add_child(attachment);
  attachment->set_owner(root);
  attachment->set_bone_name("Head");
  auto* face_mesh = memnew(MeshInstance3D);
  face_mesh->set_name("FaceMesh");
  face_mesh->set_mesh(face);
  attachment->add_child(face_mesh);
  face_mesh->set_owner(root);
  Vector3 iris;
  error = ReadVector(configuration.get("iris_color", Variant()), iris);
  ERR_FAIL_COND_V(error != OK, error);
  const Array eyes = face->get_meta("eye_centers");
  ERR_FAIL_COND_V(eyes.size() != 2, ERR_INVALID_DATA);
  for (int side = 0; side < 2; ++side) {
    const String name = side == 0 ? "LeftEye" : "RightEye";
    const int bone = skeleton->find_bone(name);
    ERR_FAIL_COND_V(bone < 0 || skeleton->get_bone_parent(bone) != head,
                    ERR_INVALID_DATA);
    const Vector3 center = eyes[side];
    skeleton->set_bone_rest(bone, Transform3D(Basis(), center));
    skeleton->reset_bone_pose(bone);
    auto* eye = memnew(BoneAttachment3D);
    eye->set_name(name + "Attachment");
    skeleton->add_child(eye);
    eye->set_owner(root);
    eye->set_bone_name(name);
    AddEyePart(root, eye, "White", eye_radius, 1, 0, Color(0.91, 0.89, 0.84));
    AddEyePart(root, eye, "Iris", eye_radius * 0.55, 0.12,
               eye_radius * 0.91, Color(iris.x, iris.y, iris.z));
    AddEyePart(root, eye, "Pupil", eye_radius * 0.27, 0.10,
               eye_radius * 1.01, Color(0.025, 0.019, 0.014));
  }
  root->set_meta("character_face", configuration);
  result["face_channels"] = face->get_blend_shape_count();
  result["replaced_head_triangles"] = removed;
  result["eye_centers"] = eyes;
  return OK;
}
}  // namespace CookerCharacter
