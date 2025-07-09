#pragma once

#include "BaseTransport.hpp"
#include <qunet/buffers/CircularByteBuffer.hpp>
#include <qsox/TcpStream.hpp>

namespace qn {

class TcpTransport : public BaseTransport {
public:
    ~TcpTransport() override;
    TcpTransport(TcpTransport&&) = default;
    TcpTransport& operator=(TcpTransport&&) = default;

    static qsox::NetResult<TcpTransport> connect(const qsox::SocketAddress& address, const asp::time::Duration& timeout);

    TransportResult<> close() override;
    bool isClosed() const override;
    TransportResult<> sendMessage(QunetMessage data, bool reliable) override;
    TransportResult<bool> poll(const std::optional<asp::time::Duration>& dur) override;
    TransportResult<bool> processIncomingData() override;

private:
    friend class MultiPoller;

    qsox::TcpStream m_socket;
    CircularByteBuffer m_recvBuffer;
    bool m_closed = false;

    TcpTransport(qsox::TcpStream socket);
};

}
