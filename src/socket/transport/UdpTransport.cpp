#include <qunet/socket/transport/UdpTransport.hpp>
#include <qunet/buffers/HeapByteWriter.hpp>
#include <qunet/protocol/constants.hpp>
#include <qunet/Log.hpp>

#include <qsox/Poll.hpp>

using namespace qsox;
using namespace asp::time;

namespace qn {

UdpTransport::UdpTransport(qsox::UdpSocket socket) : m_socket(std::move(socket)) {}

UdpTransport::~UdpTransport() {}

NetResult<UdpTransport> UdpTransport::connect(const SocketAddress& address) {
    auto socket = GEODE_UNWRAP(UdpSocket::bindAny(address.isV6()));
    GEODE_UNWRAP(socket.connect(address));

    return Ok(UdpTransport(std::move(socket)));
}

TransportResult<> UdpTransport::sendMessage(QunetMessage message) {
    HeapByteWriter writer;
    auto res = message.encode(writer);
    if (!res) {
        log::warn("Failed to encode message: {}", res.unwrapErr().message());
        return Err(TransportError::EncodingFailed);
    }

    auto data = writer.written();
    auto cres = (m_socket.send(data.data(), data.size()));

    if (!cres) {
        auto err = cres.unwrapErr();
        return Err(this->makeError(err));
    }

    return Ok();
}

TransportResult<bool> UdpTransport::poll(const Duration& dur) {
    auto res = qsox::pollOne(m_socket.handle(), PollType::Read, dur.millis());
    if (!res) {
        return Err(this->makeError(res.unwrapErr()));
    }

    return Ok(res.unwrap() == PollResult::Readable);
}

TransportResult<QunetMessage> UdpTransport::receiveMessage() {
    uint8_t buffer[UDP_PACKET_LIMIT];

    auto res = m_socket.recv(buffer, sizeof(buffer));
    if (!res) {
        return Err(this->makeError(res.unwrapErr()));
    }

    auto bytesRead = res.unwrap();
    if (bytesRead == 0) {
        return Err(TransportError::DecodingFailed);
    }

    ByteReader reader(buffer, bytesRead);
    auto msgRes = QunetMessage::decode(reader);
    if (!msgRes) {
        return Err(TransportError::DecodingFailed);
    }

    auto msg = std::move(msgRes).unwrap();
    return Ok(std::move(msg));
}

}
