#ifndef VIRTUAL_GEOMETRY_CLUSTER_CODEC_H
#define VIRTUAL_GEOMETRY_CLUSTER_CODEC_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace VirtualGeometry {

constexpr uint32_t MAX_CLUSTER_TRIANGLES = 128;
constexpr uint32_t MAX_CLUSTER_VERTICES = 256;
constexpr uint32_t MAX_CLUSTER_MATERIALS = 64;
constexpr int32_t MIN_POSITION_PRECISION = -20;
constexpr int32_t MAX_POSITION_PRECISION = 43;
constexpr uint32_t MAX_POSITION_BITS = 21;

struct ClusterMaterialRange {
	uint32_t start = 0;
	uint32_t count = 0;
	// A six-bit palette index, not a truncated model material identity.
	uint32_t material = 0;
};

struct PackedClusterMaterials {
	uint32_t info = 0;
	std::vector<uint32_t> table;
};

enum class MaterialCodecError {
	OK,
	INVALID_TRIANGLE_COUNT,
	INVALID_RANGES,
	INVALID_TABLE,
	TABLE_OFFSET_LIMIT,
	MEMORY,
};

// Ranges cover 1..128 triangles, ordered by descending count then material ID.
// Three ranges fit inline; more use a 19-bit word offset and a six-bit count.
// The table output contains this cluster's words; p_table_offset places those
// words within the eventual page table. Decode borrows that complete table.
// No native struct layout is serialized. Refusal preserves the old output.
MaterialCodecError pack_cluster_materials(const std::vector<ClusterMaterialRange> &p_ranges, uint32_t p_triangles,
		uint32_t p_table_offset, PackedClusterMaterials &r_packed);
MaterialCodecError unpack_cluster_materials(uint32_t p_info, const std::vector<uint32_t> &p_table,
		uint32_t p_triangles, std::vector<ClusterMaterialRange> &r_ranges);

struct ClusterPositionLayout {
	int32_t precision = 0;
	std::array<int32_t, 3> origin = {};
	std::array<uint32_t, 3> bits = {};
	uint32_t vertices = 0;
};

struct QuantizedClusterPositions {
	ClusterPositionLayout layout;
	std::vector<std::array<uint32_t, 3>> values;
};

enum class PositionCodecError {
	OK,
	INVALID_LAYOUT,
	NON_FINITE,
	COORDINATE_RANGE,
	INVALID_VALUES,
	INVALID_LENGTH,
	INVALID_PADDING,
	MEMORY,
};

// All clusters of a model must use one precision. Float32 scaling, adding 0.5f
// and floor match the reference's grid rounding. Values are relative
// to each cluster's signed origin, with at most 21 bits per axis.
PositionCodecError quantize_cluster_positions(const std::vector<std::array<float, 3>> &p_positions,
		int32_t p_precision, QuantizedClusterPositions &r_positions);
// Payload is vertex-major X/Y/Z, least-significant bit first. Zero-bit axes
// occupy no payload; final high padding bits must be zero. Layout is separate.
PositionCodecError pack_cluster_positions(const QuantizedClusterPositions &p_positions, std::vector<uint8_t> &r_bytes);
PositionCodecError unpack_cluster_positions(const ClusterPositionLayout &p_layout, const uint8_t *p_bytes,
		size_t p_size, QuantizedClusterPositions &r_positions);
PositionCodecError reconstruct_cluster_positions(const QuantizedClusterPositions &p_positions,
		std::vector<std::array<float, 3>> &r_positions);

} // namespace VirtualGeometry

#endif // VIRTUAL_GEOMETRY_CLUSTER_CODEC_H
