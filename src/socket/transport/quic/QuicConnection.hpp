#pragma once

#include <qunet/socket/transport/QuicTransport.hpp>
#include "QuicStream.hpp"
#include "../tls/ClientTlsSession.hpp"

#include <qsox/UdpSocket.hpp>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <asp/time/Duration.hpp>
#include <chrono>

namespace qn {

inline uint64_t timestamp() {
    return static_cast<ngtcp2_tstamp>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count());
}

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
        const ClientTlsContext* tlsContext
    );

    // Blocks until data is available to be received, or the timeout expires.
    // Returns true if data is available, false if the timeout expired, or an error if something went wrong.
    TransportResult<bool> pollReadable(const asp::time::Duration& dur);

    // Sends data over the QUIC stream. Returns the number of bytes sent, or an error.
    TransportResult<size_t> send(const uint8_t* data, size_t len);

    // Sends data over the QUIC stream. Blocks until all the data is sent or an error occurs.
    TransportResult<> sendAll(const uint8_t* data, size_t len);

    // Receives data from the QUIC stream. Returns the number of bytes received, or an error.
    TransportResult<size_t> receive(uint8_t* buffer, size_t len);

    ngtcp2_conn* rawHandle() const;
    ngtcp2_crypto_conn_ref* connRef() const;

private:
    friend class ClientTlsSession;
    friend class QuicStream;

    ngtcp2_conn* m_conn = nullptr;
    ngtcp2_crypto_conn_ref m_connRef;
    ngtcp2_path_storage m_networkPath;

    std::optional<ClientTlsSession> m_tlsSession;
    std::optional<qsox::UdpSocket> m_socket;
    std::optional<QuicStream> m_mainStream;

    QuicConnection(ngtcp2_conn* conn);

    QuicResult<QuicStream> openBidiStream();
    TransportResult<> performHandshake(const asp::time::Duration& timeout);

    TransportResult<> waitUntilWritable(int64_t streamId);
    TransportResult<> waitForBufferSpace(int64_t streamId);

    TransportResult<> doRecv();

    // Returns true if the given error is related to congestion, flow control or buffering,
    // and the application should wait before sending more data.
    bool isCongestionRelatedError(const TransportError& err);
};

}