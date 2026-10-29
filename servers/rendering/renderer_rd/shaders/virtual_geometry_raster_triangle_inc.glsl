// Shared fixed-point coverage. Classification can supply prepared positions;
// the separate masked path still supplies one triangle per physical quad.
#ifndef VG_RASTER_POSITION
#define VG_RASTER_POSITION vg_raster_position
#endif
int vg_edge(ivec2 a, ivec2 b, ivec2 point) {
	ivec2 d = b - a;
	ivec2 q = point - a;
	return d.x * q.y - d.y * q.x;
}

int vg_bias(ivec2 a, ivec2 b) {
	ivec2 d = b - a;
	return d.y < 0 || (d.y == 0 && d.x > 0) ? 0 : -1;
}

void vg_raster_prepared_triangle(uint index, uint triangle, uint lane, vec4 clip0, vec4 clip1, vec4 clip2, vec3 shadow_clips) {
	vec4 clip[3] = vec4[3](clip0, clip1, clip2);
	ivec2 fixed_position[3];
	for (uint corner = 0u; corner < 3u; ++corner) {
		fixed_position[corner] = ivec2(floor(vg_screen(clip[corner]) * 256.0 + 0.5));
	}
	ivec2 lower = min(min(fixed_position[0], fixed_position[1]), fixed_position[2]);
	ivec2 upper = max(max(fixed_position[0], fixed_position[1]), fixed_position[2]);
	ivec2 first = max(ivec2(0), ivec2(ceil((vec2(lower) - 128.0) / 256.0)));
	ivec2 last = min(ivec2(raster_info.raster.xy) - 1, ivec2(floor((vec2(upper) - 128.0) / 256.0)));
	if (any(greaterThan(first, last))) {
		return;
	}
	// Rebase 16.8 coordinates before products; classifier bounds the triangle
	// extent to 63 pixels. Either winding uses the same top-left convention.
	ivec2 origin = first * 256 + 128;
	for (uint corner = 0u; corner < 3u; ++corner) {
		fixed_position[corner] -= origin;
	}
	int area = vg_edge(fixed_position[0], fixed_position[1], fixed_position[2]);
	if (area == 0) {
		return;
	}
	uint payload = vg_pixel_payload(index, triangle);
	if (vg_cull_material(payload, area > 0)) {
		return;
	}
	if (area < 0) {
		ivec2 position = fixed_position[1];
		fixed_position[1] = fixed_position[2];
		fixed_position[2] = position;
		vec4 value = clip[1];
		clip[1] = clip[2];
		clip[2] = value;
		float shadow_value = shadow_clips.y;
		shadow_clips.y = shadow_clips.z;
		shadow_clips.z = shadow_value;
		area = -area;
	}
	ivec3 bias = ivec3(vg_bias(fixed_position[1], fixed_position[2]), vg_bias(fixed_position[2], fixed_position[0]), vg_bias(fixed_position[0], fixed_position[1]));
	vec3 depths = vec3(clip[0].z / clip[0].w, clip[1].z / clip[1].w, clip[2].z / clip[2].w);
	// Edge equations are linear in pixel coordinates. Preserve the exact
	// fixed-point coverage and bias while advancing with integer additions.
	ivec3 edge_step_x = -ivec3(fixed_position[2].y - fixed_position[1].y, fixed_position[0].y - fixed_position[2].y, fixed_position[1].y - fixed_position[0].y) * 256;
	ivec3 edge_step_y = ivec3(fixed_position[2].x - fixed_position[1].x, fixed_position[0].x - fixed_position[2].x, fixed_position[1].x - fixed_position[0].x) * 256;
#ifdef VG_MASKED
	ivec2 quad_first = first & ~ivec2(1);
	ivec2 lane_offset = ivec2(lane & 1u, lane >> 1u);
	ivec2 origin_point = (quad_first + lane_offset - first) * 256;
	ivec3 row_edges = ivec3(vg_edge(fixed_position[1], fixed_position[2], origin_point), vg_edge(fixed_position[2], fixed_position[0], origin_point), vg_edge(fixed_position[0], fixed_position[1], origin_point));
	for (int y = quad_first.y; y <= last.y; y += 2) {
		ivec3 edges = row_edges;
		for (int x = quad_first.x; x <= last.x; x += 2) {
			ivec2 pixel = ivec2(x, y) + lane_offset;
			// Every physical quad evaluates the same triangle and the same pixel
			// origin. Do not branch on coverage before derivative/texture work.
			float material_depth;
			bool accepted = vg_raster_coverage(payload, vec2(pixel) + 0.5, material_depth);
			if (accepted && dot(shadow_clips, vec3(edges) / float(area)) <= 0.0 && all(greaterThanEqual(pixel, first)) && all(lessThanEqual(pixel, last)) && all(greaterThanEqual(edges + bias, ivec3(0)))) {
				float depth = depths.x + (depths.y - depths.x) * (float(edges.y) / float(area)) + (depths.z - depths.x) * (float(edges.z) / float(area));
#ifdef VG_DEPTH_USED
				depth = material_depth;
#endif
				vg_write_pixel(pixel, depth, payload);
			}
			edges += edge_step_x * 2;
		}
		row_edges += edge_step_y * 2;
	}
#else
	ivec3 row_edges = ivec3(vg_edge(fixed_position[1], fixed_position[2], ivec2(0)), vg_edge(fixed_position[2], fixed_position[0], ivec2(0)), vg_edge(fixed_position[0], fixed_position[1], ivec2(0)));
	for (int y = first.y; y <= last.y; ++y) {
		ivec3 edges = row_edges;
		for (int x = first.x; x <= last.x; ++x) {
			if (dot(shadow_clips, vec3(edges) / float(area)) <= 0.0 && all(greaterThanEqual(edges + bias, ivec3(0)))) {
				float depth = depths.x + (depths.y - depths.x) * (float(edges.y) / float(area)) + (depths.z - depths.x) * (float(edges.z) / float(area));
				vg_write_pixel(ivec2(x, y), depth, payload);
			}
			edges += edge_step_x;
		}
		row_edges += edge_step_y;
	}
#endif
}

// Both software entry points use the same vertex evaluation and coverage.
void vg_raster_triangle(uint index, uvec2 selected, uint triangle, uint lane) {
	if (!vg_raster_material_active(selected, triangle)) {
		return;
	}
	uvec3 vertices = vg_triangle(selected.y, triangle);
	vec4 clip[3];
	vec3 shadow_clips;
	for (uint corner = 0u; corner < 3u; ++corner) {
		vec3 point = VG_RASTER_POSITION(selected.x, selected.y, vertices[corner]);
		clip[corner] = vg_project(selected.x, point);
		shadow_clips[corner] = vg_shadow_clip(selected.x, point);
	}
	vg_raster_prepared_triangle(index, triangle, lane, clip[0], clip[1], clip[2], shadow_clips);
}
