#include <qunet/compression/Lz4Compressor.hpp>
#include <qunet/Log.hpp>
#include <lz4.h>

namespace qn {

CompressorResult<> Lz4Compressor::compress(
    const void* src, size_t srcSize,
    void* dst, size_t& dstSize
) {
    int dsize = LZ4_compress_default(
        (const char*)src,
        (char*)dst,
        srcSize,
        dstSize
    );

    if (dsize <= 0) {
        log::warn("Lz4Compressor: compression failed: code {}", dsize);
        return Err(CompressorError::CompressionFailed);
    }

    dstSize = dsize;
    return Ok();
}

size_t Lz4Compressor::compressBound(size_t srcSize) {
    return LZ4_compressBound(srcSize);
}

CompressorResult<> compressLz4(
    const void* src, size_t srcSize,
    void* dst, size_t& dstSize
) {
    return Lz4Compressor().compress(src, srcSize, dst, dstSize);
}

}