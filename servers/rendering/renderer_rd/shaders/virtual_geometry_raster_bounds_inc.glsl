// Material-bin proof for the original software classifier. A failure only
// retains ordinary classification; it never removes geometry or visibility.
bool vg_bounds_prove_software(uvec2 selected, uint record) {
	if ((uint(params.lod.z) & 6u) != 0u) {
		return false;
	}
	vec3 lower = uintBitsToFloat(uvec3(geometry.data[record + 18u], geometry.data[record + 19u], geometry.data[record + 20u]));
	vec3 upper = uintBitsToFloat(uvec3(geometry.data[record + 21u], geometry.data[record + 22u], geometry.data[record + 23u]));
	float minimum_scale = uintBitsToFloat(vg_source_data(selected.x, 49u));
	float maximum_scale = uintBitsToFloat(vg_source_data(selected.x, 50u));
	float displacement = uintBitsToFloat(vg_source_data(selected.x, 48u)) * 1.0001;
	if (any(greaterThan(lower, upper)) || minimum_scale <= 0.0 || maximum_scale > minimum_scale * 16.0 || displacement < 0.0) {
		return false;
	}
	mat4 transform = instance_transform(selected.x);
	float edge_bound = length(upper - lower) + 3.464102 * displacement / minimum_scale;
	vec4 sphere = uintBitsToFloat(uvec4(geometry.data[record + 12u], geometry.data[record + 13u], geometry.data[record + 14u], geometry.data[record + 15u]));
	float threshold = maximum_scale * edge_bound * params.lod.y / 32.0;
	if (!(projected_edge(selected.x, sphere, transform) > threshold * 1.0001 + 0.000001)) {
		return false;
	}
	// Center/radius interval arithmetic covers the complete affine box without
	// eight corner projections. World displacement is clamped per axis.
	vec3 center = (lower + upper) * 0.5;
	vec3 radius = (upper - lower) * 0.5;
	vec3 view_center = (transform * vec4(center, 1.0)).xyz;
	vec3 view_radius = abs(transform[0].xyz) * radius.x + abs(transform[1].xyz) * radius.y + abs(transform[2].xyz) * radius.z;
	view_radius += (abs(params.world_to_view[0].xyz) + abs(params.world_to_view[1].xyz) + abs(params.world_to_view[2].xyz)) * displacement;
	vec4 clip_center = params.projection * vec4(view_center, 1.0);
	vec4 clip_radius = abs(params.projection[0]) * view_radius.x + abs(params.projection[1]) * view_radius.y + abs(params.projection[2]) * view_radius.z;
	vec3 view_bound = abs(transform[0].xyz) * abs(center.x) + abs(transform[1].xyz) * abs(center.y) + abs(transform[2].xyz) * abs(center.z) + abs(transform[3].xyz) + view_radius;
	vec4 clip_bound = abs(params.projection[0]) * view_bound.x + abs(params.projection[1]) * view_bound.y + abs(params.projection[2]) * view_bound.z + abs(params.projection[3]);
	clip_center.xy += occlusion.raster_view.xy * clip_center.w;
	clip_radius.xy += abs(occlusion.raster_view.xy) * clip_radius.w;
	clip_bound.xy += abs(occlusion.raster_view.xy) * clip_bound.w;
	vec4 guard = (clip_bound + vec4(1.0)) * 0.000004;
	vec4 lo = clip_center - clip_radius - guard, hi = clip_center + clip_radius + guard;
	if (any(isnan(lo)) || any(isinf(lo)) || any(isnan(hi)) || any(isinf(hi)) || lo.w <= 0.0 || lo.z <= 0.0 || hi.z >= lo.w) {
		return false;
	}
	vec2 pixel_min = (min(lo.xy / lo.w, lo.xy / hi.w) * 0.5 + 0.5) * occlusion.raster_view.zw - vec2(0.015625);
	vec2 pixel_max = (max(hi.xy / lo.w, hi.xy / hi.w) * 0.5 + 0.5) * occlusion.raster_view.zw + vec2(0.015625);
	return all(greaterThan(occlusion.raster_view.zw, vec2(0.0))) && all(lessThan(abs(pixel_min), vec2(1048576.0))) && all(lessThan(abs(pixel_max), vec2(1048576.0))) && all(lessThan(pixel_max - pixel_min, vec2(63.0)));
}
