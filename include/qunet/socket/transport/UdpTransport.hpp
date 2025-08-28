#pragma once

#include "BaseTransport.hpp"
#include "udp/ReliableStore.hpp"
#include "udp/FragmentStore.hpp"
#include <qsox/UdpSocket.hpp>

namespace qn {

class UdpTransport : public BaseTransport {
public:
    ~UdpTransport() override;
    UdpTransport(UdpTransport&&) = default;
    UdpTransport& operator=(UdpTransport&&) = default;

    static qsox::NetResult<UdpTransport> connect(
        const qsox::SocketAddress& address,
        const struct ConnectionOptions& connOptions
    );

    TransportResult<QunetMessage> performHandshake(
        HandshakeStartMessage handshakeStart,
        const std::optional<asp::time::Duration>& timeout
    ) override;

    TransportResult<> close() override;
    bool isClosed() const override;
    TransportResult<> sendMessage(QunetMessage data, bool reliable) override;
    TransportResult<bool> poll(const std::optional<asp::time::Duration>& dur) override;
    TransportResult<bool> processIncomingData() override;

    asp::time::Duration untilTimerExpiry() const override;
    TransportResult<> handleTimerExpiry() override;

private:
    friend class MultiPoller;

    ReliableStore m_relStore;
    FragmentStore m_fragStore;
    qsox::UdpSocket m_socket;
    size_t m_mtu;
    bool m_closed = false;
    size_t m_unackedKeepalives = 0;
    float m_lossSim = 0.f;
    std::optional<asp::time::Duration> m_activeKeepaliveInterval;

    UdpTransport(qsox::UdpSocket socket, size_t mtu, const struct ConnectionOptions& connOptions);

    // Performs fragmentation (if needed) and sends the message.
    // Reliability and compression headers should already be set in the message, if they are needed.
    TransportResult<> doSendUnfragmentedData(QunetMessage& message, bool retransmission = false);

    bool shouldLosePacket();
    asp::time::Duration untilKeepalive() const;
};

}
