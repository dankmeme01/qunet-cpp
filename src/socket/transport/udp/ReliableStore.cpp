#include <qunet/socket/transport/udp/ReliableStore.hpp>
#include <qunet/util/assert.hpp>
#include <qunet/util/algo.hpp>
#include <qunet/Log.hpp>

using namespace asp::time;

namespace qn {

ReliableStore::ReliableStore() {}

ReliableStore::~ReliableStore() {}

TransportResult<bool> ReliableStore::handleIncoming(QunetMessageMeta& message) {
    QN_ASSERT(message.reliabilityHeader.has_value());

    auto& rlh = *message.reliabilityHeader;
    this->processAcks(rlh);

    if (rlh.messageId == 0) {
        // not a reliable message itself
        return Ok(true);
    }

    // a message is duplicate if its id is less than next expected and distance is small enough (because of wraparound)
    bool isDuplicate = (rlh.messageId < m_nextRemoteId && (m_nextRemoteId - rlh.messageId < 16384));

    // a message is out of order if it is not a duplicate and is not the next expected message
    bool isOutOfOrder = !isDuplicate && rlh.messageId != m_nextRemoteId;

    if (isDuplicate) {
        // likely a duplicate message, our ACK might have been lost and we need to resend it
        log::debug("ReliableStore: Duplicate message: expected {} but got {}, will re-ACK it", m_nextRemoteId, rlh.messageId);
        this->pushRemoteDuplicate(rlh.messageId);
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

TransportResult<> ReliableStore::storeRemoteOutOfOrder(QunetMessageMeta& meta) {
    if (m_remoteOutOfOrder.size() >= 32) {
        return Err(TransportError::TooUnreliable);
    }

    // check if the message is already in the out of order queue
    for (const auto& msg : m_remoteOutOfOrder) {
        if (msg.meta.reliabilityHeader->messageId == meta.reliabilityHeader->messageId) {
            log::debug("ReliableStore: Out of order message with ID {} is already stored, ignoring", meta.reliabilityHeader->messageId);
            return Ok();
        }
    }

    m_remoteOutOfOrder.push_back({
        .meta = std::move(meta),
        .receivedAt = Instant::now(),
    });

    return Ok();
}

// pushes a new unacked remote message with the next message ID
void ReliableStore::pushRemoteUnacked() {
    uint16_t id = m_nextRemoteId++;

    if (m_nextRemoteId == 0) {
        m_nextRemoteId = 1;
    }

    log::debug("ReliableStore: Got a remote message with ID {}", id);

    return this->pushRemoteUnackedWithId(id, false);
}

void ReliableStore::pushRemoteDuplicate(uint16_t messageId) {
    return this->pushRemoteUnackedWithId(messageId, true);
}

void ReliableStore::pushRemoteUnackedWithId(uint16_t messageId, bool dupe) {
    // if this is a duplicate message, check if it's already in the unacked queue
    if (dupe) {
        for (auto& msg : m_remoteUnacked) {
            if (msg.messageId == messageId) {
                return;
            }
        }
    }

    UnackedRemoteMessage msg {
        .messageId = messageId,
        .receivedAt = Instant::now(),
    };

    m_remoteUnacked.push_back(std::move(msg));

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

// Outgoing messages

void ReliableStore::processAcks(const ReliabilityHeader& header) {
    for (size_t i = 0; i < header.ackCount; i++) {
        this->ackLocal(header.acks[i]);
    }
}

void ReliableStore::ackLocal(uint16_t messageId) {
    log::debug("ReliableStore: remote acknowledged local message with ID {}", messageId);

    // find the message in the local unacked queue
    for (auto it = m_localUnacked.begin(); it != m_localUnacked.end(); it++) {
        if (it->messageId == messageId) {
            // update average rtt
            this->updateRtt(it->sentAt.elapsed());

            m_localUnacked.erase(it);
            return;
        }
    }

    log::warn("ReliableStore: ignored ack, did not find a local message with this ID");
}

uint16_t ReliableStore::nextMessageId() {
    auto id = m_nextLocalId++;

    if (m_nextLocalId == 0) {
        m_nextLocalId = 1;
    }

    return id;
}

void ReliableStore::setOutgoingAcks(ReliabilityHeader& header) {
    header.ackCount = 0;

    for (size_t i = 0; i < 8; i++) {
        if (m_remoteUnacked.empty()) {
            return;
        }

        auto& msg = m_remoteUnacked.front();
        header.acks[i] = msg.messageId;
        header.ackCount++;
        log::debug("ReliableStore: sending ACK for message {}", msg.messageId);

        m_remoteUnacked.pop_front();
    }
}

void ReliableStore::pushLocalUnacked(QunetMessage&& msg) {
    QN_DEBUG_ASSERT(msg.is<DataMessage>());
    QN_DEBUG_ASSERT(msg.as<DataMessage>().relHeader.has_value());

    if (m_localUnacked.size() >= 64) {
        log::warn("ReliableStore: unacked message queue is full, dropping older message");
        m_localUnacked.pop_front();
    }

    uint16_t msgId = msg.as<DataMessage>().relHeader->messageId;

    log::debug("ReliableStore: pushing local unacked message with ID {}", msgId);

    m_localUnacked.push_back({
        .messageId = msgId,
        .msg = std::move(msg),
        .sentAt = Instant::now(),
        .retransmitAttempts = 0,
    });
}

Duration ReliableStore::untilTimerExpiry() const {
    // timer here is used for 2 purposes:
    // 1. retransitting unacked local messages
    // 2. sending ACKs for unacked remote messages

    // go through all messages and set the timer expiry to the smallest deadline

    auto expiry = Duration::infinite();

    // fast return
    if (m_localUnacked.empty() && m_remoteUnacked.empty()) {
        return expiry;
    }

    auto ackDelay = this->calcAckDeadline();

    for (auto& msg : m_localUnacked) {
        auto retransDelay = this->calcRetransmissionDeadline(msg.retransmitAttempts);

        auto dur = retransDelay - msg.sentAt.elapsed();
        expiry = std::min(expiry, dur);
    }

    for (auto& msg : m_remoteUnacked) {
        auto dur = ackDelay - msg.receivedAt.elapsed();
        expiry = std::min(expiry, dur);
    }

    return expiry;
}

void ReliableStore::updateRtt(asp::time::Duration rtt) {
    m_avgRttMicros = exponentialMovingAverage<uint64_t>(m_avgRttMicros, rtt.micros(), 0.35);
}

QunetMessage* ReliableStore::maybeRetransmit() {
    for (auto& msg : m_localUnacked) {
        auto retransDelay = this->calcRetransmissionDeadline(msg.retransmitAttempts);

        if (msg.sentAt.elapsed() >= retransDelay) {
            log::debug("ReliableStore: retransmitting local message with ID {}", msg.messageId);
            msg.sentAt = Instant::now();
            return &msg.msg;
        }
    }

    return nullptr;
}

bool ReliableStore::hasUrgentOutgoingAcks() {
    auto ackDelay = this->calcAckDeadline();

    for (auto& msg : m_remoteUnacked) {
        if (msg.receivedAt.elapsed() >= ackDelay) {
            log::debug("ReliableStore: urgent outgoing ACK for message ID {}", msg.messageId);
            return true;
        }
    }

    return false;
}

Duration ReliableStore::calcRetransmissionDeadline(size_t nthAttempt) const {
    auto rtt = Duration::fromMicros(m_avgRttMicros);

    if (rtt.isZero()) {
        rtt = Duration::fromMillis(250);
    }

    auto ackdl = this->calcAckDeadline();

    // this is kinda arbitrary tbh
    return Duration::fromMicros(
        static_cast<double>((rtt + ackdl).micros()) * 1.5
        + static_cast<double>(nthAttempt) * 75'000.0
    );
}

Duration ReliableStore::calcAckDeadline() const {
    return Duration::fromMillis(100);
}

}