// SPDX-License-Identifier: MIT
#include "virtual_geometry_snapshot_copy.h"

#include "core/profiling/profiling.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

namespace RendererRD {

VirtualGeometrySnapshotCopy::~VirtualGeometrySnapshotCopy() {
	pipeline.free();
	if (version.is_valid()) {
		shader.version_free(version);
	}
}

Error VirtualGeometrySnapshotCopy::_record_direct(RID p_first_destination, Span<const Copy> p_first, RID p_second_destination, Span<const Copy> p_second) {
	RD *rd = RD::get_singleton();
	const RID outputs[2] = { p_first_destination, p_second_destination };
	const Span<const Copy> lists[2] = { p_first, p_second };
	for (uint32_t output = 0; output < 2; ++output) {
		for (const Copy &copy : lists[output]) {
			const Error error = rd->buffer_copy(copy.source, outputs[output], copy.source_byte, copy.destination_byte, copy.bytes);
			if (error != OK) {
				return error;
			}
		}
	}
#ifdef GODOT_USE_TRACY
	if (TracyIsConnected) {
		ZoneNamedN(counts, "Render/VirtualGeometry/snapshot_copy_counts", true);
		const CharString text = vformat("gathered=0 direct=%d sources=0 bytes=0", p_first.size() + p_second.size()).utf8();
		counts.Text(text.get_data(), text.length());
	}
#endif
	return OK;
}

Error VirtualGeometrySnapshotCopy::record(RID p_first_destination, Span<const Copy> p_first, RID p_second_destination, Span<const Copy> p_second) {
#ifdef GODOT_USE_TRACY
	ZoneNamedN(copy_zone, "Render/VirtualGeometry/snapshot_copy", true);
#endif
	jobs.clear();
	selected.clear();
	sources.clear();
	if (p_first.is_empty() && p_second.is_empty()) {
		return OK;
	}
	RD *rd = RD::get_singleton();
	const auto direct = [this, p_first_destination, p_first, p_second_destination, p_second]() {
		return _record_direct(p_first_destination, p_first, p_second_destination, p_second);
	};
	if (!rd->has_feature(RD::SUPPORTS_BUFFER_DEVICE_ADDRESS) ||
			(!p_first.is_empty() && !p_second.is_empty() && p_first_destination == p_second_destination)) {
		return direct();
	}
	const RID outputs[2] = { p_first_destination, p_second_destination };
	const Span<const Copy> lists[2] = { p_first, p_second };
	uint32_t output_sizes[2] = {};
	// Preserve ordered copy semantics for aliases or overlapping destination
	// ranges. The three snapshot producers normally append disjoint spans.
	for (uint32_t output = 0; output < 2; ++output) {
		if (lists[output].is_empty()) {
			continue;
		}
		RD::BufferInfo info;
		if (!rd->buffer_get_info(outputs[output], info) || !info.storage) {
			return direct();
		}
		output_sizes[output] = info.size;
		uint64_t previous_end = 0;
		for (const Copy &copy : lists[output]) {
			if (copy.source == p_first_destination || copy.source == p_second_destination || copy.destination_byte < previous_end) {
				return direct();
			}
			previous_end = uint64_t(copy.destination_byte) + copy.bytes;
		}
	}
	uint64_t gathered_bytes = 0;
	const uint32_t max_x = uint32_t(MIN(rd->limit_get(RD::LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_X), uint64_t(VirtualGeometrySnapshotCopyPlan::MAX_DISPATCH_AXIS)));
	const uint32_t max_y = uint32_t(MIN(rd->limit_get(RD::LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_Y), uint64_t(VirtualGeometrySnapshotCopyPlan::MAX_DISPATCH_AXIS)));
	const uint32_t max_tiles = max_x * max_y;
	uint32_t tile_count = 0;
	for (uint32_t output = 0; output < 2; ++output) {
		for (uint32_t index = 0; index < lists[output].size(); ++index) {
			const Copy &copy = lists[output][index];
			if (!VirtualGeometrySnapshotCopyPlan::gatherable(copy.source_byte, copy.destination_byte, copy.bytes, jobs.size(), tile_count, max_tiles)) {
				continue;
			}
			RD::BufferInfo source;
			if (const RD::BufferInfo *cached = sources.getptr(copy.source)) {
				source = *cached;
			} else {
				if (!rd->buffer_get_info(copy.source, source) || !source.storage || !source.device_address) {
					continue;
				}
				sources.insert(copy.source, source);
			}
			ERR_FAIL_COND_V_MSG(!VirtualGeometrySnapshotCopyPlan::valid_range(copy.source_byte, copy.bytes, source.size) ||
							!VirtualGeometrySnapshotCopyPlan::valid_range(copy.destination_byte, copy.bytes, output_sizes[output]),
					ERR_INVALID_PARAMETER, "Virtual geometry snapshot copy range exceeds its source or destination.");
			ERR_FAIL_COND_V(source.device_address > UINT64_MAX - (uint64_t(copy.source_byte) + copy.bytes), ERR_INVALID_PARAMETER);
			const uint64_t address = source.device_address + copy.source_byte;
			Job job = {};
			job.source[0] = uint32_t(address);
			job.source[1] = uint32_t(address >> 32);
			job.destination_word = copy.destination_byte / 4;
			job.words = copy.bytes / 4;
			job.output = output;
			job.first_tile = tile_count;
			jobs.push_back(job);
			tile_count += VirtualGeometrySnapshotCopyPlan::tiles(copy.bytes);
			selected.push_back({ output, index });
			gathered_bytes += copy.bytes;
		}
	}
	if (jobs.is_empty()) {
		return direct();
	}
	if (job_buffer.is_null() || job_capacity < jobs.size()) {
		const uint32_t capacity = MIN(VirtualGeometrySnapshotCopyPlan::MAX_JOBS, MAX(jobs.size(), job_capacity + MAX(16u, job_capacity / 4)));
		const RID candidate = memory.storage(capacity * VirtualGeometrySnapshotCopyPlan::JOB_BYTES);
		if (candidate.is_null()) {
			// Snapshot aggregation is optional; exhausted work capacity must not
			// reject a view that can still use the original transfer path.
			return direct();
		}
		memory.free(job_buffer);
		job_buffer = candidate;
		job_capacity = capacity;
	}
	if (version.is_null()) {
		Vector<String> modes;
		modes.push_back("");
		shader.initialize(modes);
		version = shader.version_create();
		pipeline.create_compute_pipeline(shader.version_get_shader(version, 0));
	}
	const RID program = shader.version_get_shader(version, 0);
	ERR_FAIL_COND_V(program.is_null() || pipeline.get_rid().is_null(), ERR_CANT_CREATE);
	uint32_t selected_index = 0;
	uint32_t direct_copies = 0;
	for (uint32_t output = 0; output < 2; ++output) {
		for (uint32_t index = 0; index < lists[output].size(); ++index) {
			if (selected_index < selected.size() && selected[selected_index].output == output && selected[selected_index].index == index) {
				++selected_index;
				continue;
			}
			const Copy &copy = lists[output][index];
			const Error error = rd->buffer_copy(copy.source, outputs[output], copy.source_byte, copy.destination_byte, copy.bytes);
			if (error != OK) {
				return error;
			}
			++direct_copies;
		}
	}
	const Error upload = rd->buffer_update(job_buffer, 0, jobs.size() * VirtualGeometrySnapshotCopyPlan::JOB_BYTES, jobs.ptr());
	if (upload != OK) {
		return upload;
	}
	// First writes may replace an immutable driver's buffer. Resolve cached
	// descriptors after the uploads/transfers have finalized those bindings.
	const RID first_output = p_first.is_empty() ? p_second_destination : p_first_destination;
	const RID second_output = p_second.is_empty() ? first_output : p_second_destination;
	const RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache(program, 0,
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, { job_buffer }),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, { first_output }),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, { second_output }));
	ERR_FAIL_COND_V(uniform_set.is_null(), ERR_CANT_CREATE);
	const RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, pipeline.get_rid());
	for (const KeyValue<RID, RD::BufferInfo> &source : sources) {
		rd->compute_list_add_buffer_read(list, source.key, source.value);
	}
	rd->compute_list_bind_uniform_set(list, uniform_set, 0);
	const uint32_t push[2] = { jobs.size(), tile_count };
	rd->compute_list_set_push_constant(list, push, sizeof(push));
	const uint32_t groups_x = MIN(tile_count, max_x);
	const uint32_t groups_y = uint32_t((uint64_t(tile_count) + groups_x - 1) / groups_x);
	rd->compute_list_dispatch(list, groups_x, groups_y, 1);
	rd->compute_list_end();
#ifdef GODOT_USE_TRACY
	if (TracyIsConnected) {
		ZoneNamedN(counts, "Render/VirtualGeometry/snapshot_copy_counts", true);
		const CharString text = vformat("gathered=%d direct=%d sources=%d bytes=%d tiles=%d", jobs.size(), direct_copies, sources.size(), gathered_bytes, tile_count).utf8();
		counts.Text(text.get_data(), text.length());
	}
#endif
	return OK;
}

} // namespace RendererRD
