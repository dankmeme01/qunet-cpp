#pragma once

#include "Error.hpp"
#include <stddef.h>

namespace qn {

class Lz4Decompressor {
public:
    /// Decompress data from `src` to `dst`.
    /// `dstSize` will be set to the size of the decompressed data.
    DecompressorResult<> decompress(
        const void* src, size_t srcSize,
        void* dst, size_t& dstSize
    );
};

DecompressorResult<> decompressLz4(
    const void* src, size_t srcSize,
    void* dst, size_t& dstSize
);

}