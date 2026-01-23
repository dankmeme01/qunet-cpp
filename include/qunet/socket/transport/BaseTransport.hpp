#pragma once

#include "Error.hpp"
#include <qunet/socket/message/QunetMessage.hpp>
#include <qunet/compression/ZstdDecompressor.hpp>
#include <qunet/compression/ZstdCompressor.hpp>
#include <qunet/compression/Lz4Compressor.hpp>
#include <qunet/compression/Lz4Decompressor.hpp>
#include <qunet/database/QunetDatabase.hpp>
#include <qunet/util/StatTracker.hpp>

#include <qsox/Error.hpp>
#include <arc/future/Future.hpp>
#include <arc/util/Result.hpp>
#include <asp/time/Duration.hpp>
#include <asp/time/Instant.hpp>
#include <stdint.h>

namespace qn {

struct SentMessageContext {
    bool reliable = false;
    std::optional<ReliabilityHeader> relHeader;

    size_t originalSize = 0;
    std::optional<size_t> compressedSize;

    std::string tag;
};

class BaseTransport {
public:
    BaseTransport() = default;
    BaseTransport(BaseTransport&&) noexcept = default;
    BaseTransport& operator=(BaseTransport&&) noexcept = default;

    virtual ~BaseTransport() = default;
    virtual arc::Future<TransportResult<>> sendMessage(QunetMessage message, SentMessageContext& ctx) = 0;
    virtual arc::Future<TransportResult<>> sendMessage(QunetMessage data, bool reliable = true);

    /// Sends the qunet hadnshake to the server and waits for a response.
    /// The default implementation should only be used in reliable and ordered transports,
    /// it will return the first message as soon as it is received.
    virtual arc::Future<TransportResult<QunetMessage>> performHandshake(
        HandshakeStartMessage handshakeStart
    );

    /// Like `performHandshake` but sends a reconnect message and waits for a reconnect success or failure.
    virtual arc::Future<TransportResult<QunetMessage>> performReconnect(
        uint64_t connectionId
    );

    /// Polls until any kind of data is available to be read.
    virtual arc::Future<TransportResult<>> poll() = 0;
    virtual arc::Future<TransportResult<bool>> pollTimeout(asp::time::Duration timeout);

    /// Returns how much time is left until the transport timer expires.
    virtual asp::time::Duration untilTimerExpiry() const;
    /// Handles the timer expiry. This may send various messages to the remote.
    virtual arc::Future<TransportResult<>> handleTimerExpiry();

    /// Receives a message from the transport. If no message is available, this will block until a message is received or an error occurs.
    virtual arc::Future<TransportResult<QunetMessage>> receiveMessage() = 0;

    // Closes the transport. This method may or may not block until the transport is fully closed.
    // This does not send a `ClientClose` message.
    // After invoking, keep calling `isClosed()` to check if the transport is fully closed.
    virtual arc::Future<TransportResult<>> close();
    virtual TransportResult<> closeSync() = 0;
    virtual bool isClosed() const = 0;

    virtual void setConnectionId(uint64_t connectionId);
    virtual void setMessageSizeLimit(size_t limit);

    /// Initializes compressors and decompressors with the given QunetDatabase.
    /// It can be null.
    virtual TransportResult<> initCompressors(const QunetDatabase* qdb = nullptr);

    /// Returns the average latency (round-trip time) of the transport.
    virtual asp::time::Duration getLatency() const;

    TransportResult<QunetMessage> decodePreFinalDataMessage(QunetMessageMeta&& meta);

    StatTracker& _tracker() { return m_tracker; }

protected:
    friend class Socket;

    StatTracker m_tracker;
    uint64_t m_connectionId = 0;
    size_t m_messageSizeLimit = -1;

    // compressors
    ZstdCompressor m_zstdCompressor;
    ZstdDecompressor m_zstdDecompressor;
    Lz4Compressor m_lz4Compressor;
    Lz4Decompressor m_lz4Decompressor;

    uint64_t m_lastRttMicros = 0;
    std::optional<asp::time::Instant> m_lastActivity;
    std::optional<asp::time::Instant> m_lastKeepalive;
    size_t m_totalKeepalives = 0;
    size_t m_unackedKeepalives = 0;

    /// Call this function with the round-trip time of the latest exchange with the server.
    /// This is used to calculate the average latency, which in turn can be used for other purposes,
    /// for example calculating retransmission timeouts.
    void updateLatency(asp::time::Duration rtt);

    void updateLastActivity();
    void updateLastKeepalive();
    asp::time::Duration sinceLastActivity() const;
    asp::time::Duration sinceLastKeepalive() const;

    uint64_t getKeepaliveTimestamp() const;

    /// Call this whenever an incoming message is received, for statistics
    void onIncomingMessage(const QunetMessage& msg);

    void logOutgoingMessage(uint8_t headerByte, const SentMessageContext& ctx);
};

}