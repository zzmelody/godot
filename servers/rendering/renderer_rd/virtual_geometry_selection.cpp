// SPDX-License-Identifier: MIT
#include "virtual_geometry_selection.h"

#include "core/profiling/profiling.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/utilities.h"

namespace RendererRD {

VirtualGeometrySelection::~VirtualGeometrySelection() {
	for (RID buffer : { scratch, result_buffer, metadata_buffer, transforms_buffer, feedback_buffer, dispatch_buffer, dispatch_inputs, view_parameters, source_addresses }) {
		if (buffer.is_valid()) {
			memory.free(buffer);
		}
	}
	for (auto &variant : pipelines) {
		for (auto &pipeline : variant) {
			pipeline.free();
		}
	}
	dispatch_pipeline.free();
	if (version.is_valid()) {
		shader.version_free(version);
	}
	if (dispatch_version.is_valid()) {
		dispatch_shader.version_free(dispatch_version);
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
	used = 0;
	struct ResultGuard {
		uint32_t &count;
		bool complete = false;
		~ResultGuard() { if (!complete) { count = 0; } }
	} guard{ used };
	uint64_t roots_total = 0;
	uint64_t extra_total = 0;
	uint64_t instances_total = 0;
	uint64_t desired_nodes = 0;
	uint32_t maximum_depth = 0;
	for (const Batch &batch : batches) {
		if (batch.count == 0) {
			continue;
		}
		const uint64_t roots = uint64_t(batch.count) * batch.geometry.root_clusters;
		ERR_FAIL_COND_V(roots == 0 || roots > UINT32_MAX, ERR_OUT_OF_MEMORY);
		const uint64_t desired = p_root_only ? roots : MAX(roots, MIN(uint64_t(FINE_WORK_TARGET), uint64_t(batch.count) * batch.geometry.clusters));
		roots_total += roots;
		instances_total += batch.count;
		extra_total += desired - roots;
		desired_nodes = MIN(uint64_t(FINE_WORK_TARGET), desired_nodes + MIN(uint64_t(FINE_WORK_TARGET), uint64_t(batch.count) * batch.geometry.nodes));
		maximum_depth = MAX(maximum_depth, batch.geometry.hierarchy_depth);
		ERR_FAIL_COND_V(roots_total > UINT32_MAX || instances_total > UINT32_MAX, ERR_OUT_OF_MEMORY);
	}
	if (instances_total == 0) {
		guard.complete = true;
		return OK;
	}
	ERR_FAIL_COND_V(maximum_depth == 0, ERR_INVALID_DATA);
	const uint64_t extra_capacity = roots_total < FINE_WORK_TARGET ? FINE_WORK_TARGET - roots_total : 0;
	const uint32_t node_capacity = p_root_only ? 0 : uint32_t(MAX(instances_total, desired_nodes));
	uint64_t result_size = 0, feedback_size = 0, metadata_size = 0, transform_size = 0;
	uint64_t candidate_capacity = 0, root_tasks = 0, first_instance = 0;
	float maximum_displacement = 0;
	RID geometry;
	metadata_offsets.clear();
	metadata_copies.clear();
	transform_copies.clear();
	parameter_data.clear();
	RD *rd = RD::get_singleton();
	bool direct_sources = rd->has_feature(RD::SUPPORTS_BUFFER_DEVICE_ADDRESS);
	source_reads.clear();
	source_address_data.clear();
	if (direct_sources) {
		GodotProfileZone("Render/VirtualGeometry/source_resolve");
		for (const Batch &batch : batches) {
			if (batch.count == 0) {
				continue;
			}
			SourceAddresses addresses;
			direct_sources = source_reads.resolve(rd, batch.geometry.metadata, 0, uint64_t(batch.geometry.metadata_words) * 4, addresses.metadata);
			if (direct_sources && batch.stride != 0) {
				const uint64_t stride_bytes = uint64_t(batch.stride) * 16;
				direct_sources = batch.count <= UINT32_MAX / stride_bytes && batch.transform_offset <= UINT32_MAX / stride_bytes &&
						source_reads.resolve(rd, batch.transforms, uint64_t(batch.transform_offset) * stride_bytes, uint64_t(batch.count) * stride_bytes, addresses.transforms);
			}
			if (!direct_sources) {
				// Optional BDA must not reject an otherwise valid legacy buffer.
				source_reads.clear();
				source_address_data.clear();
				break;
			}
			source_address_data.push_back(addresses);
		}
	}
	for (const Batch &batch : batches) {
		if (batch.count == 0) {
			continue;
		}
		ERR_FAIL_COND_V(batch.geometry.metadata.is_null() || batch.geometry.geometry.is_null() || batch.geometry.metadata_words == 0 || batch.geometry.root_pages == 0 ||
				(batch.stride != 0 && (batch.stride < 3 || batch.transforms.is_null())) ||
				!batch.model.is_finite() || batch.model.basis.determinant() == 0 || lod_scale <= 0 ||
				!Math::is_finite(batch.max_vertex_displacement) || batch.max_vertex_displacement < 0, ERR_INVALID_PARAMETER);
		if (geometry.is_null()) {
			geometry = batch.geometry.geometry;
		}
		ERR_FAIL_COND_V(geometry != batch.geometry.geometry, ERR_INVALID_DATA);
		uint32_t metadata_word;
		const uint32_t *found = metadata_offsets.getptr(batch.geometry.metadata);
		if (found != nullptr) {
			metadata_word = *found;
		} else {
			ERR_FAIL_COND_V(metadata_size + batch.geometry.metadata_words > UINT32_MAX / 4, ERR_OUT_OF_MEMORY);
			metadata_word = uint32_t(metadata_size);
			metadata_offsets.insert(batch.geometry.metadata, metadata_word);
			if (!direct_sources) {
				metadata_copies.push_back({ batch.geometry.metadata, 0, metadata_word * 4, batch.geometry.metadata_words * 4 });
			}
			metadata_size += batch.geometry.metadata_words;
		}
		const uint64_t roots = uint64_t(batch.count) * batch.geometry.root_clusters;
		const uint64_t desired = p_root_only ? roots : MAX(roots, MIN(uint64_t(FINE_WORK_TARGET), uint64_t(batch.count) * batch.geometry.clusters));
		const uint64_t extra = extra_total > extra_capacity ? (desired - roots) * extra_capacity / extra_total : desired - roots;
		const uint32_t clusters = uint32_t(roots + extra);
		const uint32_t requests = p_root_only ? 0 : MIN(uint64_t(65536), uint64_t(batch.count) * batch.geometry.pages);
		ERR_FAIL_COND_V(result_size > UINT32_MAX || feedback_size > UINT32_MAX || root_tasks > UINT32_MAX, ERR_OUT_OF_MEMORY);
		Parameters input = {};
		MaterialStorage::store_transform(batch.model, input.model);
		input.source[0] = batch.count;
		input.source[1] = batch.stride;
		input.source[2] = uint32_t(transform_size / 4);
		input.source[3] = batch.geometry.root_pages;
		input.limits[1] = clusters;
		input.limits[2] = uint32_t(roots);
		input.limits[3] = requests;
		input.offsets[0] = metadata_word + batch.geometry.parts_word;
		input.offsets[1] = metadata_word + batch.geometry.page_table_word;
		input.offsets[2] = metadata_word + batch.geometry.groups_word;
		memcpy(&input.offsets[3], &batch.max_vertex_displacement, sizeof(float));
		input.addresses[0] = metadata_word;
		input.addresses[1] = uint32_t(feedback_size);
		input.addresses[2] = uint32_t(result_size);
		input.addresses[3] = uint32_t(first_instance);
		input.prefix[0] = uint32_t(root_tasks);
		input.prefix[1] = batch.geometry.hierarchy_depth;
		parameter_data.push_back(input);
		if (batch.stride != 0) {
			const uint64_t words = uint64_t(batch.count) * batch.stride * 4;
			const uint64_t source_byte = uint64_t(batch.transform_offset) * batch.stride * 16;
			ERR_FAIL_COND_V(transform_size + words > UINT32_MAX / 4 || source_byte > UINT32_MAX, ERR_OUT_OF_MEMORY);
			if (!direct_sources) {
				transform_copies.push_back({ batch.transforms, uint32_t(source_byte), uint32_t(transform_size * 4), uint32_t(words * 4) });
			}
			transform_size += words;
		}
		if (used == results.size()) {
			results.push_back(Result());
		}
		Result &result = results[used++];
		result.batch = batch;
		result.work_word = uint32_t(result_size);
		result.selected_capacity = clusters;
		result.root_capacity = uint32_t(roots);
		result_size += 16 + uint64_t(batch.count) * 16 + uint64_t(clusters) * 2;
		feedback_size += 16 + uint64_t(requests) * 2 + (p_root_only ? 0 : 256);
		candidate_capacity += clusters;
		root_tasks += uint64_t(batch.count) * batch.geometry.root_pages;
		first_instance += batch.count;
		maximum_displacement = MAX(maximum_displacement, batch.max_vertex_displacement);
	}
	ERR_FAIL_COND_V(root_tasks > UINT32_MAX || candidate_capacity > UINT32_MAX, ERR_OUT_OF_MEMORY);
#ifdef GODOT_USE_TRACY
	if (TracyIsConnected) {
		ZoneNamedN(counts, "Render/VirtualGeometry/view_work", true);
		// One value record per view, before allocation. These are admitted
		// capacities and snapshot bytes, not visible primitives or GPU timings.
		const CharString text = vformat("shadow_flags=%d root_only=%d batches=%d instances=%d metadata_sources=%d transform_sources=%d metadata_bytes=%d transform_bytes=%d root_tasks=%d candidate_capacity=%d node_capacity=%d direct_sources=%d direct_buffers=%d metadata_span_bytes=%d transform_span_bytes=%d", shadow_flags, int(p_root_only), used, instances_total, metadata_copies.size(), transform_copies.size(), direct_sources ? 0 : metadata_size * 4, direct_sources ? 0 : transform_size * 4, root_tasks, candidate_capacity, node_capacity, int(direct_sources), source_reads.size(), metadata_size * 4, transform_size * 4).utf8();
		counts.Text(text.get_data(), text.length());
	}
#endif
	const uint64_t scratch_size = 16 + uint64_t(node_capacity) * 8 + candidate_capacity * 4;
	struct Allocation { RID &buffer; uint32_t &words; uint64_t required; bool indirect = false; };
	Allocation allocations[] = {
		{ result_buffer, result_words, result_size },
		{ feedback_buffer, feedback_words, feedback_size },
		{ metadata_buffer, metadata_words, direct_sources ? 1 : metadata_size },
		{ transforms_buffer, transforms_words, direct_sources ? 12 : MAX(uint64_t(12), transform_size) },
		{ source_addresses, source_address_words, MAX(uint64_t(1), uint64_t(source_address_data.size()) * 4) },
		{ scratch, scratch_words, scratch_size },
		{ dispatch_inputs, dispatch_input_words, uint64_t(used) * 36 },
		{ dispatch_buffer, dispatch_words, 3, true }
	};
	for (Allocation &allocation : allocations) {
		const Error error = memory.ensure(allocation.buffer, allocation.words, allocation.required, allocation.indirect);
		if (error != OK) {
			return error;
		}
	}
	if (view_parameters.is_null()) {
		view_parameters = memory.uniform(256);
	}
	ERR_FAIL_COND_V(view_parameters.is_null(), ERR_CANT_CREATE);
	struct ViewParameters {
		float model[16], view[16], projection[16], lod[4];
		uint32_t source[4], limits[4], offsets[4];
	} view = {};
	static_assert(sizeof(ViewParameters) == 256);
	static_assert(sizeof(Parameters) == 144);
	MaterialStorage::store_transform(Transform3D(), view.model);
	MaterialStorage::store_transform(camera_inverse, view.view);
	MaterialStorage::store_camera(projection, view.projection);
	view.lod[0] = near_plane;
	view.lod[1] = lod_scale;
	view.lod[2] = uint32_t(orthographic) | shadow_flags;
	view.lod[3] = frustum_margin;
	view.source[0] = used;
	view.source[1] = uint32_t(instances_total);
	view.source[2] = uint32_t(root_tasks);
	view.source[3] = p_root_only ? 1 : 0;
	view.limits[0] = node_capacity;
	view.limits[1] = uint32_t(candidate_capacity);
	view.offsets[0] = 16;
	view.offsets[1] = uint32_t(16 + uint64_t(node_capacity) * 4);
	view.offsets[2] = uint32_t(16 + uint64_t(node_capacity) * 8);
	memcpy(&view.offsets[3], &maximum_displacement, sizeof(float));
	rd->buffer_update(view_parameters, 0, sizeof(view), &view);
	rd->buffer_update(dispatch_inputs, 0, used * sizeof(Parameters), parameter_data.ptr());
	if (direct_sources) {
		rd->buffer_update(source_addresses, 0, uint32_t(uint64_t(source_address_data.size()) * sizeof(SourceAddresses)), source_address_data.ptr());
	}
	// Source uploads precede this render boundary. Direct readers register the
	// borrowed RIDs below; fallback retains immutable per-view snapshots.
	if (!direct_sources) {
		const Error copied = snapshot_copies.record(metadata_buffer, Span<const BufferCopy>(metadata_copies.ptr(), metadata_copies.size()),
				transforms_buffer, Span<const BufferCopy>(transform_copies.ptr(), transform_copies.size()));
		if (copied != OK) {
			return copied;
		}
	}
	rd->buffer_clear(scratch, 0, 64);
	for (uint32_t index = 0; index < used; ++index) {
		results[index].work = result_buffer;
		results[index].parameters = view_parameters;
		results[index].transforms = direct_sources ? RID() : transforms_buffer;
		results[index].direct_source_reads = direct_sources;
		results[index].transform_source = direct_sources && results[index].batch.stride != 0 ? source_reads.get_resolved(results[index].batch.transforms) : VirtualGeometrySourceReads::ResolvedBuffer{};
		results[index].transform_offset = parameter_data[index].source[2];
	}
	if (version.is_null()) {
		const bool device_addresses = rd->has_feature(RD::SUPPORTS_BUFFER_DEVICE_ADDRESS);
		Vector<String> modes;
		modes.push_back("");
		modes.push_back("#define VG_DIRECT_SOURCE_READS\n");
		shader.initialize(modes);
		shader.set_variant_enabled(1, device_addresses);
		version = shader.version_create();
		modes.resize(1);
		dispatch_shader.initialize(modes);
		dispatch_version = dispatch_shader.version_create();
		dispatch_pipeline.create_compute_pipeline(dispatch_shader.version_get_shader(dispatch_version, 0));
	}
	const uint32_t mode = direct_sources ? 1 : 0;
	const RID selection_shader = shader.version_get_shader(version, mode);
	static constexpr uint32_t phases[] = { 0, 2, 4, 5, 6, 7, 10, 11 };
	RID compute[12] = {};
	for (uint32_t phase : phases) {
		if ((p_root_only && phase != 0 && phase != 5 && phase != 7 && phase != 10) || (!p_root_only && phase == 7)) {
			continue;
		}
		if (pipelines[mode][phase].get_rid().is_null()) {
			RD::PipelineSpecializationConstant specialization;
			specialization.constant_id = 0;
			specialization.type = RD::PIPELINE_SPECIALIZATION_CONSTANT_TYPE_INT;
			specialization.int_value = phase;
			Vector<RD::PipelineSpecializationConstant> constants;
			constants.push_back(specialization);
			pipelines[mode][phase].create_compute_pipeline(selection_shader, constants);
		}
	}
	// Complete every required pipeline before publishing any view commands.
	for (uint32_t phase : phases) {
		if ((p_root_only && phase != 0 && phase != 5 && phase != 7 && phase != 10) || (!p_root_only && phase == 7)) {
			continue;
		}
		compute[phase] = pipelines[mode][phase].get_rid();
		ERR_FAIL_COND_V(compute[phase].is_null(), ERR_CANT_CREATE);
	}
	const RID prepare = dispatch_pipeline.get_rid();
	ERR_FAIL_COND_V(prepare.is_null(), ERR_CANT_CREATE);
	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	LocalVector<RD::Uniform> uniforms;
	if (!direct_sources) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, { metadata_buffer }));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, { transforms_buffer }));
	} else {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, { source_addresses }));
	}
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, { geometry }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 3, { view_parameters }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, { scratch }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, { result_buffer }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, { dispatch_inputs }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, { feedback_buffer }));
	const RID selection_uniforms = cache->get_cache_vec(selection_shader, 0, uniforms);
	const RID dispatch_uniforms = cache->get_cache(dispatch_shader.version_get_shader(dispatch_version, 0), 0,
			RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 3, { view_parameters }),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, { scratch }),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, { dispatch_buffer }),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, { dispatch_inputs }),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, { feedback_buffer }));
	ERR_FAIL_COND_V(selection_uniforms.is_null() || dispatch_uniforms.is_null(), ERR_CANT_CREATE);
	RENDER_TIMESTAMP("Virtual Geometry Selection Begin");
	const RD::ComputeListID list = rd->compute_list_begin();
	if (direct_sources) {
		source_reads.record(rd, list);
	}
	auto dispatch = [&](uint32_t p_mode, uint32_t p_round, uint32_t p_items, bool p_indirect = false) {
		const uint32_t phase[3] = { 0, p_mode, p_round };
		rd->compute_list_bind_compute_pipeline(list, compute[p_mode]);
		rd->compute_list_bind_uniform_set(list, selection_uniforms, 0);
		rd->compute_list_set_push_constant(list, phase, sizeof(phase));
		if (p_indirect) {
			rd->compute_list_dispatch_indirect(list, dispatch_buffer, 0);
		} else {
			const uint32_t groups = uint32_t(MAX(uint64_t(1), (uint64_t(p_items) + 63) / 64));
			rd->compute_list_dispatch(list, MIN(65535u, groups), (groups + 65534) / 65535, 1);
		}
	};
	auto prepare_dispatch = [&](uint32_t p_mode, uint32_t p_round) {
		const uint32_t phase[3] = { 0, p_mode, p_round };
		rd->compute_list_bind_compute_pipeline(list, prepare);
		rd->compute_list_bind_uniform_set(list, dispatch_uniforms, 0);
		rd->compute_list_set_push_constant(list, phase, sizeof(phase));
		const uint32_t groups = p_mode == 1 ? uint32_t(MAX(uint64_t(1), (uint64_t(used) + 63) / 64)) : 1;
		rd->compute_list_dispatch(list, MIN(65535u, groups), (groups + 65534u) / 65535u, 1);
		rd->compute_list_add_barrier(list);
	};
	dispatch(10, 0, used);
	rd->compute_list_add_barrier(list);
	dispatch(0, 0, uint32_t(instances_total));
	rd->compute_list_add_barrier(list);
	if (!p_root_only) {
		for (uint32_t depth = 0; depth < maximum_depth; ++depth) {
			prepare_dispatch(1, depth);
			dispatch(2, depth, 0, true);
			rd->compute_list_add_barrier(list);
		}
		prepare_dispatch(11, maximum_depth - 1);
		dispatch(11, maximum_depth - 1, 0, true);
		rd->compute_list_add_barrier(list);
		prepare_dispatch(3, 0);
		dispatch(4, 0, 0, true);
		rd->compute_list_add_barrier(list);
	}
	dispatch(5, 0, uint32_t(root_tasks));
	rd->compute_list_add_barrier(list);
	dispatch(p_root_only ? 7 : 6, 0, used);
	rd->compute_list_end();
	RENDER_TIMESTAMP("Virtual Geometry Selection End");
	if (!p_root_only) {
		for (uint32_t index = 0; index < used; ++index) {
			const Batch &batch = results[index].batch;
			const Parameters &input = parameter_data[index];
			const Error feedback = MeshStorage::get_singleton()->record_virtual_geometry_requests(batch.mesh, batch.geometry.generation,
					feedback_buffer, input.addresses[1] + 16, input.limits[3], input.addresses[1]);
			if (feedback != OK && feedback != ERR_BUSY) {
				WARN_PRINT_ONCE("Virtual geometry page feedback is unavailable; retaining current resident coverage.");
			}
		}
	}
	guard.complete = true;
	return OK;
}

Error VirtualGeometrySelection::use_root_coverage() {
	return record(true);
}

uint64_t VirtualGeometrySelection::get_buffer_bytes() const {
	return snapshot_copies.get_buffer_bytes() + (view_parameters.is_valid() ? 256 : 0) + (uint64_t(result_words) + metadata_words + transforms_words + feedback_words + dispatch_words + dispatch_input_words + scratch_words + source_address_words) * 4;
}

} // namespace RendererRD
