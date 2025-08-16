#pragma once

#include "Error.hpp"
#include <stddef.h>

struct ZSTD_CCtx_s;
struct ZSTD_CDict_s;

namespace qn {

class ZstdCompressor {
public:
    ZstdCompressor();
    ~ZstdCompressor();

    ZstdCompressor(const ZstdCompressor&) = delete;
    ZstdCompressor& operator=(const ZstdCompressor&) = delete;
    ZstdCompressor(ZstdCompressor&&);
    ZstdCompressor& operator=(ZstdCompressor&&);

    CompressorResult<> init(int level = 3);
    CompressorResult<> initWithDictionary(const void* dict, size_t size, int level = 3);

    /// Compress data from `src` to `dst`.
    /// `dstSize` will be set to the size of the compressed data.
    CompressorResult<> compress(
        const void* src, size_t srcSize,
        void* dst, size_t& dstSize
    );

    /// Returns the maximum size of the compressed data for a given source size.
    static size_t compressBound(size_t srcSize);

private:
    ZSTD_CCtx_s* m_ctx = nullptr;
    ZSTD_CDict_s* m_dict = nullptr;
    int m_level = 0;
};

/// Compress data from `src` to `dst` using zstandard.
/// `dstSize` will be set to the size of the compressed data.
CompressorResult<> compressZstd(
    const void* src, size_t srcSize,
    void* dst, size_t& dstSize,
    int level
);

}