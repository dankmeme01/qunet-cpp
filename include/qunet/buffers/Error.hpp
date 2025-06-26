#pragma once

#include <qunet/util/Error.hpp>

#include <Geode/Result.hpp>

namespace qn {

QN_MAKE_ERROR_STRUCT(ByteReaderError,
    OutOfBoundsRead,
    VarintOverflow,
    StringTooLong,
);

QN_MAKE_ERROR_STRUCT(ByteWriterError,
    OutOfBoundsWrite,
    StringTooLong,
);

}