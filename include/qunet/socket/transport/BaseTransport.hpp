#pragma once

#include <span>
#include <stdint.h>
#include <qsox/Error.hpp>

namespace qn {

class BaseTransport {
public:
    virtual ~BaseTransport() = default;
    virtual qsox::NetResult<> sendMessage(std::span<const uint8_t> data) = 0;
};

}
