#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
#ifdef SOURCE_MSAA
layout(set = 0, binding = 0) uniform sampler2DMS source_depth;
#else
layout(set = 0, binding = 0) uniform sampler2D source_depth;
#endif
layout(r32f, set = 0, binding = 1) uniform writeonly image2D destination;

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	ivec2 dst_size = imageSize(destination);
	if (any(greaterThanEqual(pixel, dst_size))) {
		return;
	}
#ifdef SOURCE_MSAA
	ivec2 src_size = textureSize(source_depth);
#else
	ivec2 src_size = textureSize(source_depth, 0);
#endif
	// Overlapping footprints also cover odd dimensions. No edge texel can be
	// omitted, and every MSAA sample participates in conservative reduction.
	ivec2 lo = pixel * src_size / dst_size;
	ivec2 hi = ((pixel + 1) * src_size + dst_size - 1) / dst_size;
	float depth = 1.0;
	for (int y = lo.y; y < hi.y; ++y) {
		for (int x = lo.x; x < hi.x; ++x) {
#ifdef SOURCE_MSAA
			for (int sample_index = 0; sample_index < textureSamples(source_depth); ++sample_index) {
				depth = min(depth, texelFetch(source_depth, ivec2(x, y), sample_index).r);
			}
#else
			depth = min(depth, texelFetch(source_depth, ivec2(x, y), 0).r);
#endif
		}
	}
	imageStore(destination, pixel, vec4(depth));
}
