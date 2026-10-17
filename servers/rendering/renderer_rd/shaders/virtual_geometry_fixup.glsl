#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer Commands {
	uvec4 words[];
} commands;

layout(set = 0, binding = 1, std430) buffer ModelMetadata {
	uint words[];
} metadata;

layout(set = 0, binding = 2, std430) buffer GeometryPool {
	uint words[];
} geometry;

layout(push_constant, std430) uniform Parameters {
	uint count;
} params;

void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= params.count) {
		return;
	}
	uvec4 command = commands.words[index];
	if (command.x == 0u) {
		// Duplicate group-part updates carry the same reference in one phase.
		atomicExchange(metadata.words[command.y], command.z);
	} else if (command.w == 1u) {
		atomicOr(geometry.words[command.y], command.z);
	} else {
		atomicAnd(geometry.words[command.y], ~command.z);
	}
}
