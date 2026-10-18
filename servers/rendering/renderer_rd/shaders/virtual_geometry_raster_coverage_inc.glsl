// Both raster paths evaluate authored fragment coverage before atomic visibility.
// Reconstruct the same source triangle, varyings and clamped deformation as the
// deferred material path. Uncovered software lanes still evaluate as helpers.
#define VG_EVALUATE_VERTEX vg_evaluate_vertex
#include "virtual_geometry_surface_inc.glsl"
#include "virtual_geometry_material_fragment_inc.glsl"

bool vg_raster_coverage(uint payload, vec2 point) {
	vg_setup_vertex(vg_visible_reference((payload >> 7u) - 1u).x);
	VGSurface surface;
	bool valid = vg_surface(payload, point, projection_matrix, vg_view.dimensions.xy, vg_view.dimensions.zw, surface);
#ifdef VG_COMPUTE_FRAGMENT
	valid = subgroupQuadBroadcast(valid, 0u) && subgroupQuadBroadcast(valid, 1u) && subgroupQuadBroadcast(valid, 2u) && subgroupQuadBroadcast(valid, 3u);
#endif
	if (!valid) {
		atomicOr(vg_status.data[0], 2u);
		return false;
	}
	vg_evaluate_fragment(surface, point);
	return !vg_killed;
}
