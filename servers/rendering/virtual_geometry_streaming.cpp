#include "virtual_geometry_streaming.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/io/marshalls.h"
#include "core/os/thread.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_device.h"

#include <cmath>
#include <limits>

VirtualGeometryStreaming *VirtualGeometryStreaming::singleton = nullptr;

VirtualGeometryStreaming::VirtualGeometryStreaming() {
	DEV_ASSERT(singleton == nullptr);
	singleton = this;
	pool_bytes = GLOBAL_DEF("rendering/virtual_geometry/pool_bytes", int64_t(pool_bytes));
	metadata_bytes = GLOBAL_DEF("rendering/virtual_geometry/metadata_bytes", int64_t(metadata_bytes));
	work_bytes = GLOBAL_DEF("rendering/virtual_geometry/work_bytes", int64_t(work_bytes));
}

VirtualGeometryStreaming::~VirtualGeometryStreaming() {
	DEV_ASSERT(reads.empty() && ready.empty() && assets.is_empty());
	// RenderingServer has stopped its owners. The device itself can outlive
	// this service, so its process-resident capacity is never lent out again.
	for (const KeyValue<uint64_t, Transfer> &entry : transfers) {
		_release(entry.value.admission);
		Asset *asset = entry.value.asset;
		--asset->pending;
		_delete_if_finished(asset);
	}
	singleton = nullptr;
}

Error VirtualGeometryStreaming::set_admission(const Callable &p_acquire, const Callable &p_received, const Callable &p_release) {
	ERR_FAIL_COND_V(!Thread::is_main_thread(), ERR_UNAUTHORIZED);
	if (configured || !assets.is_empty()) {
		return ERR_ALREADY_IN_USE;
	}
	if (!p_acquire.is_valid() || !p_received.is_valid() || !p_release.is_valid()) {
		return ERR_INVALID_PARAMETER;
	}
	acquire = p_acquire;
	received = p_received;
	release = p_release;
	return OK;
}

uint64_t VirtualGeometryStreaming::_acquire(uint32_t p_kind, uint64_t p_owner, const String &p_key, uint64_t p_bytes) {
	if (acquire.is_null()) {
		// Standalone engine/editor uses the same explicit configured capacities.
		// Veya installs process-budget callbacks before any scene is projected.
		return ++transaction;
	}
	if (!acquire.is_valid()) {
		return 0;
	}
	const Variant result = acquire.call(p_kind, int64_t(p_owner), p_key, int64_t(p_bytes));
	return result.get_type() == Variant::INT && int64_t(result) > 0 ? uint64_t(int64_t(result)) : 0;
}

void VirtualGeometryStreaming::_received(uint64_t p_token) {
	if (p_token != 0 && received.is_valid()) {
		received.call(int64_t(p_token));
	}
}

void VirtualGeometryStreaming::_release(uint64_t p_token) {
	if (p_token != 0 && release.is_valid()) {
		release.call(int64_t(p_token));
	}
}

Error VirtualGeometryStreaming::_configure() {
	if (configured) {
		return OK;
	}
	RD *rd = RS::get_singleton()->get_rendering_device();
	const uint32_t required = RD::SUBGROUP_BASIC_BIT | RD::SUBGROUP_BALLOT_BIT | RD::SUBGROUP_QUAD_BIT;
	if (rd == nullptr || RS::get_singleton()->get_current_rendering_method() != "forward_plus" ||
			!rd->has_feature(RD::SUPPORTS_BUFFER_ATOMIC_UINT64_MIN_MAX) ||
			rd->limit_get(RD::LIMIT_MAX_STORAGE_IMAGES_PER_SHADER_STAGE) < 12 ||
			rd->limit_get(RD::LIMIT_MAX_STORAGE_BUFFERS_PER_SHADER_STAGE) < 10 ||
			(rd->limit_get(RD::LIMIT_SUBGROUP_OPERATIONS) & required) != required ||
			(rd->limit_get(RD::LIMIT_SUBGROUP_IN_SHADERS) & RD::SHADER_STAGE_COMPUTE_BIT) == 0) {
		return ERR_UNAVAILABLE;
	}
	if (pool_bytes == 0 || pool_bytes > UINT32_MAX || pool_bytes % VirtualGeometry::ROOT_PAGE_GPU_CAPACITY != 0 ||
			metadata_bytes > UINT32_MAX || work_bytes > UINT32_MAX) {
		return ERR_INVALID_PARAMETER;
	}
	const uint64_t host_capacity = uint64_t(MAX_DEMANDS) * sizeof(Demand) * 2 + uint64_t(MAX_READS) * (sizeof(Read) + 128);
	const uint64_t token = _acquire(1, 0, "virtual-geometry:physical-capacity", pool_bytes + metadata_bytes + work_bytes + host_capacity);
	if (token == 0) {
		return ERR_BUSY;
	}
	const Error error = RS::get_singleton()->virtual_geometry_set_budget(pool_bytes, metadata_bytes, work_bytes);
	if (error != OK) {
		_release(token);
		return error;
	}
	pool_admission = token;
	configured = true;
	return OK;
}

Error VirtualGeometryStreaming::install(const PackedByteArray &p_root, const VirtualGeometry::GeometryModelRoot &p_model,
		const TypedArray<RID> &p_materials, const String &p_container, RID &r_mesh) {
	ERR_FAIL_COND_V(!Thread::is_main_thread(), ERR_UNAUTHORIZED);
	if (stopping || p_container.is_empty() || p_container.contains("::")) {
		return ERR_INVALID_PARAMETER;
	}
	const Error configured_error = _configure();
	if (configured_error != OK) {
		return configured_error;
	}
	const uint32_t roots = p_model.hierarchy.root_pages;
	if ((pinned_roots + roots) * VirtualGeometry::ROOT_PAGE_GPU_CAPACITY > pool_bytes || roots > MAX_DEMANDS) {
		return ERR_OUT_OF_MEMORY;
	}
	if (demands.size() + roots > MAX_DEMANDS) {
		return ERR_BUSY;
	}
	// The storage grant accounts for its decoded root and GPU metadata. This
	// separate owner accounts only for host directory, page states and queues.
	const uint64_t resource_bytes = p_root.size() + sizeof(p_model) + p_model.materials.capacity() * sizeof(uint32_t) +
			p_model.groups.capacity() * sizeof(VirtualGeometry::ModelGroupPages) + p_model.hierarchy.nodes.capacity() * sizeof(VirtualGeometry::HierarchyNode) +
			p_model.hierarchy.parts.capacity() * sizeof(VirtualGeometry::HierarchyPart) + p_model.hierarchy.pages.capacity() * sizeof(VirtualGeometry::HierarchyPage) +
			p_model.directory.pages.capacity() * sizeof(VirtualGeometry::PageLocation) + p_model.directory.dependencies.capacity() * sizeof(uint32_t);
	const uint64_t host_bytes = resource_bytes + sizeof(Asset) + uint64_t(p_model.directory.pages.size()) * (sizeof(Page) + sizeof(VirtualGeometry::PageLocation) + sizeof(Demand) + sizeof(Key) + 64) +
			uint64_t(p_model.directory.dependencies.size()) * sizeof(uint32_t) + uint64_t(p_container.length()) * sizeof(char32_t);
	const uint64_t next_generation = ++generation;
	const uint64_t token = _acquire(1, next_generation, vformat("virtual-geometry:host:%s", next_generation), host_bytes);
	if (token == 0) {
		return ERR_BUSY;
	}
	RID mesh = RS::get_singleton()->mesh_create();
	const Error error = RS::get_singleton()->mesh_set_virtual_geometry(mesh, next_generation, p_root, p_materials);
	if (error != OK) {
		RS::get_singleton()->free_rid(mesh);
		_release(token);
		return error;
	}
	Asset *asset = memnew(Asset);
	asset->mesh = mesh;
	asset->generation = next_generation;
	asset->container = p_container;
	asset->directory = p_model.directory;
	asset->pages.resize(asset->directory.pages.size());
	asset->roots = roots;
	asset->metadata_lease = token;
	assets.insert(mesh, asset);
	pinned_roots += roots;
	for (uint32_t page = 0; page < roots; ++page) {
		const bool queued = _demand(asset, page, std::numeric_limits<float>::max());
		DEV_ASSERT(queued);
	}
	r_mesh = mesh;
	return OK;
}

VirtualGeometryStreaming::Asset *VirtualGeometryStreaming::_find(const Key &p_key) const {
	Asset *const *found = assets.getptr(p_key.mesh);
	return found != nullptr && (*found)->generation == p_key.generation ? *found : nullptr;
}

void VirtualGeometryStreaming::_touch(Asset *p_asset, uint32_t p_page) {
	Page &page = p_asset->pages[p_page];
	if (page.listed) {
		residents.splice(residents.end(), residents, page.position);
	}
}

bool VirtualGeometryStreaming::_demand(Asset *p_asset, uint32_t p_page, float p_priority, uint32_t p_depth) {
	if (p_page >= p_asset->pages.size() || p_depth > VirtualGeometry::MAX_HIERARCHY_DEPTH || !p_asset->active || p_asset->error != OK) {
		return false;
	}
	Page &page = p_asset->pages[p_page];
	if (page.state == Page::RETIRING || page.state == Page::FAILED) {
		return false;
	}
	if (page.state != Page::ABSENT) {
		_touch(p_asset, p_page);
		return true;
	}
	const auto &location = p_asset->directory.pages[p_page];
	for (uint32_t index = 0; index < location.dependency_count; ++index) {
		const uint32_t dependency = p_asset->directory.dependencies[location.dependency_start + index];
		if (!_demand(p_asset, dependency, p_priority, p_depth + 1)) {
			return false;
		}
	}
	if (demands.size() == MAX_DEMANDS) {
		return false;
	}
	for (uint32_t index = 0; index < location.dependency_count; ++index) {
		++p_asset->pages[p_asset->directory.dependencies[location.dependency_start + index]].users;
	}
	page.holds_dependencies = true;
	page.state = Page::QUEUED;
	demands.push({ { p_asset->mesh, p_asset->generation, p_page }, p_priority, ++order });
	return true;
}

void VirtualGeometryStreaming::_drop_dependencies(Asset *p_asset, uint32_t p_page) {
	Page &page = p_asset->pages[p_page];
	if (!page.holds_dependencies) {
		return;
	}
	const auto &location = p_asset->directory.pages[p_page];
	for (uint32_t index = 0; index < location.dependency_count; ++index) {
		Page &dependency = p_asset->pages[p_asset->directory.dependencies[location.dependency_start + index]];
		DEV_ASSERT(dependency.users != 0);
		--dependency.users;
	}
	page.holds_dependencies = false;
}

void VirtualGeometryStreaming::_read(void *p_read) {
	Read &read = *static_cast<Read *>(p_read);
	Ref<FileAccess> file = FileAccess::open(read.path, FileAccess::READ, &read.error);
	if (file.is_null()) {
		return;
	}
	if (file->get_length() != read.container_bytes || read.offset > read.container_bytes || read.length > read.container_bytes - read.offset) {
		read.error = ERR_FILE_CORRUPT;
		return;
	}
	if (read.bytes.resize(read.length) != OK) {
		read.error = ERR_OUT_OF_MEMORY;
		return;
	}
	file->seek(read.offset);
	if (file->get_buffer(read.bytes.ptrw(), read.length) != read.length) {
		read.bytes.clear();
		read.error = ERR_FILE_CANT_READ;
	}
}

void VirtualGeometryStreaming::_delete_if_finished(Asset *p_asset) {
	if (!p_asset->active && p_asset->pending == 0) {
		_release(p_asset->metadata_lease);
		memdelete(p_asset);
	}
}

void VirtualGeometryStreaming::_fail_roots(Asset *p_asset, Error p_error) {
	if (p_asset->error == OK) {
		p_asset->error = p_error == OK ? FAILED : p_error;
		// A partial mandatory root set cannot be published. Cancel all accepted
		// uploads while keeping the Mesh RID valid for its scene/resource owner.
		RS::get_singleton()->mesh_clear(p_asset->mesh);
	}
}

void VirtualGeometryStreaming::remove(RID p_mesh) {
	if (!Thread::is_main_thread()) {
		MutexLock lock(removals_mutex);
		deferred_removals.push_back(p_mesh);
		return;
	}
	Asset **found = assets.getptr(p_mesh);
	if (found == nullptr) {
		return;
	}
	Asset *asset = *found;
	assets.erase(p_mesh);
	asset->active = false;
	pinned_roots -= asset->roots;
	// Whole-resource removal is rare. No ordinary frame scans asset page arrays.
	for (Page &page : asset->pages) {
		if (page.listed) {
			residents.erase(page.position);
			page.listed = false;
		}
	}
	RS::get_singleton()->free_rid(p_mesh);
	_delete_if_finished(asset);
}

void VirtualGeometryStreaming::_outcomes() {
	const PackedByteArray bytes = RS::get_singleton()->virtual_geometry_get_feedback(outcomes_after);
	if (bytes.size() < 88 || decode_uint32(bytes.ptr() + 84) != 60 || bytes.size() != 88 + int64_t(decode_uint32(bytes.ptr() + 80)) * 60) {
		return;
	}
	occupied = decode_uint64(bytes.ptr() + 24);
	retiring = decode_uint64(bytes.ptr() + 32);
	for (uint32_t index = 0; index < decode_uint32(bytes.ptr() + 80); ++index) {
		const uint8_t *record = bytes.ptr() + 88 + index * 60;
		const uint64_t lease = decode_uint64(record + 24);
		Transfer *found = transfers.getptr(lease);
		if (found == nullptr || found->asset->mesh.get_id() != decode_uint64(record + 8) || found->asset->generation != decode_uint64(record + 16) ||
				found->page != decode_uint32(record + 40) || found->upload != decode_uint64(record + 32)) {
			continue;
		}
		const Transfer transfer = *found;
		const uint32_t status = decode_uint32(record + 44);
		if ((!transfer.retiring && status != 0 && status != 2) || (transfer.retiring && status != 1 && status != 3)) {
			continue;
		}
		Asset *asset = transfer.asset;
		Page &page = asset->pages[transfer.page];
		_release(transfer.admission);
		transfers.erase(lease);
		--asset->pending;
		if (page.slot_reserved) {
			reserved_slots -= transfer.page < asset->roots ? VirtualGeometry::ROOT_PAGE_GPU_CAPACITY : VirtualGeometry::STREAMING_PAGE_GPU_CAPACITY;
			page.slot_reserved = false;
		}
		if (!transfer.retiring && status == 0 && asset->active) {
			page.state = Page::RESIDENT;
			if (transfer.page < asset->roots) {
				++asset->roots_ready;
			} else {
				residents.push_back({ asset->mesh, asset->generation, transfer.page });
				page.position = --residents.end();
				page.listed = true;
			}
		} else if (transfer.retiring && status == 3 && asset->active) {
			// Refused uninstall retains the original GPU owner and dependency pins.
			page.state = Page::RESIDENT;
			residents.push_back({ asset->mesh, asset->generation, transfer.page });
			page.position = --residents.end();
			page.listed = true;
		} else {
			_drop_dependencies(asset, transfer.page);
			page.state = transfer.retiring ? Page::ABSENT : Page::FAILED;
			if (!transfer.retiring && asset->active) {
				++failed_reads;
				if (transfer.page < asset->roots) {
					_fail_roots(asset, Error(decode_uint32(record + 48)));
				}
				WARN_PRINT(vformat("Virtual geometry page %s failed for %s (error %s).", transfer.page, asset->container, decode_uint32(record + 48)));
			}
		}
		_delete_if_finished(asset);
	}
	outcomes_after = decode_uint64(bytes.ptr() + 64);
}

void VirtualGeometryStreaming::_requests() {
	const PackedByteArray bytes = RS::get_singleton()->virtual_geometry_get_requests(requests_after);
	if (bytes.size() < 32 || decode_uint32(bytes.ptr() + 28) != 32 || bytes.size() != 32 + int64_t(decode_uint32(bytes.ptr() + 24)) * 32) {
		return;
	}
	for (uint32_t index = 0; index < decode_uint32(bytes.ptr() + 24); ++index) {
		const uint8_t *record = bytes.ptr() + 32 + index * 32;
		const Key key{ RID::from_uint64(decode_uint64(record + 8)), decode_uint64(record + 16), decode_uint32(record + 24) };
		Asset *asset = _find(key);
		const float priority = decode_float(record + 28);
		if (asset != nullptr && priority == -1.0f) {
			if (key.page < asset->pages.size()) {
				_touch(asset, key.page);
			}
			continue;
		}
		if (asset != nullptr && std::isfinite(priority) && priority >= 0) {
			_demand(asset, key.page, priority);
		}
	}
	requests_after = decode_uint64(bytes.ptr());
}

void VirtualGeometryStreaming::_receive() {
	for (size_t index = 0; index < reads.size();) {
		Read *read = reads[index];
		if (read->task != WorkerThreadPool::INVALID_TASK_ID && !WorkerThreadPool::get_singleton()->is_task_completed(read->task)) {
			++index;
			continue;
		}
		if (read->task != WorkerThreadPool::INVALID_TASK_ID) {
			WorkerThreadPool::get_singleton()->wait_for_task_completion(read->task);
		}
		reads[index] = reads.back();
		reads.pop_back();
		_received(read->admission);
		if (read->asset->active && read->error == OK && !stopping) {
			ready.push_back(read);
			continue;
		}
		Asset *asset = read->asset;
		Page &page = asset->pages[read->key.page];
		page.state = Page::FAILED;
		_drop_dependencies(asset, read->key.page);
		if (page.slot_reserved) {
			reserved_slots -= read->key.page < asset->roots ? VirtualGeometry::ROOT_PAGE_GPU_CAPACITY : VirtualGeometry::STREAMING_PAGE_GPU_CAPACITY;
			page.slot_reserved = false;
		}
		if (asset->active && read->error != OK) {
			++failed_reads;
			if (read->key.page < asset->roots) {
				_fail_roots(asset, read->error);
			}
			WARN_PRINT(vformat("Virtual geometry range read failed: %s, page %s, error %s.", read->path, read->key.page, read->error));
		}
		_release(read->admission);
		--asset->pending;
		memdelete(read);
		_delete_if_finished(asset);
	}
}

void VirtualGeometryStreaming::_submit() {
	uint32_t submitted = 0;
	for (size_t index = 0; index < ready.size() && submitted < MAX_SUBMISSIONS;) {
		Read *read = ready[index];
		Asset *asset = read->asset;
		Page &page = asset->pages[read->key.page];
		Error error = ERR_SKIP;
		bool dependency_failed = false;
		bool dependencies_ready = asset->roots_ready == asset->roots;
		if (read->key.page < asset->roots) {
			dependencies_ready = true; // GPU storage commits mandatory roots together.
		} else {
			const auto &location = asset->directory.pages[read->key.page];
			for (uint32_t dependency = 0; dependency < location.dependency_count; ++dependency) {
				const Page::State state = asset->pages[asset->directory.dependencies[location.dependency_start + dependency]].state;
				dependency_failed |= state == Page::FAILED;
				dependencies_ready &= state == Page::RESIDENT;
			}
		}
		if (asset->active && asset->error == OK && !stopping && !dependency_failed && !dependencies_ready) {
			++index;
			continue;
		}
		if (asset->active && asset->error == OK && !stopping && !dependency_failed) {
			// IDs are storage transaction identities. The process admission token
			// remains private to this host and is released only on its outcome.
			const uint64_t lease = ++transaction;
			PackedByteArray records;
			records.resize(44);
			uint8_t *record = records.ptrw();
			encode_uint64(asset->mesh.get_id(), record);
			encode_uint64(asset->generation, record + 8);
			encode_uint64(lease, record + 16);
			encode_uint64(lease, record + 24);
			encode_uint32(read->key.page, record + 32);
			encode_uint32(0, record + 36);
			encode_uint32(0, record + 40);
			TypedArray<PackedByteArray> payloads;
			payloads.push_back(read->bytes);
			error = RS::get_singleton()->virtual_geometry_submit_pages(records, payloads);
			if (error == ERR_BUSY || error == ERR_OUT_OF_MEMORY) {
				if (error == ERR_OUT_OF_MEMORY && retiring == 0) {
					_evict();
				}
				++index;
				continue;
			}
			if (error == OK) {
				const uint64_t slot = read->key.page < asset->roots ? VirtualGeometry::ROOT_PAGE_GPU_CAPACITY : VirtualGeometry::STREAMING_PAGE_GPU_CAPACITY;
				reserved_slots -= slot;
				page.slot_reserved = false;
				occupied += slot;
				page.state = Page::UPLOADING;
				page.lease = page.upload = lease;
				transfers.insert(lease, { asset, read->key.page, lease, read->admission, false });
				++submitted;
			}
		}
		if (error != OK) {
			page.state = Page::FAILED;
			_drop_dependencies(asset, read->key.page);
			if (page.slot_reserved) {
				reserved_slots -= read->key.page < asset->roots ? VirtualGeometry::ROOT_PAGE_GPU_CAPACITY : VirtualGeometry::STREAMING_PAGE_GPU_CAPACITY;
				page.slot_reserved = false;
			}
			_release(read->admission);
			--asset->pending;
			if (asset->active) {
				++failed_reads;
				if (read->key.page < asset->roots) {
					_fail_roots(asset, error);
				}
				WARN_PRINT(vformat("Virtual geometry upload refused for %s, page %s, error %s.", asset->container, read->key.page, error));
			}
		}
		ready[index] = ready.back();
		ready.pop_back();
		memdelete(read);
		_delete_if_finished(asset);
	}
}

bool VirtualGeometryStreaming::_evict() {
	for (auto it = residents.begin(); it != residents.end(); ++it) {
		Asset *asset = _find(*it);
		DEV_ASSERT(asset != nullptr);
		Page &page = asset->pages[it->page];
		if (page.users != 0) {
			continue;
		}
		PackedByteArray records;
		records.resize(44);
		uint8_t *record = records.ptrw();
		encode_uint64(asset->mesh.get_id(), record);
		encode_uint64(asset->generation, record + 8);
		encode_uint64(page.lease, record + 16);
		encode_uint64(page.upload, record + 24);
		encode_uint32(it->page, record + 32);
		encode_uint32(UINT32_MAX, record + 36);
		encode_uint32(1, record + 40);
		const Error error = RS::get_singleton()->virtual_geometry_submit_pages(records, {});
		if (error != OK) {
			return false;
		}
		transfers.insert(page.lease, { asset, it->page, page.upload, 0, true });
		++asset->pending;
		page.state = Page::RETIRING;
		page.listed = false;
		residents.erase(it);
		++evictions;
		return true;
	}
	return false;
}

void VirtualGeometryStreaming::_start_reads() {
	while (!demands.empty() && reads.size() + ready.size() < MAX_READS) {
		const Demand demand = demands.top();
		Asset *asset = _find(demand.key);
		if (asset == nullptr || asset->error != OK) {
			demands.pop();
			continue;
		}
		Page &page = asset->pages[demand.key.page];
		const uint64_t slot = demand.key.page < asset->roots ? VirtualGeometry::ROOT_PAGE_GPU_CAPACITY : VirtualGeometry::STREAMING_PAGE_GPU_CAPACITY;
		if (occupied + reserved_slots + slot > pool_bytes) {
			// Only one retirement is requested per frame. Its fence, rather than
			// this request, makes the capacity available to a future read.
			if (retiring == 0) {
				_evict();
			}
			break;
		}
		const auto &location = asset->directory.pages[demand.key.page];
		// disk_bytes includes the trailing fixup chunk in the published format.
		const uint32_t length = location.disk_bytes;
		const String key = vformat("virtual-geometry:page:%s:%s:%s", asset->mesh.get_id(), asset->generation, demand.key.page);
		const uint64_t owner = (asset->generation << 32) | uint64_t(demand.key.page);
		const uint64_t token = _acquire(0, owner, key, length);
		if (token == 0) {
			++refused_reads;
			break;
		}
		demands.pop();
		Read *read = memnew(Read);
		read->key = demand.key;
		read->asset = asset;
		read->path = asset->container;
		read->offset = location.container_offset;
		read->container_bytes = asset->directory.container_bytes;
		read->length = length;
		read->admission = token;
		page.state = Page::READING;
		page.slot_reserved = true;
		reserved_slots += slot;
		++asset->pending;
		read->task = WorkerThreadPool::get_singleton()->add_native_task(&_read, read, false, "Virtual geometry page read");
		reads.push_back(read);
	}
}

Error VirtualGeometryStreaming::get_status(RID p_mesh, bool &r_ready) const {
	r_ready = false;
	Asset *const *found = assets.getptr(p_mesh);
	if (found == nullptr) {
		return ERR_DOES_NOT_EXIST;
	}
	r_ready = (*found)->error == OK && (*found)->roots_ready == (*found)->roots;
	return (*found)->error;
}

Dictionary VirtualGeometryStreaming::get_statistics() const {
	Dictionary result;
	result["assets"] = assets.size();
	result["queued_pages"] = int64_t(demands.size());
	result["reading_pages"] = int64_t(reads.size());
	result["ready_pages"] = int64_t(ready.size());
	result["gpu_pending"] = transfers.size();
	result["resident_detail_pages"] = int64_t(residents.size());
	result["pool_bytes"] = int64_t(configured ? pool_bytes : 0);
	result["reserved_read_bytes"] = int64_t(reserved_slots);
	result["read_deferrals"] = int64_t(refused_reads);
	result["failed_pages"] = int64_t(failed_reads);
	result["evictions"] = int64_t(evictions);
	return result;
}

void VirtualGeometryStreaming::update() {
	ERR_FAIL_COND(!Thread::is_main_thread());
	Vector<RID> removals;
	{
		MutexLock lock(removals_mutex);
		SWAP(removals, deferred_removals);
	}
	for (RID mesh : removals) {
		remove(mesh);
	}
	if (!configured || stopping) {
		return;
	}
	_outcomes();
	_receive();
	_submit();
	_requests();
	_start_reads();
}

void VirtualGeometryStreaming::stop() {
	stopping = true;
	while (!assets.is_empty()) {
		remove(assets.begin()->key);
	}
	for (Read *read : reads) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(read->task);
		read->task = WorkerThreadPool::INVALID_TASK_ID;
	}
	_receive();
	_submit();
	while (!demands.empty()) {
		demands.pop();
	}
}
