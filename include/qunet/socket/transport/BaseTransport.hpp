#pragma once

#include <stdint.h>
#include <qsox/Error.hpp>
#include <qunet/socket/message/QunetMessage.hpp>
#include <asp/time/Duration.hpp>

namespace qn {

// TODO: make this a proper std variant, so the user can get better errors
QN_MAKE_ERROR_STRUCT(TransportError,
    NetworkError,
    ConnectionClosed,
    ConnectionRefused,
    EncodingFailed,
    DecodingFailed,
    TransportCreationFailed,
    ConnectionTimedOut,
    HandshakeFailed,
    InvalidMessage,
    ZeroLengthMessage,
    MessageTooLong,
);

template <typename T = void>
using TransportResult = geode::Result<T, TransportError>;

class BaseTransport {
public:
    virtual ~BaseTransport() = default;
    virtual TransportResult<> sendMessage(QunetMessage message) = 0;
    virtual TransportResult<bool> poll(const asp::time::Duration& dur) = 0;
    virtual TransportResult<QunetMessage> receiveMessage() = 0;

    virtual void setConnectionId(uint64_t connectionId);
    virtual void setMessageSizeLimit(size_t limit);

protected:
    uint64_t m_connectionId = 0;
    size_t m_messageSizeLimit = -1;

    TransportError makeError(qsox::Error err);
};

}
