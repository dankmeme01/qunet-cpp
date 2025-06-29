#pragma once

#include "transport/BaseTransport.hpp"
#include "transport/tls/ClientTlsContext.hpp"

#include <asp/time/Duration.hpp>
#include <qsox/SocketAddress.hpp>
#include <memory>

namespace qn {

enum class ConnectionType {
    Unknown,
    Tcp,
    Udp,
    Quic,
};

class Socket {
public:
    // Attempts to connect to the specified address using the given connection type.
    static TransportResult<Socket> connect(
        const qsox::SocketAddress& address,
        ConnectionType type,
        const asp::time::Duration& timeout,
        const ClientTlsContext* tlsContext = nullptr
    );

private:
    std::shared_ptr<BaseTransport> m_transport;

    Socket(std::shared_ptr<BaseTransport> transport) : m_transport(std::move(transport)) {}

    static TransportResult<std::shared_ptr<BaseTransport>> createTransport(
        const qsox::SocketAddress& address,
        ConnectionType type,
        const asp::time::Duration& timeout,
        const ClientTlsContext* tlsContext = nullptr
    );

    TransportResult<> sendHandshake();
    TransportResult<> waitForHandshakeResponse(asp::time::Duration timeout);
};

}
