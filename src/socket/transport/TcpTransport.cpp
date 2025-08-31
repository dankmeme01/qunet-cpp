#include <qunet/socket/transport/TcpTransport.hpp>
#include <qunet/buffers/HeapByteWriter.hpp>
#include <qunet/protocol/constants.hpp>
#include <qunet/Connection.hpp>
#include <qunet/Log.hpp>
#include "Common.hpp"

#include <qsox/Poll.hpp>

#define MAP_UNWRAP(x) GEODE_UNWRAP((x).mapErr([](const auto& err) { return TransportError::EncodingFailed; }))

using namespace qsox;
using namespace asp::time;

namespace qn {

TcpTransport::TcpTransport(qsox::TcpStream socket) : m_socket(std::move(socket)), m_recvBuffer(512) {}

TcpTransport::~TcpTransport() {}

NetResult<TcpTransport> TcpTransport::connect(const SocketAddress& address, const Duration& timeout, const ConnectionOptions& connOptions) {
    auto socket = GEODE_UNWRAP(TcpStream::connect(address, timeout.millis()));

    TcpTransport ret(std::move(socket));
    ret.m_activeKeepaliveInterval = connOptions.activeKeepaliveInterval;

    return Ok(std::move(ret));
}

TransportResult<> TcpTransport::close() {
    GEODE_UNWRAP(m_socket.shutdown(ShutdownMode::Both));

    m_closed = true;

    return Ok();
}

bool TcpTransport::isClosed() const {
    return m_closed;
}

TransportResult<> TcpTransport::sendMessage(QunetMessage message, bool reliable) {
    this->updateLastActivity();

    if (message.is<KeepaliveMessage>()) {
        message.as<KeepaliveMessage>().timestamp = this->getKeepaliveTimestamp();
        this->updateLastKeepalive();
    }

    return streamcommon::sendMessage(std::move(message), m_socket);
}

TransportResult<bool> TcpTransport::poll(const std::optional<Duration>& dur) {
    int timeout = dur ? dur->millis() : -1;

    auto res = GEODE_UNWRAP(qsox::pollOne(m_socket, PollType::Read, timeout));

    return Ok(res == PollResult::Readable);
}

TransportResult<bool> TcpTransport::processIncomingData() {
    GEODE_UNWRAP(streamcommon::processIncomingData(
        m_socket, *this, m_recvBuffer, m_messageSizeLimit, m_recvMsgQueue, m_unackedKeepalives
    ));

    return Ok(!m_recvMsgQueue.empty());
}

asp::time::Duration TcpTransport::untilTimerExpiry() const {
    return this->untilKeepalive();
}

TransportResult<> TcpTransport::handleTimerExpiry() {
    if (m_unackedKeepalives >= 3) {
        return Err(TransportError::TimedOut);
    }

    return this->sendMessage(KeepaliveMessage{}, false);
}

Duration TcpTransport::untilKeepalive() const {
    // similar to UDP, we send more keepalives at the start to figure out the latency
    if (m_unackedKeepalives > 0) {
        return Duration::fromSecs(3) - this->sinceLastActivity();
    }

    auto orActive = [&](const Duration& dur) {
        if (m_activeKeepaliveInterval) {
            return std::min(dur, *m_activeKeepaliveInterval) - this->sinceLastKeepalive();
        } else {
            return dur - this->sinceLastKeepalive();
        }
    };

    switch (m_totalKeepalives) {
        case 0:
        case 1:
            return orActive(Duration::fromSecs(3));
        case 2:
            return orActive(Duration::fromSecs(10));
        case 3:
            return orActive(Duration::fromSecs(25));
        default: {
            if (m_activeKeepaliveInterval) {
                return std::min(
                    Duration::fromSecs(45) - this->sinceLastActivity(),
                    *m_activeKeepaliveInterval - this->sinceLastKeepalive()
                );
            } else {
                return Duration::fromSecs(45) - this->sinceLastActivity();
            }
        }
    }
}

}
