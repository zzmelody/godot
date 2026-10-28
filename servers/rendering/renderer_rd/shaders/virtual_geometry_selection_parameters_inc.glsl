// Immutable per-view source records. Work items carry source indices.
struct SelectionParameters {
	mat4 model;
	uvec4 source; // count, vec4 stride, vec4 transform start, root pages
	uvec4 limits; // reserved, selected capacity, root capacity, request capacity
	uvec4 offsets; // absolute parts, page table, groups, displacement bits
	uvec4 addresses; // metadata, feedback header, result header, first instance
	uvec4 prefix; // first root-page task, depth, reserved, reserved
};
layout(set = 0, binding = 7, std430) readonly buffer SelectionInputs { SelectionParameters data[]; } selection_inputs;
layout(push_constant, std430) uniform Phase {
	uint batch;
	uint mode;
	uint round;
} phase;
uint vg_batch_index;
#define VG_SELECTION_INPUT selection_inputs.data[vg_batch_index]
#define VG_SELECTION_WORK(index) result.data[VG_SELECTION_INPUT.addresses.z + (index)]
#define VG_SELECTION_REQUEST(index) request_work.data[VG_SELECTION_INPUT.addresses.y + (index)]
uint vg_selection_mode() { return phase.mode; }
