#pragma once

#include "BaseTransport.hpp"
#include <qunet/buffers/CircularByteBuffer.hpp>
#include <arc/net/TcpStream.hpp>

namespace qn {

class TcpTransport : public BaseTransport {
public:
    ~TcpTransport() override;
    TcpTransport(TcpTransport&&) = default;
    TcpTransport& operator=(TcpTransport&&) = default;

    static arc::Future<qsox::NetResult<TcpTransport>> connect(
        const qsox::SocketAddress& address,
        const asp::time::Duration& timeout,
        const struct ConnectionOptions& connOptions
    );

    // TCP shutdown is already synchronous, so no need for async close
    TransportResult<> closeSync() override;
    bool isClosed() const override;
    arc::Future<TransportResult<>> sendMessage(QunetMessage data, bool reliable) override;
    arc::Future<TransportResult<>> poll() override;
    arc::Future<TransportResult<QunetMessage>> receiveMessage() override;

    asp::time::Duration untilTimerExpiry() const override;
    arc::Future<TransportResult<>> handleTimerExpiry() override;

private:
    friend class MultiPoller;

    arc::TcpStream m_socket;
    CircularByteBuffer m_recvBuffer;
    std::optional<asp::time::Duration> m_activeKeepaliveInterval;
    bool m_closed = false;

    TcpTransport(arc::TcpStream socket);
    asp::time::Duration untilKeepalive() const;
};

}
