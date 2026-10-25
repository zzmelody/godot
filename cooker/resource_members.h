#pragma once

#include <cstring>
#include <limits>

#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

namespace CookerResourceMembers {

// Cooked presentation data. The gameplay resource index remains independent.
// Columns retain all authored matrices; hidden stage-one render matrices are
// stored only in MultiMesh, never in this source-pose column.
struct Columns {
  PackedByteArray id_bytes;
  PackedInt32Array id_offsets;
  PackedStringArray kind_names;
  PackedInt32Array kind_indices;
  PackedInt32Array indices;
  PackedByteArray stages;
  PackedVector3Array poses;
  PackedVector3Array ground_pivots;

  int64_t Size() const {
    return indices.size();
  }

  Transform3D Pose(int64_t row) const {
    const Vector3* values = poses.ptr() + row * 4;
    return Transform3D(Basis(values[0], values[1], values[2]), values[3]);
  }

  Error Read(const Variant& encoded, int64_t instance_count) {
    if (encoded.get_type() != Variant::DICTIONARY) {
      return ERR_INVALID_DATA;
    }
    const Dictionary block = encoded;
    const Variant version = block.get("format", Variant());
    const Variant bytes = block.get("id_bytes", Variant());
    const Variant offsets = block.get("id_offsets", Variant());
    const Variant names = block.get("kind_names", Variant());
    const Variant kinds = block.get("kind_indices", Variant());
    const Variant member_indices = block.get("indices", Variant());
    const Variant member_stages = block.get("stages", Variant());
    const Variant member_poses = block.get("poses", Variant());
    const Variant pivots = block.get("ground_pivots", Variant());
    if (version.get_type() != Variant::INT || static_cast<int64_t>(version) != 2 ||
        bytes.get_type() != Variant::PACKED_BYTE_ARRAY ||
        offsets.get_type() != Variant::PACKED_INT32_ARRAY ||
        names.get_type() != Variant::PACKED_STRING_ARRAY ||
        kinds.get_type() != Variant::PACKED_INT32_ARRAY ||
        member_indices.get_type() != Variant::PACKED_INT32_ARRAY ||
        member_stages.get_type() != Variant::PACKED_BYTE_ARRAY ||
        member_poses.get_type() != Variant::PACKED_VECTOR3_ARRAY ||
        pivots.get_type() != Variant::PACKED_VECTOR3_ARRAY) {
      return ERR_INVALID_DATA;
    }
    id_bytes = bytes;
    id_offsets = offsets;
    kind_names = names;
    kind_indices = kinds;
    indices = member_indices;
    stages = member_stages;
    poses = member_poses;
    ground_pivots = pivots;
    const int64_t count = Size();
    if (instance_count < 0 || count > instance_count ||
        count > std::numeric_limits<int32_t>::max() / 4 ||
        id_bytes.size() > std::numeric_limits<int32_t>::max() ||
        id_offsets.size() != count + 1 || kind_indices.size() != count ||
        stages.size() != count || poses.size() != count * 4 ||
        ground_pivots.size() != count || kind_names.size() > count ||
        id_offsets[0] != 0 || id_offsets[count] != id_bytes.size()) {
      return ERR_INVALID_DATA;
    }
    for (const String& kind : kind_names) {
      if (kind.is_empty() || kind.length() > 48 ||
          !kind.is_valid_identifier()) {
        return ERR_INVALID_DATA;
      }
    }
    const uint8_t* data = id_bytes.ptr();
    int32_t previous = -1;
    for (int64_t row = 0; row < count; ++row) {
      const int64_t first = id_offsets[row];
      const int64_t last = id_offsets[row + 1];
      if (first < 0 || last <= first || last > id_bytes.size() ||
          last - first > 384 || std::memchr(data + first, 0, last - first)) {
        return ERR_INVALID_DATA;
      }
      String identity;
      if (identity.append_utf8(reinterpret_cast<const char*>(data + first),
                               static_cast<int>(last - first)) != OK ||
          identity.length() > 96 || !identity.is_valid_identifier()) {
        return ERR_INVALID_DATA;
      }
      if (indices[row] <= previous || indices[row] >= instance_count ||
          stages[row] > 1 || kind_indices[row] < 0 ||
          kind_indices[row] >= kind_names.size() || !Pose(row).is_finite() ||
          !Math::is_finite(Pose(row).basis.determinant()) ||
          Pose(row).basis.determinant() == 0 ||
          !ground_pivots[row].is_finite()) {
        return ERR_INVALID_DATA;
      }
      previous = indices[row];
    }
    return OK;
  }

  Error Append(const String& identity, const String& kind, int32_t index,
               uint8_t stage, const Transform3D& pose, const Vector3& pivot) {
    const CharString bytes = identity.utf8();
    return AppendBytes(reinterpret_cast<const uint8_t*>(bytes.get_data()),
                       bytes.length(), kind, index, stage, pose, pivot);
  }

  Error AppendTransformed(const Columns& source, int64_t row, int32_t first,
                          const Transform3D& transform) {
    const int32_t begin = source.id_offsets[row];
    return AppendBytes(source.id_bytes.ptr() + begin,
                       source.id_offsets[row + 1] - begin,
                       source.kind_names[source.kind_indices[row]],
                       first + source.indices[row], source.stages[row],
                       transform * source.Pose(row),
                       transform.xform(source.ground_pivots[row]));
  }

  Dictionary Encode() const {
    Dictionary block;
    block["format"] = 2;
    block["id_bytes"] = id_bytes;
    if (id_offsets.is_empty()) {
      PackedInt32Array empty_offsets;
      empty_offsets.push_back(0);
      block["id_offsets"] = empty_offsets;
    } else {
      block["id_offsets"] = id_offsets;
    }
    block["kind_names"] = kind_names;
    block["kind_indices"] = kind_indices;
    block["indices"] = indices;
    block["stages"] = stages;
    block["poses"] = poses;
    block["ground_pivots"] = ground_pivots;
    return block;
  }

 private:
  Error AppendBytes(const uint8_t* data, int64_t bytes, const String& kind,
                    int32_t index, uint8_t stage, const Transform3D& pose,
                    const Vector3& pivot) {
    const int64_t count = Size();
    if (count >= std::numeric_limits<int32_t>::max() / 4 || bytes <= 0 ||
        bytes > 384 ||
        id_bytes.size() > std::numeric_limits<int32_t>::max() - bytes ||
        index < 0 || (count && index <= indices[count - 1]) || stage > 1 ||
        !pose.is_finite() || !Math::is_finite(pose.basis.determinant()) ||
        pose.basis.determinant() == 0 || !pivot.is_finite()) {
      return ERR_INVALID_DATA;
    }
    int64_t kind_index = kind_names.find(kind);
    if (kind_index < 0) {
      kind_index = kind_names.size();
      if (kind_names.push_back(kind)) {
        return ERR_OUT_OF_MEMORY;
      }
    }
    const int64_t begin = id_bytes.size();
    if (id_bytes.resize(begin + bytes) != OK ||
        id_offsets.resize(count + 2) != OK ||
        kind_indices.resize(count + 1) != OK ||
        indices.resize(count + 1) != OK || stages.resize(count + 1) != OK ||
        poses.resize((count + 1) * 4) != OK ||
        ground_pivots.resize(count + 1) != OK) {
      return ERR_OUT_OF_MEMORY;
    }
    std::memcpy(id_bytes.ptrw() + begin, data, static_cast<size_t>(bytes));
    id_offsets.set(count, static_cast<int32_t>(begin));
    id_offsets.set(count + 1, static_cast<int32_t>(begin + bytes));
    kind_indices.set(count, static_cast<int32_t>(kind_index));
    indices.set(count, index);
    stages.set(count, stage);
    for (int column = 0; column < 3; ++column) {
      poses.set(count * 4 + column, pose.basis.get_column(column));
    }
    poses.set(count * 4 + 3, pose.origin);
    ground_pivots.set(count, pivot);
    return OK;
  }
};

}  // namespace CookerResourceMembers
