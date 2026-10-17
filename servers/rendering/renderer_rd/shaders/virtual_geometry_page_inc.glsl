// Random access to admitted VGGP pages. The caller declares geometry.data.
#ifndef VIRTUAL_GEOMETRY_PAGE_INCLUDED
#define VIRTUAL_GEOMETRY_PAGE_INCLUDED

uint vg_bits(uint base, uint bit, uint count) {
	if (count == 0u) {
		return 0u;
	}
	uint shift = bit & 31u;
	uint value = geometry.data[base + (bit >> 5u)] >> shift;
	if (shift + count > 32u) {
		value |= geometry.data[base + (bit >> 5u) + 1u] << (32u - shift);
	}
	// Lossless UVs can use 32 bits at any bit alignment. Never shift by 32.
	return value & (0xffffffffu >> (32u - count));
}

uint vg_record(uint reference) {
	return (reference >> 8u) * 8192u + 8u + (reference & 255u) * 72u;
}

vec3 vg_position(uint reference, uint vertex) {
	uint record = vg_record(reference);
	uvec3 bits = uvec3(geometry.data[record + 28u], geometry.data[record + 29u], geometry.data[record + 30u]);
	uint bit = vertex * (bits.x + bits.y + bits.z);
	uint base = (reference >> 8u) * 8192u + (geometry.data[record + 31u] >> 2u);
	ivec3 origin = ivec3(geometry.data[record + 25u], geometry.data[record + 26u], geometry.data[record + 27u]);
	ivec3 point = origin + ivec3(vg_bits(base, bit, bits.x), vg_bits(base, bit + bits.x, bits.y), vg_bits(base, bit + bits.x + bits.y, bits.z));
	int position_precision = int(geometry.data[record + 24u]);
	return vec3(point) * uintBitsToFloat(uint(127 - position_precision) << 23u);
}

uvec3 vg_triangle(uint reference, uint triangle) {
	uint record = vg_record(reference);
	uint base_bits = uint(findMSB(geometry.data[record + 4u] - 1u)) + 1u;
	uint base = (reference >> 8u) * 8192u + (geometry.data[record + 63u] >> 2u);
	uint value = vg_bits(base, triangle * (base_bits + 10u), base_bits + 10u);
	uint first = value & ((1u << base_bits) - 1u);
	return uvec3(first, first + ((value >> base_bits) & 31u), first + ((value >> (base_bits + 5u)) & 31u));
}

vec2 vg_screen_position(vec4 clip, vec2 size) {
	return (clip.xy / clip.w * 0.5 + 0.5) * size;
}
vec2 vg_snap(vec2 pixel) {
	return floor(pixel * 256.0 + 0.5) / 256.0;
}
vec4 vg_snap_clip(vec4 clip, vec2 size) {
	if (clip.w > 0.0) {
		clip.xy = (vg_snap(vg_screen_position(clip, size)) / size * 2.0 - 1.0) * clip.w;
	}
	return clip;
}

#endif
