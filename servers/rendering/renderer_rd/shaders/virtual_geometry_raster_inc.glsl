// GPU-page random access and the current view's selected source instances.
layout(set = 0, binding = 1, std430) readonly buffer Geometry { uint data[]; } geometry;
layout(set = 0, binding = 3, std140) uniform Parameters {
	mat4 model;
	mat4 world_to_view;
	mat4 projection;
	vec4 lod;
	uvec4 source;
	uvec4 limits;
	uvec4 offsets;
} params;
layout(set = 0, binding = 4, std430) readonly buffer Work { uint data[]; } work;
#ifdef VG_BIN_BUILD
layout(set = 0, binding = 5, std430) buffer Bins { uint data[]; } bins;
#else
layout(set = 0, binding = 5, std430) readonly buffer Bins { uint data[]; } bins;
#endif
layout(push_constant, std430) uniform RasterParameters {
	uvec4 raster; // width, height, global visible-cluster base, bin capacity
	vec2 jitter;
	uint source_base;
} raster_info;

uint instance_base() { return 16u; }

#include "virtual_geometry_view_inc.glsl"

uint vg_bits(uint base, uint bit, uint count) {
	if (count == 0u) { return 0u; }
	uint shift = bit & 31u;
	uint value = geometry.data[base + (bit >> 5u)] >> shift;
	if (shift + count > 32u) { value |= geometry.data[base + (bit >> 5u) + 1u] << (32u - shift); }
	return value & ((1u << count) - 1u);
}

uint vg_record(uint reference) {
	return (reference >> 8u) * 8192u + 8u + (reference & 255u) * 72u;
}

uvec2 vg_selected(uint index) {
	uint word = work.data[11] + index * 2u;
	return uvec2(work.data[word], work.data[word + 1u]);
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

vec4 vg_clip(uint instance, uint reference, uint vertex) {
	vec4 clip = params.projection * instance_transform(instance) * vec4(vg_position(reference, vertex), 1.0);
	clip.xy += raster_info.jitter * clip.w;
	return clip;
}

vec2 vg_screen(vec4 clip) { return (clip.xy / clip.w * 0.5 + 0.5) * vec2(raster_info.raster.xy); }
vec2 vg_snap(vec2 pixel) { return floor(pixel * 256.0 + 0.5) / 256.0; }
