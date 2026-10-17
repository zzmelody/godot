// Shared selection/raster view math. Requires params, work and instance_base().
mat4 instance_transform(uint instance) {
#ifdef VG_GLOBAL_RASTER
	return vg_source_matrix(instance, 12u);
#else
	uint base = instance_base() + instance * 16u;
	return transpose(mat4(uintBitsToFloat(uvec4(work.data[base], work.data[base + 1u], work.data[base + 2u], work.data[base + 3u])),
			uintBitsToFloat(uvec4(work.data[base + 4u], work.data[base + 5u], work.data[base + 6u], work.data[base + 7u])),
			uintBitsToFloat(uvec4(work.data[base + 8u], work.data[base + 9u], work.data[base + 10u], work.data[base + 11u])), vec4(0.0, 0.0, 0.0, 1.0)));
#endif
}

// NaniteClusterCulling.usf::GetProjectedEdgeScales(). Godot looks along -Z.
// Clamp roundoff and the camera-inside-sphere case before divisions.
float projected_edge(uint instance, vec4 sphere, mat4 transform) {
	uint flags = uint(params.lod.z);
	if ((flags & 1u) != 0u && (flags & 4u) == 0u) {
		return 1.0;
	}
	vec3 center = (transform * vec4(sphere.xyz, 1.0)).xyz;
#ifdef VG_GLOBAL_RASTER
	uint source_word = vg_source_word(instance);
	float radius = sphere.w * uintBitsToFloat(scene.data[source_word + 50u]) + uintBitsToFloat(scene.data[source_word + 48u]) * 1.7320509;
#else
	float radius = sphere.w * uintBitsToFloat(work.data[instance_base() + instance * 16u + 13u]) + uintBitsToFloat(params.offsets.w) * 1.7320509;
#endif
	if ((flags & 4u) != 0u) {
		// On the admitted hemisphere, the paraboloid derivative is bounded by
		// twice the reciprocal distance. Crossing its seam keeps the finer cut.
		return max(0.0, length(center) - radius) * 0.5;
	}
	float distance_squared = dot(center, center);
	float z = -center.z;
	float near_plane = params.lod.x;
	if (z + radius <= near_plane || distance_squared <= radius * radius) {
		return 0.0;
	}
	float x = sqrt(max(0.0, distance_squared - z * z));
	float tangent_distance = sqrt(max(0.0, distance_squared - radius * radius));
	float cosine = (-radius * x + tangent_distance * z) / distance_squared;
	if (cosine * tangent_distance < near_plane) {
		float h = near_plane - z;
		float tx = x + sqrt(max(0.0, radius * radius - h * h));
		cosine = near_plane * inversesqrt(tx * tx + near_plane * near_plane);
	}
	return max(z - radius, near_plane) * max(cosine, 0.0);
}

