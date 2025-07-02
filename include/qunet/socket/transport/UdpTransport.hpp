#pragma once

#include "BaseTransport.hpp"
#include <qsox/UdpSocket.hpp>

namespace qn {

class UdpTransport : public BaseTransport {
public:
    ~UdpTransport() override;

    static qsox::NetResult<UdpTransport> connect(const qsox::SocketAddress& address);

    UdpTransport(UdpTransport&&) = default;
    UdpTransport& operator=(UdpTransport&&) = default;

    TransportResult<> close() override;
    bool isClosed() const override;
    TransportResult<> sendMessage(QunetMessage data) override;
    TransportResult<bool> poll(const asp::time::Duration& dur) override;
    TransportResult<QunetMessage> receiveMessage() override;

private:
    qsox::UdpSocket m_socket;
    bool m_closed = false;

    UdpTransport(qsox::UdpSocket socket);
};

}
