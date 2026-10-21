#ifndef VIRTUAL_GEOMETRY_MODEL_DECODER_H
#define VIRTUAL_GEOMETRY_MODEL_DECODER_H

#include "core/io/virtual_geometry/model_codec.h"

#include <map>

namespace VirtualGeometry {

// Cooker-only sequential reader. The immutable model outlives this reader.
// Visit get_order() in order. Parent geometry is retained only until its last
// dependent page is decoded. Root-only extraction uses the root page prefix.
// A returned geometry pointer is borrowed until the next decode/initialize.
class GeometryModelPageDecoder {
	const GeometryModelRoot *root = nullptr;
	std::vector<uint32_t> order;
	std::vector<uint32_t> users;
	std::map<uint32_t, GeometryPage> parents;
	GeometryPage current;
	size_t next = 0;

public:
	ModelCodecResult initialize(const GeometryModelRoot &p_root, uint32_t p_pages);
	const std::vector<uint32_t> &get_order() const { return order; }
	ModelCodecResult decode(uint32_t p_page, const uint8_t *p_bytes, size_t p_size,
			const GeometryPage *&r_geometry, HierarchyPage &r_fixups, std::vector<uint8_t> *r_gpu = nullptr);
};

} // namespace VirtualGeometry

#endif // VIRTUAL_GEOMETRY_MODEL_DECODER_H
