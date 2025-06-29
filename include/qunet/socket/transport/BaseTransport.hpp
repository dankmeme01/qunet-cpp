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

    virtual void setConnectionId(uint64_t connectionId);
    virtual void setMessageSizeLimit(size_t limit);

protected:
    uint64_t m_connectionId = 0;
    size_t m_messageSizeLimit = -1;
};

}
