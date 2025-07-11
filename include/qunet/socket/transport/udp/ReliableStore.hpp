#pragma once

#include <qunet/socket/message/meta.hpp>
#include <qunet/socket/transport/Error.hpp>
#include <asp/time/Instant.hpp>
#include <queue>

namespace qn {

class ReliableStore {
public:
    ReliableStore();
    ~ReliableStore();

    // Handling of incoming messages

    /// Handles an incoming reliability header from a message. Returns false if this message should be rejected,
    /// for example if it is a duplicate or out of order.
    TransportResult<bool> handleIncoming(QunetMessageMeta& header);

    bool hasDelayedMessage() const;
    std::optional<QunetMessageMeta> popDelayedMessage();

    // Handling of outgoing messages

    /// Returns the next message ID to be used for reliable outgoing messages.
    uint16_t nextMessageId();
    /// Modifies the reliability header to add ACKs for unacknowledged remote messages, if any.
    void setOutgoingAcks(ReliabilityHeader& header);

    /// Stores a local message for potential retransmission. It must be a reliable data message.
    void pushLocalUnacked(QunetMessage&& msg);

    asp::time::Duration untilTimerExpiry() const;

    /// Checks if any messages need to be retransmitted. Returns null if none.
    QunetMessage* maybeRetransmit();

    /// Returns whether there are any unacked remote messages that must be acknowledged as soon as possible.
    bool hasUrgentOutgoingAcks();

private:
    struct UnackedRemoteMessage {
        uint16_t messageId;
        asp::time::Instant receivedAt;
    };

    struct UnackedLocalMessage {
        uint16_t messageId;
        QunetMessage msg;
        asp::time::Instant sentAt;
        size_t retransmitAttempts = 0;
    };

    struct StoredOutOfOrderMessage {
        QunetMessageMeta meta;
        asp::time::Instant receivedAt;
    };

    std::deque<UnackedRemoteMessage> m_remoteUnacked;
    std::vector<StoredOutOfOrderMessage> m_remoteOutOfOrder;
    std::queue<QunetMessageMeta> m_remoteDelayedQueue;
    uint16_t m_nextRemoteId = 1;

    uint16_t m_nextLocalId = 1;
    std::deque<UnackedLocalMessage> m_localUnacked;

    uint64_t m_avgRttMicros = 0;

    void processAcks(const ReliabilityHeader& header);
    void ackLocal(uint16_t messageId);
    TransportResult<void> storeRemoteOutOfOrder(QunetMessageMeta& meta);
    void pushRemoteUnacked();
    void pushRemoteDuplicate(uint16_t messageId);
    void pushRemoteUnackedWithId(uint16_t messageId, bool dupe = false);

    void maybeRestoreRemote();

    void updateRtt(asp::time::Duration rtt);
    asp::time::Duration calcRetransmissionDeadline(size_t nthAttempt) const;
    asp::time::Duration calcAckDeadline() const;

};

}