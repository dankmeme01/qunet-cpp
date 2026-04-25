#pragma once

#ifdef QUNET_QUIC_SUPPORT

#include "BaseTransport.hpp"
#include <xtls/Context.hpp>
#include <dbuf/CircularByteBuffer.hpp>
#include <qsox/SocketAddress.hpp>
#include <memory>

namespace qn {

template <typename T = void>
using QuicResult = geode::Result<T, QuicError>;

class QuicTransport : public BaseTransport {
public:
    ~QuicTransport() override;
    QuicTransport(QuicTransport&&) noexcept;
    QuicTransport& operator=(QuicTransport&&) noexcept;

    static arc::Future<TransportResult<QuicTransport>> connect(
        const qsox::SocketAddress& address,
        const asp::time::Duration& timeout,
        std::shared_ptr<xtls::Context> tlsContext = nullptr,
        const struct ConnectionOptions* connOptions = nullptr,
        const std::string& hostname = ""
    );

    arc::Future<TransportResult<>> close() override;
    TransportResult<> closeSync() override;
    bool isClosed() const override;
    arc::Future<TransportResult<>> sendMessage(QunetMessage data, SentMessageContext& ctx) override;
    arc::Future<TransportResult<>> poll() override;
    arc::Future<TransportResult<QunetMessage>> receiveMessage() override;

    asp::time::Duration untilTimerExpiry() const override;
    arc::Future<TransportResult<>> handleTimerExpiry() override;

    class QuicConnection& connection();

private:
    std::shared_ptr<class QuicConnection> m_conn;
    dbuf::CircularByteBuffer m_recvBuffer;

    QuicTransport(std::shared_ptr<QuicConnection> connection);
};

}

#endif
