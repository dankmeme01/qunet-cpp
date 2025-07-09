#include <qunet/compression/ZstdDecompressor.hpp>
#include <qunet/util/assert.hpp>
#include <qunet/Log.hpp>
#include <zstd.h>

namespace qn {

ZstdDecompressor::ZstdDecompressor() {}

ZstdDecompressor::~ZstdDecompressor() {
    ZSTD_freeDCtx(m_ctx);
    ZSTD_freeDDict(m_dict);
}

ZstdDecompressor::ZstdDecompressor(ZstdDecompressor&& other) {
    *this = std::move(other);
}

ZstdDecompressor& ZstdDecompressor::operator=(ZstdDecompressor&& other) {
    if (this != &other) {
        ZSTD_freeDCtx(m_ctx);
        ZSTD_freeDDict(m_dict);

        m_ctx = other.m_ctx;
        m_dict = other.m_dict;
        other.m_ctx = nullptr;
        other.m_dict = nullptr;
    }

    return *this;
}

DecompressorResult<> ZstdDecompressor::init() {
    if (m_ctx) {
        return Err(DecompressorError::AlreadyInitialized);
    }

    m_ctx = ZSTD_createDCtx();
    QN_ASSERT(m_ctx && "Failed to create ZSTD decompression context");

    return Ok();
}

DecompressorResult<> ZstdDecompressor::initWithDictionary(const void* data, size_t size) {
    GEODE_UNWRAP(this->init());

    m_dict = ZSTD_createDDict(data, size);

    if (!m_dict) {
        return Err(DecompressorError::InvalidDictionary);
    }

    return Ok();
}

DecompressorResult<> ZstdDecompressor::decompress(
    const void* src, size_t srcSize,
    void* dst, size_t& dstSize
) {
    if (!m_ctx) {
        return Err(DecompressorError::NotInitialized);
    }

    size_t dsize = 0;

    if (m_dict) {
        dsize = ZSTD_decompress_usingDDict(m_ctx, dst, dstSize, src, srcSize, m_dict);
    } else {
        dsize = ZSTD_decompressDCtx(m_ctx, dst, dstSize, src, srcSize);
    }

    if (ZSTD_isError(dsize)) {
        auto ec = ZSTD_getErrorCode(dsize);
        auto msg = ZSTD_getErrorName(ec);
        log::warn("ZstdDecompressor: Decompression failed: {} ({})", msg, (int) ec);
        return Err(DecompressorError::DecompressionFailed);
    }

    dstSize = dsize;
    return Ok();
}

}