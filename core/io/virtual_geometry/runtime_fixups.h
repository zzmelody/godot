#ifndef VIRTUAL_GEOMETRY_RUNTIME_FIXUPS_H
#define VIRTUAL_GEOMETRY_RUNTIME_FIXUPS_H

#include "model_codec.h"

namespace VirtualGeometry {

// Runtime-only std430 transfer record, explicitly encoded as four LE words.
// Buffer 0 is this model's root/table, buffer 1 the shared physical page pool.
// Buffer 0 uses exchange=0. Buffer 1 uses atomic OR=1 or AND with ~value=2.
struct GeometryGpuFixup {
	uint32_t buffer = 0;
	uint32_t word = 0;
	uint32_t value = 0;
	uint32_t operation = 0;
};

struct GeometryGpuLayout {
	uint32_t metadata_words = 0;
	uint32_t page_table_word = 0;
	uint32_t pool_words = 0;
};

enum class GpuFixupError { OK,
	INVALID_LAYOUT,
	INVALID_REFERENCE,
	MEMORY };

// Resolve admitted logical fixups through the installer's slot snapshot.
// Slots use 32 KiB units; UINT32_MAX means no physical allocation. A runtime
// branch reference packs slot<<8 | first_cluster, matching UE's page/cluster
// addressing structure with Veya's own pool units and serialized node layout.
// Includes this page's table update. Old output survives every refusal.
GpuFixupError resolve_geometry_gpu_fixups(const GeometryModelRoot &p_root, uint32_t p_page, bool p_uninstall,
		const GeometryFixupUpdates &p_updates, const std::vector<uint32_t> &p_slots,
		const GeometryGpuLayout &p_layout, std::vector<GeometryGpuFixup> &r_commands);

} // namespace VirtualGeometry

#endif // VIRTUAL_GEOMETRY_RUNTIME_FIXUPS_H
