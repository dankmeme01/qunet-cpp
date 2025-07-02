#pragma once

#include "Error.hpp"
#include <qunet/socket/message/QunetMessage.hpp>

#include <stdint.h>
#include <qsox/Error.hpp>
#include <asp/time/Duration.hpp>

namespace qn {

class BaseTransport {
public:
    virtual ~BaseTransport() = default;
    virtual TransportResult<> sendMessage(QunetMessage message) = 0;
    virtual TransportResult<bool> poll(const asp::time::Duration& dur) = 0;
    virtual TransportResult<QunetMessage> receiveMessage() = 0;

    // Closes the transport. This method may or may not block until the transport is fully closed.
    // This does not send a `ClientClose` message.
    // After invoking, keep calling `isClosed()` to check if the transport is fully closed.
    virtual TransportResult<> close() = 0;
    virtual bool isClosed() const = 0;

    virtual void setConnectionId(uint64_t connectionId);
    virtual void setMessageSizeLimit(size_t limit);

protected:
    uint64_t m_connectionId = 0;
    size_t m_messageSizeLimit = -1;
};

}
