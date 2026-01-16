#pragma once

#include "Error.hpp"
#include <stddef.h>

struct ZSTD_DCtx_s;
struct ZSTD_DDict_s;

namespace qn {

class ZstdDecompressor {
public:
    ZstdDecompressor();
    ~ZstdDecompressor();

    ZstdDecompressor(const ZstdDecompressor&) = delete;
    ZstdDecompressor& operator=(const ZstdDecompressor&) = delete;
    ZstdDecompressor(ZstdDecompressor&&);
    ZstdDecompressor& operator=(ZstdDecompressor&&);

    DecompressorResult<> init();
    DecompressorResult<> initWithDictionary(const void* dict, size_t size);

    /// Decompress data from `src` to `dst`.
    /// `dstSize` will be set to the size of the decompressed data.
    DecompressorResult<> decompress(
        const void* src, size_t srcSize,
        void* dst, size_t& dstSize,
        bool noDict = false
    );

private:
    ZSTD_DCtx_s* m_ctx = nullptr;
    ZSTD_DDict_s* m_dict = nullptr;
};

DecompressorResult<> decompressZstd(
    const void* src, size_t srcSize,
    void* dst, size_t& dstSize
);

}