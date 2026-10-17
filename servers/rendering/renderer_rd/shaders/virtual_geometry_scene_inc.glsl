// Frame-local source records, visible-cluster references and material map.
// Header words point to 48-word sources, two-word references and material IDs.
#ifdef VG_SCENE_WRITE
layout(set = 0, binding = 8, std430) buffer Scene { uint data[]; } scene;
#else
layout(set = 0, binding = 8, std430) readonly buffer Scene { uint data[]; } scene;
#endif

uint vg_source_word(uint index) { return scene.data[0] + index * 48u; }
vec4 vg_source_vector(uint source, uint offset) {
	uint word = vg_source_word(source) + offset;
	return uintBitsToFloat(uvec4(scene.data[word], scene.data[word + 1u], scene.data[word + 2u], scene.data[word + 3u]));
}
mat4 vg_source_matrix(uint source, uint offset) {
	return transpose(mat4(vg_source_vector(source, offset), vg_source_vector(source, offset + 4u), vg_source_vector(source, offset + 8u), vec4(0.0, 0.0, 0.0, 1.0)));
}
uvec2 vg_visible_reference(uint index) {
	uint word = scene.data[1] + index * 2u;
	return uvec2(scene.data[word], scene.data[word + 1u]);
}

#ifdef VG_MATERIAL_LOOKUP
#include "virtual_geometry_page_inc.glsl"
uint vg_material(uint payload) {
	uvec2 visible = vg_visible_reference((payload >> 7u) - 1u);
	uint base = (visible.y >> 8u) * 8192u;
	uint record = vg_record(visible.y);
	uint triangle = payload & 127u;
	uint info = geometry.data[record + 65u];
	uint palette_index = 0xffffffffu;
	if ((info >> 25u) != 127u) {
		uint first = ((info >> 18u) & 127u) + 1u;
		uint second = info >> 25u;
		palette_index = triangle < first ? info & 63u : triangle < first + second ? (info >> 6u) & 63u : (info >> 12u) & 63u;
	} else {
		uint count = ((info >> 19u) & 63u) + 1u;
		uint table = base + (info & 0x7ffffu);
		for (uint index = 0u; index < count; ++index) {
			uint range = geometry.data[table + index];
			uint first = range & 255u;
			if (triangle >= first && triangle < first + ((range >> 8u) & 255u)) {
				palette_index = (range >> 16u) & 63u;
				break;
			}
		}
	}
	if (palette_index >= geometry.data[record + 6u]) { return 0xffffffffu; }
	uint model_slot = geometry.data[base + (geometry.data[record + 68u] >> 2u) + palette_index];
	uint map = scene.data[vg_source_word(visible.x) + 47u];
	return scene.data[scene.data[2] + map + model_slot];
}
#endif
