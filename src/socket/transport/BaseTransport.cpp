#include <qunet/socket/transport/BaseTransport.hpp>

namespace qn {

void BaseTransport::setConnectionId(uint64_t connectionId) {
    m_connectionId = connectionId;
}

void BaseTransport::setMessageSizeLimit(size_t limit) {
    m_messageSizeLimit = limit;
}

TransportResult<QunetMessage> BaseTransport::receiveMessage() {
    if (!m_recvMsgQueue.empty()) {
        auto msg = std::move(m_recvMsgQueue.front());
        m_recvMsgQueue.pop();
        return Ok(std::move(msg));
    }

    // block until a message is available
    while (!GEODE_UNWRAP(this->processIncomingData()));

    return this->receiveMessage();
}

bool BaseTransport::messageAvailable() {
    return !m_recvMsgQueue.empty();
}

}