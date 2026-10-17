// Decodes the existing admitted vertex bit stream; no resource or page writes.
#ifndef VIRTUAL_GEOMETRY_ATTRIBUTES_INC
#define VIRTUAL_GEOMETRY_ATTRIBUTES_INC
#include "virtual_geometry_page_inc.glsl"

struct VGVertex {
	vec3 position;
	vec3 normal;
	vec4 tangent;
	vec4 color;
	vec2 uv[4];
};

uint vg_read_bits(uint base, inout uint cursor, uint count) {
	uint value = vg_bits(base, cursor, count);
	cursor += count;
	return value;
}

vec3 vg_decode_normal(uint code, uint bits) {
	uint mask = (1u << bits) - 1u;
	// Keep axis ties exact before the tangent basis selects its reference axis.
	ivec2 xy = ivec2(code & mask, code >> bits) * 2 - int(mask);
	int z = int(mask) - abs(xy.x) - abs(xy.y);
	int shift = max(-z, 0);
	xy += ivec2(xy.x >= 0 ? -shift : shift, xy.y >= 0 ? -shift : shift);
	return normalize(vec3(xy, z));
}

vec3 vg_decode_tangent(uint code, vec3 normal, uint bits) {
	bool swap_xz = abs(normal.z) > abs(normal.x);
	if (swap_xz) {
		normal = normal.zyx;
	}
	vec3 x = normalize(vec3(-normal.y, normal.x, 0.0));
	vec3 y = cross(normal, x);
	float angle = float(code) * (6.283185307179586 / float(1u << bits));
	vec3 tangent = normalize(x * cos(angle) + y * sin(angle));
	return swap_xz ? tangent.zyx : tangent;
}

float vg_decode_uv(uint code) {
	bool negative = code <= 0x7ffffu;
	uint magnitude = (negative ? ~code : code) & 0x7ffffu;
	float value = uintBitsToFloat(0x3f000000u + (magnitude << 9u));
	float decoded = min(value * 2.0 - 1.0, value);
	return negative ? -decoded : decoded;
}

VGVertex vg_vertex(uint reference, uint vertex) {
	uint record = vg_record(reference);
	uint normal_bits = geometry.data[record + 33u];
	uint tangent_bits = geometry.data[record + 34u];
	uint flags = geometry.data[record + 35u];
	uint uv_sets = geometry.data[record + 36u];
	uint color_widths = geometry.data[record + 40u];
	uint stride = normal_bits * 2u + ((flags & 1u) != 0u ? tangent_bits + 1u : 0u);
	for (uint channel = 0u; channel < 4u; ++channel) {
		stride += (color_widths >> (channel * 4u)) & 15u;
	}
	for (uint component = 0u; component < uv_sets * 2u; ++component) {
		stride += geometry.data[record + 41u + component] & 63u;
	}
	uint base = (reference >> 8u) * 8192u + (geometry.data[record + 37u] >> 2u);
	uint cursor = vertex * stride;
	VGVertex result;
	result.position = vg_position(reference, vertex);
	result.normal = vg_decode_normal(vg_read_bits(base, cursor, normal_bits * 2u), normal_bits);
	result.tangent = vec4(0.0, 0.0, 0.0, 1.0);
	if ((flags & 1u) != 0u) {
		uint code = vg_read_bits(base, cursor, tangent_bits + 1u);
		result.tangent = vec4(vg_decode_tangent(code & ((1u << tangent_bits) - 1u), result.normal, tangent_bits), (code & (1u << tangent_bits)) != 0u ? -1.0 : 1.0);
	}
	result.color = vec4(1.0);
	for (uint channel = 0u; channel < 4u; ++channel) {
		uint origin = (geometry.data[record + 39u] >> (channel * 8u)) & 255u;
		uint count = (color_widths >> (channel * 4u)) & 15u;
		result.color[channel] = float(origin + vg_read_bits(base, cursor, count)) / 255.0;
	}
	for (uint uv = 0u; uv < 4u; ++uv) {
		result.uv[uv] = vec2(0.0);
		if (uv >= uv_sets) {
			continue;
		}
		for (uint axis = 0u; axis < 2u; ++axis) {
			uint format_word = geometry.data[record + 41u + uv * 2u + axis];
			uint code = vg_read_bits(base, cursor, format_word & 63u);
			result.uv[uv][axis] = (flags & 4u) != 0u ? uintBitsToFloat(code) : vg_decode_uv((format_word >> 6u) + code);
		}
	}
	return result;
}
#endif
