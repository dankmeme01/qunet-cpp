#pragma once

#include <stdint.h>
#include <qsox/Error.hpp>
#include <qunet/socket/message/QunetMessage.hpp>
#include <asp/time/Duration.hpp>

namespace qn {

QN_MAKE_ERROR_STRUCT(TransportError,
    NetworkError,
    ConnectionClosed,
    EncodingFailed,
    DecodingFailed,
    TransportCreationFailed,
    ConnectionTimedOut,
    HandshakeFailed,
    InvalidMessage,
);

template <typename T = void>
using TransportResult = geode::Result<T, TransportError>;

class BaseTransport {
public:
    virtual ~BaseTransport() = default;
    virtual TransportResult<> sendMessage(QunetMessage message) = 0;
    virtual TransportResult<bool> poll(const asp::time::Duration& dur) = 0;
    virtual TransportResult<QunetMessage> receiveMessage() = 0;

protected:
    TransportError makeError(qsox::Error err);
};

}
