#pragma once

#include <qunet/util/Error.hpp>

namespace qn {

QN_MAKE_ERROR_STRUCT(DecompressorError,
    NotInitialized,
    AlreadyInitialized,
    InvalidDictionary,
    DecompressionFailed,
    SizeMismatch,
);

template <typename T = void>
using DecompressorResult = Result<T, DecompressorError>;

QN_MAKE_ERROR_STRUCT(CompressorError,
    NotInitialized,
    AlreadyInitialized,
    InvalidDictionary,
    CompressionFailed,
);

template <typename T = void>
using CompressorResult = Result<T, CompressorError>;

}
