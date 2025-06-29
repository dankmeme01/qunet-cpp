#pragma once

#include "BaseTransport.hpp"
#include <qsox/TcpStream.hpp>

namespace qn {

class TcpTransport : public BaseTransport {
public:
    ~TcpTransport() override;

    static qsox::NetResult<TcpTransport> connect(const qsox::SocketAddress& address, const asp::time::Duration& timeout);

    TcpTransport(TcpTransport&&) = default;
    TcpTransport& operator=(TcpTransport&&) = default;

    TransportResult<> sendMessage(QunetMessage data) override;
    TransportResult<bool> poll(const asp::time::Duration& dur) override;
    TransportResult<QunetMessage> receiveMessage() override;

private:
    qsox::TcpStream m_socket;
    std::vector<uint8_t> m_readBuffer;
    size_t m_readBufferPos = 0;

    TcpTransport(qsox::TcpStream socket);
};

}
