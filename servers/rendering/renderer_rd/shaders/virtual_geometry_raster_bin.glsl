#[compute]

#version 450

#VERSION_DEFINES

#define VG_BIN_BUILD
#include "virtual_geometry_raster_inc.glsl"
#define VG_SCENE_WRITE
#include "virtual_geometry_scene_inc.glsl"

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
shared uint maximum_edge;
shared uint needs_hardware;

void main() {
	uint index = gl_WorkGroupID.x + gl_WorkGroupID.y * 65535u;
	if (index >= work.data[7]) { return; }
	uint lane = gl_LocalInvocationIndex;
	uvec2 selected = vg_selected(index);
	uint record = vg_record(selected.y);
	if (lane == 0u) { maximum_edge = 0u; needs_hardware = 0u; }
	barrier();
	for (uint triangle = lane; triangle < geometry.data[record + 5u]; triangle += 64u) {
		uvec3 vertices = vg_triangle(selected.y, triangle);
		vec3 points[3];
		vec2 lower = vec2(3.402823e38);
		vec2 upper = vec2(-3.402823e38);
		for (uint corner = 0u; corner < 3u; ++corner) {
			points[corner] = vg_position(selected.y, vertices[corner]);
			vec4 clip = vg_clip(selected.x, selected.y, vertices[corner]);
			if (clip.w <= 0.0 || clip.z <= 0.0 || clip.z >= clip.w || any(isnan(clip)) || any(isinf(clip))) {
				atomicOr(needs_hardware, 1u);
			} else {
				vec2 pixel = vg_screen(clip);
				lower = min(lower, pixel);
				upper = max(upper, pixel);
				if (any(greaterThan(abs(pixel), vec2(1048576.0)))) { atomicOr(needs_hardware, 1u); }
			}
		}
		float edge = max(max(length(points[1] - points[0]), length(points[2] - points[1])), length(points[0] - points[2]));
		atomicMax(maximum_edge, floatBitsToUint(edge));
		// Software setup is bounded. Crossing a clip plane or a larger screen
		// extent routes the entire cluster to hardware before any pixel writes.
		if (any(greaterThan(upper - lower, vec2(63.0)))) { atomicOr(needs_hardware, 1u); }
	}
	barrier();
	if (lane == 0u) {
		uint visible_word = scene.data[1] + (raster_info.raster.z + index) * 2u;
		scene.data[visible_word] = raster_info.source_base + selected.x;
		scene.data[visible_word + 1u] = selected.y;
		vec4 sphere = uintBitsToFloat(uvec4(geometry.data[record + 12u], geometry.data[record + 13u], geometry.data[record + 14u], geometry.data[record + 15u]));
		float scale = uintBitsToFloat(work.data[instance_base() + selected.x * 16u + 13u]);
		// UE SmallEnoughToDraw hardware threshold, default MinPixelsPerEdgeHW=32.
		// Prototype derives maximum edge from decoded geometry on the GPU;
		// it is not a per-cluster CPU draw or a stored UE half-float field.
		bool hardware = needs_hardware != 0u || projected_edge(selected.x, sphere, instance_transform(selected.x)) <
				scale * uintBitsToFloat(maximum_edge) * params.lod.y / 32.0;
		uint count_word = hardware ? 0u : 1u;
		uint position = atomicAdd(bins.data[count_word], 1u);
		if (position < raster_info.raster.w) {
			bins.data[4u + (hardware ? 0u : raster_info.raster.w) + position] = index;
		} else { atomicOr(bins.data[2], 1u); }
	}
}
