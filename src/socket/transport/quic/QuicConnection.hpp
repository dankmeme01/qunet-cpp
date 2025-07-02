#pragma once

#include <qunet/socket/transport/QuicTransport.hpp>
#include "QuicStream.hpp"
#include "../tls/ClientTlsSession.hpp"

#include <qsox/UdpSocket.hpp>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <asp/time/Duration.hpp>
#include <asp/time/Instant.hpp>
#include <asp/sync/Atomic.hpp>
#include <asp/thread/Thread.hpp>
#include <semaphore>

namespace qn {

inline ngtcp2_tstamp timestamp() {
    return asp::time::Instant::now().rawNanos();
}

struct QuicConnectionStats {
    size_t totalSent = 0, totalReceived = 0, totalDataSent = 0, totalDataReceived = 0;
};

class QuicConnection {
public:
    ~QuicConnection();

    QuicConnection(const QuicConnection&) = delete;
    QuicConnection& operator=(const QuicConnection&) = delete;
    QuicConnection(QuicConnection&&) noexcept = delete;
    QuicConnection& operator=(QuicConnection&&) noexcept = delete;

    static TransportResult<std::unique_ptr<QuicConnection>> connect(
        const qsox::SocketAddress& address,
        const asp::time::Duration& timeout,
        const ClientTlsContext* tlsContext,
        const struct ConnectionDebugOptions* debugOptions
    );

    // Blocks until data is available to be received, or the timeout expires.
    // Returns true if data is available, false if the timeout expired, or an error if something went wrong.
    TransportResult<bool> pollReadable(const asp::time::Duration& dur);

    TransportResult<bool> pollWritable(const asp::time::Duration& dur);

    // Sends data over the QUIC stream. Returns the number of bytes sent, or an error.
    TransportResult<size_t> send(const uint8_t* data, size_t len);

    // Sends data over the QUIC stream. Blocks until all the data is sent or an error occurs.
    TransportResult<> sendAll(const uint8_t* data, size_t len);

    // Receives data from the QUIC stream. Returns the number of bytes received, or an error.
    // Blocks until data is available or an error occurs.
    TransportResult<size_t> receive(uint8_t* buffer, size_t len);

    QuicConnectionStats connStats() const;

    // Attempts to cleanly close the connection. If this returns an error, consider it fatal and destroy the connection.
    // Destroying will not send a close packet and will abruptly terminate the connection.
    TransportResult<> close();

    // Returns whether the connection is fully closed now.
    bool finishedClosing() const;

    ngtcp2_conn* rawHandle() const;
    ngtcp2_crypto_conn_ref* connRef() const;

private:
    friend class ClientTlsSession;
    friend class QuicStream;

    asp::Mutex<ngtcp2_conn*, true> m_conn = nullptr;
    ngtcp2_crypto_conn_ref m_connRef;
    ngtcp2_path_storage m_networkPath;

    float m_lossSimulation = 0.0f;

    std::optional<ClientTlsSession> m_tlsSession;
    std::optional<qsox::UdpSocket> m_socket;
    std::optional<QuicStream> m_mainStream;
    asp::Thread<> m_connThread;
    enum class ThreadState {
        Idle,
        Handshaking,
        Running,
        Stopping,
        Stopped,
    } m_connThreadState = ThreadState::Idle;
    ngtcp2_tstamp m_connThrExpiry = UINT64_MAX;
    asp::time::Duration m_connTimeout;
    TransportResult<> m_handshakeResult = Ok();
    std::optional<TransportError> m_fatalError;
    asp::AtomicBool m_terminating{false};
    asp::AtomicBool m_terminateCleanly{false};
    asp::AtomicBool m_closed{false};

    std::binary_semaphore m_connectionReadySema{0};

    // for tracking total bytes sent and received
    std::atomic_size_t m_totalBytesSent = 0;
    std::atomic_size_t m_totalBytesReceived = 0;
    std::atomic_size_t m_totalDataBytesSent = 0;
    std::atomic_size_t m_totalDataBytesReceived = 0;

    QuicConnection(ngtcp2_conn* conn);

    QuicResult<QuicStream> openBidiStream();
    TransportResult<> performHandshake(const asp::time::Duration& timeout);

    TransportResult<> doRecv();

    TransportResult<bool> pollReadableSocket(const asp::time::Duration& dur);

    TransportResult<> sendNonStreamPacket(bool handshake = false);
    TransportResult<> sendClosePacket();

    // vvv notifications and waiters vvv
    /// the waiter functions release the lock for you before waiting, you MUST hold it before calling them
    void notifyDataWritten();
    void notifyWritable(asp::MutexGuard<void>& lock);
    void notifyReadable(asp::MutexGuard<void>& lock);
    bool waitUntilWritable(const asp::time::Duration& timeout, asp::MutexGuard<void>& lock);
    bool waitUntilReadable(const asp::time::Duration& timeout, asp::MutexGuard<void>& lock);

    // Returns true if the given error is related to congestion, flow control or buffering,
    // and the application should wait before sending more data.
    bool isCongestionRelatedError(const TransportError& err);

    // vvv thread functions vvv
    void threadFunc(asp::StopToken<>& token);

    struct ThrPollResult {
        bool sockReadable = false;
        bool newDataAvail = false;
    };

    asp::Mutex<> m_waiterMutex;

#ifdef _WIN32
#else
    // for [notify|waitUntil]Writable
    int ackPipeRead = -1, ackPipeWrite = -1;
    size_t m_ackPipeWaiters = 0;

    // for [notify|waitUntil]Readable
    int recvPipeRead = -1, recvPipeWrite = -1;
    size_t m_recvPipeWaiters = 0;

    // for notifyDataWritten
    int wrbPipeRead = -1, wrbPipeWrite = -1;
#endif

    void thrPlatformSetup();
    void thrPlatformCleanup();
    void thrOnIdleTimeout();
    void thrOnFatalError(const TransportError& err);
    void thrHandleError(const TransportError& err);
    ThrPollResult thrPoll(const asp::time::Duration& timeout);

    bool shouldLosePacket() const;
};

}