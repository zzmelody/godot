#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 0, r32f) uniform image2D reactivity;
layout(set = 0, binding = 1) uniform sampler2D scene_depth;
layout(set = 0, binding = 2) uniform sampler2D scene_reactivity;
layout(set = 0, binding = 3, std430) readonly buffer Visibility {
	uvec2 data[];
}
visibility;
layout(push_constant, std430) uniform Parameters {
	uint combine_scene;
}
params;

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = imageSize(reactivity);
	if (any(greaterThanEqual(pixel, size))) {
		return;
	}
	uvec2 visible = visibility.data[pixel.y * size.x + pixel.x];
	float value = 0.0;
	if (visible.x != 0u && uintBitsToFloat(visible.y) + 0.000001 >= texelFetch(scene_depth, pixel, 0).r) {
		value = imageLoad(reactivity, pixel).r;
	}
	if (params.combine_scene != 0u) {
		value = max(value, texelFetch(scene_reactivity, pixel, 0).r);
	}
	imageStore(reactivity, pixel, vec4(value));
}
