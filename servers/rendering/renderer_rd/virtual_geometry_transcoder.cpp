#include "virtual_geometry_transcoder.h"

#include "core/io/virtual_geometry/page_codec.h"
#include "core/io/virtual_geometry_page_directory.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

namespace RendererRD {

VirtualGeometryTranscoder::~VirtualGeometryTranscoder() {
	pipeline.free();
	if (shader_version.is_valid()) {
		shader.version_free(shader_version);
	}
}

Error VirtualGeometryTranscoder::record(RID p_source, RID p_destination, const Vector<Page> &p_pages) {
	if (p_pages.is_empty()) {
		return OK;
	}
	ERR_FAIL_COND_V(p_source.is_null() || p_destination.is_null() || p_source == p_destination, ERR_INVALID_PARAMETER);
	for (const Page &page : p_pages) {
		ERR_FAIL_COND_V((page.source_byte & 3) != 0 || (page.destination_byte & 3) != 0 ||
						(page.gpu_bytes & 3) != 0 || page.clusters == 0 || page.clusters > 256 ||
						page.gpu_bytes < VirtualGeometry::GEOMETRY_PAGE_HEADER_BYTES + page.clusters * VirtualGeometry::GEOMETRY_PAGE_CLUSTER_BYTES ||
						page.gpu_bytes > VirtualGeometry::STREAMING_PAGE_GPU_CAPACITY,
				ERR_INVALID_PARAMETER);
	}
	if (shader_version.is_null()) {
		Vector<String> versions;
		versions.push_back("");
		shader.initialize(versions);
		shader_version = shader.version_create();
		const RID program = shader.version_get_shader(shader_version, 0);
		ERR_FAIL_COND_V(program.is_null(), ERR_CANT_CREATE);
		pipeline.create_compute_pipeline(program);
	}
	const RID program = shader.version_get_shader(shader_version, 0);
	const RID pipeline_rid = pipeline.get_rid();
	ERR_FAIL_COND_V(program.is_null() || pipeline_rid.is_null(), ERR_CANT_CREATE);
	const RD::Uniform source(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, { p_source });
	const RD::Uniform destination(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, { p_destination });
	const RID uniforms = UniformSetCacheRD::get_singleton()->get_cache(program, 0, source, destination);
	ERR_FAIL_COND_V(uniforms.is_null(), ERR_CANT_CREATE);
	const RD::ComputeListID commands = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(commands, pipeline_rid);
	RD::get_singleton()->compute_list_bind_uniform_set(commands, uniforms, 0);
	for (uint32_t phase = 0; phase < 2; ++phase) {
		if (phase != 0) {
			RD::get_singleton()->compute_list_add_barrier(commands);
		}
		for (const Page &page : p_pages) {
			const uint32_t parameters[5] = { page.source_byte / 4, page.destination_byte / 4, page.clusters, page.gpu_bytes, phase };
			RD::get_singleton()->compute_list_set_push_constant(commands, parameters, sizeof(parameters));
			RD::get_singleton()->compute_list_dispatch(commands, page.clusters, 1, 1);
		}
	}
	RD::get_singleton()->compute_list_end();
	return OK;
}

} // namespace RendererRD
