#include <qunet/socket/transport/UdpTransport.hpp>

using namespace qsox;

namespace qn {

UdpTransport::UdpTransport(qsox::UdpSocket socket) : m_socket(std::move(socket)) {}

UdpTransport::~UdpTransport() {}

NetResult<UdpTransport> UdpTransport::connect(const SocketAddress& address) {
    auto socket = GEODE_UNWRAP(UdpSocket::bindAny(address.isV6()));
    GEODE_UNWRAP(socket.connect(address));

    return Ok(UdpTransport(std::move(socket)));
}

NetResult<> UdpTransport::sendMessage(std::span<const uint8_t> data) {
    auto count = GEODE_UNWRAP(m_socket.send(data.data(), data.size()));

    return Ok();
}

}
