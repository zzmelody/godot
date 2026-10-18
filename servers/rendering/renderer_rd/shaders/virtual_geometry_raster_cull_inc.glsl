#ifndef VIRTUAL_GEOMETRY_RASTER_CULL_INCLUDED
#define VIRTUAL_GEOMETRY_RASTER_CULL_INCLUDED
#ifdef VG_RASTER_MATERIALS
#define VG_MATERIAL_LOOKUP
#include "virtual_geometry_scene_inc.glsl"
layout(set = 0, binding = 11, std430) readonly buffer RasterMaterials {
	uint data[];
}
raster_materials;
bool vg_cull_material(uint payload, bool front_facing) {
	uint material = vg_material(payload);
	// Invalid admitted geometry is diagnosed by the later bin validation. Do
	// not read beyond the material table or hide its visibility payload here.
	if (material >= scene.data[3]) {
		return false;
	}
	uint mode = raster_materials.data[material * 4u];
	return (mode == 1u && !front_facing) || (mode == 2u && front_facing);
}
#else
bool vg_cull_material(uint payload, bool front_facing) {
	return false;
}
#endif

bool vg_raster_material_active(uvec2 selected, uint triangle) {
#ifdef VG_RASTER_MATERIALS
	uint material = vg_material_reference(uvec2(
#ifdef VG_GLOBAL_RASTER
												  selected.x,
#else
												  raster_info.source_base + selected.x,
#endif
												  selected.y),
			triangle);
	if (raster_info.material != 0xffffffffu) {
		return material == raster_info.material;
	}
	return material >= scene.data[3] || raster_materials.data[material * 4u + 2u] == 0u;
#else
	return true;
#endif
}

#endif
