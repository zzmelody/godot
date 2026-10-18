#include "virtual_geometry_storage.h"

#include "core/io/marshalls.h"
#include "core/object/callable_mp.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

#include <cmath>
#include <limits>

namespace RendererRD {
namespace {

uint64_t root_memory(const VirtualGeometry::GeometryModelRoot &p_root) {
	const auto &hierarchy = p_root.hierarchy;
	return sizeof(p_root) + p_root.materials.capacity() * sizeof(uint32_t) +
			p_root.groups.capacity() * sizeof(VirtualGeometry::ModelGroupPages) +
			hierarchy.nodes.capacity() * sizeof(VirtualGeometry::HierarchyNode) +
			hierarchy.parts.capacity() * sizeof(VirtualGeometry::HierarchyPart) +
			hierarchy.pages.capacity() * sizeof(VirtualGeometry::HierarchyPage) +
			p_root.directory.pages.capacity() * sizeof(VirtualGeometry::PageLocation) +
			p_root.directory.dependencies.capacity() * sizeof(uint32_t);
}

} // namespace

VirtualGeometryStorage::~VirtualGeometryStorage() {
	diagnostics_pipeline.free();
	if (diagnostics_version.is_valid()) {
		diagnostics_shader.version_free(diagnostics_version);
	}
	if (diagnostics_buffer.is_valid()) {
		RD::get_singleton()->free_rid(diagnostics_buffer);
	}
	for (RequestSlot &slot : request_slots) {
		if (slot.buffer.is_valid()) {
			RD::get_singleton()->free_rid(slot.buffer);
		}
	}
	request_pipeline.free();
	if (request_version.is_valid()) {
		request_shader.version_free(request_version);
	}
	for (const KeyValue<RID, Asset *> &entry : assets) {
		detached.insert(entry.value);
	}
	for (Asset *asset : detached) {
		if (asset->bindings.metadata.is_valid()) {
			RD::get_singleton()->free_rid(asset->bindings.metadata);
		}
		for (const KeyValue<uint32_t, Page *> &entry : asset->pages) {
			if (entry.value->source.is_valid()) {
				RD::get_singleton()->free_rid(entry.value->source);
			}
			memdelete(entry.value);
		}
		memdelete(asset);
	}
	if (pool.is_valid()) {
		RD::get_singleton()->free_rid(pool);
	}
	fixup_pipeline.free();
	if (fixup_version.is_valid()) {
		fixup_shader.version_free(fixup_version);
	}
}

const VirtualGeometryStorage::Bindings *VirtualGeometryStorage::get_bindings(RID p_mesh) const {
	Asset *const *found = assets.getptr(p_mesh);
	return found != nullptr && (*found)->roots_published == (*found)->root.hierarchy.root_pages ? &(*found)->bindings : nullptr;
}

RendererMeshStorage::VirtualGeometryMaterials VirtualGeometryStorage::get_materials(RID p_mesh) const {
	Asset *const *found = assets.getptr(p_mesh);
	if (found == nullptr) {
		return {};
	}
	const Asset &asset = **found;
	return { Span<RID>(asset.materials.ptr(), asset.materials.size()), Span<uint32_t>(asset.root.materials.data(), asset.root.materials.size()) };
}

Error VirtualGeometryStorage::set_material(RID p_mesh, uint32_t p_slot, RID p_material) {
	Asset **found = assets.getptr(p_mesh);
	if (found == nullptr || p_slot >= uint32_t((*found)->materials.size())) {
		return ERR_INVALID_PARAMETER;
	}
	(*found)->materials.write[p_slot] = p_material;
	return OK;
}

void VirtualGeometryStorage::_prune_outcomes() {
	const uint64_t consumed = acknowledged.load(std::memory_order_acquire);
	uint32_t first = 0;
	while (first < outcomes.size() && outcomes[first].sequence <= consumed) {
		++first;
	}
	for (uint32_t index = first; index < outcomes.size(); ++index) {
		outcomes[index - first] = outcomes[index];
	}
	outcomes.resize(outcomes.size() - first);
}

void VirtualGeometryStorage::_publish_feedback() {
	_prune_outcomes();
	Vector<uint8_t> &value = feedback_spare;
	value.resize(SNAPSHOT_BYTES + outcomes.size() * OUTCOME_BYTES);
	uint8_t *bytes = value.ptrw();
	const uint64_t header[] = { RD::get_singleton()->get_recording_submission_serial(), RD::get_singleton()->get_completed_submission_serial(),
		pool_bytes, occupied_bytes, retiring_bytes, metadata_reserved, staging_bytes, work_reserved, sequence, gpu_metadata_bytes };
	for (uint32_t word = 0; word < 10; ++word) {
		encode_uint64(header[word], bytes + word * 8);
	}
	encode_uint32(outcomes.size(), bytes + 80);
	encode_uint32(OUTCOME_BYTES, bytes + 84);
	for (uint32_t index = 0; index < outcomes.size(); ++index) {
		const Outcome &outcome = outcomes[index];
		uint8_t *record = bytes + SNAPSHOT_BYTES + index * OUTCOME_BYTES;
		const uint64_t words[] = { outcome.sequence, outcome.mesh, outcome.generation, outcome.lease, outcome.upload };
		for (uint32_t word = 0; word < 5; ++word) {
			encode_uint64(words[word], record + word * 8);
		}
		encode_uint32(outcome.page, record + 40);
		encode_uint32(outcome.status, record + 44);
		encode_uint32(outcome.error, record + 48);
		encode_uint64(outcome.completed, record + 52);
	}
	MutexLock lock(feedback_mutex);
	SWAP(snapshot, feedback_spare);
}

void VirtualGeometryStorage::_consume_requests() {
	if (diagnostics_readback.is_valid() && diagnostics_readback->ready) {
		const Vector<uint8_t> bytes = diagnostics_readback->bytes;
		diagnostics_readback.unref();
		if (bytes.size() != 8) {
			ERR_PRINT("Virtual geometry GPU diagnostic readback has invalid length.");
		} else {
			const uint32_t view_flags = decode_uint32(bytes.ptr());
			const uint32_t material_flags = decode_uint32(bytes.ptr() + 4);
			if ((view_flags & ~reported_diagnostics[0]) != 0 || (material_flags & ~reported_diagnostics[1]) != 0) {
				ERR_PRINT(vformat("Virtual geometry GPU view refusal through submission %d: raster/deformation=%d material=%d", diagnostics_serial, view_flags, material_flags));
				reported_diagnostics[0] |= view_flags;
				reported_diagnostics[1] |= material_flags;
			}
		}
	}
	for (RequestSlot &slot : request_slots) {
		if (slot.readback.is_null() || !slot.readback->ready) {
			continue;
		}
		const Vector<uint8_t> bytes = slot.readback->bytes;
		slot.readback.unref();
		Asset *const *found = assets.getptr(slot.mesh);
		if (found == nullptr || (*found)->generation != slot.generation) {
			continue;
		}
		if (bytes.size() != REQUEST_BUFFER_BYTES) {
			++request_invalid;
			continue;
		}
		const uint8_t *data = bytes.ptr();
		const uint32_t count = decode_uint32(data);
		request_overflow += decode_uint32(data + 4) != 0u ? 1u : 0u;
		if (count > REQUEST_CAPACITY || (decode_uint32(data + 8) & 16u) != 0u) {
			++request_invalid;
			continue;
		}
		for (uint32_t index = 0; index < count; ++index) {
			const uint32_t page = decode_uint32(data + 16 + index * 8);
			const uint32_t priority = decode_uint32(data + 20 + index * 8);
			float value;
			memcpy(&value, &priority, sizeof(value));
			if (page >= (*found)->root.directory.pages.size() || !Math::is_finite(value) || (value < 0.0f && value != -1.0f)) {
				++request_invalid;
				continue;
			}
			// A read/upload already owns this demand, or the page became resident
			// while this view's asynchronous copy was in flight.
			if ((value != -1.0f && (*found)->pages.has(page)) || (value == -1.0f && !(*found)->pages.has(page))) {
				continue;
			}
			if (requests.size() == MAX_REQUESTS) {
				++request_overflow;
				continue;
			}
			requests.push_back({ ++request_sequence, slot.mesh.get_id(), slot.generation, page, priority });
		}
	}
	if (assets.is_empty()) {
		for (RequestSlot &slot : request_slots) {
			if (slot.readback.is_null() && slot.buffer.is_valid()) {
				RD::get_singleton()->free_rid(slot.buffer);
				freed_buffers.push_back({ RD::get_singleton()->get_recording_submission_serial(), slot.charge, false, false });
				slot.buffer = RID();
				slot.charge = 0;
			}
		}
	}
}

void VirtualGeometryStorage::_publish_requests() {
	const uint64_t consumed = requests_acknowledged.load(std::memory_order_acquire);
	uint32_t first = 0;
	while (first < requests.size() && requests[first].sequence <= consumed) {
		++first;
	}
	for (uint32_t index = first; index < requests.size(); ++index) {
		requests[index - first] = requests[index];
	}
	requests.resize(requests.size() - first);
	request_spare.resize(REQUEST_HEADER_BYTES + requests.size() * REQUEST_RECORD_BYTES);
	uint8_t *bytes = request_spare.ptrw();
	encode_uint64(request_sequence, bytes);
	encode_uint64(request_overflow, bytes + 8);
	encode_uint64(request_invalid, bytes + 16);
	encode_uint32(requests.size(), bytes + 24);
	encode_uint32(REQUEST_RECORD_BYTES, bytes + 28);
	for (uint32_t index = 0; index < requests.size(); ++index) {
		const RequestRecord &request = requests[index];
		uint8_t *record = bytes + REQUEST_HEADER_BYTES + index * REQUEST_RECORD_BYTES;
		encode_uint64(request.sequence, record);
		encode_uint64(request.mesh, record + 8);
		encode_uint64(request.generation, record + 16);
		encode_uint32(request.page, record + 24);
		encode_uint32(request.priority, record + 28);
	}
	MutexLock lock(feedback_mutex);
	SWAP(request_snapshot, request_spare);
}

Vector<uint8_t> VirtualGeometryStorage::get_requests(uint64_t p_after_sequence) const {
	Vector<uint8_t> value;
	{
		MutexLock lock(feedback_mutex);
		value = request_snapshot;
	}
	if (value.is_empty() || p_after_sequence > decode_uint64(value.ptr())) {
		return {};
	}
	uint64_t previous = requests_acknowledged.load(std::memory_order_relaxed);
	while (previous < p_after_sequence && !requests_acknowledged.compare_exchange_weak(previous, p_after_sequence, std::memory_order_release, std::memory_order_relaxed)) {
	}
	uint32_t first = 0;
	const uint32_t count = decode_uint32(value.ptr() + 24);
	while (first < count && decode_uint64(value.ptr() + REQUEST_HEADER_BYTES + first * REQUEST_RECORD_BYTES) <= p_after_sequence) {
		++first;
	}
	if (first != 0) {
		uint8_t *bytes = value.ptrw();
		memmove(bytes + REQUEST_HEADER_BYTES, bytes + REQUEST_HEADER_BYTES + first * REQUEST_RECORD_BYTES, (count - first) * REQUEST_RECORD_BYTES);
		encode_uint32(count - first, bytes + 24);
		value.resize(REQUEST_HEADER_BYTES + (count - first) * REQUEST_RECORD_BYTES);
	}
	return value;
}

Error VirtualGeometryStorage::record_requests(RID p_mesh, uint64_t p_generation, RID p_work, uint32_t p_word, uint32_t p_capacity) {
	Asset *const *found = assets.getptr(p_mesh);
	ERR_FAIL_COND_V(found == nullptr || (*found)->generation != p_generation || p_capacity == 0, ERR_INVALID_PARAMETER);
	if (requests.size() == MAX_REQUESTS) {
		(*found)->request_waiter.remove_from_list();
		return OK;
	}
	RequestSlot *available = nullptr;
	for (RequestSlot &slot : request_slots) {
		if (slot.readback.is_valid()) {
			if (slot.mesh == p_mesh && slot.generation == p_generation) {
				return OK; // One pending view per shared asset is sufficient.
			}
		} else if (available == nullptr) {
			available = &slot;
		}
	}
	Asset *asset = *found;
	asset->request_seen_epoch = request_epoch;
	if (!asset->request_waiter.in_list()) {
		request_waiters.add_last(&asset->request_waiter);
	}
	while (request_waiters.first() && request_waiters.first()->self()->request_seen_epoch + 1 < request_epoch) {
		request_waiters.first()->remove_from_list();
	}
	if (request_waiters.first() != &asset->request_waiter) {
		return ERR_BUSY;
	}
	if (available == nullptr) {
		return ERR_BUSY;
	}
	RD *rd = RD::get_singleton();
	if (request_version.is_null()) {
		Vector<String> modes = { "" };
		request_shader.initialize(modes);
		request_version = request_shader.version_create();
		RID program = request_shader.version_get_shader(request_version, 0);
		ERR_FAIL_COND_V(program.is_null(), ERR_CANT_CREATE);
		request_pipeline.create_compute_pipeline(program);
	}
	ERR_FAIL_COND_V(request_pipeline.get_rid().is_null(), ERR_CANT_CREATE);
	if (available->buffer.is_null()) {
		// Reserve output, returned bytes and aligned download payload together.
		// Backend allocator granularity is separate from these byte capacities.
		const uint64_t charge = ((REQUEST_BUFFER_BYTES + 31u) & ~31u) * 3u + sizeof(RequestReadback);
		if (charge > work_budget - work_reserved) {
			++request_overflow;
			return ERR_BUSY;
		}
		available->buffer = rd->storage_buffer_create(REQUEST_BUFFER_BYTES);
		ERR_FAIL_COND_V(available->buffer.is_null(), ERR_CANT_CREATE);
		work_reserved += charge;
		available->charge = charge;
	}
	RID program = request_shader.version_get_shader(request_version, 0);
	RID uniforms = UniformSetCacheRD::get_singleton()->get_cache(program, 0,
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, { p_work }),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, { available->buffer }));
	ERR_FAIL_COND_V(uniforms.is_null(), ERR_CANT_CREATE);
	const uint32_t push[3] = { p_word, p_capacity, REQUEST_CAPACITY };
	const RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, request_pipeline.get_rid());
	rd->compute_list_bind_uniform_set(list, uniforms, 0);
	rd->compute_list_set_push_constant(list, push, sizeof(push));
	rd->compute_list_dispatch(list, (REQUEST_CAPACITY + 63) / 64, 1, 1);
	rd->compute_list_end();
	available->mesh = p_mesh;
	available->generation = p_generation;
	asset->request_waiter.remove_from_list();
	available->readback.instantiate();
	Ref<RefCounted> keep_alive = available->readback;
	const Error error = rd->buffer_get_data_async(available->buffer,
			callable_mp(available->readback.ptr(), &RequestReadback::complete).bind(keep_alive), 0, REQUEST_BUFFER_BYTES);
	if (error != OK) {
		available->readback.unref();
		++request_invalid;
	}
	return error;
}

Error VirtualGeometryStorage::record_diagnostics(RID p_view_status, RID p_material_status) {
	ERR_FAIL_COND_V(p_view_status.is_null() || p_material_status.is_null(), ERR_INVALID_PARAMETER);
	RD *rd = RD::get_singleton();
	if (diagnostics_buffer.is_null()) {
		const uint64_t charge = 32u * 3u + sizeof(RequestReadback);
		if (charge > work_budget - work_reserved) {
			return ERR_BUSY;
		}
		diagnostics_buffer = rd->storage_buffer_create(8);
		ERR_FAIL_COND_V(diagnostics_buffer.is_null(), ERR_CANT_CREATE);
		work_reserved += charge;
		rd->buffer_clear(diagnostics_buffer, 0, 8);
	}
	if (diagnostics_version.is_null()) {
		Vector<String> modes = { "" };
		diagnostics_shader.initialize(modes);
		diagnostics_version = diagnostics_shader.version_create();
		const RID program = diagnostics_shader.version_get_shader(diagnostics_version, 0);
		ERR_FAIL_COND_V(program.is_null(), ERR_CANT_CREATE);
		diagnostics_pipeline.create_compute_pipeline(program);
	}
	const RID program = diagnostics_shader.version_get_shader(diagnostics_version, 0);
	ERR_FAIL_COND_V(diagnostics_pipeline.get_rid().is_null(), ERR_CANT_CREATE);
	const RID uniforms = UniformSetCacheRD::get_singleton()->get_cache(program, 0,
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, { p_view_status }),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, { p_material_status }),
			RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, { diagnostics_buffer }));
	ERR_FAIL_COND_V(uniforms.is_null(), ERR_CANT_CREATE);
	const RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, diagnostics_pipeline.get_rid());
	rd->compute_list_bind_uniform_set(list, uniforms, 0);
	rd->compute_list_dispatch(list, 1, 1, 1);
	rd->compute_list_end();
	if (diagnostics_readback.is_valid()) {
		return OK; // Later views keep accumulating while the prior copy is pending.
	}
	diagnostics_readback.instantiate();
	const Ref<RefCounted> keep_alive = diagnostics_readback;
	const Error error = rd->buffer_get_data_async(diagnostics_buffer,
			callable_mp(diagnostics_readback.ptr(), &RequestReadback::complete).bind(keep_alive), 0, 8);
	if (error != OK) {
		diagnostics_readback.unref();
		return error;
	}
	diagnostics_serial = rd->get_recording_submission_serial();
	// The copy precedes this clear in the device command graph. Error bits from
	// subsequent views remain accumulated until the next successful snapshot.
	rd->buffer_clear(diagnostics_buffer, 0, 8);
	return OK;
}

Vector<uint8_t> VirtualGeometryStorage::get_feedback(uint64_t p_after_sequence) const {
	Vector<uint8_t> value;
	{
		MutexLock lock(feedback_mutex);
		value = snapshot;
	}
	if (value.is_empty() || p_after_sequence > decode_uint64(value.ptr() + 64)) {
		return {};
	}
	uint64_t previous = acknowledged.load(std::memory_order_relaxed);
	while (previous < p_after_sequence && !acknowledged.compare_exchange_weak(previous, p_after_sequence, std::memory_order_release, std::memory_order_relaxed)) {
	}
	uint32_t first = 0;
	const uint32_t count = decode_uint32(value.ptr() + 80);
	while (first < count && decode_uint64(value.ptr() + SNAPSHOT_BYTES + first * OUTCOME_BYTES) <= p_after_sequence) {
		++first;
	}
	if (first != 0) {
		uint8_t *bytes = value.ptrw();
		memmove(bytes + SNAPSHOT_BYTES, bytes + SNAPSHOT_BYTES + first * OUTCOME_BYTES, (count - first) * OUTCOME_BYTES);
		encode_uint32(count - first, bytes + 80);
		value.resize(SNAPSHOT_BYTES + (count - first) * OUTCOME_BYTES);
	}
	return value;
}

Error VirtualGeometryStorage::set_budget(uint64_t p_pool_bytes, uint64_t p_metadata_bytes, uint64_t p_work_bytes) {
	if (p_pool_bytes == 0 || (p_pool_bytes % UNIT_BYTES) != 0 || p_pool_bytes > UINT32_MAX || p_metadata_bytes < metadata_reserved || p_work_bytes < work_reserved) {
		return ERR_INVALID_PARAMETER;
	}
	// Reserve the bounded result queue, both owned snapshots and slot bytes.
	// Copies retained by a feedback reader belong to that reader's allocation.
	const uint64_t bookkeeping = pool.is_null() ? 2 * SNAPSHOT_BYTES + uint64_t(MAX_OUTCOMES) * (sizeof(Outcome) + 2 * OUTCOME_BYTES) + p_pool_bytes / UNIT_BYTES +
					2 * REQUEST_HEADER_BYTES + uint64_t(MAX_REQUESTS) * (sizeof(RequestRecord) + 2 * REQUEST_RECORD_BYTES)
												: 0;
	if (bookkeeping > p_metadata_bytes - metadata_reserved) {
		return ERR_OUT_OF_MEMORY;
	}
	if (p_pool_bytes != pool_bytes) {
		// Pool migration is not yet connected. Preserve the current allocation and
		// grant until all mesh and GPU owners are gone; never overwrite live slots.
		if (!assets.is_empty() || !detached.is_empty() || occupied_bytes != 0 || staging_bytes != 0) {
			return ERR_BUSY;
		}
		// Release uses RD's deferred buffer destruction. A replacement would
		// overlap physical allocations, so changing a nonempty grant is refused.
		if (pool.is_valid()) {
			return ERR_UNAVAILABLE;
		}
		if (p_pool_bytes != 0) {
			const RID candidate = RD::get_singleton()->storage_buffer_create(p_pool_bytes);
			if (candidate.is_null()) {
				return ERR_CANT_CREATE;
			}
			pool = candidate;
			slots.resize(p_pool_bytes / UNIT_BYTES);
			for (uint8_t &slot : slots) {
				slot = 0;
			}
		}
		pool_bytes = p_pool_bytes;
	}
	metadata_budget = p_metadata_bytes;
	work_budget = p_work_bytes;
	metadata_reserved += bookkeeping;
	_publish_feedback();
	return OK;
}

bool VirtualGeometryStorage::reserve_view_work(uint64_t p_bytes) {
	if (p_bytes == 0 || p_bytes > work_budget - work_reserved) {
		WARN_PRINT_ONCE(vformat("Virtual geometry work capacity refused: requested=%s reserved=%s budget=%s bytes.", p_bytes, work_reserved, work_budget));
		return false;
	}
	work_reserved += p_bytes;
	return true;
}

void VirtualGeometryStorage::retire_view_work(uint64_t p_bytes) {
	if (p_bytes != 0) {
		freed_buffers.push_back({ RD::get_singleton()->get_recording_submission_serial(), p_bytes, false, false });
	}
}

Error VirtualGeometryStorage::register_mesh(RID p_mesh, uint64_t p_generation, const Vector<uint8_t> &p_root, const TypedArray<RID> &p_materials, AABB &r_bounds) {
	if (has(p_mesh)) {
		return ERR_ALREADY_IN_USE;
	}
	if (p_generation == 0 || p_generation > INT64_MAX || p_root.size() < VirtualGeometry::MODEL_ROOT_HEADER_BYTES || p_root.size() > 64 * 1024 * 1024) {
		return ERR_INVALID_PARAMETER;
	}
	VirtualGeometry::GeometryModelRoot root;
	const auto decoded = VirtualGeometry::decode_geometry_model_root(p_root.ptr(), p_root.size(), decode_uint64(p_root.ptr() + 184), metadata_budget - metadata_reserved, root);
	if (decoded.error != VirtualGeometry::ModelCodecError::OK) {
		return decoded.error == VirtualGeometry::ModelCodecError::MEMORY_LIMIT ? ERR_OUT_OF_MEMORY : ERR_INVALID_DATA;
	}
	if (p_materials.size() != int64_t(root.materials.size())) {
		return ERR_INVALID_PARAMETER;
	}
	AABB bounds;
	for (uint32_t axis = 0; axis < 3; ++axis) {
		bounds.position[axis] = root.lower[axis];
		const double extent = double(root.upper[axis]) - double(root.lower[axis]);
		bounds.size[axis] = extent;
		if (double(bounds.size[axis]) < extent) {
			bounds.size[axis] = std::nextafter(bounds.size[axis], std::numeric_limits<real_t>::infinity());
		}
		if (!Math::is_finite(bounds.size[axis])) {
			return ERR_INVALID_DATA;
		}
	}
	const uint32_t table_byte = (uint32_t(p_root.size()) + 15) & ~15u;
	const uint64_t gpu_bytes = table_byte + uint64_t(root.hierarchy.pages.size()) * 4;
	const uint64_t charge = sizeof(Asset) + root_memory(root) + uint64_t(p_materials.size()) * sizeof(RID) + uint64_t(root.hierarchy.pages.size()) * 9;
	if (charge + gpu_bytes > metadata_budget - metadata_reserved || gpu_bytes > UINT32_MAX || pool.is_null() || uint64_t(root.hierarchy.root_pages) * UNIT_BYTES > pool_bytes) {
		return ERR_OUT_OF_MEMORY;
	}
	Vector<uint8_t> gpu = p_root;
	if (gpu.resize(gpu_bytes) != OK) {
		return ERR_OUT_OF_MEMORY;
	}
	uint8_t *gpu_data = gpu.ptrw();
	memset(gpu_data + p_root.size(), 0, table_byte - p_root.size());
	memset(gpu_data + table_byte, 0xff, gpu_bytes - table_byte);
	std::vector<VirtualGeometry::GeometryGpuFixup> root_paths;
	const auto paths = VirtualGeometry::resolve_geometry_gpu_root_paths(root, gpu_bytes / 4, root_paths);
	if (paths != VirtualGeometry::GpuFixupError::OK) {
		return paths == VirtualGeometry::GpuFixupError::MEMORY ? ERR_OUT_OF_MEMORY : ERR_INVALID_DATA;
	}
	for (const auto &command : root_paths) {
		encode_uint32(command.value, gpu_data + command.word * 4);
	}
	const RID metadata = RD::get_singleton()->storage_buffer_create(gpu_bytes, gpu);
	if (metadata.is_null()) {
		return ERR_CANT_CREATE;
	}
	Asset *asset = memnew(Asset);
	asset->mesh = p_mesh;
	asset->generation = p_generation;
	asset->root = std::move(root);
	asset->materials.resize(p_materials.size());
	for (int index = 0; index < p_materials.size(); ++index) {
		asset->materials.write[index] = p_materials[index];
	}
	asset->metadata = charge;
	asset->gpu_bytes = gpu_bytes;
	asset->bindings = { metadata, pool, table_byte / 4, p_generation };
	asset->bindings.parts_word = decode_uint64(p_root.ptr() + 144) / 4;
	asset->bindings.groups_word = decode_uint64(p_root.ptr() + 192) / 4;
	asset->bindings.nodes = asset->root.hierarchy.nodes.size();
	asset->bindings.clusters = asset->root.cluster_count;
	asset->bindings.root_pages = asset->root.hierarchy.root_pages;
	asset->bindings.pages = asset->root.hierarchy.pages.size();
	// The root was validated as a bounded tree before registration.
	const auto depth = [&](auto &&self, uint32_t node) -> uint32_t {
		uint32_t result = 1;
		for (const auto &child : asset->root.hierarchy.nodes[node].children) {
			if (child.node != UINT32_MAX) {
				result = MAX(result, 1 + self(self, child.node));
			}
		}
		return result;
	};
	asset->bindings.hierarchy_depth = depth(depth, 0);
	asset->committed.resize(asset->root.hierarchy.pages.size(), 0);
	asset->physical_slots.resize(asset->root.hierarchy.pages.size(), UINT32_MAX);
	asset->dependency_users.resize(asset->root.hierarchy.pages.size(), 0);
	metadata_reserved += charge + gpu_bytes;
	gpu_metadata_bytes += gpu_bytes;
	assets.insert(p_mesh, asset);
	r_bounds = bounds;
	_publish_feedback();
	return OK;
}

void VirtualGeometryStorage::_outcome(Page *p_page, uint32_t p_status, Error p_error) {
	if (p_status == PAGE_RESIDENT || p_status == PAGE_FAILED) {
		p_page->install_pending = false;
	}
	--outcome_reservations;
	outcomes.push_back({ ++sequence, p_page->asset->mesh.get_id(), p_page->asset->generation, p_page->lease,
			p_page->upload, p_page->index, p_status, p_error, RD::get_singleton()->get_completed_submission_serial() });
}

uint32_t VirtualGeometryStorage::_allocate(uint32_t p_units) {
	// Pack roots from the high end, preserving aligned four-unit detail ranges.
	for (uint32_t index = 0; index < slots.size() / p_units; ++index) {
		const uint32_t first = p_units == 1 ? slots.size() - 1 - index : index * p_units;
		bool available = true;
		for (uint32_t offset = 0; offset < p_units; ++offset) {
			available &= slots[first + offset] == 0;
		}
		if (available) {
			for (uint32_t offset = 0; offset < p_units; ++offset) {
				slots[first + offset] = 1;
			}
			occupied_bytes += uint64_t(p_units) * UNIT_BYTES;
			return first;
		}
	}
	return UINT32_MAX;
}

void VirtualGeometryStorage::_retire(Page *p_page) {
	if (p_page->canceled) {
		return;
	}
	p_page->canceled = true;
	if (p_page->root_counted) {
		--p_page->asset->roots_transcoded;
		p_page->root_counted = false;
	}
	_drop_dependencies(p_page);
	if (p_page->state != Page::QUEUED && p_page->state != Page::TRANSCODING) {
		_begin_retirement(p_page);
	}
}

void VirtualGeometryStorage::_remove_page_reference(LocalVector<Page *> &p_list, Page *p_page) {
	for (uint32_t index = 0; index < p_list.size(); ++index) {
		if (p_list[index] == p_page) {
			p_list.remove_at_unordered(index);
			return;
		}
	}
}

Span<uint32_t> VirtualGeometryStorage::_dependencies(const Page *p_page) {
	const auto &directory = p_page->asset->root.directory;
	const auto &location = directory.pages[p_page->index];
	return location.dependency_count == 0 ? Span<uint32_t>() : Span<uint32_t>(directory.dependencies.data() + location.dependency_start, location.dependency_count);
}

void VirtualGeometryStorage::_remove_waiting(Page *p_page) {
	Asset *asset = p_page->asset;
	if (p_page->waiting_roots) {
		_remove_page_reference(asset->root_waiters, p_page);
		p_page->waiting_roots = false;
	}
	for (uint32_t dependency : _dependencies(p_page)) {
		LocalVector<Page *> *list = asset->waiters.getptr(dependency);
		if (list != nullptr) {
			_remove_page_reference(*list, p_page);
			if (list->is_empty()) {
				asset->waiters.erase(dependency);
			}
		}
	}
}

void VirtualGeometryStorage::_drop_dependencies(Page *p_page) {
	if (p_page->dependencies_held) {
		for (uint32_t dependency : _dependencies(p_page)) {
			--p_page->asset->dependency_users[dependency];
		}
		p_page->dependencies_held = false;
	}
}

void VirtualGeometryStorage::_begin_retirement(Page *p_page) {
	if (p_page->state == Page::UNINSTALLING || p_page->state == Page::RETIRING) {
		return;
	}
	if (p_page->state == Page::PUBLISHING) {
		_remove_page_reference(publishing, p_page);
	} else if (p_page->state == Page::INSTALL_READY) {
		_remove_page_reference(ready, p_page);
	}
	_remove_waiting(p_page);
	p_page->state = Page::UNINSTALLING;
	uninstalls.push_back(p_page);
}

void VirtualGeometryStorage::_queue_ready(Page *p_page) {
	if (p_page->state == Page::TRANSCODED && !p_page->canceled && p_page->unresolved == 0 && p_page->asset->roots_committed) {
		p_page->state = Page::INSTALL_READY;
		ready.push_back(p_page);
	}
}

void VirtualGeometryStorage::_notify_committed(Page *p_page) {
	Asset *asset = p_page->asset;
	LocalVector<Page *> *found = asset->waiters.getptr(p_page->index);
	if (found != nullptr) {
		LocalVector<Page *> listeners = std::move(*found);
		asset->waiters.erase(p_page->index);
		for (Page *listener : listeners) {
			--listener->unresolved;
			_queue_ready(listener);
		}
	}
}

void VirtualGeometryStorage::_free_metadata(Asset *p_asset) {
	if (p_asset->bindings.metadata.is_valid()) {
		RD::get_singleton()->free_rid(p_asset->bindings.metadata);
		freed_buffers.push_back({ RD::get_singleton()->get_recording_submission_serial(), p_asset->gpu_bytes, true });
		p_asset->bindings.metadata = RID();
	}
}

void VirtualGeometryStorage::release_mesh(RID p_mesh) {
	Asset **found = assets.getptr(p_mesh);
	if (found == nullptr) {
		return;
	}
	Asset *asset = *found;
	assets.erase(p_mesh);
	asset->request_waiter.remove_from_list();
	asset->destroyed = true;
	_free_metadata(asset);
	if (asset->pages.is_empty()) {
		metadata_reserved -= asset->metadata;
		memdelete(asset);
	} else {
		detached.insert(asset);
		for (const KeyValue<uint32_t, Page *> &entry : asset->pages) {
			_retire(entry.value);
		}
	}
	_publish_feedback();
}

void VirtualGeometryStorage::_delete_page(Page *p_page) {
	_remove_waiting(p_page);
	_drop_dependencies(p_page);
	if (p_page->slot != UINT32_MAX) {
		for (uint32_t offset = 0; offset < p_page->units; ++offset) {
			slots[p_page->slot + offset] = 0;
		}
		occupied_bytes -= uint64_t(p_page->units) * UNIT_BYTES;
	}
	work_reserved -= p_page->work;
	metadata_reserved -= sizeof(Page) + 64; // Includes a conservative hash entry allowance.
	Asset *asset = p_page->asset;
	asset->physical_slots[p_page->index] = UINT32_MAX;
	if (p_page->root_counted) {
		--asset->roots_transcoded;
	}
	asset->pages.erase(p_page->index);
	memdelete(p_page);
	if (asset->destroyed && asset->pages.is_empty()) {
		detached.erase(asset);
		metadata_reserved -= asset->metadata;
		memdelete(asset);
	}
}

void VirtualGeometryStorage::_free_source(Page *p_page) {
	if (p_page->source.is_valid()) {
		RD::get_singleton()->free_rid(p_page->source);
		freed_buffers.push_back({ RD::get_singleton()->get_recording_submission_serial(), uint64_t(p_page->bytes.size()) });
		// The allocation remains charged after RID release, until RD has waited
		// the frame fence that owns deferred destruction of this buffer.
		p_page->work -= p_page->bytes.size();
		p_page->source = RID();
	}
	p_page->bytes.clear();
}

Error VirtualGeometryStorage::submit_pages(const Vector<uint8_t> &p_records, const TypedArray<PackedByteArray> &p_payloads) {
	if (p_records.is_empty() || p_records.size() % REQUEST_BYTES != 0 || p_records.size() / REQUEST_BYTES > 1024) {
		return ERR_INVALID_PARAMETER;
	}
	_prune_outcomes();
	const uint32_t count = p_records.size() / REQUEST_BYTES;
	if (outcomes.size() + outcome_reservations + count > MAX_OUTCOMES) {
		return ERR_BUSY;
	}
	HashMap<RID, HashSet<uint32_t>> seen;
	LocalVector<Page *> prepared;
	LocalVector<Page *> removals;
	uint64_t added_work = 0;
	uint64_t added_metadata = 0;
	Error refusal = OK;
	for (uint32_t index = 0; index < count; ++index) {
		const uint8_t *record = p_records.ptr() + index * REQUEST_BYTES;
		const RID mesh = RID::from_uint64(decode_uint64(record));
		Asset **found = assets.getptr(mesh);
		const uint64_t generation = decode_uint64(record + 8);
		const uint64_t lease = decode_uint64(record + 16);
		const uint64_t upload = decode_uint64(record + 24);
		const uint32_t page = decode_uint32(record + 32);
		const uint32_t payload = decode_uint32(record + 36);
		const uint32_t operation = decode_uint32(record + 40);
		if (found == nullptr || (*found)->generation != generation || lease == 0 || upload == 0 || page >= (*found)->root.directory.pages.size() || operation > 1 || seen[mesh].has(page)) {
			refusal = ERR_INVALID_PARAMETER;
			break;
		}
		seen[mesh].insert(page);
		Asset *asset = *found;
		Page **existing = asset->pages.getptr(page);
		if (operation == 1) {
			if (page < asset->root.hierarchy.root_pages || payload != UINT32_MAX || existing == nullptr || (*existing)->lease != lease || (*existing)->upload != upload || (*existing)->canceled) {
				refusal = ERR_INVALID_PARAMETER;
				break;
			}
			removals.push_back(*existing);
			continue;
		}
		if (existing != nullptr || payload >= uint64_t(p_payloads.size())) {
			refusal = existing != nullptr ? ERR_ALREADY_IN_USE : ERR_INVALID_PARAMETER;
			break;
		}
		const Vector<uint8_t> bytes = p_payloads[payload];
		const auto &location = asset->root.directory.pages[page];
		// Bound both owned bytes and fixup preparation before decoding. RD's
		// internal transfer allocator is not reported as measured service memory.
		const uint64_t work = uint64_t(bytes.size()) * 2 + uint64_t(location.fixup_bytes) * 4;
		added_work += work;
		added_metadata += sizeof(Page) + 64;
		if (added_work > work_budget - work_reserved || added_metadata > metadata_budget - metadata_reserved) {
			refusal = ERR_OUT_OF_MEMORY;
			break;
		}
		Page *candidate = memnew(Page);
		prepared.push_back(candidate);
		candidate->asset = asset;
		candidate->index = page;
		candidate->lease = lease;
		candidate->upload = upload;
		candidate->work = work;
		candidate->bytes = bytes;
		candidate->units = page < asset->root.hierarchy.root_pages ? 1 : 4;
		if (VirtualGeometry::validate_geometry_model_page_layout(asset->root, page, bytes.ptr(), bytes.size(), candidate->fixups).error != VirtualGeometry::ModelCodecError::OK) {
			refusal = ERR_INVALID_DATA;
			break;
		}
		if (page < asset->root.hierarchy.root_pages) {
			const uint32_t mask = VirtualGeometry::CLUSTER_FLAG_ROOT_LEAF | VirtualGeometry::CLUSTER_FLAG_ROOT_GROUP;
			for (uint32_t cluster = 0; cluster < asset->root.hierarchy.pages[page].clusters; ++cluster) {
				const uint32_t offset = VirtualGeometry::GEOMETRY_PAGE_HEADER_BYTES + cluster * VirtualGeometry::GEOMETRY_PAGE_CLUSTER_BYTES + 28;
				candidate->root_clusters += (decode_uint32(bytes.ptr() + offset) & mask) == mask;
			}
		}
		for (uint32_t dependency : _dependencies(candidate)) {
			Page **target = asset->pages.getptr(dependency);
			if (target != nullptr && (*target)->canceled) {
				refusal = ERR_BUSY;
				break;
			}
		}
		if (refusal != OK) {
			break;
		}
	}
	if (refusal == OK) {
		HashMap<RID, HashMap<uint32_t, uint32_t>> retiring_users;
		HashMap<RID, HashMap<uint32_t, uint32_t>> incoming_users;
		for (Page *page : prepared) {
			for (uint32_t dependency : _dependencies(page)) {
				++incoming_users[page->asset->mesh][dependency];
			}
		}
		for (Page *page : removals) {
			for (uint32_t dependency : _dependencies(page)) {
				++retiring_users[page->asset->mesh][dependency];
			}
		}
		for (Page *page : removals) {
			if (uint64_t(page->asset->dependency_users[page->index]) + incoming_users[page->asset->mesh][page->index] > retiring_users[page->asset->mesh][page->index]) {
				refusal = ERR_BUSY;
				break;
			}
			const uint64_t patch_bytes = uint64_t(page->fixups.hierarchy_fixups.size() + page->fixups.cluster_fixups.size() + 1) * 16;
			if (patch_bytes > page->work) {
				added_work += patch_bytes - page->work;
			}
		}
		if (added_work > work_budget - work_reserved) {
			refusal = ERR_OUT_OF_MEMORY;
		}
	}
	if (refusal == OK) {
		// Admission owns physical slots before accepting caller payloads. This
		// makes queued uploads part of the same capacity ledger as resident and
		// retiring pages, even before the next render submission is recorded.
		for (Page *page : prepared) {
			page->slot = _allocate(page->units);
			if (page->slot == UINT32_MAX) {
				refusal = ERR_OUT_OF_MEMORY;
				break;
			}
		}
	}
	if (refusal != OK) {
		for (Page *page : prepared) {
			if (page->slot != UINT32_MAX) {
				for (uint32_t unit = 0; unit < page->units; ++unit) {
					slots[page->slot + unit] = 0;
				}
				occupied_bytes -= uint64_t(page->units) * UNIT_BYTES;
			}
			memdelete(page);
		}
		return refusal;
	}
	outcome_reservations += count;
	work_reserved += added_work;
	metadata_reserved += added_metadata;
	for (Page *page : prepared) {
		Asset *asset = page->asset;
		asset->pages.insert(page->index, page);
		asset->physical_slots[page->index] = page->slot;
		page->dependencies_held = true;
		for (uint32_t dependency : _dependencies(page)) {
			++asset->dependency_users[dependency];
			if (page->index >= asset->root.hierarchy.root_pages && dependency >= asset->root.hierarchy.root_pages && !asset->committed[dependency]) {
				asset->waiters[dependency].push_back(page);
				++page->unresolved;
			}
		}
		if (page->index >= asset->root.hierarchy.root_pages && !asset->roots_committed) {
			asset->root_waiters.push_back(page);
			page->waiting_roots = true;
		}
		queued.push_back(page);
	}
	for (Page *page : removals) {
		const uint64_t patch_bytes = uint64_t(page->fixups.hierarchy_fixups.size() + page->fixups.cluster_fixups.size() + 1) * 16;
		page->work = MAX(page->work, patch_bytes);
		page->retire_outcome = true;
		_retire(page);
	}
	_publish_feedback();
	return OK;
}

Error VirtualGeometryStorage::_make_patch_batch(Page *p_page, bool p_uninstall, PatchBatch &r_batch) {
	Asset *asset = p_page->asset;
	VirtualGeometry::GeometryFixupUpdates updates;
	const auto planned = VirtualGeometry::plan_geometry_fixups(asset->root.hierarchy, p_page->index, p_page->fixups, p_uninstall, asset->committed, updates);
	if (planned != VirtualGeometry::FixupPlanError::OK) {
		return planned == VirtualGeometry::FixupPlanError::MEMORY ? ERR_OUT_OF_MEMORY : ERR_INVALID_DATA;
	}
	std::vector<VirtualGeometry::GeometryGpuFixup> commands;
	const VirtualGeometry::GeometryGpuLayout layout{ asset->gpu_bytes / 4, asset->bindings.page_table_word, uint32_t(pool_bytes / 4) };
	const auto resolved = VirtualGeometry::resolve_geometry_gpu_fixups(asset->root, p_page->index, p_uninstall, updates, asset->physical_slots, layout, commands);
	if (resolved != VirtualGeometry::GpuFixupError::OK) {
		return resolved == VirtualGeometry::GpuFixupError::MEMORY ? ERR_OUT_OF_MEMORY : ERR_INVALID_DATA;
	}
	r_batch.page = p_page;
	if (r_batch.commands.resize(commands.size() * 16) != OK) {
		return ERR_OUT_OF_MEMORY;
	}
	uint8_t *bytes = r_batch.commands.ptrw();
	for (uint32_t index = 0; index < commands.size(); ++index) {
		const auto &command = commands[index];
		encode_uint32(command.buffer, bytes + index * 16);
		encode_uint32(command.word, bytes + index * 16 + 4);
		encode_uint32(command.value, bytes + index * 16 + 8);
		encode_uint32(command.operation, bytes + index * 16 + 12);
	}
	return OK;
}

Error VirtualGeometryStorage::_record_fixup_batch(LocalVector<PatchBatch> &p_batches) {
	if (p_batches.is_empty()) {
		return OK;
	}
	if (fixup_version.is_null()) {
		Vector<String> variants;
		variants.push_back("");
		fixup_shader.initialize(variants);
		fixup_version = fixup_shader.version_create();
		const RID program = fixup_shader.version_get_shader(fixup_version, 0);
		if (program.is_null()) {
			return ERR_CANT_CREATE;
		}
		fixup_pipeline.create_compute_pipeline(program);
	}
	const RID program = fixup_shader.version_get_shader(fixup_version, 0);
	const RID pipeline = fixup_pipeline.get_rid();
	if (program.is_null() || pipeline.is_null()) {
		return ERR_CANT_CREATE;
	}
	Error error = OK;
	// Complete allocation/binding preflight before recording any destination
	// write. Failure can then restore the CPU committed snapshot atomically.
	for (PatchBatch &batch : p_batches) {
		if (uint64_t(batch.commands.size()) > batch.page->work) {
			error = ERR_OUT_OF_MEMORY;
			break;
		}
		batch.source = RD::get_singleton()->storage_buffer_create(batch.commands.size(), batch.commands);
		if (batch.source.is_null()) {
			error = ERR_CANT_CREATE;
			break;
		}
		staging_bytes += batch.commands.size();
		batch.page->work -= batch.commands.size();
		const RD::Uniform input(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, { batch.source });
		const RD::Uniform metadata(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, { batch.page->asset->bindings.metadata });
		const RD::Uniform geometry(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, { pool });
		batch.uniforms = UniformSetCacheRD::get_singleton()->get_cache(program, 0, input, metadata, geometry);
		if (batch.uniforms.is_null()) {
			error = ERR_CANT_CREATE;
			break;
		}
	}
	if (error == OK) {
		const RD::ComputeListID commands = RD::get_singleton()->compute_list_begin();
		RD::get_singleton()->compute_list_bind_compute_pipeline(commands, pipeline);
		for (const PatchBatch &batch : p_batches) {
			const uint32_t count = batch.commands.size() / 16;
			RD::get_singleton()->compute_list_bind_uniform_set(commands, batch.uniforms, 0);
			RD::get_singleton()->compute_list_set_push_constant(commands, &count, sizeof(count));
			RD::get_singleton()->compute_list_dispatch(commands, (count + 63) / 64, 1, 1);
		}
		RD::get_singleton()->compute_list_end();
	}
	for (const PatchBatch &batch : p_batches) {
		if (batch.source.is_valid()) {
			RD::get_singleton()->free_rid(batch.source);
			freed_buffers.push_back({ RD::get_singleton()->get_recording_submission_serial(), uint64_t(batch.commands.size()), false });
		}
	}
	return error;
}

void VirtualGeometryStorage::_process_installs() {
	while (!ready.is_empty()) {
		LocalVector<Page *> pages = std::move(ready);
		for (Page *page : pages) {
			page->asset->committed[page->index] = 1;
		}
		LocalVector<PatchBatch> batches;
		Error error = OK;
		for (Page *page : pages) {
			PatchBatch batch;
			error = _make_patch_batch(page, false, batch);
			if (error != OK) {
				break;
			}
			batches.push_back(std::move(batch));
		}
		if (error == OK) {
			error = _record_fixup_batch(batches);
		}
		if (error != OK) {
			for (Page *page : pages) {
				page->asset->committed[page->index] = 0;
				page->install_error = error;
				_retire(page);
			}
			continue;
		}
		const uint64_t serial = RD::get_singleton()->get_recording_submission_serial();
		for (Page *page : pages) {
			page->state = Page::PUBLISHING;
			page->serial = serial;
			publishing.push_back(page);
		}
		for (Page *page : pages) {
			Asset *asset = page->asset;
			if (!asset->roots_committed && page->index < asset->root.hierarchy.root_pages) {
				asset->roots_committed = true; // All roots were preflighted in this batch.
				LocalVector<Page *> listeners = std::move(asset->root_waiters);
				for (Page *listener : listeners) {
					listener->waiting_roots = false;
					_queue_ready(listener);
				}
			}
			_notify_committed(page);
		}
	}
}

void VirtualGeometryStorage::_process_uninstalls() {
	if (uninstalls.is_empty()) {
		return;
	}
	LocalVector<Page *> pages = std::move(uninstalls);
	LocalVector<Page *> committed;
	for (Page *page : pages) {
		if (page->asset->committed[page->index] && !page->asset->destroyed) {
			committed.push_back(page);
		}
		page->asset->committed[page->index] = 0;
	}
	LocalVector<PatchBatch> batches;
	Error error = OK;
	for (Page *page : committed) {
		PatchBatch batch;
		error = _make_patch_batch(page, true, batch);
		if (error != OK) {
			break;
		}
		batches.push_back(std::move(batch));
	}
	if (error == OK) {
		error = _record_fixup_batch(batches);
	}
	if (error != OK) {
		for (Page *page : committed) {
			page->asset->committed[page->index] = 1;
			page->canceled = false;
			page->dependencies_held = true;
			for (uint32_t dependency : _dependencies(page)) {
				++page->asset->dependency_users[dependency];
			}
			if (page->retire_outcome) {
				_outcome(page, PAGE_RETIRE_FAILED, error);
				page->retire_outcome = false;
			}
			page->state = page->install_pending ? Page::PUBLISHING : Page::RESIDENT;
			if (page->install_pending) {
				publishing.push_back(page);
			}
		}
	}
	const uint64_t serial = RD::get_singleton()->get_recording_submission_serial();
	for (Page *page : pages) {
		if (page->state != Page::UNINSTALLING) {
			continue; // A refused uninstall preserved its published ownership.
		}
		page->state = Page::RETIRING;
		page->serial = serial;
		retiring_bytes += uint64_t(page->units) * UNIT_BYTES;
		retiring.push_back(page);
	}
}

void VirtualGeometryStorage::update() {
	if (pool.is_null() && assets.is_empty() && snapshot.is_empty()) {
		return;
	}
	const uint64_t recording = RD::get_singleton()->get_recording_submission_serial();
	if (recording == updated_serial) {
		return;
	}
	updated_serial = recording;
	++request_epoch;
	_consume_requests();
	_publish_requests();
	const uint64_t completed = RD::get_singleton()->get_completed_submission_serial();
	const uint64_t disposed = RD::get_singleton()->get_retired_resource_serial();
	for (uint32_t index = 0; index < freed_buffers.size();) {
		if (freed_buffers[index].serial <= disposed) {
			if (freed_buffers[index].metadata) {
				gpu_metadata_bytes -= freed_buffers[index].bytes;
				metadata_reserved -= freed_buffers[index].bytes;
			} else {
				if (freed_buffers[index].staging) {
					staging_bytes -= freed_buffers[index].bytes;
				}
				work_reserved -= freed_buffers[index].bytes;
			}
			freed_buffers.remove_at_unordered(index);
		} else {
			++index;
		}
	}
	for (uint32_t index = 0; index < retiring.size();) {
		Page *page = retiring[index];
		if (page->serial > completed) {
			++index;
			continue;
		}
		retiring.remove_at_unordered(index);
		retiring_bytes -= uint64_t(page->units) * UNIT_BYTES;
		if (page->install_pending) {
			_outcome(page, PAGE_FAILED, page->install_error);
		}
		if (page->retire_outcome) {
			_outcome(page, PAGE_RETIRED, OK);
		}
		_delete_page(page);
	}
	for (uint32_t index = 0; index < publishing.size();) {
		Page *page = publishing[index];
		if (page->serial > completed) {
			++index;
			continue;
		}
		publishing.remove_at_unordered(index);
		page->state = Page::RESIDENT;
		if (page->index < page->asset->root.hierarchy.root_pages) {
			page->asset->bindings.root_clusters += page->root_clusters;
			++page->asset->roots_published;
		}
		_outcome(page, PAGE_RESIDENT, OK);
	}
	for (uint32_t index = 0; index < inflight.size();) {
		Page *page = inflight[index];
		if (page->serial > completed) {
			++index;
			continue;
		}
		inflight.remove_at_unordered(index);
		_free_source(page);
		page->state = Page::TRANSCODED;
		if (page->canceled) {
			_begin_retirement(page);
		} else if (page->index < page->asset->root.hierarchy.root_pages) {
			Asset *asset = page->asset;
			page->root_counted = true;
			if (++asset->roots_transcoded == asset->root.hierarchy.root_pages) {
				for (uint32_t root = 0; root < asset->root.hierarchy.root_pages; ++root) {
					Page *root_page = asset->pages[root];
					root_page->state = Page::INSTALL_READY;
					ready.push_back(root_page);
				}
			}
		} else {
			_queue_ready(page);
		}
	}
	_process_uninstalls();
	_process_installs();
	for (Page *page : queued) {
		if (page->canceled) {
			_outcome(page, PAGE_FAILED, page->install_error);
			if (page->retire_outcome) {
				_outcome(page, PAGE_RETIRED, OK);
			}
			_delete_page(page);
			continue;
		}
		Error error = OK;
		if (error == OK) {
			page->source = RD::get_singleton()->storage_buffer_create(page->bytes.size(), page->bytes);
			if (page->source.is_null()) {
				error = ERR_CANT_CREATE;
			} else {
				staging_bytes += page->bytes.size();
				const auto &location = page->asset->root.directory.pages[page->index];
				Vector<VirtualGeometryTranscoder::Page> jobs;
				jobs.push_back({ 0, page->slot * UNIT_BYTES, page->asset->root.hierarchy.pages[page->index].clusters, location.gpu_bytes });
				error = transcoder.record(page->source, pool, jobs);
			}
		}
		if (error != OK) {
			_free_source(page);
			_outcome(page, PAGE_FAILED, error);
			_delete_page(page);
			continue;
		}
		// Upload staging may have flushed and begun a newer RD recording frame.
		// The ticket must identify the frame that actually contains the dispatch.
		page->serial = RD::get_singleton()->get_recording_submission_serial();
		page->state = Page::TRANSCODING;
		inflight.push_back(page);
	}
	queued.clear();
	_publish_feedback();
}

} // namespace RendererRD
