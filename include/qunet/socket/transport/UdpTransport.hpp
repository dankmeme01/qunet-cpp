#pragma once

#include "BaseTransport.hpp"
#include "udp/ReliableStore.hpp"
#include "udp/FragmentStore.hpp"
#include <arc/net/UdpSocket.hpp>

namespace qn {

class UdpTransport : public BaseTransport {
public:
    using BaseTransport::sendMessage;

    ~UdpTransport() override;
    UdpTransport(UdpTransport&&) noexcept = default;
    UdpTransport& operator=(UdpTransport&&) noexcept = default;

    static arc::Future<qsox::NetResult<UdpTransport>> connect(
        const qsox::SocketAddress& address,
        const struct ConnectionOptions& connOptions
    );

    arc::Future<TransportResult<QunetMessage>> performHandshake(
        HandshakeStartMessage handshakeStart
    ) override;

    TransportResult<> closeSync() override;
    bool isClosed() const override;
    arc::Future<TransportResult<>> sendMessage(QunetMessage data, SentMessageContext& ctx) override;
    arc::Future<TransportResult<>> poll() override;
    arc::Future<TransportResult<QunetMessage>> receiveMessage() override;

    asp::time::Duration untilTimerExpiry() const override;
    arc::Future<TransportResult<>> handleTimerExpiry() override;

private:
    friend class MultiPoller;

    ReliableStore m_relStore;
    FragmentStore m_fragStore;
    arc::UdpSocket m_socket;
    size_t m_mtu;
    bool m_closed = false;
    float m_lossSim = 0.f;
    std::optional<asp::time::Duration> m_activeKeepaliveInterval;
    std::queue<QunetMessage> m_oobMessages;

    UdpTransport(arc::UdpSocket socket, size_t mtu, const struct ConnectionOptions& connOptions);

    // Performs fragmentation (if needed) and sends the message.
    // Reliability and compression headers should already be set in the message, if they are needed.
    arc::Future<TransportResult<>> doSendUnfragmentedData(QunetMessage& message, SentMessageContext& ctx, bool retransmission = false);

    bool shouldLosePacket();
    asp::time::Duration untilKeepalive() const;

    arc::Future<TransportResult<std::optional<QunetMessage>>> receiveMessageInner();
};

}
