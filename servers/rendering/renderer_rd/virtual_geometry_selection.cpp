// SPDX-License-Identifier: MIT
#include "virtual_geometry_selection.h"

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/utilities.h"

namespace RendererRD {

VirtualGeometrySelection::~VirtualGeometrySelection() {
	for (const Result &result : results) {
		for (RID uniforms : { result.selection_uniforms, result.dispatch_uniforms, result.compact_uniforms }) {
			if (RD::get_singleton()->uniform_set_is_valid(uniforms)) {
				RD::get_singleton()->free_rid(uniforms);
			}
		}
		if (result.work.is_valid()) {
			memory.free(result.work);
		}
		if (result.parameters.is_valid()) {
			memory.free(result.parameters);
		}
		if (result.dispatch.is_valid()) {
			memory.free(result.dispatch);
		}
	}
	if (dummy_transforms.is_valid()) {
		memory.free(dummy_transforms);
	}
	if (scratch.is_valid()) {
		memory.free(scratch);
	}
	pipeline.free();
	dispatch_pipeline.free();
	compact_pipeline.free();
	if (version.is_valid()) {
		shader.version_free(version);
	}
	if (dispatch_version.is_valid()) {
		dispatch_shader.version_free(dispatch_version);
	}
	if (compact_version.is_valid()) {
		compact_shader.version_free(compact_version);
	}
}

void VirtualGeometrySelection::begin(const Transform3D &p_camera, const Projection &p_projection, bool p_flip_y, const Size2i &p_size, uint32_t p_shadow_flags, float p_shadow_far) {
	used = 0;
	batches.clear();
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
	shadow_flags = p_shadow_flags;
	if ((shadow_flags & 4u) != 0u) {
		near_plane = p_shadow_far;
		lod_scale = 0.5f * p_size.y;
	}
}

Error VirtualGeometrySelection::record(bool p_root_only) {
	uint64_t roots_total = 0;
	uint64_t extra_total = 0;
	uint64_t instances_total = 0;
	for (const Batch &batch : batches) {
		if (batch.count == 0) {
			continue;
		}
		const uint64_t roots = uint64_t(batch.count) * batch.geometry.root_clusters;
		ERR_FAIL_COND_V(roots == 0 || roots > MAX_WORK_ITEMS || batch.count > MAX_WORK_ITEMS, ERR_OUT_OF_MEMORY);
		const uint64_t desired = p_root_only ? roots : MIN(uint64_t(MAX_WORK_ITEMS), uint64_t(batch.count) * batch.geometry.clusters);
		ERR_FAIL_COND_V(desired < roots, ERR_INVALID_DATA);
		roots_total += roots;
		instances_total += batch.count;
		extra_total += desired - roots;
	}
	// Reserve complete roots first. The view shares the remaining fine-output
	// capacity instead of reserving a full maximum for every source batch.
	const uint64_t extra_capacity = roots_total < MAX_WORK_ITEMS ? MAX_WORK_ITEMS - roots_total : 0;
	for (const Batch &batch : batches) {
		const uint64_t roots = uint64_t(batch.count) * batch.geometry.root_clusters;
		const uint64_t desired = p_root_only ? roots : MIN(uint64_t(MAX_WORK_ITEMS), uint64_t(batch.count) * batch.geometry.clusters);
		const uint64_t extra = extra_total > extra_capacity ? (desired - roots) * extra_capacity / extra_total : desired - roots;
		const Error error = _record(batch, uint32_t(roots + extra), p_root_only);
		if (error != OK) {
			WARN_PRINT_ONCE(vformat("Virtual geometry selection view refused: batches=%s instances=%s roots=%s batch_index=%s batch_instances=%s selected_capacity=%s retained_bytes=%s root_only=%s.", batches.size(), instances_total, roots_total, used, batch.count, roots + extra, get_buffer_bytes(), p_root_only));
			used = 0;
			return error;
		}
	}
	return OK;
}

Error VirtualGeometrySelection::_record(const Batch &p_batch, uint32_t p_cluster_capacity, bool p_root_only) {
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
	const uint32_t nodes = p_root_only ? 0 : MIN(uint64_t(MAX_WORK_ITEMS), uint64_t(p_batch.count) * p_batch.geometry.nodes);
	const uint32_t clusters = p_cluster_capacity;
	const uint32_t requests = p_root_only ? 0 : MIN(uint64_t(65536), uint64_t(p_batch.count) * p_batch.geometry.pages);
	const uint32_t words = 16 + p_batch.count * 16 + clusters * 2 + roots * 2;
	const uint32_t work_words = 16 + p_batch.count * 16 + nodes * 4 + clusters * 4 + roots * 2 + requests * 2 + 128 * 2;
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
		compact_shader.initialize(modes);
		compact_version = compact_shader.version_create();
		compact_pipeline.create_compute_pipeline(compact_shader.version_get_shader(compact_version, 0));
	}
	if (dummy_transforms.is_null()) {
		dummy_transforms = memory.storage(48);
	}
	const RID program = shader.version_get_shader(version, 0);
	const RID compute = pipeline.get_rid();
	const RID dispatch_program = dispatch_shader.version_get_shader(dispatch_version, 0);
	const RID prepare = dispatch_pipeline.get_rid();
	const RID compact_program = compact_shader.version_get_shader(compact_version, 0);
	const RID compact = compact_pipeline.get_rid();
	ERR_FAIL_COND_V(program.is_null() || compute.is_null() || dispatch_program.is_null() || prepare.is_null() || compact_program.is_null() || compact.is_null() || dummy_transforms.is_null(), ERR_CANT_CREATE);
	const Error scratch_error = memory.ensure(scratch, scratch_words, work_words);
	if (scratch_error != OK) {
		return scratch_error;
	}
	if (used == results.size()) {
		results.push_back(Result());
	}
	Result &result = results[used];
	const Error result_error = memory.ensure(result.work, result.words, words);
	if (result_error != OK) {
		return result_error;
	}
	if (result.parameters.is_null()) {
		result.parameters = memory.uniform(256);
	}
	if (result.dispatch.is_null()) {
		result.dispatch = memory.storage(12, RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
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
	parameters.lod[2] = uint32_t(orthographic) | shadow_flags;
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
	rd->buffer_clear(scratch, 0, 64);
	// Result slots change source identity as views and SDFGI regions change.
	// Own one current binding per slot instead of retaining every combination
	// of live source buffers in the renderer-wide uniform cache.
	const RID transforms = p_batch.stride != 0 ? p_batch.transforms : dummy_transforms;
	const RID previous_transforms = result.batch.stride != 0 ? result.batch.transforms : dummy_transforms;
	if (result.batch.geometry.metadata != p_batch.geometry.metadata || result.batch.geometry.geometry != p_batch.geometry.geometry || previous_transforms != transforms) {
		if (rd->uniform_set_is_valid(result.selection_uniforms)) {
			rd->free_rid(result.selection_uniforms);
		}
	}
	if (!rd->uniform_set_is_valid(result.selection_uniforms)) {
		const Vector<RD::Uniform> uniforms = {
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, { p_batch.geometry.metadata }),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, { p_batch.geometry.geometry }),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, { transforms }),
			RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 3, { result.parameters }),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, { scratch })
		};
		result.selection_uniforms = rd->uniform_set_create(uniforms, program, 0);
	}
	if (!rd->uniform_set_is_valid(result.dispatch_uniforms)) {
		const Vector<RD::Uniform> uniforms = {
			RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 3, { result.parameters }),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, { scratch }),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, { result.dispatch })
		};
		result.dispatch_uniforms = rd->uniform_set_create(uniforms, dispatch_program, 0);
	}
	if (!rd->uniform_set_is_valid(result.compact_uniforms)) {
		const Vector<RD::Uniform> uniforms = {
			RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 3, { result.parameters }),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, { scratch }),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, { result.work })
		};
		result.compact_uniforms = rd->uniform_set_create(uniforms, compact_program, 0);
	}
	ERR_FAIL_COND_V(result.selection_uniforms.is_null() || result.dispatch_uniforms.is_null() || result.compact_uniforms.is_null(), ERR_CANT_CREATE);
	rd->draw_command_begin_label("Virtual Geometry Selection");
	RENDER_TIMESTAMP(p_batch.stride == 0 ? "Virtual Geometry Mesh Begin" : "Virtual Geometry MultiMesh Begin");
	const RD::ComputeListID list = rd->compute_list_begin();
	auto dispatch = [&](uint32_t p_mode, uint32_t p_round, uint32_t p_count, bool p_indirect = false) {
		const bool preparing = p_mode == 1 || p_mode == 3 || p_mode == 8;
		rd->compute_list_bind_compute_pipeline(list, preparing ? prepare : compute);
		rd->compute_list_bind_uniform_set(list, preparing ? result.dispatch_uniforms : result.selection_uniforms, 0);
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
	if (!p_root_only) {
		for (uint32_t depth = 0; depth < p_batch.geometry.hierarchy_depth; ++depth) {
			dispatch(1, depth, 1);
			dispatch(2, depth, 0, true);
		}
		dispatch(3, 0, 1);
		dispatch(4, 0, 0, true);
	}
	dispatch(5, 0, p_batch.count * p_batch.geometry.root_pages);
	dispatch(p_root_only ? 7 : 6, p_root_only ? 0 : p_batch.geometry.hierarchy_depth - 1, 1);
	dispatch(8, 0, 1);
	rd->compute_list_bind_compute_pipeline(list, compact);
	rd->compute_list_bind_uniform_set(list, result.compact_uniforms, 0);
	const uint32_t root_only = 0;
	rd->compute_list_set_push_constant(list, &root_only, sizeof(root_only));
	rd->compute_list_dispatch_indirect(list, result.dispatch, 0);
	rd->compute_list_end();
	const uint32_t request_word = 16 + p_batch.count * 16 + nodes * 4 + clusters * 4 + roots * 2;
	const Error feedback_error = p_root_only ? OK : MeshStorage::get_singleton()->record_virtual_geometry_requests(p_batch.mesh, p_batch.geometry.generation, scratch, request_word, requests);
	if (feedback_error != OK && feedback_error != ERR_BUSY) {
		WARN_PRINT_ONCE("Virtual geometry page feedback is unavailable; retaining current resident coverage.");
	}
	rd->draw_command_end_label();
	RENDER_TIMESTAMP(p_batch.stride == 0 ? "Virtual Geometry Mesh End" : "Virtual Geometry MultiMesh End");
	result.batch = p_batch;
	result.selected_capacity = clusters;
	result.root_capacity = roots;
	++used;
	return OK;
}

void VirtualGeometrySelection::use_root_coverage() {
	RD *rd = RD::get_singleton();
	const RID program = compact_shader.version_get_shader(compact_version, 0);
	for (uint32_t index = 0; index < used; ++index) {
		Result &result = results[index];
		// A later batch can grow the shared scratch and invalidate this binding.
		// Root-only compaction reads the retained result, not scratch contents.
		if (!rd->uniform_set_is_valid(result.compact_uniforms)) {
			const Vector<RD::Uniform> uniforms = {
				RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 3, { result.parameters }),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, { scratch }),
				RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, { result.work })
			};
			result.compact_uniforms = rd->uniform_set_create(uniforms, program, 0);
		}
		ERR_FAIL_COND(!rd->uniform_set_is_valid(result.compact_uniforms));
		const RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, compact_pipeline.get_rid());
		rd->compute_list_bind_uniform_set(list, result.compact_uniforms, 0);
		const uint32_t root_only = 1;
		rd->compute_list_set_push_constant(list, &root_only, sizeof(root_only));
		rd->compute_list_dispatch(list, 1, 1, 1);
		rd->compute_list_end();
		result.selected_capacity = result.root_capacity;
	}
}

uint64_t VirtualGeometrySelection::get_buffer_bytes() const {
	uint64_t bytes = 0;
	for (const Result &result : results) {
		bytes += (result.work.is_valid() ? uint64_t(result.words) * 4 : 0) + (result.parameters.is_valid() ? 256 : 0) + (result.dispatch.is_valid() ? 12 : 0);
	}
	return bytes + (scratch.is_valid() ? uint64_t(scratch_words) * 4 : 0) + (dummy_transforms.is_valid() ? 48 : 0);
}

} // namespace RendererRD
