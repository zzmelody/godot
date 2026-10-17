#[compute]

#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_shader_atomic_int64 : require

#VERSION_DEFINES

#include "virtual_geometry_raster_inc.glsl"
#include "virtual_geometry_write_inc.glsl"

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

int vg_edge(ivec2 a, ivec2 b, ivec2 point) {
	ivec2 d = b - a;
	ivec2 q = point - a;
	return d.x * q.y - d.y * q.x;
}

int vg_bias(ivec2 a, ivec2 b) {
	ivec2 d = b - a;
	return d.y < 0 || (d.y == 0 && d.x > 0) ? 0 : -1;
}

void vg_raster_triangle(uint index, uvec2 selected, uint triangle) {
	uvec3 vertices = vg_triangle(selected.y, triangle);
	vec4 clip[3];
	ivec2 fixed_position[3];
	for (uint corner = 0u; corner < 3u; ++corner) {
		clip[corner] = vg_clip(selected.x, selected.y, vertices[corner]);
		fixed_position[corner] = ivec2(floor(vg_screen(clip[corner]) * 256.0 + 0.5));
	}
	ivec2 lower = min(min(fixed_position[0], fixed_position[1]), fixed_position[2]);
	ivec2 upper = max(max(fixed_position[0], fixed_position[1]), fixed_position[2]);
	ivec2 first = max(ivec2(0), ivec2(ceil((vec2(lower) - 128.0) / 256.0)));
	ivec2 last = min(ivec2(raster_info.raster.xy) - 1, ivec2(floor((vec2(upper) - 128.0) / 256.0)));
	if (any(greaterThan(first, last))) { return; }
	// Rebase 16.8 coordinates before products; classifier bounds the triangle
	// extent to 63 pixels. Either winding uses the same top-left convention.
	ivec2 origin = first * 256 + 128;
	for (uint corner = 0u; corner < 3u; ++corner) { fixed_position[corner] -= origin; }
	int area = vg_edge(fixed_position[0], fixed_position[1], fixed_position[2]);
	if (area == 0) { return; }
	if (area < 0) {
		ivec2 position = fixed_position[1]; fixed_position[1] = fixed_position[2]; fixed_position[2] = position;
		vec4 value = clip[1]; clip[1] = clip[2]; clip[2] = value;
		area = -area;
	}
	ivec3 bias = ivec3(vg_bias(fixed_position[1], fixed_position[2]), vg_bias(fixed_position[2], fixed_position[0]), vg_bias(fixed_position[0], fixed_position[1]));
	vec3 depths = vec3(clip[0].z / clip[0].w, clip[1].z / clip[1].w, clip[2].z / clip[2].w);
	uint payload = ((raster_info.raster.z + index + 1u) << 7u) | triangle;
	for (int y = first.y; y <= last.y; ++y) {
		for (int x = first.x; x <= last.x; ++x) {
			ivec2 point = (ivec2(x, y) - first) * 256;
			ivec3 edges = ivec3(vg_edge(fixed_position[1], fixed_position[2], point), vg_edge(fixed_position[2], fixed_position[0], point), vg_edge(fixed_position[0], fixed_position[1], point));
			if (all(greaterThanEqual(edges + bias, ivec3(0)))) {
				float depth = depths.x + (depths.y - depths.x) * (float(edges.y) / float(area)) + (depths.z - depths.x) * (float(edges.z) / float(area));
				vg_write_pixel(ivec2(x, y), depth, payload);
			}
		}
	}
}

void main() {
	uint job = gl_WorkGroupID.x + gl_WorkGroupID.y * 65535u;
	if (job >= bins.data[1] || bins.data[2] != 0u) { return; }
	uint index = bins.data[4u + raster_info.raster.w + job];
	uvec2 selected = vg_selected(index);
	uint count = geometry.data[vg_record(selected.y) + 5u];
	for (uint triangle = gl_LocalInvocationIndex; triangle < count; triangle += 64u) {
		vg_raster_triangle(index, selected, triangle);
	}
}
