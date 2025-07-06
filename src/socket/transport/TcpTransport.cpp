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

TcpTransport::TcpTransport(qsox::TcpStream socket) : m_socket(std::move(socket)), m_recvBuffer(512) {}

TcpTransport::~TcpTransport() {}

NetResult<TcpTransport> TcpTransport::connect(const SocketAddress& address, const Duration& timeout) {
    auto socket = GEODE_UNWRAP(TcpStream::connect(address, timeout.millis()));

    return Ok(TcpTransport(std::move(socket)));
}

TransportResult<> TcpTransport::close() {
    GEODE_UNWRAP(m_socket.shutdown(ShutdownMode::Both));

    m_closed = true;

    return Ok();
}

bool TcpTransport::isClosed() const {
    return m_closed;
}

TransportResult<> TcpTransport::sendMessage(QunetMessage message) {
    return streamcommon::sendMessage(std::move(message), m_socket);
}

TransportResult<bool> TcpTransport::poll(const std::optional<Duration>& dur) {
    int timeout = dur ? dur->millis() : -1;

    auto res = GEODE_UNWRAP(qsox::pollOne(m_socket, PollType::Read, timeout));

    return Ok(res == PollResult::Readable);
}

TransportResult<bool> TcpTransport::processIncomingData() {
    GEODE_UNWRAP(streamcommon::processIncomingData(
        m_socket, m_recvBuffer, m_messageSizeLimit, m_recvMsgQueue
    ));

    return Ok(!m_recvMsgQueue.empty());
}

}
