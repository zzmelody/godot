#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 9, std430) readonly buffer Shading {
	uint data[];
}
shading;
layout(set = 0, binding = 10, std430) writeonly buffer Arguments {
	uint data[];
}
arguments;
layout(set = 0, binding = 11, std430) writeonly buffer DrawArguments {
	uint data[];
}
draw_arguments;
layout(push_constant, std430) uniform Parameters {
	uint materials;
}
params;

void main() {
	uint material = (gl_WorkGroupID.x + gl_WorkGroupID.y * 65535u) * 64u + gl_LocalInvocationIndex;
	if (material >= params.materials) {
		return;
	}
	uint groups = (shading.data[2u + material] + 15u) / 16u;
	arguments.data[material * 3u] = shading.data[1] == 0u ? min(65535u, groups) : 0u;
	arguments.data[material * 3u + 1u] = max(1u, (groups + 65534u) / 65535u);
	arguments.data[material * 3u + 2u] = 1u;
	draw_arguments.data[material * 4u] = shading.data[1] == 0u ? 6u : 0u;
	draw_arguments.data[material * 4u + 1u] = shading.data[1] == 0u ? shading.data[2u + material] : 0u;
	draw_arguments.data[material * 4u + 2u] = 0u;
	draw_arguments.data[material * 4u + 3u] = 0u;
}
