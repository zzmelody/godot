layout(set = 0, binding = 26, std430) readonly buffer PreviousSources {
	uint data[];
}
previous_sources;

vec4 vg_previous_vector(uint source, uint offset) {
	uint word = source * 24u + offset;
	return uintBitsToFloat(uvec4(previous_sources.data[word], previous_sources.data[word + 1u], previous_sources.data[word + 2u], previous_sources.data[word + 3u]));
}

vec2 vg_motion(uint payload, VGSurface surface) {
	if (vg_history_view.state.y == 0.0) {
		return vec2(0.0);
	}
	if ((vg_view.flags.w & 1u) != 0u) {
		// No compatible numeric-parameter snapshot exists. The caller also marks
		// this view reactive; geometry topology changes still evaluate real motion.
		return vec2(0.0);
	}
	uvec2 visible = vg_visible_reference((payload >> 7u) - 1u);
	uvec3 vertices = vg_triangle(visible.y, payload & 127u);
	mat4 previous_world = transpose(mat4(vg_previous_vector(visible.x, 0u), vg_previous_vector(visible.x, 4u), vg_previous_vector(visible.x, 8u), vec4(0.0, 0.0, 0.0, 1.0)));
	float previous_determinant = determinant(mat3(previous_world));
	if (isnan(previous_determinant) || isinf(previous_determinant) || abs(previous_determinant) <= 1e-20) {
		return vec2(0.0);
	}
	read_model_matrix = previous_world;
	vg_world_to_view = vg_history_view.previous_view;
	vg_camera = vg_history_view.previous_inverse_view;
	modelview = vg_world_to_view * previous_world;
	model_normal_matrix = transpose(inverse(mat3(previous_world)));
	modelview_normal = mat3(vg_world_to_view) * model_normal_matrix;
	projection_matrix = vg_history_view.previous_projection;
	inverse_projection_matrix = inverse(projection_matrix);
	instance_custom = vg_previous_vector(visible.x, 16u);
	vg_instance_color = vg_previous_vector(visible.x, 12u);
	vg_time = vg_history_view.state.x;
	vg_previous_evaluation = true;
	vg_previous_invalid = false;
	vec3 previous_position = vec3(0.0);
	for (uint corner = 0u; corner < 3u; ++corner) {
		VGVertex value = vg_vertex(visible.y, vertices[corner]);
		vg_evaluate_vertex(value, visible.x, corner);
		previous_position += value.position * surface.barycentrics.value[corner];
	}
	vec4 previous_clip = projection_matrix * modelview * vec4(previous_position, 1.0);
	vec4 current_clip = vg_view.projection * vec4(surface.view_position, 1.0);
	if (vg_previous_invalid || previous_clip.w <= 1e-8 || current_clip.w <= 1e-8) {
		return vec2(0.0);
	}
	// Both projections are unjittered here. This equals the ordinary scene's
	// previous-minus-current UV convention after removing each frame's jitter.
	vec2 velocity = (previous_clip.xy / previous_clip.w - current_clip.xy / current_clip.w) * 0.5;
	return any(isnan(velocity)) || any(isinf(velocity)) ? vec2(0.0) : velocity;
}
