#ifndef VIRTUAL_GEOMETRY_PROJECTION_INCLUDED
#define VIRTUAL_GEOMETRY_PROJECTION_INCLUDED

// Flags 2/4/8 select directional pancake, dual paraboloid and its second side.
// The paraboloid matches Godot's existing straight-edge shadow approximation.
vec4 vg_view_clip(vec3 point, mat4 projection, uint flags, float shadow_far) {
	vec4 clip;
	if ((flags & 4u) != 0u) {
		float side = (flags & 8u) != 0u ? -1.0 : 1.0;
		float radius = length(point);
		float denominator = max(1e-8, radius - point.z * side);
		vec2 position = point.xy / denominator;
		position *= vec2(projection[0][0], projection[1][1]);
		clip = vec4(position, 1.0 - radius / shadow_far, 1.0);
	} else {
		clip = projection * vec4(point, 1.0);
	}
	if ((flags & 2u) != 0u) {
		clip.z = min(clip.z, clip.w);
	}
	return clip;
}

#endif
