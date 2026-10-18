// Three orthographic captures write the existing Godot SDFGI voxel formats.
// Depth testing is disabled: interior surfaces must also contribute.
layout(set = 1, binding = 0, r16ui) uniform writeonly uimage3D vg_gi_albedo;
layout(set = 1, binding = 1, r32ui) uniform writeonly uimage3D vg_gi_emission;
layout(set = 1, binding = 2, r32ui) uniform writeonly uimage3D vg_gi_emission_aniso;
layout(set = 1, binding = 3, r32ui) uniform uimage3D vg_gi_facing;
layout(set = 1, binding = 4, std140) uniform VoxelCapture {
	mat4 world_to_bounds;
	ivec4 offset;
	ivec4 size;
} vg_gi;

void vg_capture_sdfgi(uint payload, vec2 point) {
	vg_setup_vertex(vg_visible_reference((payload >> 7u) - 1u).x);
	VGSurface surface;
	if (!vg_surface(payload, point, projection_matrix, vg_view.dimensions.xy, vg_view.dimensions.zw, surface)) {
		atomicOr(vg_status.data[0], 2u);
		return;
	}
	vg_evaluate_fragment(surface, point);
	if (vg_killed) {
		return;
	}
	vec3 world = (vg_view.inverse_view * vec4(surface.view_position, 1.0)).xyz;
	vec3 local = (vg_gi.world_to_bounds * vec4(world, 1.0)).xyz;
	if (any(lessThan(local, vec3(0.0))) || any(greaterThanEqual(local, vec3(1.0)))) {
		return;
	}
	ivec3 cell = vg_gi.offset.xyz + ivec3(floor(local * vec3(vg_gi.size.xyz)));
	uvec3 color = uvec3(clamp(albedo_highp, vec3(0.0), vec3(1.0)) * 31.0);
	imageStore(vg_gi_albedo, cell, uvec4(1u | (color.r << 11u) | (color.g << 6u) | (color.b << 1u)));
	const vec3 directions[6] = vec3[](
			vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), vec3(0.0, 0.0, 1.0),
			vec3(-1.0, 0.0, 0.0), vec3(0.0, -1.0, 0.0), vec3(0.0, 0.0, -1.0));
	vec3 world_normal = normalize(mat3(vg_view.inverse_view) * normal_highp);
	uint facing = 0u;
	float closest = -2.0;
	float lumas[6];
	vec3 light_total = vec3(0.0);
	for (uint direction = 0u; direction < 6u; ++direction) {
		float weight = dot(world_normal, directions[direction]);
		if (weight > closest) {
			closest = weight;
			facing = 1u << direction;
		}
		vec3 light = max(emission * vg_view.state.y, vec3(0.0)) * max(weight, 0.0);
		light_total += light;
		lumas[direction] = max(light.r, max(light.g, light.b));
	}
	imageAtomicOr(vg_gi_facing, cell, facing);
	float maximum = max(light_total.r, max(light_total.g, light_total.b));
	if (maximum <= 0.001) {
		return;
	}
	uint anisotropy_bits = 0u;
	for (uint direction = 0u; direction < 6u; ++direction) {
		anisotropy_bits |= min(31u, uint(lumas[direction] / maximum * 31.0)) << (direction * 5u);
	}
	// Godot's 8985 emission encoding reserves two bits for neighbor data.
	vec3 light = clamp(light_total, vec3(0.0), vec3(65408.0));
	maximum = max(light.r, max(light.g, light.b));
	float exponent = max(-16.0, floor(log2(maximum))) + 16.0;
	if (floor(maximum / exp2(exponent - 24.0) + 0.5) >= 512.0) {
		exponent += 1.0;
	}
	uvec3 mantissa = uvec3(floor(light / exp2(exponent - 24.0) + 0.5));
	uint encoded = ((mantissa.r & 511u) >> 1u) | ((mantissa.g & 511u) << 8u) |
			(((mantissa.b & 511u) >> 1u) << 17u) | ((uint(exponent) & 31u) << 25u);
	imageStore(vg_gi_emission, cell, uvec4(encoded));
	imageStore(vg_gi_emission_aniso, cell, uvec4(anisotropy_bits));
}
