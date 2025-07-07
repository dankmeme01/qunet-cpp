#include <qunet/socket/transport/BaseTransport.hpp>
#include <asp/time/Instant.hpp>

using namespace asp::time;

namespace qn {

void BaseTransport::setConnectionId(uint64_t connectionId) {
    m_connectionId = connectionId;
}

void BaseTransport::setMessageSizeLimit(size_t limit) {
    m_messageSizeLimit = limit;
}

TransportResult<QunetMessage> BaseTransport::performHandshake(
    HandshakeStartMessage handshakeStart,
    const std::optional<asp::time::Duration>& timeout
) {
    auto startedAt = Instant::now();

    GEODE_UNWRAP(this->sendMessage(std::move(handshakeStart)));

    while (true) {
        auto remTimeout = timeout ? std::optional(*timeout - startedAt.elapsed()) : std::nullopt;
        if (remTimeout && remTimeout->isZero()) {
            return Err(TransportError::TimedOut);
        }

        if (!GEODE_UNWRAP(this->poll(remTimeout))) {
            continue;
        }

        bool msgAvailable = GEODE_UNWRAP(this->processIncomingData());
        if (msgAvailable) {
            break;
        }
    }

    QN_DEBUG_ASSERT(!m_recvMsgQueue.empty());

    return this->receiveMessage();
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