#include "character_actions.h"

#include "asset_files.h"
#include "atomic_save.h"
#include "core/io/file_access.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"

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
      for (int surface = 0; surface < source.surfaces.size(); ++surface) {
        if (fields.size() == 2 && fields[1] == source.surfaces[surface].group) {
          selected = surface;
          break;
        }
      }
    } else if (fields[0] == "f" && selected >= 0) {
      ERR_FAIL_COND_V(fields.size() < 4 || fields.size() > 5, ERR_FILE_CORRUPT);
      FaceCorner corners[4];
      bool inside = true;
      for (int i = 1; i < fields.size(); ++i) {
        const Error error = ReadCorner(fields[i], source, corners[i - 1]);
        ERR_FAIL_COND_V(error != OK, error);
        inside &= source.positions[corners[i - 1].source].y >=
                  source.surfaces[selected].minimum_y;
      }
      if (!inside) {
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
}  // namespace CookerCharacter
