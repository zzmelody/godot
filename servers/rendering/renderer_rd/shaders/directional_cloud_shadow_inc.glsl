// Shared by Mobile/Forward+ and vertex lighting. Source atlas stores
// the same sRGB plate as the sky; reconstruct its perceptual max-RGB coverage.
float directional_cloud_transmittance(uint index, vec3 world_position) {
	vec4 params = directional_lights.data[index].cloud_shadow_params;
	vec4 rect = directional_lights.data[index].cloud_shadow_rect;
	if (rect.z <= 0.0 || params.z <= 0.0 || params.w <= 0.0) {
		return 1.0;
	}
	vec3 sun = directional_lights.data[index].cloud_shadow_direction.xyz;
	float horizon = smoothstep(0.02, 0.12, sun.y);
	vec3 relative = world_position - directional_lights.data[index].cloud_shadow_origin.xyz;
	if (horizon <= 0.0 || relative.y >= params.x) {
		return 1.0;
	}
	vec2 deck = relative.xz + sun.xz * ((params.x - relative.y) / max(sun.y, 0.02));
	vec3 direction = normalize(vec3(deck.x, params.x, deck.y));
	float longitude = dot(direction.xz, direction.xz) > 1e-12 ? atan(direction.x, -direction.z) : 0.0;
	vec2 uv = vec2(fract(longitude / (2.0 * M_PI) + params.y), acos(clamp(direction.y, -1.0, 1.0)) / M_PI);
	vec2 half_texel = 0.5 / (vec2(textureSize(sampler2D(decal_atlas, DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), 0)) * rect.zw);
	float longitude_u = uv.x;
	uv = clamp(uv, half_texel, vec2(1.0) - half_texel);
	vec3 coverage_rgb = textureLod(sampler2D(decal_atlas_srgb, DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), rect.xy + uv * rect.zw, 0.0).rgb;
	// Repeat longitude inside THIS atlas entry, never blend with a neighbour.
	// Match texture repeat at the panorama seam using its two edge texels.
	if (longitude_u < half_texel.x || longitude_u > 1.0 - half_texel.x) {
		vec2 opposite = vec2(longitude_u < half_texel.x ? 1.0 - half_texel.x : half_texel.x, uv.y);
		vec3 wrapped = textureLod(sampler2D(decal_atlas_srgb, DEFAULT_SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP), rect.xy + opposite * rect.zw, 0.0).rgb;
		float blend = longitude_u < half_texel.x ? 0.5 - longitude_u / (2.0 * half_texel.x) : 0.5 + (longitude_u - 1.0) / (2.0 * half_texel.x);
		coverage_rgb = mix(coverage_rgb, wrapped, blend);
	}
	float coverage = clamp(pow(max(max(coverage_rgb.r, coverage_rgb.g), coverage_rgb.b), 1.0 / 2.2), 0.0, 1.0);
	// Continuous transmission; retains an artist-controlled indirect-looking
	// floor through direct sunlight without ever touching actual indirect terms.
	return 1.0 - coverage * params.z * params.w * horizon;
}
