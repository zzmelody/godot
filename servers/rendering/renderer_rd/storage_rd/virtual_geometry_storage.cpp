#include "virtual_geometry_storage.h"

#include "core/io/marshalls.h"

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
	for (const KeyValue<RID, Asset *> &entry : assets) {
		detached.insert(entry.value);
	}
	for (Asset *asset : detached) {
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
		pool_bytes, occupied_bytes, retiring_bytes, metadata_reserved, staging_bytes, work_reserved, sequence };
	for (uint32_t word = 0; word < 9; ++word) {
		encode_uint64(header[word], bytes + word * 8);
	}
	encode_uint32(outcomes.size(), bytes + 72);
	encode_uint32(OUTCOME_BYTES, bytes + 76);
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
	const uint32_t count = decode_uint32(value.ptr() + 72);
	while (first < count && decode_uint64(value.ptr() + SNAPSHOT_BYTES + first * OUTCOME_BYTES) <= p_after_sequence) {
		++first;
	}
	if (first != 0) {
		uint8_t *bytes = value.ptrw();
		memmove(bytes + SNAPSHOT_BYTES, bytes + SNAPSHOT_BYTES + first * OUTCOME_BYTES, (count - first) * OUTCOME_BYTES);
		encode_uint32(count - first, bytes + 72);
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
	const uint64_t bookkeeping = pool.is_null() ? 2 * SNAPSHOT_BYTES + uint64_t(MAX_OUTCOMES) * (sizeof(Outcome) + 2 * OUTCOME_BYTES) + p_pool_bytes / UNIT_BYTES : 0;
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

Error VirtualGeometryStorage::register_mesh(RID p_mesh, uint64_t p_generation, const Vector<uint8_t> &p_root, const TypedArray<RID> &p_materials) {
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
	const uint64_t charge = sizeof(Asset) + root_memory(root) + uint64_t(p_materials.size()) * sizeof(RID);
	if (charge > metadata_budget - metadata_reserved || pool.is_null() || uint64_t(root.hierarchy.root_pages) * UNIT_BYTES > pool_bytes) {
		return ERR_OUT_OF_MEMORY;
	}
	Asset *asset = memnew(Asset);
	asset->mesh = p_mesh;
	asset->generation = p_generation;
	asset->root = std::move(root);
	asset->materials = p_materials.duplicate();
	asset->metadata = charge;
	metadata_reserved += charge;
	assets.insert(p_mesh, asset);
	_publish_feedback();
	return OK;
}

void VirtualGeometryStorage::_outcome(Page *p_page, uint32_t p_status, Error p_error) {
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
	p_page->canceled = true;
	if (p_page->state == Page::TRANSCODED) {
		p_page->state = Page::RETIRING;
		p_page->serial = RD::get_singleton()->get_recording_submission_serial();
		retiring_bytes += uint64_t(p_page->units) * UNIT_BYTES;
		retiring.push_back(p_page);
	}
}

void VirtualGeometryStorage::release_mesh(RID p_mesh) {
	Asset **found = assets.getptr(p_mesh);
	if (found == nullptr) {
		return;
	}
	Asset *asset = *found;
	assets.erase(p_mesh);
	asset->destroyed = true;
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
	if (p_page->slot != UINT32_MAX) {
		for (uint32_t offset = 0; offset < p_page->units; ++offset) {
			slots[p_page->slot + offset] = 0;
		}
		occupied_bytes -= uint64_t(p_page->units) * UNIT_BYTES;
	}
	work_reserved -= p_page->work;
	metadata_reserved -= sizeof(Page) + 64; // Includes a conservative hash entry allowance.
	Asset *asset = p_page->asset;
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
			if (payload != UINT32_MAX || existing == nullptr || (*existing)->lease != lease || (*existing)->upload != upload || (*existing)->canceled) {
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
	}
	if (refusal != OK) {
		for (Page *page : prepared) {
			memdelete(page);
		}
		return refusal;
	}
	outcome_reservations += count;
	work_reserved += added_work;
	metadata_reserved += added_metadata;
	for (Page *page : prepared) {
		page->asset->pages.insert(page->index, page);
		queued.push_back(page);
	}
	for (Page *page : removals) {
		page->retire_outcome = true;
		_retire(page);
	}
	_publish_feedback();
	return OK;
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
	const uint64_t completed = RD::get_singleton()->get_completed_submission_serial();
	const uint64_t disposed = RD::get_singleton()->get_retired_resource_serial();
	for (uint32_t index = 0; index < freed_buffers.size();) {
		if (freed_buffers[index].serial <= disposed) {
			staging_bytes -= freed_buffers[index].bytes;
			work_reserved -= freed_buffers[index].bytes;
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
		if (page->retire_outcome) {
			_outcome(page, 1, OK);
		}
		_delete_page(page);
	}
	for (uint32_t index = 0; index < inflight.size();) {
		Page *page = inflight[index];
		if (page->serial > completed) {
			++index;
			continue;
		}
		inflight.remove_at_unordered(index);
		_free_source(page);
		_outcome(page, page->canceled ? 2 : 0, page->canceled ? ERR_SKIP : OK);
		page->state = Page::TRANSCODED;
		if (page->canceled) {
			_retire(page);
		}
	}
	for (Page *page : queued) {
		if (page->canceled) {
			_outcome(page, 2, ERR_SKIP);
			if (page->retire_outcome) {
				_outcome(page, 1, OK);
			}
			_delete_page(page);
			continue;
		}
		page->slot = _allocate(page->units);
		Error error = page->slot == UINT32_MAX ? ERR_OUT_OF_MEMORY : OK;
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
			_outcome(page, 2, error);
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
