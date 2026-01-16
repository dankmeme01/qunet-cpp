#pragma once

#include "Error.hpp"
#include <stddef.h>

namespace qn {

class Lz4Compressor {
public:
    /// Compress data from `src` to `dst`.
    /// `dstSize` will be set to the size of the compressed data.
    CompressorResult<> compress(
        const void* src, size_t srcSize,
        void* dst, size_t& dstSize
    );

    /// Returns the maximum size of the compressed data for a given source size.
    static size_t compressBound(size_t srcSize);
};

/// Compress data from `src` to `dst` using LZ4.
/// `dstSize` will be set to the size of the compressed data.
CompressorResult<> compressLz4(
    const void* src, size_t srcSize,
    void* dst, size_t& dstSize
);

}