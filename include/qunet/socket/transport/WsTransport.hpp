#pragma once

#ifdef QUNET_WS_SUPPORT

#include "BaseTransport.hpp"
#include <wsx/AsyncClient.hpp>
#include <dbuf/CircularByteBuffer.hpp>

namespace qn {

class WsTransport : public BaseTransport {
public:
    using BaseTransport::sendMessage;

    ~WsTransport() override;
    WsTransport(WsTransport&&) noexcept = default;
    WsTransport& operator=(WsTransport&&) noexcept = default;

    static arc::Future<TransportResult<WsTransport>> connect(
        const asp::time::Duration& timeout,
        const struct ConnectionOptions& connOptions,
        const wsx::ClientConnectOptions& wsxOpts
    );

    arc::Future<TransportResult<>> close() override;
    TransportResult<> closeSync() override;
    bool isClosed() const override;
    arc::Future<TransportResult<>> sendMessage(QunetMessage data, SentMessageContext& ctx) override;
    arc::Future<TransportResult<>> poll() override;
    arc::Future<TransportResult<QunetMessage>> receiveMessage() override;

    asp::time::Duration untilTimerExpiry() const override;
    arc::Future<TransportResult<>> handleTimerExpiry() override;

private:
    friend class MultiPoller;

    wsx::AsyncClient m_ws;
    dbuf::CircularByteBuffer m_recvBuffer;
    std::optional<asp::time::Duration> m_activeKeepaliveInterval;
    bool m_closed = false;

    WsTransport(wsx::AsyncClient ws);
    asp::time::Duration untilKeepalive() const;
};

}

#endif
