#pragma once

#include "transport/BaseTransport.hpp"
#include "transport/tls/ClientTlsContext.hpp"

#include <asp/time/Duration.hpp>
#include <qsox/SocketAddress.hpp>
#include <memory>

namespace qn {

enum class ConnectionType {
    Unknown,
    Tcp,
    Udp,
    Quic,
};

struct TransportOptions {
    qsox::SocketAddress address;
    ConnectionType type;
    asp::time::Duration timeout;
    const struct ConnectionOptions* connOptions = nullptr;
#ifdef QUNET_TLS_SUPPORT
    const ClientTlsContext* tlsContext = nullptr;
#endif
    bool reconnecting = false;
};

class Socket {
public:
    // Attempts to connect to the specified address using the given connection type.
    static TransportResult<Socket> connect(const TransportOptions& options);
    static TransportResult<Socket> reconnect(const TransportOptions& options, Socket& prev);

    // Closes the transport. This does not send a `ClientClose` message. This may or may not block - see notes in BaseTransport::close.
    TransportResult<> close();

    // Returns true if `close()` was called and the transport finished closing.
    bool isClosed() const;

    // Attempts to reconnect to the currently connected remote.
    TransportResult<> reconnect();

    /// Send a message over the transport. Note: if it's a Data message, reliability and compression headers are ignored.
    /// The `reliable` argument is used to make the message reliable, and compression is applied automatically if needed.
    TransportResult<> sendMessage(QunetMessage&& message, bool reliable = true);
    TransportResult<QunetMessage> receiveMessage(const std::optional<asp::time::Duration>& timeout);

    /// Processes incoming data from the transport. This method does not block, it will try to read as much data as available,
    /// and return whether an entire message is available to be read.
    TransportResult<bool> processIncomingData();

    /// Returns whether there is a message available to be read from the transport.
    /// In this case, `receiveMessage()` will return a message without blocking.
    bool messageAvailable();

    /// Returns the average latency of the connection.
    asp::time::Duration getLatency() const;

    /// Returns how much time is left until the transport timer expires.
    /// Currently, this is only used for UDP transports to determine when to retransmit messages (or send ACKs),
    /// and by UDP/TCP to determine when to send keepalive messages.
    /// After this time elapses, you must call `handleTimerExpiry()` to handle the expiry.
    asp::time::Duration untilTimerExpiry() const;
    TransportResult<> handleTimerExpiry();

    std::shared_ptr<BaseTransport> transport() const;

private:
    std::shared_ptr<BaseTransport> m_transport;
    asp::time::Duration m_connTimeout;

    Socket(std::shared_ptr<BaseTransport> transport) : m_transport(std::move(transport)) {}

    static TransportResult<std::shared_ptr<BaseTransport>> createTransport(const TransportOptions& options);
    static TransportResult<std::pair<Socket, asp::time::Duration>> createSocket(const TransportOptions& options);

    TransportResult<> onHandshakeSuccess(const HandshakeFinishMessage& msg);
    TransportResult<> onReconnectSuccess(Socket& older);

    CompressionType shouldCompress(size_t size) const;
    CompressorResult<> doCompressZstd(DataMessage& message) const;
    CompressorResult<> doCompressLz4(DataMessage& message) const;
};

}
