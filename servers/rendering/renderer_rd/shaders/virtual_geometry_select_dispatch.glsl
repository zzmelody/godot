#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
// Same 256-byte parameter block as selection. Only the limits vector is read.
layout(set = 0, binding = 3, std140) uniform Parameters { uvec4 data[16]; } params;
layout(set = 0, binding = 4, std430) buffer Work { uint data[]; } work;
layout(set = 0, binding = 5, std430) writeonly buffer Dispatch { uint groups[3]; } dispatch;
layout(push_constant, std430) uniform Phase { uint mode; uint round; } phase;

void main() {
	if (gl_GlobalInvocationID.x != 0u) { return; }
	uint count;
	if (phase.mode == 1u) {
		uint parity = phase.round & 1u;
		work.data[1u - parity] = 0u;
		count = min(work.data[parity], params.data[14].x);
	} else if (phase.mode == 8u) {
		count = max(params.data[13].x * 16u, max(min(work.data[3], params.data[14].y) * 2u, min(work.data[6], params.data[14].z) * 2u));
	} else {
		count = min(work.data[2], params.data[14].y);
	}
	uint groups = max(1u, (count + 63u) / 64u);
	dispatch.groups[0] = min(groups, 65535u);
	dispatch.groups[1] = max(1u, (groups + 65534u) / 65535u);
	dispatch.groups[2] = 1u;
}
