#include <qunet/socket/transport/TcpTransport.hpp>
#include <qunet/buffers/HeapByteWriter.hpp>
#include <qunet/protocol/constants.hpp>
#include <qunet/Connection.hpp>
#include <qunet/Log.hpp>
#include "Common.hpp"

#include <qsox/Poll.hpp>
#include <arc/time/Timeout.hpp>

#define MAP_UNWRAP(x) GEODE_UNWRAP((x).mapErr([](const auto& err) { return TransportError::EncodingFailed; }))

using namespace arc;
using qsox::SocketAddress;
using namespace asp::time;

namespace qn {

TcpTransport::TcpTransport(TcpStream socket) : m_socket(std::move(socket)), m_recvBuffer(512) {}

TcpTransport::~TcpTransport() {}

Future<NetResult<TcpTransport>> TcpTransport::connect(const SocketAddress& address, const Duration& timeout, const ConnectionOptions& connOptions) {
    auto result = ARC_CO_UNWRAP((co_await arc::timeout(
        timeout,
        TcpStream::connect(address)
    )).mapErr([](const auto&) { return qsox::Error::TimedOut; }));

    auto socket = ARC_CO_UNWRAP(std::move(result));

    TcpTransport ret(std::move(socket));
    ret.m_activeKeepaliveInterval = connOptions.activeKeepaliveInterval;

    co_return Ok(std::move(ret));
}

TransportResult<> TcpTransport::closeSync() {
    auto& stream = m_socket.inner();
    ARC_UNWRAP(stream.shutdown(qsox::ShutdownMode::Both));
    m_closed = true;
    return Ok();
}

bool TcpTransport::isClosed() const {
    return m_closed;
}

Future<TransportResult<>> TcpTransport::sendMessage(QunetMessage message, SentMessageContext& ctx) {
    this->updateLastActivity();

    if (message.is<KeepaliveMessage>()) {
        message.as<KeepaliveMessage>().timestamp = this->getKeepaliveTimestamp();
        this->updateLastKeepalive();
    }

    return streamcommon::sendMessage(std::move(message), m_socket, *this, ctx);
}

Future<TransportResult<>> TcpTransport::poll() {
    ARC_CO_UNWRAP(co_await m_socket.pollReadable());
    co_return Ok();
}

Future<TransportResult<QunetMessage>> TcpTransport::receiveMessage() {
    return streamcommon::receiveMessage(
        m_socket, *this, m_recvBuffer, m_messageSizeLimit, m_unackedKeepalives
    );
}

asp::time::Duration TcpTransport::untilTimerExpiry() const {
    return this->untilKeepalive();
}

Future<TransportResult<>> TcpTransport::handleTimerExpiry() {
    if (m_unackedKeepalives >= 3) {
        co_return Err(TransportError::TimedOut);
    }

    co_return co_await this->sendMessage(KeepaliveMessage{});
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
