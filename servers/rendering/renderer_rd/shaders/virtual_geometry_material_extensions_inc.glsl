#ifdef VG_EXTENDED_MATERIAL
#ifndef VG_EXTENSION_SET
#define VG_EXTENSION_SET 0
#endif
layout(set = VG_EXTENSION_SET, binding = 64, std430)
#ifdef VG_EXTENSION_READONLY
readonly
#endif
buffer MaterialExtensions {
	uint data[];
}
vg_light_data;

vec3 vg_extension_load3(uint word) {
	return uintBitsToFloat(uvec3(vg_light_data.data[word], vg_light_data.data[word + 1u], vg_light_data.data[word + 2u]));
}
vec4 vg_extension_load4(uint word) {
	return vec4(vg_extension_load3(word), uintBitsToFloat(vg_light_data.data[word + 3u]));
}
#ifndef VG_EXTENSION_READONLY
void vg_extension_store3(uint word, vec3 value) {
	for (uint axis = 0u; axis < 3u; ++axis) {
		vg_light_data.data[word + axis] = floatBitsToUint(value[axis]);
	}
}
void vg_extension_store4(uint word, vec4 value) {
	vg_extension_store3(word, value.xyz);
	vg_light_data.data[word + 3u] = floatBitsToUint(value.w);
}
#endif
#endif
