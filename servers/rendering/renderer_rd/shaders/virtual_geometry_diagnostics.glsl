#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0, std430) readonly buffer ViewStatus { uint data[]; } view_status;
layout(set = 0, binding = 1, std430) readonly buffer MaterialStatus { uint data[]; } material_status;
layout(set = 0, binding = 2, std430) buffer Diagnostics { uint data[]; } diagnostics;

void main() {
	atomicOr(diagnostics.data[0], view_status.data[0]);
	atomicOr(diagnostics.data[1], material_status.data[1]);
}
