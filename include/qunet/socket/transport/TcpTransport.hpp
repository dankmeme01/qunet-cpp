#pragma once

#include "BaseTransport.hpp"
#include <qunet/buffers/CircularByteBuffer.hpp>
#include <qsox/TcpStream.hpp>

namespace qn {

class TcpTransport : public BaseTransport {
public:
    ~TcpTransport() override;

    static qsox::NetResult<TcpTransport> connect(const qsox::SocketAddress& address, const asp::time::Duration& timeout);

    TcpTransport(TcpTransport&&) = default;
    TcpTransport& operator=(TcpTransport&&) = default;

    TransportResult<> close() override;
    bool isClosed() const override;
    TransportResult<> sendMessage(QunetMessage data) override;
    TransportResult<bool> poll(const asp::time::Duration& dur) override;
    TransportResult<QunetMessage> receiveMessage() override;

private:
    qsox::TcpStream m_socket;
    CircularByteBuffer m_recvBuffer;
    bool m_closed = false;

    TcpTransport(qsox::TcpStream socket);
};

}
