#include "virtual_geometry_work_memory.h"

#include "core/math/math_funcs.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"

namespace RendererRD {

Error VirtualGeometryWorkMemory::ensure(RID &r_buffer, uint32_t &r_words, uint64_t p_words, bool p_indirect) {
	if (p_words > UINT32_MAX / 4) {
		return ERR_OUT_OF_MEMORY;
	}
	if (r_words >= p_words && r_buffer.is_valid()) {
		return OK;
	}
	uint32_t words = Math::next_power_of_2(uint32_t(MAX(uint64_t(1), p_words)));
	if (uint64_t(words) * 4 > UINT32_MAX) {
		words = uint32_t(p_words);
	}
	const RID candidate = storage(words * 4, p_indirect ? RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT : 0);
	if (candidate.is_null()) {
		return ERR_OUT_OF_MEMORY;
	}
	free(r_buffer);
	r_buffer = candidate;
	r_words = words;
	return OK;
}

RID VirtualGeometryWorkMemory::storage(uint32_t p_bytes, BitField<RD::StorageBufferUsage> p_usage) {
	MeshStorage *storage = MeshStorage::get_singleton();
	if (!storage->reserve_virtual_geometry_work(p_bytes)) {
		return RID();
	}
	const RID buffer = RD::get_singleton()->storage_buffer_create(p_bytes, {}, p_usage);
	if (buffer.is_null()) {
		storage->retire_virtual_geometry_work(p_bytes);
		return RID();
	}
	buffers.insert(buffer, p_bytes);
	return buffer;
}

RID VirtualGeometryWorkMemory::uniform(uint32_t p_bytes) {
	MeshStorage *storage = MeshStorage::get_singleton();
	if (!storage->reserve_virtual_geometry_work(p_bytes)) {
		return RID();
	}
	const RID buffer = RD::get_singleton()->uniform_buffer_create(p_bytes);
	if (buffer.is_null()) {
		storage->retire_virtual_geometry_work(p_bytes);
		return RID();
	}
	buffers.insert(buffer, p_bytes);
	return buffer;
}

void VirtualGeometryWorkMemory::free(RID p_buffer) {
	if (p_buffer.is_null()) {
		return;
	}
	const uint64_t *bytes = buffers.getptr(p_buffer);
	ERR_FAIL_NULL(bytes);
	RD::get_singleton()->free_rid(p_buffer);
	MeshStorage::get_singleton()->retire_virtual_geometry_work(*bytes);
	buffers.erase(p_buffer);
}

VirtualGeometryWorkMemory::~VirtualGeometryWorkMemory() {
	while (!buffers.is_empty()) {
		free(buffers.begin()->key);
	}
}

} // namespace RendererRD
