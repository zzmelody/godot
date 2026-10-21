#ifndef VIRTUAL_GEOMETRY_FRAME_INC
#define VIRTUAL_GEOMETRY_FRAME_INC

// Two spare surface channels retain an independent binormal direction. The
// lighting stage normalizes both axes before applying anisotropy flow.
vec2 vg_encode_binormal(vec3 value) {
	float length_l1 = dot(abs(value), vec3(1.0));
	if (length_l1 == 0.0) {
		return vec2(2.0);
	}
	value /= length_l1;
	vec2 signs = vec2(value.x >= 0.0 ? 1.0 : -1.0, value.y >= 0.0 ? 1.0 : -1.0);
	return value.z < 0.0 ? (1.0 - abs(value.yx)) * signs : value.xy;
}

vec3 vg_decode_binormal(vec2 value) {
	if (value.x > 1.0) {
		return vec3(0.0);
	}
	vec3 result = vec3(value, 1.0 - abs(value.x) - abs(value.y));
	float fold = max(-result.z, 0.0);
	result.xy += vec2(result.x >= 0.0 ? -fold : fold, result.y >= 0.0 ? -fold : fold);
	return normalize(result);
}

#endif
