#include "page_entropy.h"

#include <limits>
#include <memory>
#include <new>
#include <utility>

#ifndef ZSTD_STATIC_LINKING_ONLY
#define ZSTD_STATIC_LINKING_ONLY
#endif
#include <zstd.h>

namespace VirtualGeometry {

PageCodecResult encode_geometry_page_block(const uint8_t *p_bytes, size_t p_size, std::vector<uint8_t> &r_bytes) {
	if (p_bytes == nullptr || p_size == 0 || p_size > UINT32_MAX) {
		return { PageCodecError::INVALID_LENGTH };
	}
	try {
		std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> context(ZSTD_createCCtx(), ZSTD_freeCCtx);
		if (!context) {
			return { PageCodecError::MEMORY };
		}
		if (ZSTD_isError(ZSTD_CCtx_setParameter(context.get(), ZSTD_c_compressionLevel, 3)) ||
				ZSTD_isError(ZSTD_CCtx_setParameter(context.get(), ZSTD_c_checksumFlag, 1)) ||
				ZSTD_isError(ZSTD_CCtx_setParameter(context.get(), ZSTD_c_contentSizeFlag, 1))) {
			return { PageCodecError::INVALID_LAYOUT };
		}
		std::vector<uint8_t> candidate(ZSTD_compressBound(p_size));
		const size_t size = ZSTD_compress2(context.get(), candidate.data(), candidate.size(), p_bytes, p_size);
		if (ZSTD_isError(size) || size > UINT32_MAX) {
			return { PageCodecError::INVALID_LENGTH };
		}
		candidate.resize(size);
		r_bytes = std::move(candidate);
		return {};
	} catch (const std::bad_alloc &) {
		return { PageCodecError::MEMORY };
	}
}

size_t geometry_page_block_decode_scratch_bytes() {
	return ZSTD_estimateDCtxSize();
}

PageCodecResult decode_geometry_page_block(const uint8_t *p_bytes, size_t p_size,
		uint8_t *p_output, size_t p_decoded_bytes) {
	if (p_bytes == nullptr || p_output == nullptr || p_size == 0 || p_decoded_bytes == 0 || p_decoded_bytes > UINT32_MAX) {
		return { PageCodecError::INVALID_LENGTH };
	}
	ZSTD_frameHeader header = {};
	if (ZSTD_getFrameHeader(&header, p_bytes, p_size) != 0 || header.frameType != ZSTD_frame ||
			header.frameContentSize != p_decoded_bytes || !header.checksumFlag || header.dictID != 0 ||
			ZSTD_findFrameCompressedSize(p_bytes, p_size) != p_size) {
		return { PageCodecError::INVALID_HEADER };
	}
	std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> context(ZSTD_createDCtx(), ZSTD_freeDCtx);
	if (!context) {
		return { PageCodecError::MEMORY };
	}
	const size_t size = ZSTD_decompressDCtx(context.get(), p_output, p_decoded_bytes, p_bytes, p_size);
	return ZSTD_isError(size) || size != p_decoded_bytes ? PageCodecResult{ PageCodecError::INVALID_LENGTH } : PageCodecResult{};
}

PageCodecResult decode_geometry_page_block(const uint8_t *p_bytes, size_t p_size,
		size_t p_decoded_bytes, std::vector<uint8_t> &r_bytes) {
	if (p_decoded_bytes == 0 || p_decoded_bytes > UINT32_MAX) {
		return { PageCodecError::INVALID_LENGTH };
	}
	try {
		std::vector<uint8_t> candidate(p_decoded_bytes);
		const auto decoded = decode_geometry_page_block(p_bytes, p_size, candidate.data(), candidate.size());
		if (decoded.error != PageCodecError::OK) {
			return decoded;
		}
		r_bytes = std::move(candidate);
		return {};
	} catch (const std::bad_alloc &) {
		return { PageCodecError::MEMORY };
	}
}

} // namespace VirtualGeometry
