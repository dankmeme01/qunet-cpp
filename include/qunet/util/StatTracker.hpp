#pragma once

#include <qunet/socket/message/meta.hpp>
#include <asp/time/Instant.hpp>
#include <optional>
#include <vector>
#include <span>

namespace qn {

enum class MessageType {
    HandshakeStart,
    HandshakeFinish,
    HandshakeFailure,
    // TODO
    Data,
};

struct StatSnapshot {
    asp::time::Duration period;
    size_t bytesUp, bytesDown;
    size_t messagesUp, messagesDown;
    size_t packetsUp, packetsDown;
    size_t minUpPacketSize, maxUpPacketSize;
    size_t minDownPacketSize, maxDownPacketSize;

    size_t compressedUp, compressedDown;
    float compUpAvgRatio, compDownAvgRatio;
    int64_t compUpSavedBytes, compDownSavedBytes;
    float compUpSavedPercent, compDownSavedPercent;

    size_t upReliable, downReliable;
    size_t upRetransmissions;
    size_t upUnacked;
    size_t downUnknownAcks;
    asp::time::Duration minAckDelay, maxAckDelay, avgAckDelay;

    size_t avgUpPacketSize() const;
    size_t avgDownPacketSize() const;

};

struct StatWholeSnapshot : StatSnapshot {
    StatWholeSnapshot(StatSnapshot&& s) : StatSnapshot(std::move(s)) {}

    asp::time::Duration connectionLifetime;
    asp::time::Duration tookToConnect;
    asp::time::Duration tookToHandshake;
};

/// Helper class to track various network statistics.
/// Specifically:
/// * total connection lifetime, how long it took to connect/handshake
/// * total bytes, packets, messages transferred
/// * per-type message stats: counts, avg/min/max bytes per message
/// * packet stats: avg/min/max bytes per packet, avg packets per time period, avg bytes per time period
/// * compression stats: compressed messages, avg/min/max ratio, total bytes saved (+ %), (de)compression time
/// * udp reliability stats: retransmissions, avg/min/max ack delay
/// To define some vague terms used here:
/// - Packet is a single TCP/UDP network packet. For QUIC, this will include all QUIC-specific packets and overhead.
/// - Message is a single protocol message. It can be fragmented into multiple packets. It can also be retransmitted.
class StatTracker {
public:
    StatTracker();

    StatTracker(const StatTracker&) = delete;
    StatTracker& operator=(const StatTracker&) = delete;
    StatTracker(StatTracker&&) = default;
    StatTracker& operator=(StatTracker&&) = default;

    /// Set whether the tracker should be enabled. By default is `true`, but if `false` is passed, tracking functions are no-op.
    void setEnabled(bool enable);

    /// Get a snapshot of various message data. If `period` is nonzero, will only include stats for that time period.
    /// If `period` is zero (default), will include all-time stats.
    StatSnapshot snapshot(asp::time::Duration period = {}) const;

    /// Like `snapshot` but includes all-time stats plus extra fields
    StatWholeSnapshot snapshotFull() const;

    void onConnected();

    void onUpPacket(size_t bytes);
    void onDownPacket(size_t bytes);

    void onUpMessage(uint8_t header,
                     size_t bytes,
                     std::optional<size_t> compressedBytes = {},
                     asp::time::Duration compressionTime = {},
                     std::optional<ReliabilityHeader> relHeader = {});

    void onDownMessage(uint8_t header,
                     size_t bytes,
                     std::optional<size_t> compressedBytes = {},
                     asp::time::Duration compressionTime = {},
                     std::optional<ReliabilityHeader> relHeader = {});

private:
    struct Packet {
        asp::time::Instant timestamp;
        size_t bytes;
        bool up;
    };

    struct Message {
        asp::time::Instant timestamp;
        MessageType type;
        size_t bytes;
        bool up;
        std::optional<size_t> compressedBytes;
        asp::time::Duration compressionTime;
        std::optional<ReliabilityHeader> relHeader;
    };

    asp::time::Instant m_startedAt;
    std::optional<asp::time::Instant> m_connectedAt;
    std::optional<asp::time::Instant> m_handshakeFinishedAt;
    std::vector<Packet> m_packets;
    std::vector<Message> m_messages;
    bool m_enabled = true;

    void onMessage(uint8_t header,
                   bool up,
                   size_t bytes,
                   std::optional<size_t> compressedBytes = {},
                   asp::time::Duration compressionTime = {},
                   std::optional<ReliabilityHeader> relHeader = {});

    void countRelStats(StatSnapshot& snap, std::span<const Message> messages) const;
};

}
