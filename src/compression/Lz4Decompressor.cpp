#include <qunet/compression/Lz4Decompressor.hpp>
#include <qunet/Log.hpp>
#include <lz4.h>

namespace qn {

DecompressorResult<> Lz4Decompressor::decompress(
    const void* src, size_t srcSize,
    void* dst, size_t& dstSize
) {
    int dsize = LZ4_decompress_safe((const char*)src, (char*)dst, srcSize, dstSize);
    if (dsize < 0) {
        log::warn("Lz4Decompressor: decompression failed: code {}", dsize);
        return Err(DecompressorError::DecompressionFailed);
    } else if ((size_t)dsize != dstSize) {
        log::warn("Lz4Decompressor: decompressed size mismatch: expected {}, got {}", dstSize, dsize);
        return Err(DecompressorError::SizeMismatch);
    }

    dstSize = dsize;
    return Ok();
}

DecompressorResult<> decompressLz4(
    const void* src, size_t srcSize,
    void* dst, size_t& dstSize
) {
    return Lz4Decompressor().decompress(src, srcSize, dst, dstSize);
}

}