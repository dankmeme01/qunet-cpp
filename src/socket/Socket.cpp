#include <qunet/socket/Socket.hpp>
#include <qunet/socket/transport/UdpTransport.hpp>
#include <qunet/socket/transport/TcpTransport.hpp>
#include <qunet/socket/transport/QuicTransport.hpp>
#include <qunet/buffers/HeapByteWriter.hpp>
#include <qunet/protocol/constants.hpp>
#include <qunet/Log.hpp>

#include <asp/time/Instant.hpp>

using namespace qsox;
using namespace asp::time;

namespace qn {

TransportResult<Socket> Socket::connect(
    const qsox::SocketAddress& address,
    ConnectionType type,
    const Duration& timeout,
    const ClientTlsContext* tlsContext
) {
    auto startedAt = Instant::now();

    auto transport = GEODE_UNWRAP(createTransport(address, type, timeout, tlsContext));

    if (startedAt.elapsed() > timeout) {
        return Err(TransportError::ConnectionTimedOut);
    }

    Socket socket(std::move(transport));

    GEODE_UNWRAP(socket.sendHandshake());

    auto handshakeTimeout = timeout - startedAt.elapsed();
    if (handshakeTimeout.millis() <= 0) {
        return Err(TransportError::ConnectionTimedOut);
    }

    GEODE_UNWRAP(socket.waitForHandshakeResponse(handshakeTimeout));

    return Ok(std::move(socket));
}

TransportResult<> Socket::sendHandshake() {
    return m_transport->sendMessage(HandshakeStartMessage {
        .majorVersion = MAJOR_VERSION,
        .fragLimit = UDP_PACKET_LIMIT,
        // TODO: qdb hash
        .qdbHash = std::array<uint8_t, 16>{}
    });
}

TransportResult<> Socket::waitForHandshakeResponse(Duration timeout) {
    bool res = GEODE_UNWRAP(m_transport->poll(timeout));
    if (!res) {
        return Err(TransportError::ConnectionTimedOut);
    }

    auto msg = GEODE_UNWRAP(m_transport->receiveMessage());

    if (msg.is<HandshakeFinishMessage>()) {
        auto& hf = msg.as<HandshakeFinishMessage>();
        log::debug("Handshake finished, connection ID: {}, qdb size: {}", hf.connectionId, hf.qdbData ? hf.qdbData->uncompressedSize : 0);

        m_transport->setConnectionId(hf.connectionId);

        return Ok();
    } else if (msg.is<HandshakeFailureMessage>()) {
        auto& hf = msg.as<HandshakeFailureMessage>();
        log::warn("Handshake failed: {}", hf.message());

        return Err(TransportError::HandshakeFailure(hf.message()));
    } else {
        return Err(TransportError::UnexpectedMessage);
    }
}

TransportResult<std::shared_ptr<BaseTransport>> Socket::createTransport(
    const SocketAddress& address,
    ConnectionType type,
    const Duration& timeout,
    const ClientTlsContext* tlsContext
) {
    switch (type) {
        case ConnectionType::Udp: {
            auto transport = GEODE_UNWRAP(UdpTransport::connect(address));
            auto ptr = std::make_shared<UdpTransport>(std::move(transport));
            return Ok(std::static_pointer_cast<BaseTransport>(ptr));
        } break;

        case ConnectionType::Tcp: {
            auto transport = GEODE_UNWRAP(TcpTransport::connect(address, timeout));
            auto ptr = std::make_shared<TcpTransport>(std::move(transport));
            return Ok(std::static_pointer_cast<BaseTransport>(ptr));
        } break;

        case ConnectionType::Quic: {
            auto transport = GEODE_UNWRAP(QuicTransport::connect(address, timeout, tlsContext));
            auto ptr = std::make_shared<QuicTransport>(std::move(transport));
            return Ok(std::static_pointer_cast<BaseTransport>(ptr));
        } break;

        default: {
            return Err(TransportError::NotImplemented);
        }
    }
}

}
