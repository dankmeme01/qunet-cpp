#pragma once

#ifdef QUNET_QUIC_SUPPORT

#include <qunet/socket/transport/QuicTransport.hpp>
#include <socket/transport/tls/ClientTlsSession.hpp>
#include "QuicStream.hpp"

#include <std23/function_ref.h>
#include <arc/task/CancellationToken.hpp>
#include <arc/net/UdpSocket.hpp>
#include <arc/sync/Notify.hpp>
#include <arc/sync/Mutex.hpp>
#include <asp/time/Instant.hpp>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>

namespace qn {

inline ngtcp2_tstamp timestamp() {
    return asp::time::Instant::now().rawNanos();
}

class QuicConnection {
public:
    ~QuicConnection();

    QuicConnection(const QuicConnection&) = delete;
    QuicConnection& operator=(const QuicConnection&) = delete;
    QuicConnection(QuicConnection&&) noexcept = delete;
    QuicConnection& operator=(QuicConnection&&) noexcept = delete;

    static arc::Future<TransportResult<std::shared_ptr<QuicConnection>>> connect(
        const qsox::SocketAddress& address,
        const asp::time::Duration& timeout,
        const ClientTlsContext* tlsContext,
        const struct ConnectionOptions* connOptions
    );

    arc::Future<TransportResult<>> close();
    TransportResult<> closeSync();
    bool isClosed() const;

    // Polls the given stream for readability.
    arc::Future<TransportResult<>> pollReadable(int64_t streamId);
    // Polls the main stream for readability.
    arc::Future<TransportResult<>> pollReadable();
    // Polls the given stream for writability.
    arc::Future<TransportResult<>> pollWritable(int64_t streamId);
    // Polls the main stream for writability.
    arc::Future<TransportResult<>> pollWritable();

    // Sends data over the primary QUIC stream.
    // Returns the number of bytes sent, or an error.
    arc::Future<TransportResult<size_t>> send(const void* data, size_t len);

    // Sends the data over the given QUIC stream, returns number of bytes sent.
    arc::Future<TransportResult<size_t>> send(int64_t streamId, const void* data, size_t len);

    // Sends all the given data over the primary QUIC stream.
    arc::Future<TransportResult<>> sendAll(const void* data, size_t len);

    // Sends all the given data over the given QUIC stream.
    arc::Future<TransportResult<>> sendAll(int64_t streamId, const void* data, size_t len);

    // Receives data from the primary QUIC stream. Returns the number of bytes written.
    arc::Future<TransportResult<size_t>> receive(void* buf, size_t bufSize);

    // Receives data from the given QUIC stream. Returns the number of bytes written.
    arc::Future<TransportResult<size_t>> receive(int64_t streamId, void* buf, size_t bufSize);

    // Retrieves a QUIC stream by its ID
    arc::Future<TransportResult<std::shared_ptr<QuicStream>>> getStream(int64_t streamId);

    // Creates a new bidirectional QUIC stream and returns its ID
    arc::Future<QuicResult<int64_t>> openStream();

    // Closes a QUIC stream by the given ID
    arc::Future<TransportResult<>> closeStream(int64_t id);

    ngtcp2_conn* rawHandle() const;
    ngtcp2_crypto_conn_ref* connRef() const;

    asp::time::Duration untilTimerExpiry() const;
    arc::Future<TransportResult<>> handleTimerExpiry();

private:
    // friend class ClientTlsSession;
    friend class QuicStream;

    QuicConnection(ngtcp2_conn*);

    asp::time::Instant m_connectDeadline;
    asp::time::Instant m_nextExpiry;
    asp::Mutex<> m_connLock;
    ngtcp2_conn* m_conn = nullptr;
    ngtcp2_crypto_conn_ref m_connRef;
    ngtcp2_path_storage m_networkPath;
    ngtcp2_tstamp m_connExpiry = UINT64_MAX;

    std::optional<ClientTlsSession> m_tls;
    std::optional<arc::UdpSocket> m_socket;
    arc::Mutex<std::unordered_map<int64_t, std::shared_ptr<QuicStream>>> m_streams;
    int64_t m_mainStreamId = -1;
    float m_lossSimulation = 0.f;
    bool m_closed = false;

    std::atomic<size_t> m_totalBytesSent{0};
    std::atomic<size_t> m_totalBytesReceived{0};
    asp::time::Instant m_lastSendAttempt = asp::time::Instant::now();

    std::optional<arc::TaskHandle<void>> m_workerTask;
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_workerRunning{false};
    arc::CancellationToken m_cancel;
    arc::Notify m_connectedNotify;
    arc::Notify m_workerNotify;

    arc::Future<TransportResult<>> performHandshake();

    arc::Future<TransportResult<>> sendHandshakePacket();
    arc::Future<TransportResult<>> sendNonStreamPacket();
    arc::Future<TransportResult<>> sendStreamData(QuicStream& stream, bool fin = false);
    arc::Future<TransportResult<>> sendClosePacket();
    arc::Future<TransportResult<>> sendPacket(const uint8_t* buf, size_t size);
    /// This should only be called from the worker thread after an established connection
    arc::Future<TransportResult<>> receivePacket();
    TransportResult<size_t> wrapWritePacket(uint8_t* buf, size_t size, bool handshake);

    bool shouldLosePacket() const;

    void onReceivedData(int64_t streamId, const uint8_t* data, size_t len);
    void onAckedData(int64_t streamId, uint64_t offset, size_t len);

    arc::Future<> workerLoop();
    arc::Future<TransportResult<>> workerHandleWrites();

    auto withLockedConn(auto&& func) {
        auto guard = m_connLock.lock();
        return func();
    }
};

}

#endif
