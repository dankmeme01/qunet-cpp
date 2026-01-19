#pragma once
#ifdef QUNET_TLS_SUPPORT

#include "TcpTlsSession.hpp"
#include <qunet/socket/transport/Error.hpp>
#include <arc/net/TcpStream.hpp>

namespace qn {

/// An asynchronous TCP socket that uses TLS v1.3 for encryption.
/// Can be used for implementing anything that requires TLS,
/// such as DNS over TLS, HTTPS or WebSockets over TLS.
class TlsSocket : arc::EventIoBase<TlsSocket> {
public:
    ~TlsSocket();

    // Creates a new TLS stream, connecting to the given address.
    // A TLS context and session are implicitly created for this connection.
    static arc::Future<TransportResult<TlsSocket>> connect(
        qsox::SocketAddress address,
        std::string_view serverName = ""
    );

    // Creates a new TLS stream, connecting to the given address and using the given TLS context.
    static arc::Future<TransportResult<TlsSocket>> connect(
        qsox::SocketAddress address,
        std::shared_ptr<TcpTlsContext> tlsContext,
        std::string_view serverName = ""
    );

    TlsSocket(TlsSocket&& other) noexcept = default;
    TlsSocket& operator=(TlsSocket&& other) noexcept = default;

    // Shuts down the stream for reading, writing, or both.
    arc::Future<qsox::NetResult<void>> shutdown(qsox::ShutdownMode mode);

    // Sets the TCP_NODELAY option on the socket, which disables or enables the Nagle algorithm.
    // If `noDelay` is true, small packets are sent immediately without waiting for larger packets to accumulate.
    qsox::NetResult<void> setNoDelay(bool noDelay);

    // Sends data over this socket. Returns amount of bytes sent.
    arc::Future<TransportResult<size_t>> send(const void* data, size_t size);

    // Sends data over this socket, waiting until all data is sent, or an error occurs.
    arc::Future<TransportResult<void>> sendAll(const void* data, size_t size);

    // Receives data from the socket. Returns amount of bytes received.
    arc::Future<TransportResult<size_t>> receive(void* buffer, size_t size);

    // Receives data from the socket, waiting until the given buffer is full or an error occurs.
    arc::Future<TransportResult<void>> receiveExact(void* buffer, size_t size);

    qsox::NetResult<qsox::SocketAddress> localAddress() const;
    qsox::NetResult<qsox::SocketAddress> remoteAddress() const;

    /// Get the handle to the inner `qsox::TcpStream`.
    inline qsox::TcpStream& inner() noexcept {
        return m_stream;
    }

private:
    qsox::TcpStream m_stream;
    TcpTlsSession m_tls;

    TlsSocket(
        qsox::TcpStream stream,
        arc::Registration io,
        TcpTlsSession tls
    );

    std::optional<TransportResult<size_t>> pollWrite(const void* data, size_t size, uint64_t& id);
    std::optional<TransportResult<size_t>> pollRead(void* buf, size_t size, uint64_t& id, bool peek = false);
    std::optional<TransportResult<>> pollHandshake(uint64_t& id);
    std::optional<TransportResult<>> pollTls(
        uint64_t& id,
        arc::Interest initialInterest,
        std23::function_ref<int()> fn
    );

    arc::Future<TransportResult<void>> handshake();
};

}

#endif
