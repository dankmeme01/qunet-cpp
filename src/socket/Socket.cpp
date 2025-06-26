#include <qunet/socket/Socket.hpp>
#include <qunet/socket/transport/UdpTransport.hpp>
#include <qunet/buffers/HeapByteWriter.hpp>
#include <qunet/protocol/constants.hpp>

using namespace qsox;
using namespace asp::time;

namespace qn {

NetResult<Socket> Socket::connect(const qsox::SocketAddress& address, ConnectionType type, const Duration& timeout) {
    auto transport = GEODE_UNWRAP(createTransport(address, type, timeout));

    Socket socket(std::move(transport));

    GEODE_UNWRAP(socket.sendHandshake());

    return Ok(std::move(socket));
}

NetResult<> Socket::sendHandshake() {
    HeapByteWriter writer;
    writer.writeU8(MSG_HANDSHAKE_START);
    writer.writeU16(MAJOR_VERSION);

    // TODO: fragmentation limit, make it configurable
    writer.writeU16(UDP_PACKET_LIMIT);

    // TODO: put qdb hash here
    writer.writeZeroes(16);

    auto data = writer.written();

    return m_transport->sendMessage(data);
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
