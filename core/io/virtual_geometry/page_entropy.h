#ifndef VIRTUAL_GEOMETRY_PAGE_ENTROPY_H
#define VIRTUAL_GEOMETRY_PAGE_ENTROPY_H

#include "page_codec.h"

namespace VirtualGeometry {

// One checksummed, content-sized Zstd frame per complete geometry/fixup block.
// Decoder accepts exactly one frame with the directory's exact decoded size.
// Outputs survive refusal. No file, renderer, Resource or global codec state.
PageCodecResult encode_geometry_page_block(const uint8_t *p_bytes, size_t p_size, std::vector<uint8_t> &r_bytes);
PageCodecResult decode_geometry_page_block(const uint8_t *p_bytes, size_t p_size,
		size_t p_decoded_bytes, std::vector<uint8_t> &r_bytes);
// Caller-owned output can contain partial data on failure and must be discarded.
PageCodecResult decode_geometry_page_block(const uint8_t *p_bytes, size_t p_size,
		uint8_t *p_output, size_t p_decoded_bytes);
size_t geometry_page_block_decode_scratch_bytes();

} // namespace VirtualGeometry

#endif // VIRTUAL_GEOMETRY_PAGE_ENTROPY_H
