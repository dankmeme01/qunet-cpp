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
    QuicTransport(QuicTransport&&);
    QuicTransport& operator=(QuicTransport&&);

    static TransportResult<QuicTransport> connect(
        const qsox::SocketAddress& address,
        const asp::time::Duration& timeout,
        const ClientTlsContext* tlsContext = nullptr,
        const struct ConnectionDebugOptions* debugOptions = nullptr
    );

    TransportResult<> close() override;
    bool isClosed() const override;
    TransportResult<> sendMessage(QunetMessage data) override;
    TransportResult<bool> poll(const std::optional<asp::time::Duration>& dur) override;
    TransportResult<bool> processIncomingData() override;

    class QuicConnection& connection();

private:
    std::unique_ptr<class QuicConnection> m_conn = nullptr;
    CircularByteBuffer m_recvBuffer;

    QuicTransport(std::unique_ptr<QuicConnection> connection);
};

}
