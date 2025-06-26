#include <qunet/socket/Socket.hpp>
#include <qunet/socket/transport/UdpTransport.hpp>
#include <qunet/buffers/HeapByteWriter.hpp>
#include <qunet/protocol/constants.hpp>
#include <qunet/Log.hpp>

#include <asp/time/Instant.hpp>

using namespace qsox;
using namespace asp::time;

namespace qn {

TransportResult<Socket> Socket::connect(const qsox::SocketAddress& address, ConnectionType type, const Duration& timeout) {
    auto startedAt = Instant::now();

    auto transport = GEODE_UNWRAP(createTransport(address, type, timeout).mapErr([](const auto& err) {
        return TransportError::TransportCreationFailed;
    }));

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
        log::debug("Handshake finished, connection ID: {}", hf.connectionId);

        return Ok();
    } else if (msg.is<HandshakeFailureMessage>()) {
        return Err(TransportError::HandshakeFailed);
    } else {
        return Err(TransportError::InvalidMessage);
    }
}

NetResult<std::shared_ptr<BaseTransport>> Socket::createTransport(const SocketAddress& address, ConnectionType type, const Duration& timeout) {
    switch (type) {
        case ConnectionType::Udp: {
            auto transport = GEODE_UNWRAP(UdpTransport::connect(address));
            auto ptr = std::make_shared<UdpTransport>(std::move(transport));
            return Ok(std::static_pointer_cast<BaseTransport>(ptr));
        } break;

        default: {
            return Err(Error::Unimplemented);
        }
    }
}

}
