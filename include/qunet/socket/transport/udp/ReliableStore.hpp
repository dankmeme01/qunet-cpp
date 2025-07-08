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

    /// Handles an incoming reliability header from a message. Returns false if this message should be rejected,
    /// for example if it is a duplicate or out of order.
    TransportResult<bool> handleIncoming(QunetMessageMeta& header);

    bool hasDelayedMessage() const;
    std::optional<QunetMessageMeta> popDelayedMessage();

private:
    struct UnackedRemoteMessage {
        uint16_t messageId;
        asp::time::Instant receivedAt;
    };

    struct StoredOutOfOrderMessage {
        QunetMessageMeta meta;
        asp::time::Instant receivedAt;
    };

    std::queue<UnackedRemoteMessage> m_remoteUnacked;
    std::vector<StoredOutOfOrderMessage> m_remoteOutOfOrder;
    std::queue<QunetMessageMeta> m_remoteDelayedQueue;
    uint16_t m_nextRemoteId = 1;

    void processAcks(const ReliabilityHeader& header);
    TransportResult<void> storeRemoteOutOfOrder(QunetMessageMeta& meta);
    void pushRemoteUnacked();

    void maybeRestoreRemote();
};

}