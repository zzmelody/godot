#ifndef VIRTUAL_GEOMETRY_ATTRIBUTE_CODEC_H
#define VIRTUAL_GEOMETRY_ATTRIBUTE_CODEC_H

#include "cluster_codec.h"

namespace VirtualGeometry {

struct ClusterAttributeFormat {
	bool has_tangents = false;
	bool has_colors = false;
	uint32_t uv_sets = 0;
	bool lossless_uvs = false;
	uint32_t normal_bits = 8;
	uint32_t tangent_bits = 7;

	size_t scalar_count() const { return 3 + (has_tangents ? 4 : 0) + (has_colors ? 4 : 0) + static_cast<size_t>(uv_sets) * 2; }
};

struct ClusterAttributeLayout {
	ClusterAttributeFormat format;
	uint32_t vertices = 0;
	std::array<uint32_t, 4> color_min = { 255, 255, 255, 255 };
	std::array<uint32_t, 4> color_bits = {};
	std::array<std::array<uint32_t, 2>, 4> uv_min = {};
	std::array<std::array<uint32_t, 2>, 4> uv_bits = {};
};

struct AttributeEncodingError {
	double normal_radians = 0;
	double tangent_radians = 0;
	double color_absolute = 0;
	double uv_absolute = 0;
	double uv_relative = 0;
};

struct PackedClusterAttributes {
	ClusterAttributeLayout layout;
	std::vector<uint8_t> bytes;
	// Offline diagnostics; these are not part of the vertex bit stream.
	AttributeEncodingError errors;
};

enum class AttributeCodecError {
	OK,
	INVALID_LAYOUT,
	INVALID_VALUES,
	NON_FINITE,
	INVALID_NORMAL,
	INVALID_TANGENT,
	UV_RANGE,
	INVALID_LENGTH,
	INVALID_PADDING,
	MEMORY,
};

struct AttributeCodecResult {
	AttributeCodecError error = AttributeCodecError::OK;
	uint32_t vertex = UINT32_MAX;
};

// Sorted UV float codes: five exponent bits, fourteen mantissa bits, reversed
// negative ordering and a uniform [0,1] interval. Refuses saturation beyond the
// finite +/-2147418112 range. Signed zero is preserved. Old output survives.
AttributeCodecError encode_attribute_uv(float p_value, uint32_t &r_code);
AttributeCodecError decode_attribute_uv(uint32_t p_code, float &r_value);

// Vertex-major order: normal3, optional tangent3/sign1, optional RGBA4, UV pairs.
// The payload stores two octahedral normal coordinates, tangent angle/sign,
// relative linear RGBA8, then relative UV float codes (or exact float32 bits).
// Tangents use the decoded normal. Zero/parallel tangent data is refused rather
// than copied from a preceding vertex. Input, layout and old output are owned
// by the caller. Arrays are bounded by 256 vertices and four UV sets.
AttributeCodecResult pack_cluster_attributes(const ClusterAttributeFormat &p_format,
		const std::vector<float> &p_values, uint32_t p_vertices, PackedClusterAttributes &r_attributes);
AttributeCodecResult unpack_cluster_attributes(const ClusterAttributeLayout &p_layout,
		const uint8_t *p_bytes, size_t p_size, std::vector<float> &r_values);
// Checks metadata and only this vertex's values; no unaligned over-fetch.
AttributeCodecResult unpack_cluster_attribute(const ClusterAttributeLayout &p_layout,
		const uint8_t *p_bytes, size_t p_size, uint32_t p_vertex, std::vector<float> &r_values);

} // namespace VirtualGeometry

#endif // VIRTUAL_GEOMETRY_ATTRIBUTE_CODEC_H
