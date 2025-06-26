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

    qsox::NetResult<> sendMessage(std::span<const uint8_t> data) override;

private:
    qsox::UdpSocket m_socket;

    UdpTransport(qsox::UdpSocket socket);
};

}
