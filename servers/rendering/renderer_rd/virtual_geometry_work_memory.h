#ifndef VIRTUAL_GEOMETRY_WORK_MEMORY_H
#define VIRTUAL_GEOMETRY_WORK_MEMORY_H

#include "core/templates/hash_map.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

// Render-thread owner for view buffers. All views, page staging and feedback
// use the same granted work capacity. Freed bytes remain charged through RD's
// deferred resource destruction serial; replacement can require overlap space.
class VirtualGeometryWorkMemory {
	HashMap<RID, uint64_t> buffers;

public:
	RID storage(uint32_t p_bytes, BitField<RD::StorageBufferUsage> p_usage = 0);
	RID uniform(uint32_t p_bytes);
	Error ensure(RID &r_buffer, uint32_t &r_words, uint64_t p_words, bool p_indirect = false);
	void free(RID p_buffer);
	~VirtualGeometryWorkMemory();
};

} // namespace RendererRD

#endif // VIRTUAL_GEOMETRY_WORK_MEMORY_H
