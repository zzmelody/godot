#[compute]

#version 450

#VERSION_DEFINES

#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_buffer_reference_uvec2 : require

// clang-format off
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer WordReference {
	uint data[];
};
// clang-format on

struct CopyJob {
	uvec2 source;
	uint destination_word;
	uint words;
	uint output_index;
	uint padding;
};

layout(set = 0, binding = 0, std430) readonly buffer Jobs {
	CopyJob data[];
}
jobs;
layout(set = 0, binding = 1, std430) writeonly buffer FirstOutput {
	uint data[];
}
first_output;
layout(set = 0, binding = 2, std430) writeonly buffer SecondOutput {
	uint data[];
}
second_output;
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

void main() {
	CopyJob job = jobs.data[gl_WorkGroupID.x];
	WordReference source = WordReference(job.source);
	for (uint word = gl_LocalInvocationIndex; word < job.words; word += 64u) {
		uint value = source.data[word];
		if (job.output_index == 0u) {
			first_output.data[job.destination_word + word] = value;
		} else {
			second_output.data[job.destination_word + word] = value;
		}
	}
}
