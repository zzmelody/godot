// SPDX-License-Identifier: MIT
#pragma once

#include "core/profiling/profiling.h"
#include "core/templates/a_hash_map.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

// Render-boundary borrow only. Reset before resolving a new view; no address
// survives source uploads or resource retirement into the next boundary.
class VirtualGeometrySourceReads {
	AHashMap<RID, RD::BufferInfo> buffers;

	static bool _address(const RD::BufferInfo &p_info, uint64_t p_offset, uint64_t p_bytes, uint64_t &r_address) {
		r_address = 0;
		if (!p_info.storage || p_info.device_address == 0 || p_offset > p_info.size ||
				p_bytes > uint64_t(p_info.size) - p_offset || p_info.device_address > UINT64_MAX - (p_offset + p_bytes)) {
			return false;
		}
		r_address = p_info.device_address + p_offset;
		return true;
	}

public:
	// Value-only proof from this render boundary. Consumers may reuse it before
	// source mutation/retirement; it is neither a resource owner nor a frame cache.
	struct ResolvedBuffer {
		RID buffer;
		RD::BufferInfo info;
	};

	void clear() { buffers.clear(); }
	uint32_t size() const { return buffers.size(); }
	ResolvedBuffer get_resolved(RID p_buffer) const {
		const RD::BufferInfo *info = buffers.getptr(p_buffer);
		return info != nullptr ? ResolvedBuffer{ p_buffer, *info } : ResolvedBuffer{};
	}

	bool resolve(const ResolvedBuffer &p_buffer, uint64_t p_offset, uint64_t p_bytes, uint64_t &r_address) {
		r_address = 0;
		if (p_buffer.buffer.is_null() || !_address(p_buffer.info, p_offset, p_bytes, r_address)) {
			return false;
		}
		if (const RD::BufferInfo *cached = buffers.getptr(p_buffer.buffer)) {
			if (cached->size != p_buffer.info.size || cached->storage != p_buffer.info.storage || cached->device_address != p_buffer.info.device_address) {
				r_address = 0;
				return false;
			}
		} else {
			buffers.insert(p_buffer.buffer, p_buffer.info);
		}
		return true;
	}

	bool resolve(RD *p_device, RID p_buffer, uint64_t p_offset, uint64_t p_bytes, uint64_t &r_address) {
		r_address = 0;
		RD::BufferInfo info;
		if (const RD::BufferInfo *cached = buffers.getptr(p_buffer)) {
			info = *cached;
		} else {
			if (!p_device->buffer_get_info(p_buffer, info) || !info.storage || info.device_address == 0) {
				return false;
			}
			buffers.insert(p_buffer, info);
		}
		return _address(info, p_offset, p_bytes, r_address);
	}

	void record(RD *p_device, RD::ComputeListID p_list) const {
		GodotProfileZone("Render/VirtualGeometry/source_read_edges");
		for (const KeyValue<RID, RD::BufferInfo> &buffer : buffers) {
			p_device->compute_list_add_buffer_read(p_list, buffer.key, buffer.value);
		}
	}
};

} // namespace RendererRD
