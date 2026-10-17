layout(set = 2, binding = 15, std430) readonly buffer ViewStatus {
	uint data[];
}
vg_status;
// Material evaluation has finished. Reuse the scene's lighting and fog with
// material outputs and the exact visibility depth. All resources are borrowed.
layout(set = 2, binding = 1, std430) readonly buffer Geometry {
	uint data[];
}
geometry;
layout(set = 2, binding = 2, std430) readonly buffer Visibility {
	uvec2 data[];
}
vg_visibility;
layout(set = 2, binding = 3, std430) readonly buffer Shading {
	uint data[];
}
vg_shading;
#define VG_SCENE_SET 2
#define VG_MATERIAL_LOOKUP
#include "../virtual_geometry_scene_inc.glsl"
layout(set = 2, binding = 16) uniform texture2D vg_albedo;
layout(set = 2, binding = 17) uniform texture2D vg_normal;
layout(set = 2, binding = 18) uniform texture2D vg_emission;
layout(set = 2, binding = 19) uniform texture2D vg_specular;
layout(set = 2, binding = 20) uniform texture2D vg_backlight;
layout(set = 2, binding = 21) uniform texture2D vg_anisotropy;
layout(set = 2, binding = 22) uniform utexture2D vg_flags;
layout(set = 2, binding = 23) uniform texture2D vg_tangent;
layout(set = 2, binding = 24) uniform texture2D vg_geometric_normal;

InstanceData vg_instance(uint source) {
	InstanceData value;
	value.transform = mat3x4(vg_source_vector(source, 0u), vg_source_vector(source, 4u), vg_source_vector(source, 8u));
	value.compressed_aabb_position_pad = vec4(0.0);
	value.compressed_aabb_size_pad = vec3(0.0);
	value.material_feedback_index = 0xffffffffu;
	value.uv_scale = vec4(1.0);
	// Baked/instance GI projection and temporal state are separate source fields
	// still to be connected. They must not read an unrelated ordinary instance.
	value.flags = 255u << INSTANCE_FLAGS_FADE_SHIFT;
	value.instance_uniforms_ofs = scene.data[vg_source_word(source) + 45u];
	value.gi_offset = 0u;
	value.layer_mask = scene.data[vg_source_word(source) + 46u];
	value.prev_transform = value.transform;
	value.lightmap_uv_scale = vec4(0.0);
#ifdef USE_DOUBLE_PRECISION
	value.model_precision = vec4(0.0);
	value.prev_model_precision = vec4(0.0);
#endif
	return value;
}
