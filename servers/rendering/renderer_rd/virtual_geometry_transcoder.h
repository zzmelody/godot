#ifndef VIRTUAL_GEOMETRY_TRANSCODER_RD_H
#define VIRTUAL_GEOMETRY_TRANSCODER_RD_H

#include "servers/rendering/renderer_rd/pipeline_deferred_rd.h"
#include "servers/rendering/renderer_rd/shaders/virtual_geometry_transcode.glsl.gen.h"

namespace RendererRD {

// Rendering-thread recorder. Staging buffers, pool allocations and their GPU
// completion lifetimes belong to the page installer, not this shader owner.
class VirtualGeometryTranscoder {
	VirtualGeometryTranscodeShaderRD shader;
	RID shader_version;
	PipelineDeferredRD pipeline;

public:
	struct Page {
		uint32_t source_byte = 0;
		uint32_t destination_byte = 0;
		uint32_t clusters = 0;
		uint32_t gpu_bytes = 0;
	};

	~VirtualGeometryTranscoder();

	// Borrows publication-validated pages and distinct staging/pool buffers.
	// The installer supplies aligned, disjoint destination ranges inside its
	// allocations and retains both buffers until GPU completion. Records graph
	// work only: OK does not mean a page is ready, and never releases a lease.
	Error record(RID p_source, RID p_destination, const Vector<Page> &p_pages);
};

} // namespace RendererRD

#endif // VIRTUAL_GEOMETRY_TRANSCODER_RD_H
