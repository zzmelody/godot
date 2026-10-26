// One programmable material and source instance own this workgroup. Admitted
// clusters contain at most 256 vertices and 128 triangles. Cache only vertices
// referenced by active material triangles: evaluating an unused vertex could
// incorrectly reject the view when its authored deformation is nonfinite.
shared uint vg_used_vertices[8];
shared uint vg_cached_triangles[128];
shared vec4 vg_cached_clip[256];
#ifdef VG_BIN_BUILD
shared vec3 vg_cached_position[256];
#else
shared float vg_cached_shadow_clip[256];
#endif

bool vg_cached_triangle_active(uint triangle) {
	return (vg_cached_triangles[triangle] & 0x80000000u) != 0u;
}

uvec3 vg_cached_triangle(uint triangle) {
	uint packed = vg_cached_triangles[triangle];
	return uvec3(packed, packed >> 8u, packed >> 16u) & uvec3(255u);
}

void vg_cache_raster_vertices(uvec2 selected, uint group_size) {
	uint lane = gl_LocalInvocationIndex;
	if (lane < 8u) {
		vg_used_vertices[lane] = 0u;
	}
	barrier();
	uint record = vg_record(selected.y);
	for (uint triangle = lane; triangle < geometry.data[record + 5u]; triangle += group_size) {
		uint packed = 0u;
		if (vg_raster_material_active(selected, triangle)) {
			uvec3 vertices = vg_triangle(selected.y, triangle);
			packed = 0x80000000u | vertices.x | (vertices.y << 8u) | (vertices.z << 16u);
			for (uint corner = 0u; corner < 3u; ++corner) {
				uint vertex = vertices[corner];
				atomicOr(vg_used_vertices[vertex >> 5u], 1u << (vertex & 31u));
			}
		}
		vg_cached_triangles[triangle] = packed;
	}
	barrier();
	for (uint vertex = lane; vertex < geometry.data[record + 4u]; vertex += group_size) {
		if ((vg_used_vertices[vertex >> 5u] & (1u << (vertex & 31u))) == 0u) {
			continue;
		}
		vec3 position = vg_raster_position(selected.x, selected.y, vertex);
		vg_cached_clip[vertex] = vg_project(selected.x, position);
#ifdef VG_BIN_BUILD
		vg_cached_position[vertex] = position;
#else
		vg_cached_shadow_clip[vertex] = vg_shadow_clip(selected.x, position);
#endif
	}
	barrier();
}
