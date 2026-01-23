#pragma once

#include "transport/BaseTransport.hpp"
#include <qunet/tls/TlsContext.hpp>

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
    std::string hostname;
    const struct ConnectionOptions* connOptions = nullptr;
#ifdef QUNET_TLS_SUPPORT
    std::shared_ptr<TlsContext> tlsContext;
#endif
    bool reconnecting = false;
};

struct OutgoingMessage {
    QunetMessage message;
    bool reliable = false;
    bool uncompressed = false;
    std::string tag;
};

class Socket {
public:
    // Attempts to connect to the specified address using the given connection type.
    static arc::Future<TransportResult<Socket>> connect(
        const TransportOptions& options,
        std::optional<std::filesystem::path> qdbFolder = std::nullopt
    );
    static arc::Future<TransportResult<Socket>> reconnect(const TransportOptions& options, Socket& prev);

    Socket(Socket&&) noexcept = default;
    Socket& operator=(Socket&&) noexcept = default;

    // Closes the transport. This does not send a `ClientClose` message.
    // This may or may not block - see notes in BaseTransport::close.
    arc::Future<TransportResult<>> close();

    /// Closes the transport without blocking. This may not fully clean up resources.
    TransportResult<> closeSync();

    // Returns true if `close()` was called and the transport finished closing.
    bool isClosed() const;

    // Attempts to reconnect to the currently connected remote.
    arc::Future<TransportResult<>> reconnect();

    /// Send a message over the transport. Note: if it's a Data message, reliability and compression headers are ignored.
    /// The `reliable` argument is used to make the message reliable, and compression is applied automatically if needed, unless `uncompressed` is set to true.
    arc::Future<TransportResult<>> sendMessage(OutgoingMessage message);
    arc::Future<TransportResult<QunetMessage>> receiveMessage();

    /// Returns the average latency of the connection.
    asp::time::Duration getLatency() const;

    /// Returns how much time is left until the transport timer expires.
    /// Currently, this is only used for UDP transports to determine when to retransmit messages (or send ACKs),
    /// and by UDP/TCP to determine when to send keepalive messages.
    /// After this time elapses, you must call `handleTimerExpiry()` to handle the expiry.
    asp::time::Duration untilTimerExpiry() const;
    arc::Future<TransportResult<>> handleTimerExpiry();

    std::shared_ptr<BaseTransport> transport() const;

private:
    std::shared_ptr<BaseTransport> m_transport;
    qsox::SocketAddress m_remoteAddress;
    asp::time::Duration m_connTimeout;
    std::optional<QunetDatabase> m_usedQdb;
    std::optional<std::filesystem::path> m_qdbFolder;

    Socket(std::shared_ptr<BaseTransport> transport, qsox::SocketAddress remote)
        : m_transport(std::move(transport)), m_remoteAddress(std::move(remote)) {}

    static arc::Future<TransportResult<std::shared_ptr<BaseTransport>>> createTransport(const TransportOptions& options);
    static arc::Future<TransportResult<std::pair<Socket, asp::time::Duration>>> createSocket(const TransportOptions& options);

    arc::Future<TransportResult<>> onHandshakeSuccess(const HandshakeFinishMessage& msg);
    TransportResult<> onReconnectSuccess(Socket& older);

    CompressionType shouldCompress(std::span<const uint8_t> data) const;
    CompressorResult<> doCompressZstd(DataMessage& message, bool useDict = true) const;
    CompressorResult<> doCompressLz4(DataMessage& message) const;
};

}
