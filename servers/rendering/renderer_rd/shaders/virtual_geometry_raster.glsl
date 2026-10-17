#[vertex]

#version 450

#VERSION_DEFINES

#include "virtual_geometry_raster_inc.glsl"

layout(location = 0) flat out uint pixel_value;

void main() {
	uint index = bins.data[4u + uint(gl_InstanceIndex)];
	uvec2 selected = vg_selected(index);
	uint triangle = uint(gl_VertexIndex) / 3u;
	pixel_value = ((raster_info.raster.z + index + 1u) << 7u) | triangle;
	if (triangle >= geometry.data[vg_record(selected.y) + 5u]) {
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		return;
	}
	uvec3 vertices = vg_triangle(selected.y, triangle);
	vec4 clip = vg_clip(selected.x, selected.y, vertices[uint(gl_VertexIndex) % 3u]);
	gl_Position = vg_snap_clip(clip, vec2(raster_info.raster.xy));
}

#[fragment]

#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_shader_atomic_int64 : require

#VERSION_DEFINES

layout(push_constant, std430) uniform RasterParameters {
	uvec4 raster;
	vec2 jitter;
	uint source_base;
}
raster_info;
layout(location = 0) flat in uint pixel_value;

#ifdef VG_RASTER_MATERIALS
layout(set = 0, binding = 1, std430) readonly buffer Geometry {
	uint data[];
}
geometry;
#endif
#include "virtual_geometry_raster_cull_inc.glsl"
#include "virtual_geometry_write_inc.glsl"

void main() {
	if (vg_cull_material(pixel_value, gl_FrontFacing)) {
		discard;
	}
	vg_write_pixel(ivec2(gl_FragCoord.xy), gl_FragCoord.z, pixel_value);
}
