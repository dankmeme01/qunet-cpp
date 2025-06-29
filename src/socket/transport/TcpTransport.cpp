#include <qunet/socket/transport/TcpTransport.hpp>
#include <qunet/buffers/HeapByteWriter.hpp>
#include <qunet/protocol/constants.hpp>
#include <qunet/Log.hpp>
#include "Common.hpp"

#include <qsox/Poll.hpp>

#define MAP_UNWRAP(x) GEODE_UNWRAP((x).mapErr([](const auto& err) { return TransportError::EncodingFailed; }))

using namespace qsox;
using namespace asp::time;

namespace qn {

TcpTransport::TcpTransport(qsox::TcpStream socket) : m_socket(std::move(socket)) {
    m_readBuffer.resize(512);
}

TcpTransport::~TcpTransport() {}

NetResult<TcpTransport> TcpTransport::connect(const SocketAddress& address, const Duration& timeout) {
    auto socket = GEODE_UNWRAP(TcpStream::connect(address, timeout.millis()));

    return Ok(TcpTransport(std::move(socket)));
}

TransportResult<> TcpTransport::sendMessage(QunetMessage message) {
    return streamcommon::sendMessage(std::move(message), m_socket);
}

TransportResult<bool> TcpTransport::poll(const Duration& dur) {
    auto res = GEODE_UNWRAP(qsox::pollOne(m_socket, PollType::Read, dur.millis()));

    return Ok(res == PollResult::Readable);
}

TransportResult<QunetMessage> TcpTransport::receiveMessage() {
    return streamcommon::receiveMessage(m_socket, m_readBuffer, m_readBufferPos, m_messageSizeLimit);
}

}
