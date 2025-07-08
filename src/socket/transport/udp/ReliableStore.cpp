#include <qunet/socket/transport/udp/ReliableStore.hpp>
#include <qunet/util/assert.hpp>
#include <qunet/Log.hpp>

using namespace asp::time;

namespace qn {

ReliableStore::ReliableStore() {}

ReliableStore::~ReliableStore() {}

static size_t distance(uint16_t a, uint16_t b) {
    return static_cast<size_t>(std::abs(static_cast<int>(a) - static_cast<int>(b)));
}

TransportResult<bool> ReliableStore::handleIncoming(QunetMessageMeta& message) {
    QN_ASSERT(message.reliabilityHeader.has_value());

    auto& rlh = *message.reliabilityHeader;
    this->processAcks(rlh);

    if (rlh.messageId == 0) {
        // not a reliable message itself
        return Ok(true);
    }

    size_t dist = distance(rlh.messageId, m_nextRemoteId);

    // a message is duplicate if its id is less than next expected and distance is small enough (because of wraparound)
    bool isDuplicate = (rlh.messageId < m_nextRemoteId && (m_nextRemoteId - rlh.messageId < 16384));

    // a message is out of order if it is not a duplicate and is not the next expected message
    bool isOutOfOrder = !isDuplicate && rlh.messageId != m_nextRemoteId;

    if (isDuplicate) {
        // likely a duplicate message, we can safely ignore it
        log::debug("ReliableStore: Ignoring duplicate message with ID {}", rlh.messageId);
        return Ok(false);
    } else if (isOutOfOrder) {
        // out of order message, we need to store it for later processing
        log::debug("ReliableStore: Out of order message: expected {} but got {}, storing it", m_nextRemoteId, rlh.messageId);
        GEODE_UNWRAP(this->storeRemoteOutOfOrder(message));
        return Ok(false);
    }

    // otherwise, everything is fine and in order
    this->pushRemoteUnacked();

    return Ok(true);
}

void ReliableStore::processAcks(const ReliabilityHeader& header) {

}

TransportResult<> ReliableStore::storeRemoteOutOfOrder(QunetMessageMeta& meta) {
    if (m_remoteOutOfOrder.size() >= 128) {
        return Err(TransportError::TooUnreliable);
    }

    m_remoteOutOfOrder.push_back({
        .meta = std::move(meta),
        .receivedAt = Instant::now(),
    });

    return Ok();
}

// pushes a new unacked remote message with the next message ID
void ReliableStore::pushRemoteUnacked() {
    uint16_t messageId = m_nextRemoteId++;

    if (m_nextRemoteId == 0) {
        m_nextRemoteId = 1;
    }

    UnackedRemoteMessage msg {
        .messageId = messageId,
        .receivedAt = Instant::now(),
    };

    m_remoteUnacked.push(std::move(msg));

    // maybe restore some out of order messages if it's the right time
    this->maybeRestoreRemote();
}

void ReliableStore::maybeRestoreRemote() {
    for (auto it = m_remoteOutOfOrder.begin(); it != m_remoteOutOfOrder.end(); it++) {
        auto& msg = *it;

        if (msg.meta.reliabilityHeader->messageId == m_nextRemoteId) {
            m_remoteDelayedQueue.push(std::move(msg.meta));
            m_remoteOutOfOrder.erase(it);
            this->pushRemoteUnacked();
            break;
        }
    }
}

bool ReliableStore::hasDelayedMessage() const {
    return !m_remoteDelayedQueue.empty();
}

std::optional<QunetMessageMeta> ReliableStore::popDelayedMessage() {
    if (m_remoteDelayedQueue.empty()) {
        return std::nullopt;
    }

    QunetMessageMeta meta = std::move(m_remoteDelayedQueue.front());
    m_remoteDelayedQueue.pop();
    return meta;
}

}