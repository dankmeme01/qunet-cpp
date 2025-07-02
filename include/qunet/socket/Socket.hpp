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

struct TransportOptions {
    qsox::SocketAddress address;
    ConnectionType type;
    asp::time::Duration timeout;
    const struct ConnectionDebugOptions* debugOptions = nullptr;
    const ClientTlsContext* tlsContext = nullptr;
};

class Socket {
public:
    // Attempts to connect to the specified address using the given connection type.
    static TransportResult<Socket> connect(const TransportOptions& options);

    // Closes the transport. This does not send a `ClientClose` message. This may or may not block - see notes in BaseTransport::close.
    TransportResult<> close();

    // Returns true if `close()` was called and the transport finished closing.
    bool isClosed() const;

    TransportResult<> sendMessage(const QunetMessage& message);
    TransportResult<QunetMessage> receiveMessage(const asp::time::Duration& timeout);

private:
    std::shared_ptr<BaseTransport> m_transport;

    Socket(std::shared_ptr<BaseTransport> transport) : m_transport(std::move(transport)) {}

    static TransportResult<std::shared_ptr<BaseTransport>> createTransport(const TransportOptions& options);

    TransportResult<> sendHandshake();
    TransportResult<> waitForHandshakeResponse(asp::time::Duration timeout);
};

}
