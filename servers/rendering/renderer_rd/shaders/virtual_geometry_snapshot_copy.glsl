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
	uint first_tile;
};

layout(push_constant, std430) uniform Parameters {
	uint job_count;
	uint tile_count;
}
parameters;

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

shared uint selected_job;

void main() {
	uint tile = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
	if (tile >= parameters.tile_count) {
		return;
	}
	// One job describes a whole source. Tiles keep large sources parallel
	// without replicating their device addresses in a per-tile upload table.
	if (gl_LocalInvocationIndex == 0u) {
		uint low = 0u;
		uint high = parameters.job_count;
		while (low + 1u < high) {
			uint middle = low + (high - low) / 2u;
			if (jobs.data[middle].first_tile <= tile) {
				low = middle;
			} else {
				high = middle;
			}
		}
		selected_job = low;
	}
	barrier();
	CopyJob job = jobs.data[selected_job];
	uint first_word = (tile - job.first_tile) * 1024u;
	uint end_word = first_word + min(1024u, job.words - first_word);
	WordReference source = WordReference(job.source);
	for (uint word = first_word + gl_LocalInvocationIndex; word < end_word; word += 64u) {
		uint value = source.data[word];
		if (job.output_index == 0u) {
			first_output.data[job.destination_word + word] = value;
		} else {
			second_output.data[job.destination_word + word] = value;
		}
	}
}
