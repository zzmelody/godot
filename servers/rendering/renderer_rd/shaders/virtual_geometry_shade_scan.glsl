#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 128, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 9, std430) buffer Shading { uint data[]; } shading;
layout(push_constant, std430) uniform ScanParameters {
	uint mode;
	uint count;
	uint input_word;
	uint output_word;
	uint sums_word;
} params;
shared uint values[128];

void main() {
	uint block = gl_WorkGroupID.x + gl_WorkGroupID.y * 65535u;
	uint lane = gl_LocalInvocationIndex;
	uint index = block * 128u + lane;
	if (params.mode != 0u) {
		if (index < params.count) { shading.data[params.output_word + index] += shading.data[params.input_word + block]; }
		return;
	}
	if (block >= (params.count + 127u) / 128u) { return; }
	uint original = index < params.count ? shading.data[params.input_word + index] : 0u;
	values[lane] = original;
	barrier();
	for (uint stride = 1u; stride < 128u; stride *= 2u) {
		uint added = lane >= stride ? values[lane - stride] : 0u;
		barrier();
		values[lane] += added;
		barrier();
	}
	if (index < params.count) { shading.data[params.output_word + index] = values[lane] - original; }
	if (lane == 127u) {
		if (params.sums_word == 0xffffffffu) { shading.data[0] = values[lane]; }
		else { shading.data[params.sums_word + block] = values[lane]; }
	}
}
