#ifdef VG_DIRECT_SOURCE_READS
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_buffer_reference_uvec2 : require

// These addresses are supplied only for the current render boundary. Their
// source RIDs are explicit RD compute reads, including transfer dependencies.
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer VGWordSource {
	uint data[];
};
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer VGTransformSource {
	vec4 data[];
};
#endif
