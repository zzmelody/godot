// Shared authored vertex inputs for programmable coverage and material reconstruction.
layout(set = 0, binding = VG_GLOBALS_BINDING, std430) readonly buffer GlobalShaderUniformData {
	vec4 data[];
}
global_shader_uniforms;
layout(set = 0, binding = 7, std430) readonly buffer PreviousGlobalShaderUniformData {
	vec4 data[];
}
vg_previous_global_uniforms;
layout(set = 0, binding = 12, std140) uniform View {
	mat4 projection;
	mat4 inverse_projection;
	mat4 view;
	mat4 inverse_view;
	vec4 dimensions;
	vec4 state;
	uvec4 flags;
	vec4 eye_offset;
}
vg_view;
layout(set = 0, binding = 28, std140) uniform FrameHistory {
	mat4 previous_projection;
	mat4 previous_view;
	mat4 previous_inverse_view;
	mat4 main_camera;
	vec4 state;
}
vg_history_view;
#define SAMPLERS_BINDING_FIRST_INDEX 32
#include "samplers_inc.glsl"
#include "virtual_geometry_status_inc.glsl"
#ifndef VG_RASTER_MATERIALS
layout(set = 0, binding = 13, std430) readonly buffer RasterMaterials {
	uint data[];
}
raster_materials;
#endif
#ifdef MATERIAL_UNIFORMS_USED
/* clang-format off */
layout(set = 3, binding = 0, std140) uniform MaterialUniforms {
#MATERIAL_UNIFORMS
} material;
/* clang-format on */
#endif

uint vg_source;
bool vg_killed;
bool vg_front_facing;
vec4 vg_fragcoord;
mat4 read_model_matrix, modelview, projection_matrix;
mat4 inverse_projection_matrix, vg_world_to_view, vg_camera;
mat3 model_normal_matrix, modelview_normal;
vec3 vertex, normal_highp, tangent, binormal, view_highp;
vec2 uv_interp, uv2_interp;
vec4 color_interp, instance_custom;
vec4 vg_instance_color;
float vg_time;
bool vg_previous_evaluation = false;
bool vg_previous_invalid = false;

vec4 vg_global_uniform(uint index) {
	return vg_previous_evaluation ? vg_previous_global_uniforms.data[index] : global_shader_uniforms.data[index];
}
vec3 albedo_highp, emission, backlight, normal_map;
float alpha_highp, metallic_highp, roughness_highp, specular, normal_map_depth;
float ao, ao_light_affect, rim, rim_tint, clearcoat, clearcoat_roughness, anisotropy;
float alpha_scissor_threshold, alpha_hash_scale;
vec2 anisotropy_flow;
vec3 bent_normal_map, light_vertex;
float sss_strength, transmittance_depth, transmittance_boost;
vec4 transmittance_color, fog, custom_radiance, custom_irradiance;

void vg_discard() {
	vg_killed = true;
}

#GLOBALS

void vg_user_vertex() {
#CODE : VERTEX
}

void vg_evaluate_vertex(inout VGVertex value, uint source, uint corner) {
	vertex = value.position;
	normal_highp = value.normal;
	tangent = value.tangent.xyz;
	binormal = value.binormal;
	uv_interp = value.uv[0];
	uv2_interp = value.uv[1];
	color_interp = value.color * vg_instance_color;
	roughness_highp = value.roughness;
	vg_user_vertex();
	vec3 offset = mat3(read_model_matrix) * (vertex - value.position);
	if (any(isnan(offset)) || any(isinf(offset))) {
		if (vg_previous_evaluation) {
			vg_previous_invalid = true;
		} else {
			atomicOr(vg_status.data[0], 1u);
		}
	} else {
		float maximum = uintBitsToFloat(raster_materials.data[VG_MATERIAL_INDEX * 4u + 1u]);
		if (maximum > 0.0) {
			offset = clamp(offset, vec3(-maximum), vec3(maximum));
		}
		value.position += inverse(mat3(read_model_matrix)) * offset;
	}
	value.normal = normal_highp;
	value.tangent.xyz = tangent;
	value.binormal = binormal;
	value.uv[0] = uv_interp;
	value.uv[1] = uv2_interp;
	value.color = color_interp;
	value.roughness = roughness_highp;
	vg_corners[corner] = vg_varyings;
}
void vg_setup_vertex(uint source) {
	vg_source = source;
	read_model_matrix = vg_source_matrix(source, 0u);
	modelview = vg_source_matrix(source, 12u);
	model_normal_matrix = mat3(vg_source_matrix(source, 24u));
	modelview_normal = mat3(vg_view.view) * model_normal_matrix;
	projection_matrix = vg_view.projection;
	inverse_projection_matrix = vg_view.inverse_projection;
	vg_world_to_view = vg_view.view;
	vg_camera = vg_view.inverse_view;
	instance_custom = vg_source_vector(source, 40u);
	vg_instance_color = vg_source_vector(source, 36u);
	vg_time = vg_view.state.x;
	vg_previous_evaluation = false;
	vg_previous_invalid = false;
	roughness_highp = 1.0;
}
