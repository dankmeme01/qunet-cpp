#include <qunet/socket/transport/UdpTransport.hpp>
#include <qunet/buffers/HeapByteWriter.hpp>
#include <qunet/protocol/constants.hpp>
#include <qunet/Log.hpp>

#include <qsox/Poll.hpp>

#define MAP_UNWRAP(x) GEODE_UNWRAP((x).mapErr([](const auto& err) { return TransportError::EncodingFailed; }))

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

TransportResult<> UdpTransport::close()  {
    // udp, of course, does not have any cleanup
    m_closed = true;
    return Ok();
}

bool UdpTransport::isClosed() const {
    return m_closed;
}

TransportResult<> UdpTransport::sendMessage(QunetMessage message) {
    HeapByteWriter writer;

    GEODE_UNWRAP(message.encodeHeader(writer, m_connectionId));
    GEODE_UNWRAP(message.encode(writer));

    auto data = writer.written();
    auto cres = GEODE_UNWRAP(m_socket.send(data.data(), data.size()));

    return Ok();
}

TransportResult<bool> UdpTransport::poll(const std::optional<Duration>& dur) {
    int timeout = dur ? dur->millis() : -1;

    auto res = GEODE_UNWRAP(qsox::pollOne(m_socket, PollType::Read, timeout));

    return Ok(res == PollResult::Readable);
}

TransportResult<bool> UdpTransport::processIncomingData() {
    uint8_t buffer[UDP_PACKET_LIMIT];

    auto bytesRead = GEODE_UNWRAP(m_socket.recv(buffer, sizeof(buffer)));

    if (bytesRead == 0) {
        return Err(TransportError::ZeroLengthMessage);
    }

    ByteReader reader(buffer, bytesRead);
    auto msg = GEODE_UNWRAP(QunetMessage::decode(reader));

    m_recvMsgQueue.push(std::move(msg));

    return Ok(true);
}

}
