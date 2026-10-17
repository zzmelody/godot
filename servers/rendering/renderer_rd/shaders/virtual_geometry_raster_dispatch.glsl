#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 4, std430) readonly buffer Work { uint data[]; } work;
layout(set = 0, binding = 5, std430) readonly buffer Bins { uint data[]; } bins;
layout(set = 0, binding = 7, std430) writeonly buffer Arguments { uint data[]; } arguments;
layout(push_constant, std430) uniform Phase { uint mode; } phase;

void main() {
	if (gl_GlobalInvocationID.x != 0u) { return; }
	if (phase.mode == 0u) {
		uint count = work.data[7];
		arguments.data[0] = min(65535u, count);
		arguments.data[1] = max(1u, (count + 65534u) / 65535u);
		arguments.data[2] = 1u;
	} else {
		uint software = bins.data[2] == 0u ? bins.data[1] : 0u;
		arguments.data[4] = min(65535u, software);
		arguments.data[5] = max(1u, (software + 65534u) / 65535u);
		arguments.data[6] = 1u;
		arguments.data[8] = 384u;
		arguments.data[9] = bins.data[2] == 0u ? bins.data[0] : 0u;
		arguments.data[10] = 0u;
		arguments.data[11] = 0u;
	}
}
