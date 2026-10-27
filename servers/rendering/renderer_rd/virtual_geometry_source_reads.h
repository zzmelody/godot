// SPDX-License-Identifier: MIT
#pragma once

#include "core/templates/a_hash_map.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

// Render-boundary borrow only. Reset before resolving a new view; no address
// survives source uploads or resource retirement into the next boundary.
class VirtualGeometrySourceReads {
	AHashMap<RID, RD::BufferInfo> buffers;

public:
	void clear() { buffers.clear(); }
	uint32_t size() const { return buffers.size(); }

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
		if (p_offset > info.size || p_bytes > uint64_t(info.size) - p_offset ||
				info.device_address > UINT64_MAX - (p_offset + p_bytes)) {
			return false;
		}
		r_address = info.device_address + p_offset;
		return true;
	}

	void record(RD *p_device, RD::ComputeListID p_list) const {
		for (const KeyValue<RID, RD::BufferInfo> &buffer : buffers) {
			p_device->compute_list_add_buffer_read(p_list, buffer.key);
		}
	}
};

} // namespace RendererRD
