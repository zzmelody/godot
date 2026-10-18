#include "attribute_codec.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace VirtualGeometry {
namespace {

using Vector = std::array<double, 3>;
constexpr double PI = 3.14159265358979323846;
constexpr uint32_t UV_MASK = (1u << 20) - 1;
constexpr uint32_t UV_MAGNITUDE_MASK = (1u << 19) - 1;
static_assert(sizeof(float) == sizeof(uint32_t) && std::numeric_limits<float>::is_iec559);

uint32_t float_bits(float p_value) {
	uint32_t result;
	std::memcpy(&result, &p_value, sizeof(result));
	return result;
}

float bits_float(uint32_t p_value) {
	float result;
	std::memcpy(&result, &p_value, sizeof(result));
	return result;
}

uint32_t width(uint32_t p_value) {
	uint32_t bits = 0;
	while (p_value != 0) {
		++bits;
		p_value >>= 1;
	}
	return bits;
}

double dot(const Vector &p_first, const Vector &p_second) {
	return p_first[0] * p_second[0] + p_first[1] * p_second[1] + p_first[2] * p_second[2];
}

bool normalize(Vector &r_vector) {
	const double length = std::hypot(r_vector[0], r_vector[1], r_vector[2]);
	if (!(length > 0) || !std::isfinite(length)) {
		return false;
	}
	for (double &value : r_vector) {
		value /= length;
	}
	return true;
}

Vector cross(const Vector &p_first, const Vector &p_second) {
	return { p_first[1] * p_second[2] - p_first[2] * p_second[1],
		p_first[2] * p_second[0] - p_first[0] * p_second[2],
		p_first[0] * p_second[1] - p_first[1] * p_second[0] };
}

Vector decode_normal(uint32_t p_code, uint32_t p_bits) {
	const uint32_t maximum = (1u << p_bits) - 1;
	// Integer unfolding preserves exact axis ties in both CPU and GPU decoders.
	// The tangent angle depends on the resulting basis, including its tie rule.
	int32_t x = static_cast<int32_t>((p_code & maximum) * 2) - static_cast<int32_t>(maximum);
	int32_t y = static_cast<int32_t>((p_code >> p_bits) * 2) - static_cast<int32_t>(maximum);
	const int32_t z = static_cast<int32_t>(maximum) - std::abs(x) - std::abs(y);
	const int32_t shift = std::max(-z, 0);
	x += x >= 0 ? -shift : shift;
	y += y >= 0 ? -shift : shift;
	Vector result{ static_cast<double>(x), static_cast<double>(y), static_cast<double>(z) };
	normalize(result);
	return result;
}

uint32_t encode_normal(const Vector &p_normal, uint32_t p_bits) {
	const uint32_t maximum = (1u << p_bits) - 1;
	const double divisor = std::abs(p_normal[0]) + std::abs(p_normal[1]) + std::abs(p_normal[2]);
	double x = p_normal[0] / divisor;
	double y = p_normal[1] / divisor;
	if (p_normal[2] < 0) {
		const double old_x = x;
		x = x >= 0 ? 1 - std::abs(y) : std::abs(y) - 1;
		y = y >= 0 ? 1 - std::abs(old_x) : std::abs(old_x) - 1;
	}
	const uint32_t first_x = static_cast<uint32_t>(std::clamp(std::floor((x * 0.5 + 0.5) * maximum), 0.0, static_cast<double>(maximum)));
	const uint32_t first_y = static_cast<uint32_t>(std::clamp(std::floor((y * 0.5 + 0.5) * maximum), 0.0, static_cast<double>(maximum)));
	double best = -std::numeric_limits<double>::infinity();
	uint32_t result = 0;
	for (uint32_t dy = 0; dy < 2; ++dy) {
		for (uint32_t dx = 0; dx < 2; ++dx) {
			if (first_x + dx > maximum || first_y + dy > maximum) {
				continue;
			}
			const uint32_t code = (first_x + dx) | ((first_y + dy) << p_bits);
			const double score = dot(decode_normal(code, p_bits), p_normal);
			if (score > best) {
				best = score;
				result = code;
			}
		}
	}
	return result;
}

void tangent_basis(const Vector &p_normal, Vector &r_x, Vector &r_y, bool &r_swap) {
	Vector normal = p_normal;
	r_swap = std::abs(normal[2]) > std::abs(normal[0]);
	if (r_swap) {
		std::swap(normal[0], normal[2]);
	}
	r_x = { -normal[1], normal[0], 0 };
	normalize(r_x);
	r_y = cross(normal, r_x);
}

bool encode_tangent(const Vector &p_tangent, const Vector &p_normal, uint32_t p_bits, uint32_t &r_code) {
	Vector x;
	Vector y;
	bool swap;
	tangent_basis(p_normal, x, y, swap);
	Vector tangent = p_tangent;
	if (swap) {
		std::swap(tangent[0], tangent[2]);
	}
	const double projected_x = dot(tangent, x);
	const double projected_y = dot(tangent, y);
	if (projected_x * projected_x + projected_y * projected_y < 0.0001) {
		return false;
	}
	double angle = std::atan2(projected_y, projected_x);
	if (angle < PI) {
		angle += 2 * PI;
	}
	const uint32_t rounded = static_cast<uint32_t>(std::floor(angle / (2 * PI) * (1u << p_bits) + 0.5));
	r_code = rounded & ((1u << p_bits) - 1);
	return true;
}

Vector decode_tangent(uint32_t p_code, const Vector &p_normal, uint32_t p_bits) {
	Vector x;
	Vector y;
	bool swap;
	tangent_basis(p_normal, x, y, swap);
	const double angle = static_cast<double>(p_code) / (1u << p_bits) * 2 * PI;
	Vector result;
	for (size_t axis = 0; axis < 3; ++axis) {
		result[axis] = x[axis] * std::cos(angle) + y[axis] * std::sin(angle);
	}
	normalize(result);
	if (swap) {
		std::swap(result[0], result[2]);
	}
	return result;
}

bool valid_format(const ClusterAttributeFormat &p_format, bool p_canonical) {
	return p_format.uv_sets <= 4 && p_format.normal_bits >= 1 && p_format.normal_bits <= 15 &&
			(p_format.has_tangents ? p_format.tangent_bits >= 1 && p_format.tangent_bits <= 12 : p_format.tangent_bits <= 12) &&
			(!p_canonical || ((p_format.has_tangents || p_format.tangent_bits == 0) && (p_format.uv_sets != 0 || !p_format.lossless_uvs)));
}

uint32_t vertex_bits(const ClusterAttributeLayout &p_layout) {
	uint32_t result = p_layout.format.normal_bits * 2 + (p_layout.format.has_tangents ? p_layout.format.tangent_bits + 1 : 0);
	for (uint32_t bits : p_layout.color_bits) {
		result += bits;
	}
	for (uint32_t uv = 0; uv < p_layout.format.uv_sets; ++uv) {
		result += p_layout.uv_bits[uv][0] + p_layout.uv_bits[uv][1];
	}
	return result;
}

bool valid_layout(const ClusterAttributeLayout &p_layout) {
	if (!valid_format(p_layout.format, true) || p_layout.vertices == 0 || p_layout.vertices > MAX_CLUSTER_VERTICES) {
		return false;
	}
	for (size_t channel = 0; channel < 4; ++channel) {
		if (p_layout.color_min[channel] > 255 || p_layout.color_bits[channel] > 8 ||
				(!p_layout.format.has_colors && (p_layout.color_min[channel] != 255 || p_layout.color_bits[channel] != 0))) {
			return false;
		}
	}
	for (size_t uv = 0; uv < 4; ++uv) {
		for (size_t axis = 0; axis < 2; ++axis) {
			const uint32_t bits = p_layout.uv_bits[uv][axis];
			const uint32_t origin = p_layout.uv_min[uv][axis];
			if (uv >= p_layout.format.uv_sets) {
				if (bits != 0 || origin != 0) {
					return false;
				}
			} else if (p_layout.format.lossless_uvs) {
				if (bits != 32 || origin != 0) {
					return false;
				}
			} else if (bits > 20 || origin > UV_MASK) {
				return false;
			}
		}
	}
	return true;
}

AttributeCodecError validate_payload(const ClusterAttributeLayout &p_layout, const uint8_t *p_bytes, size_t p_size) {
	if (!valid_layout(p_layout)) {
		return AttributeCodecError::INVALID_LAYOUT;
	}
	const size_t bits = static_cast<size_t>(vertex_bits(p_layout)) * p_layout.vertices;
	if (p_size != ((bits + 31) / 32) * 4 || (p_size != 0 && p_bytes == nullptr)) {
		return AttributeCodecError::INVALID_LENGTH;
	}
	for (size_t bit = bits; bit < p_size * 8; ++bit) {
		if (((p_bytes[bit / 8] >> (bit % 8)) & 1u) != 0) {
			return AttributeCodecError::INVALID_PADDING;
		}
	}
	return AttributeCodecError::OK;
}

void put_bits(std::vector<uint8_t> &r_bytes, size_t &r_cursor, uint32_t p_value, uint32_t p_bits) {
	for (uint32_t bit = 0; bit < p_bits; ++bit, ++r_cursor) {
		r_bytes[r_cursor / 8] |= static_cast<uint8_t>(((p_value >> bit) & 1u) << (r_cursor % 8));
	}
}

uint32_t get_bits(const uint8_t *p_bytes, size_t &r_cursor, uint32_t p_bits) {
	uint32_t result = 0;
	for (uint32_t bit = 0; bit < p_bits; ++bit, ++r_cursor) {
		result |= static_cast<uint32_t>((p_bytes[r_cursor / 8] >> (r_cursor % 8)) & 1u) << bit;
	}
	return result;
}

AttributeCodecError decode_vertex(const ClusterAttributeLayout &p_layout, const uint8_t *p_bytes,
		uint32_t p_vertex, std::vector<float> &r_values) {
	const auto &format = p_layout.format;
	size_t cursor = static_cast<size_t>(p_vertex) * vertex_bits(p_layout);
	const Vector normal = decode_normal(get_bits(p_bytes, cursor, format.normal_bits * 2), format.normal_bits);
	for (double value : normal) {
		r_values.push_back(static_cast<float>(value));
	}
	if (format.has_tangents) {
		const uint32_t packed = get_bits(p_bytes, cursor, format.tangent_bits + 1);
		const Vector tangent = decode_tangent(packed & ((1u << format.tangent_bits) - 1), normal, format.tangent_bits);
		for (double value : tangent) {
			r_values.push_back(static_cast<float>(value));
		}
		r_values.push_back((packed & (1u << format.tangent_bits)) != 0 ? -1.0f : 1.0f);
	}
	if (format.has_colors) {
		for (size_t channel = 0; channel < 4; ++channel) {
			const uint32_t value = p_layout.color_min[channel] + get_bits(p_bytes, cursor, p_layout.color_bits[channel]);
			if (value > 255) {
				return AttributeCodecError::INVALID_VALUES;
			}
			r_values.push_back(static_cast<float>(value) / 255.0f);
		}
	}
	for (uint32_t uv = 0; uv < format.uv_sets; ++uv) {
		for (size_t axis = 0; axis < 2; ++axis) {
			const uint32_t code = get_bits(p_bytes, cursor, p_layout.uv_bits[uv][axis]);
			float value;
			if (format.lossless_uvs) {
				value = bits_float(code);
				if (!std::isfinite(value)) {
					return AttributeCodecError::NON_FINITE;
				}
			} else {
				const uint32_t absolute = p_layout.uv_min[uv][axis] + code;
				if (absolute > UV_MASK) {
					return AttributeCodecError::INVALID_VALUES;
				}
				decode_attribute_uv(absolute, value);
			}
			r_values.push_back(value);
		}
	}
	return AttributeCodecError::OK;
}

struct AttributeCodes {
	uint32_t normal = 0;
	uint32_t tangent = 0;
	std::array<uint32_t, 4> color = {};
	std::array<std::array<uint32_t, 2>, 4> uv = {};
};

void measure_errors(const ClusterAttributeFormat &p_format, const std::vector<float> &p_source,
		const std::vector<float> &p_decoded, AttributeEncodingError &r_errors) {
	const size_t stride = p_format.scalar_count();
	for (size_t vertex = 0; vertex < p_source.size() / stride; ++vertex) {
		const size_t base = vertex * stride;
		Vector normal{ p_source[base], p_source[base + 1], p_source[base + 2] };
		normalize(normal);
		Vector decoded{ p_decoded[base], p_decoded[base + 1], p_decoded[base + 2] };
		normalize(decoded);
		r_errors.normal_radians = std::max(r_errors.normal_radians, std::acos(std::clamp(dot(normal, decoded), -1.0, 1.0)));
		size_t offset = 3;
		if (p_format.has_tangents) {
			Vector tangent{ p_source[base + 3], p_source[base + 4], p_source[base + 5] };
			normalize(tangent);
			decoded = { p_decoded[base + 3], p_decoded[base + 4], p_decoded[base + 5] };
			normalize(decoded);
			r_errors.tangent_radians = std::max(r_errors.tangent_radians, std::acos(std::clamp(dot(tangent, decoded), -1.0, 1.0)));
			offset += 4;
		}
		if (p_format.has_colors) {
			for (size_t channel = 0; channel < 4; ++channel) {
				r_errors.color_absolute = std::max(r_errors.color_absolute, std::abs(static_cast<double>(p_source[base + offset + channel]) - p_decoded[base + offset + channel]));
			}
			offset += 4;
		}
		for (; offset < stride; ++offset) {
			const double source = p_source[base + offset];
			const double error = std::abs(source - p_decoded[base + offset]);
			r_errors.uv_absolute = std::max(r_errors.uv_absolute, error);
			r_errors.uv_relative = std::max(r_errors.uv_relative, error / std::max(1.0, std::abs(source)));
		}
	}
}

} // namespace

AttributeCodecError encode_attribute_uv(float p_value, uint32_t &r_code) {
	if (!std::isfinite(p_value)) {
		return AttributeCodecError::NON_FINITE;
	}
	const uint32_t bits = float_bits(p_value);
	const uint32_t magnitude = bits & 0x7fffffffu;
	if (magnitude > 0x4efffe00u) {
		return AttributeCodecError::UV_RANGE;
	}
	const uint32_t encoded = magnitude < 0x3f800000u ? static_cast<uint32_t>(static_cast<double>(bits_float(magnitude) * 16384.0f) + 0.5) : std::min(((magnitude - 0x3f000000u) + 256u) >> 9, UV_MAGNITUDE_MASK);
	r_code = encoded ^ ((1u << 19) - (bits >> 31));
	return AttributeCodecError::OK;
}

AttributeCodecError decode_attribute_uv(uint32_t p_code, float &r_value) {
	if (p_code > UV_MASK) {
		return AttributeCodecError::UV_RANGE;
	}
	const bool negative = p_code <= UV_MAGNITUDE_MASK;
	const uint32_t magnitude = (negative ? ~p_code : p_code) & UV_MAGNITUDE_MASK;
	const float value = bits_float(0x3f000000u + (magnitude << 9));
	const float decoded = std::min(value * 2.0f - 1.0f, value);
	r_value = negative ? -decoded : decoded;
	return AttributeCodecError::OK;
}

AttributeCodecResult pack_cluster_attributes(const ClusterAttributeFormat &p_format,
		const std::vector<float> &p_values, uint32_t p_vertices, PackedClusterAttributes &r_attributes) {
	if (!valid_format(p_format, false) || p_vertices == 0 || p_vertices > MAX_CLUSTER_VERTICES || p_values.size() != p_vertices * p_format.scalar_count()) {
		return { AttributeCodecError::INVALID_LAYOUT };
	}
	try {
		PackedClusterAttributes candidate;
		auto &layout = candidate.layout;
		layout.format = p_format;
		layout.format.tangent_bits = p_format.has_tangents ? p_format.tangent_bits : 0;
		layout.format.lossless_uvs = p_format.uv_sets != 0 && p_format.lossless_uvs;
		layout.vertices = p_vertices;
		std::vector<AttributeCodes> codes(p_vertices);
		std::array<uint32_t, 4> color_max = {};
		std::array<std::array<uint32_t, 2>, 4> uv_max = {};
		for (uint32_t uv = 0; uv < p_format.uv_sets; ++uv) {
			layout.uv_min[uv].fill(UV_MASK);
		}
		const size_t stride = p_format.scalar_count();
		for (uint32_t vertex = 0; vertex < p_vertices; ++vertex) {
			const size_t base = vertex * stride;
			for (size_t scalar = 0; scalar < stride; ++scalar) {
				if (!std::isfinite(p_values[base + scalar])) {
					return { AttributeCodecError::NON_FINITE, vertex };
				}
			}
			Vector normal{ p_values[base], p_values[base + 1], p_values[base + 2] };
			if (!normalize(normal)) {
				return { AttributeCodecError::INVALID_NORMAL, vertex };
			}
			auto &code = codes[vertex];
			code.normal = encode_normal(normal, p_format.normal_bits);
			size_t offset = 3;
			if (p_format.has_tangents) {
				Vector tangent{ p_values[base + 3], p_values[base + 4], p_values[base + 5] };
				if (!normalize(tangent) || !encode_tangent(tangent, decode_normal(code.normal, p_format.normal_bits), p_format.tangent_bits, code.tangent)) {
					return { AttributeCodecError::INVALID_TANGENT, vertex };
				}
				code.tangent |= p_values[base + 6] < 0 ? 1u << p_format.tangent_bits : 0;
				offset += 4;
			}
			if (p_format.has_colors) {
				for (size_t channel = 0; channel < 4; ++channel) {
					code.color[channel] = static_cast<uint32_t>(0.5f + std::clamp(p_values[base + offset + channel], 0.0f, 1.0f) * 255.0f);
					layout.color_min[channel] = std::min(layout.color_min[channel], code.color[channel]);
					color_max[channel] = std::max(color_max[channel], code.color[channel]);
				}
				offset += 4;
			}
			for (uint32_t uv = 0; uv < p_format.uv_sets; ++uv) {
				for (size_t axis = 0; axis < 2; ++axis) {
					const float value = p_values[base + offset + uv * 2 + axis];
					if (p_format.lossless_uvs) {
						code.uv[uv][axis] = float_bits(value);
					} else {
						const auto result = encode_attribute_uv(value, code.uv[uv][axis]);
						if (result != AttributeCodecError::OK) {
							return { result, vertex };
						}
						layout.uv_min[uv][axis] = std::min(layout.uv_min[uv][axis], code.uv[uv][axis]);
						uv_max[uv][axis] = std::max(uv_max[uv][axis], code.uv[uv][axis]);
					}
				}
			}
		}
		if (p_format.has_colors) {
			for (size_t channel = 0; channel < 4; ++channel) {
				layout.color_bits[channel] = width(color_max[channel] - layout.color_min[channel]);
			}
		}
		for (uint32_t uv = 0; uv < p_format.uv_sets; ++uv) {
			for (size_t axis = 0; axis < 2; ++axis) {
				layout.uv_bits[uv][axis] = p_format.lossless_uvs ? 32 : width(uv_max[uv][axis] - layout.uv_min[uv][axis]);
				if (p_format.lossless_uvs) {
					layout.uv_min[uv][axis] = 0;
				}
			}
		}
		candidate.bytes.resize(((static_cast<size_t>(vertex_bits(layout)) * p_vertices + 31) / 32) * 4, 0);
		size_t cursor = 0;
		for (const auto &code : codes) {
			put_bits(candidate.bytes, cursor, code.normal, p_format.normal_bits * 2);
			if (p_format.has_tangents) {
				put_bits(candidate.bytes, cursor, code.tangent, p_format.tangent_bits + 1);
			}
			if (p_format.has_colors) {
				for (size_t channel = 0; channel < 4; ++channel) {
					put_bits(candidate.bytes, cursor, code.color[channel] - layout.color_min[channel], layout.color_bits[channel]);
				}
			}
			for (uint32_t uv = 0; uv < p_format.uv_sets; ++uv) {
				for (size_t axis = 0; axis < 2; ++axis) {
					put_bits(candidate.bytes, cursor, code.uv[uv][axis] - layout.uv_min[uv][axis], layout.uv_bits[uv][axis]);
				}
			}
		}
		std::vector<float> decoded;
		const auto result = unpack_cluster_attributes(layout, candidate.bytes.data(), candidate.bytes.size(), decoded);
		if (result.error != AttributeCodecError::OK) {
			return result;
		}
		measure_errors(p_format, p_values, decoded, candidate.errors);
		r_attributes = std::move(candidate);
		return {};
	} catch (const std::bad_alloc &) {
		return { AttributeCodecError::MEMORY };
	}
}

AttributeCodecResult unpack_cluster_attributes(const ClusterAttributeLayout &p_layout,
		const uint8_t *p_bytes, size_t p_size, std::vector<float> &r_values) {
	const auto valid = validate_payload(p_layout, p_bytes, p_size);
	if (valid != AttributeCodecError::OK) {
		return { valid };
	}
	try {
		std::vector<float> candidate;
		candidate.reserve(p_layout.vertices * p_layout.format.scalar_count());
		for (uint32_t vertex = 0; vertex < p_layout.vertices; ++vertex) {
			const auto result = decode_vertex(p_layout, p_bytes, vertex, candidate);
			if (result != AttributeCodecError::OK) {
				return { result, vertex };
			}
		}
		r_values = std::move(candidate);
		return {};
	} catch (const std::bad_alloc &) {
		return { AttributeCodecError::MEMORY };
	}
}

AttributeCodecResult unpack_cluster_attribute(const ClusterAttributeLayout &p_layout,
		const uint8_t *p_bytes, size_t p_size, uint32_t p_vertex, std::vector<float> &r_values) {
	const auto valid = validate_payload(p_layout, p_bytes, p_size);
	if (valid != AttributeCodecError::OK) {
		return { valid };
	}
	if (p_vertex >= p_layout.vertices) {
		return { AttributeCodecError::INVALID_VALUES, p_vertex };
	}
	try {
		std::vector<float> candidate;
		candidate.reserve(p_layout.format.scalar_count());
		const auto result = decode_vertex(p_layout, p_bytes, p_vertex, candidate);
		if (result != AttributeCodecError::OK) {
			return { result, p_vertex };
		}
		r_values = std::move(candidate);
		return {};
	} catch (const std::bad_alloc &) {
		return { AttributeCodecError::MEMORY };
	}
}

} // namespace VirtualGeometry
