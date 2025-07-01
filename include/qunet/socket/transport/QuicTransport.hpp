#pragma once

#include "BaseTransport.hpp"
#include "tls/ClientTlsContext.hpp"
#include <qunet/buffers/CircularByteBuffer.hpp>
#include <qsox/SocketAddress.hpp>
#include <memory>

namespace qn {

template <typename T = void>
using QuicResult = geode::Result<T, QuicError>;

class QuicTransport : public BaseTransport {
public:
    ~QuicTransport() override;

    static TransportResult<QuicTransport> connect(
        const qsox::SocketAddress& address,
        const asp::time::Duration& timeout,
        const ClientTlsContext* tlsContext = nullptr,
        const struct ConnectionDebugOptions* debugOptions = nullptr
    );

    QuicTransport(QuicTransport&&);
    QuicTransport& operator=(QuicTransport&&);

    TransportResult<> sendMessage(QunetMessage data) override;
    TransportResult<bool> poll(const asp::time::Duration& dur) override;
    TransportResult<QunetMessage> receiveMessage() override;

private:
    std::unique_ptr<class QuicConnection> m_conn = nullptr;
    CircularByteBuffer m_recvBuffer;

    QuicTransport(std::unique_ptr<QuicConnection> connection);
};

}
