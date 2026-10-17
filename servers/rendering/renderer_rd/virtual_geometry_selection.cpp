// SPDX-License-Identifier: MIT
#include "virtual_geometry_selection.h"

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/utilities.h"

namespace RendererRD {

VirtualGeometrySelection::~VirtualGeometrySelection() {
	for (const Result &result : results) {
		if (result.work.is_valid()) {
			RD::get_singleton()->free_rid(result.work);
		}
		if (result.parameters.is_valid()) {
			RD::get_singleton()->free_rid(result.parameters);
		}
		if (result.dispatch.is_valid()) {
			RD::get_singleton()->free_rid(result.dispatch);
		}
	}
	if (dummy_transforms.is_valid()) {
		RD::get_singleton()->free_rid(dummy_transforms);
	}
	pipeline.free();
	dispatch_pipeline.free();
	if (version.is_valid()) {
		shader.version_free(version);
	}
	if (dispatch_version.is_valid()) {
		dispatch_shader.version_free(dispatch_version);
	}
}

void VirtualGeometrySelection::begin(const Transform3D &p_camera, const Projection &p_projection, bool p_flip_y, const Size2i &p_size) {
	used = 0;
	camera_inverse = p_camera.affine_inverse();
	Projection correction;
	correction.set_depth_correction(p_flip_y);
	projection = correction * p_projection;
	near_plane = p_projection.get_z_near();
	// UE FPackedView::UpdateLODScales, with a one-pixel error target. The
	// unjittered projection and actual internal render height define this view.
	lod_scale = 0.5f * Math::abs(p_projection.columns[1][1]) * p_size.y;
	frustum_margin = 2.0f / MAX(1, MIN(p_size.x, p_size.y));
	orthographic = p_projection.is_orthogonal();
}

Error VirtualGeometrySelection::record(const Batch &p_batch) {
	if (p_batch.count == 0) {
		return OK;
	}
	ERR_FAIL_COND_V(p_batch.geometry.metadata.is_null() || p_batch.geometry.geometry.is_null() ||
					(p_batch.stride != 0 && (p_batch.stride < 3 || p_batch.transforms.is_null())) ||
					!p_batch.model.is_finite() || p_batch.model.basis.determinant() == 0 || lod_scale <= 0 ||
					!Math::is_finite(p_batch.max_vertex_displacement) || p_batch.max_vertex_displacement < 0,
			ERR_INVALID_PARAMETER);
	// Root coverage is reserved before any dispatch. Refuse the whole batch
	// when even that complete cut cannot fit; never emit a truncated list.
	const uint64_t roots = uint64_t(p_batch.count) * p_batch.geometry.root_clusters;
	ERR_FAIL_COND_V(roots == 0 || roots > MAX_WORK_ITEMS || p_batch.count > MAX_WORK_ITEMS, ERR_OUT_OF_MEMORY);
	const uint32_t nodes = MIN(uint64_t(MAX_WORK_ITEMS), uint64_t(p_batch.count) * p_batch.geometry.nodes);
	const uint32_t clusters = MIN(uint64_t(MAX_WORK_ITEMS), uint64_t(p_batch.count) * p_batch.geometry.clusters);
	const uint32_t requests = MIN(uint64_t(65536), uint64_t(p_batch.count) * p_batch.geometry.pages);
	const uint32_t words = 16 + p_batch.count * 16 + nodes * 4 + clusters * 4 + roots * 2 + requests * 2;
	RD *rd = RD::get_singleton();
	if (version.is_null()) {
		Vector<String> modes;
		modes.push_back("");
		shader.initialize(modes);
		version = shader.version_create();
		pipeline.create_compute_pipeline(shader.version_get_shader(version, 0));
		dispatch_shader.initialize(modes);
		dispatch_version = dispatch_shader.version_create();
		dispatch_pipeline.create_compute_pipeline(dispatch_shader.version_get_shader(dispatch_version, 0));
		dummy_transforms = rd->storage_buffer_create(48);
	}
	const RID program = shader.version_get_shader(version, 0);
	const RID compute = pipeline.get_rid();
	const RID dispatch_program = dispatch_shader.version_get_shader(dispatch_version, 0);
	const RID prepare = dispatch_pipeline.get_rid();
	ERR_FAIL_COND_V(program.is_null() || compute.is_null() || dispatch_program.is_null() || prepare.is_null() || dummy_transforms.is_null(), ERR_CANT_CREATE);
	if (used == results.size()) {
		results.push_back(Result());
	}
	Result &result = results[used];
	if (result.words < words) {
		if (result.work.is_valid()) {
			rd->free_rid(result.work);
		}
		result.words = Math::next_power_of_2(words);
		result.work = rd->storage_buffer_create(result.words * 4);
	}
	if (result.parameters.is_null()) {
		result.parameters = rd->uniform_buffer_create(256);
	}
	if (result.dispatch.is_null()) {
		result.dispatch = rd->storage_buffer_create(12, {}, RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	}
	ERR_FAIL_COND_V(result.work.is_null() || result.parameters.is_null() || result.dispatch.is_null(), ERR_CANT_CREATE);
	struct Parameters {
		float model[16];
		float view[16];
		float projection[16];
		float lod[4];
		uint32_t source[4];
		uint32_t limits[4];
		uint32_t offsets[4];
	} parameters = {};
	static_assert(sizeof(Parameters) == 256);
	MaterialStorage::store_transform(p_batch.model, parameters.model);
	MaterialStorage::store_transform(camera_inverse, parameters.view);
	MaterialStorage::store_camera(projection, parameters.projection);
	parameters.lod[0] = near_plane;
	parameters.lod[1] = lod_scale;
	parameters.lod[2] = orthographic;
	parameters.lod[3] = frustum_margin;
	parameters.source[0] = p_batch.count;
	parameters.source[1] = p_batch.stride;
	parameters.source[2] = p_batch.transform_offset;
	parameters.source[3] = p_batch.geometry.root_pages;
	parameters.limits[0] = nodes;
	parameters.limits[1] = clusters;
	parameters.limits[2] = roots;
	parameters.limits[3] = requests;
	parameters.offsets[0] = p_batch.geometry.parts_word;
	parameters.offsets[1] = p_batch.geometry.page_table_word;
	parameters.offsets[2] = p_batch.geometry.groups_word;
	memcpy(&parameters.offsets[3], &p_batch.max_vertex_displacement, sizeof(float));
	rd->buffer_update(result.parameters, 0, sizeof(parameters), &parameters);
	rd->buffer_clear(result.work, 0, 64);
	const RID uniforms = UniformSetCacheRD::get_singleton()->get_cache(program, 0,
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, { p_batch.geometry.metadata }),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, { p_batch.geometry.geometry }),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, { p_batch.stride != 0 ? p_batch.transforms : dummy_transforms }),
			RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 3, { result.parameters }),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, { result.work }));
	ERR_FAIL_COND_V(uniforms.is_null(), ERR_CANT_CREATE);
	const RID dispatch_uniforms = UniformSetCacheRD::get_singleton()->get_cache(dispatch_program, 0,
			RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 3, { result.parameters }),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, { result.work }),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, { result.dispatch }));
	ERR_FAIL_COND_V(dispatch_uniforms.is_null(), ERR_CANT_CREATE);
	rd->draw_command_begin_label("Virtual Geometry Selection");
	RENDER_TIMESTAMP(p_batch.stride == 0 ? "Virtual Geometry Mesh Begin" : "Virtual Geometry MultiMesh Begin");
	const RD::ComputeListID list = rd->compute_list_begin();
	auto dispatch = [&](uint32_t p_mode, uint32_t p_round, uint32_t p_count, bool p_indirect = false) {
		const bool preparing = p_mode == 1 || p_mode == 3;
		rd->compute_list_bind_compute_pipeline(list, preparing ? prepare : compute);
		rd->compute_list_bind_uniform_set(list, preparing ? dispatch_uniforms : uniforms, 0);
		const uint32_t phase[2] = { p_mode, p_round };
		rd->compute_list_set_push_constant(list, phase, sizeof(phase));
		if (p_indirect) {
			rd->compute_list_dispatch_indirect(list, result.dispatch, 0);
		} else {
			rd->compute_list_dispatch(list, MAX(1u, (p_count + 63) / 64), 1, 1);
		}
		rd->compute_list_add_barrier(list);
	};
	dispatch(0, 0, p_batch.count);
	for (uint32_t depth = 0; depth <= VirtualGeometry::MAX_HIERARCHY_DEPTH; ++depth) {
		dispatch(1, depth, 1);
		dispatch(2, depth, 0, true);
	}
	dispatch(3, 0, 1);
	dispatch(4, 0, 0, true);
	dispatch(5, 0, p_batch.count * p_batch.geometry.root_pages);
	dispatch(6, VirtualGeometry::MAX_HIERARCHY_DEPTH, 1);
	rd->compute_list_end();
	rd->draw_command_end_label();
	RENDER_TIMESTAMP(p_batch.stride == 0 ? "Virtual Geometry Mesh End" : "Virtual Geometry MultiMesh End");
	result.batch = p_batch;
	result.selected_capacity = clusters;
	++used;
	return OK;
}

uint64_t VirtualGeometrySelection::get_buffer_bytes() const {
	uint64_t bytes = 0;
	for (const Result &result : results) {
		bytes += uint64_t(result.words) * 4 + 268;
	}
	return bytes + (dummy_transforms.is_valid() ? 48 : 0);
}

} // namespace RendererRD
