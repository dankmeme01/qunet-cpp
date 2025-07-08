#include <qunet/compression/ZstdCompressor.hpp>
#include <qunet/util/assert.hpp>
#include <qunet/Log.hpp>
#include <zstd.h>

namespace qn {

ZstdCompressor::ZstdCompressor() {}

ZstdCompressor::~ZstdCompressor() {
    ZSTD_freeCCtx(m_ctx);
    ZSTD_freeCDict(m_dict);
}

ZstdCompressor::ZstdCompressor(ZstdCompressor&& other) {
    *this = std::move(other);
}

ZstdCompressor& ZstdCompressor::operator=(ZstdCompressor&& other) {
    if (this != &other) {
        ZSTD_freeCCtx(m_ctx);
        ZSTD_freeCDict(m_dict);

        m_ctx = other.m_ctx;
        m_dict = other.m_dict;
        m_level = other.m_level;
        other.m_ctx = nullptr;
        other.m_dict = nullptr;
    }

    return *this;
}

CompressorResult<> ZstdCompressor::init(int level) {
    if (m_ctx) {
        return Err(CompressorError::AlreadyInitialized);
    }

    m_ctx = ZSTD_createCCtx();
    m_level = level;
    QN_ASSERT(m_ctx && "Failed to create ZSTD compression context");

    return Ok();
}

CompressorResult<> ZstdCompressor::initWithDictionary(const void* data, size_t size, int level) {
    GEODE_UNWRAP(this->init(level));

    m_dict = ZSTD_createCDict(data, size, level);

    if (!m_dict) {
        return Err(CompressorError::InvalidDictionary);
    }

    return Ok();
}

CompressorResult<> ZstdCompressor::compress(
    const void* src, size_t srcSize,
    void* dst, size_t& dstSize
) {
    if (!m_ctx) {
        return Err(CompressorError::NotInitialized);
    }

    size_t dsize = 0;

    if (m_dict) {
        dsize = ZSTD_compress_usingCDict(m_ctx, dst, dstSize, src, srcSize, m_dict);
    } else {
        dsize = ZSTD_compressCCtx(m_ctx, dst, dstSize, src, srcSize, m_level);
    }

    if (ZSTD_isError(dsize)) {
        auto ec = ZSTD_getErrorCode(dsize);
        auto msg = ZSTD_getErrorName(ec);
        log::warn("ZstdCompressor: Decompression failed: {} ({})", msg, ec);
        return Err(CompressorError::CompressionFailed);
    }

    dstSize = dsize;
    return Ok();
}

}