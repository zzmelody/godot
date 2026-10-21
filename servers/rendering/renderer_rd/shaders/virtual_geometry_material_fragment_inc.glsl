// Shared authored fragment inputs for visibility coverage and deferred shading.
// Compute callers execute complete primitive-preserving helper quads.
#ifdef VG_COMPUTE_FRAGMENT
#define dFdx vg_dFdxFine
#define dFdy vg_dFdyFine
#endif
#ifndef half
#define half float
#define VG_FRAGMENT_DEFINED_HALF
#endif
#include "scene_forward_aa_inc.glsl"
#ifdef VG_FRAGMENT_DEFINED_HALF
#undef half
#undef VG_FRAGMENT_DEFINED_HALF
#endif
#ifdef VG_COMPUTE_FRAGMENT
#undef dFdx
#undef dFdy
#endif

void vg_user_fragment() {
#CODE : FRAGMENT
}
void vg_evaluate_fragment(VGSurface surface, vec2 point) {
	vg_killed = false;
	vec3 b = surface.barycentrics.value;
#CODE : VIRTUAL_GEOMETRY_VARYINGS
	vertex = surface.view_position;
	normal_highp = mat3(vg_view.view) * surface.normal;
	tangent = mat3(vg_view.view) * surface.tangent;
	binormal = mat3(vg_view.view) * surface.binormal;
	vg_front_facing = surface.barycentrics.front_facing;
#ifdef DO_SIDE_CHECK
	if (!vg_front_facing) {
		normal_highp = -normal_highp;
	}
#endif
	view_highp = -normalize(vertex - vg_view.eye_offset.xyz);
	uv_interp = surface.uv[0];
	uv2_interp = surface.uv[1];
	color_interp = surface.color;
	vg_fragcoord = vec4(point, surface.depth, surface.reciprocal_w);
	vg_fragment_depth = surface.depth;
	albedo_highp = vec3(1.0);
	alpha_highp = 1.0;
	metallic_highp = 0.0;
	// The native fragment stage has its own default. Vertex ROUGHNESS only
	// controls vertex lighting, which this material path does not use.
	roughness_highp = 1.0;
	specular = 0.5;
	emission = vec3(0.0);
	backlight = vec3(0.0);
	normal_map = vec3(0.5);
	normal_map_depth = 1.0;
	ao = 1.0;
	ao_light_affect = 0.0;
	rim = 0.0;
	rim_tint = 0.0;
	clearcoat = 0.0;
	clearcoat_roughness = 0.0;
	anisotropy = 0.0;
	anisotropy_flow = vec2(1.0, 0.0);
	alpha_scissor_threshold = 1.0;
	alpha_hash_scale = 1.0;
	sss_strength = 0.0;
	transmittance_color = vec4(0.0, 0.0, 0.0, 1.0);
	transmittance_depth = 0.0;
	transmittance_boost = 0.0;
	bent_normal_map = vec3(0.5);
	light_vertex = vertex;
	fog = vec4(0.0, 0.0, 0.0, 1.0);
	custom_radiance = vec4(0.0);
	custom_irradiance = vec4(0.0);
	vg_user_fragment();
#ifdef VG_DEPTH_USED
	if (isnan(vg_fragment_depth) || isinf(vg_fragment_depth)) {
		atomicOr(vg_status.data[0], 2u);
		vg_killed = true;
	}
#endif
	if (isnan(alpha_highp) || isinf(alpha_highp)) {
		atomicOr(vg_status.data[0], 2u);
		vg_killed = true;
	}
#ifdef ALPHA_SCISSOR_USED
	if (isnan(alpha_scissor_threshold) || isinf(alpha_scissor_threshold)) {
		atomicOr(vg_status.data[0], 2u);
		vg_killed = true;
	}
	vg_killed = vg_killed || alpha_highp < alpha_scissor_threshold;
#endif
#ifdef ALPHA_HASH_USED
	if (isnan(alpha_hash_scale) || isinf(alpha_hash_scale) || alpha_hash_scale <= 0.0) {
		atomicOr(vg_status.data[0], 2u);
		vg_killed = true;
	} else {
		vec3 object_position = surface.local_position;
#ifdef LIGHT_VERTEX_USED
		// The native fragment stage replaces its lighting position before alpha
		// hash. Coverage must use the same authored position in every VG pass.
		object_position = (inverse(read_model_matrix) * vg_camera * vec4(light_vertex, 1.0)).xyz;
#endif
		// Even killed lanes must participate in the hash derivatives. A
		// short-circuit here would omit their subgroup quad operations.
		float hash_threshold = compute_alpha_hash_threshold(object_position, alpha_hash_scale);
		vg_killed = vg_killed || alpha_highp < hash_threshold;
	}
#endif
}
