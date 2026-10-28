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
layout(set = 0, binding = 8, std430) buffer RequestWork { uint data[]; } request_work;
#include "virtual_geometry_selection_parameters_inc.glsl"
void main() {
	uint id = (gl_WorkGroupID.x + gl_WorkGroupID.y * 65535u) * 64u + gl_LocalInvocationIndex;
	if (phase.mode == 1u && id < params.source.x) {
		// The prior node dispatch has finished. Word13 is a view-local derived
		// saturation observation, stable throughout the next node dispatch.
		vg_batch_index = id;
		VG_SELECTION_REQUEST(13u) = VG_SELECTION_REQUEST(12u) >= 128u ? 1u : 0u;
	}
	if (id != 0u) { return; }
	uint count;
	if (phase.mode == 1u) {
		uint parity = phase.round & 1u;
		if (phase.round == 0u) {
			// Initialization directly addresses the first frontier by instance ID.
			work.data[0u] = min(params.source.y, params.limits.x);
		}
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
