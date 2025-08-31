#pragma once

#include "Error.hpp"
#include <qunet/socket/message/QunetMessage.hpp>
#include <qunet/compression/ZstdDecompressor.hpp>
#include <qunet/compression/ZstdCompressor.hpp>
#include <qunet/database/QunetDatabase.hpp>

#include <qsox/Error.hpp>
#include <asp/time/Duration.hpp>
#include <asp/time/Instant.hpp>
#include <stdint.h>
#include <queue>

namespace qn {

class BaseTransport {
public:
    BaseTransport() = default;
    BaseTransport(BaseTransport&&) = default;
    BaseTransport& operator=(BaseTransport&&) = default;

    virtual ~BaseTransport() = default;
    virtual TransportResult<> sendMessage(QunetMessage message, bool reliable) = 0;

    /// Sends the qunet hadnshake to the server and waits for a response.
    /// The default implementation should only be used in reliable and ordered transports,
    /// it will return the first message as soon as it is received.
    virtual TransportResult<QunetMessage> performHandshake(
        HandshakeStartMessage handshakeStart,
        const std::optional<asp::time::Duration>& timeout
    );

    /// Like `performHandshake` but sends a reconnect message and waits for a reconnect success or failure.
    virtual TransportResult<QunetMessage> performReconnect(
        uint64_t connectionId,
        const std::optional<asp::time::Duration>& timeout
    );

    /// Polls until any kind of data is available to be read.
    virtual TransportResult<bool> poll(const std::optional<asp::time::Duration>& dur) = 0;

    /// Processes incoming data from the transport. This function may block until data is available,
    /// but it will only block for a single read call, rather than until a whole message is available.
    /// Returns whether an entire message is available to be read with `receiveMessage()`.
    virtual TransportResult<bool> processIncomingData() = 0;

    /// Returns whether there is a message available to be read from the transport.
    virtual bool messageAvailable();

    /// Returns how much time is left until the transport timer expires.
    virtual asp::time::Duration untilTimerExpiry() const;
    /// Handles the timer expiry. This may send various messages to the remote.
    virtual TransportResult<> handleTimerExpiry();

    /// Receives a message from the transport. If no message is available, this will block until a message is received or an error occurs.
    virtual TransportResult<QunetMessage> receiveMessage();

    // Closes the transport. This method may or may not block until the transport is fully closed.
    // This does not send a `ClientClose` message.
    // After invoking, keep calling `isClosed()` to check if the transport is fully closed.
    virtual TransportResult<> close() = 0;
    virtual bool isClosed() const = 0;

    virtual void setConnectionId(uint64_t connectionId);
    virtual void setMessageSizeLimit(size_t limit);

    /// Initializes compressors and decompressors with the given QunetDatabase.
    /// It can be null.
    virtual TransportResult<> initCompressors(const QunetDatabase* qdb = nullptr);

    /// Returns the average latency (round-trip time) of the transport.
    virtual asp::time::Duration getLatency() const;

    // Semi-public version of pushPreFinalDataMessage.
    // Do not use this outside of the transport implementation.
    TransportResult<> _pushPreFinalDataMessage(QunetMessageMeta&& meta);
    // Do not use this outside of the transport implementation.
    void _pushFinalControlMessage(QunetMessage&& meta);

protected:
    friend class Socket;

    std::queue<QunetMessage> m_recvMsgQueue;
    uint64_t m_connectionId = 0;
    size_t m_messageSizeLimit = -1;

    // compressors
    ZstdCompressor m_zstdCompressor;
    ZstdDecompressor m_zstdDecompressor;

    uint64_t m_lastRttMicros = 0;
    std::optional<asp::time::Instant> m_lastActivity;
    std::optional<asp::time::Instant> m_lastKeepalive;
    size_t m_totalKeepalives = 0;
    size_t m_unackedKeepalives = 0;

    // Called when a data message is almost completely ready to be dispatched.
    // Fragmentation and reliability headers are ignored, they must be processed beforehand.
    // This function will take care of decompression if needed.
    TransportResult<> pushPreFinalDataMessage(QunetMessageMeta&& meta);

    /// Call this function with the round-trip time of the latest exchange with the server.
    /// This is used to calculate the average latency, which in turn can be used for other purposes,
    /// for example calculating retransmission timeouts.
    void updateLatency(asp::time::Duration rtt);

    void updateLastActivity();
    void updateLastKeepalive();
    asp::time::Duration sinceLastActivity() const;
    asp::time::Duration sinceLastKeepalive() const;

    uint64_t getKeepaliveTimestamp() const;
};

}