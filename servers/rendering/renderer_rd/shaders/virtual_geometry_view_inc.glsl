// Shared selection/raster view math. Requires params, work and instance_base().
mat4 instance_transform(uint instance) {
	uint base = instance_base() + instance * 16u;
	return transpose(mat4(uintBitsToFloat(uvec4(work.data[base], work.data[base + 1u], work.data[base + 2u], work.data[base + 3u])),
			uintBitsToFloat(uvec4(work.data[base + 4u], work.data[base + 5u], work.data[base + 6u], work.data[base + 7u])),
			uintBitsToFloat(uvec4(work.data[base + 8u], work.data[base + 9u], work.data[base + 10u], work.data[base + 11u])), vec4(0.0, 0.0, 0.0, 1.0)));
}

// NaniteClusterCulling.usf::GetProjectedEdgeScales(). Godot looks along -Z.
// Clamp roundoff and the camera-inside-sphere case before divisions.
float projected_edge(uint instance, vec4 sphere, mat4 transform) {
	if (params.lod.z != 0.0) {
		return 1.0;
	}
	vec3 center = (transform * vec4(sphere.xyz, 1.0)).xyz;
	float radius = sphere.w * uintBitsToFloat(work.data[instance_base() + instance * 16u + 13u]) +
			uintBitsToFloat(params.offsets.w) * 1.7320509;
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

