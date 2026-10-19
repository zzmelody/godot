#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 3, std140) uniform Parameters {
	mat4 model;
	mat4 world_to_view;
	mat4 projection;
	vec4 lod;
	uvec4 source;
	uvec4 limits;
	uvec4 offsets;
} params;
layout(set = 0, binding = 4, std430) buffer Work { uint data[]; } work;
layout(set = 0, binding = 5, std430) writeonly buffer Dispatch { uint groups[]; } dispatch;
layout(push_constant, std430) uniform Phase { uint batch; uint mode; uint round; } phase;
void main() {
	if (gl_GlobalInvocationID.x != 0u) { return; }
	uint count;
	if (phase.mode == 1u) {
		uint parity = phase.round & 1u;
		work.data[1u - parity] = 0u;
		count = min(work.data[parity], params.limits.x);
	} else if (phase.mode == 11u) {
		count = min(work.data[1u - (phase.round & 1u)], params.limits.x);
	} else {
		count = min(work.data[2u], params.limits.y);
	}
	uint groups = max(1u, (count + 63u) / 64u);
	dispatch.groups[0] = min(groups, 65535u);
	dispatch.groups[1] = max(1u, (groups + 65534u) / 65535u);
	dispatch.groups[2] = 1u;
}
