#pragma once

#include <qunet/util/Error.hpp>

namespace qn {

QN_MAKE_ERROR_STRUCT(DecompressorError,
    NotInitialized,
    AlreadyInitialized,
    InvalidDictionary,
    DecompressionFailed,
);

template <typename T = void>
using DecompressorResult = geode::Result<T, DecompressorError>;

QN_MAKE_ERROR_STRUCT(CompressorError,
    NotInitialized,
    AlreadyInitialized,
    InvalidDictionary,
    CompressionFailed,
);

template <typename T = void>
using CompressorResult = geode::Result<T, CompressorError>;

}
