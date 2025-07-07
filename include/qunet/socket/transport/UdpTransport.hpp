#pragma once

#include "BaseTransport.hpp"
#include <qsox/UdpSocket.hpp>

namespace qn {

class UdpTransport : public BaseTransport {
public:
    ~UdpTransport() override;
    UdpTransport(UdpTransport&&) = default;
    UdpTransport& operator=(UdpTransport&&) = default;

    static qsox::NetResult<UdpTransport> connect(const qsox::SocketAddress& address);

    TransportResult<QunetMessage> performHandshake(
        HandshakeStartMessage handshakeStart,
        const std::optional<asp::time::Duration>& timeout
    ) override;

    TransportResult<> close() override;
    bool isClosed() const override;
    TransportResult<> sendMessage(QunetMessage data) override;
    TransportResult<bool> poll(const std::optional<asp::time::Duration>& dur) override;
    TransportResult<bool> processIncomingData() override;

private:
    friend class MultiPoller;

    qsox::UdpSocket m_socket;
    bool m_closed = false;

    UdpTransport(qsox::UdpSocket socket);
};

}
