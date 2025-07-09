#include <qunet/socket/transport/QuicTransport.hpp>
#include <qunet/Connection.hpp>
#include "quic/QuicConnection.hpp"
#include "Common.hpp"

#define MAP_UNWRAP(x) GEODE_UNWRAP((x).mapErr([](const auto& err) { return TransportError::EncodingFailed; }))

using namespace qsox;
using namespace asp::time;

namespace qn {

QuicTransport::QuicTransport(std::unique_ptr<QuicConnection> conn) : m_conn(std::move(conn)), m_recvBuffer(512) {}

QuicTransport::QuicTransport(QuicTransport&&) = default;

QuicTransport& QuicTransport::operator=(QuicTransport&&) = default;

QuicTransport::~QuicTransport() {}

TransportResult<> QuicTransport::close() {
    return m_conn->close();
}

bool QuicTransport::isClosed() const {
    return m_conn->finishedClosing();
}

TransportResult<QuicTransport> QuicTransport::connect(
    const SocketAddress& address,
    const Duration& timeout,
    const ClientTlsContext* tlsContext,
    const ConnectionDebugOptions* debugOptions
) {
    auto conn = GEODE_UNWRAP(QuicConnection::connect(
        address, timeout, tlsContext, debugOptions
    ));

    QN_DEBUG_ASSERT(conn != nullptr);

    return Ok(QuicTransport(std::move(conn)));
}

TransportResult<> QuicTransport::sendMessage(QunetMessage message, bool reliable) {
    return streamcommon::sendMessage(std::move(message), *m_conn);
}

TransportResult<bool> QuicTransport::poll(const std::optional<Duration>& dur) {
    return m_conn->pollReadable(dur);
}

TransportResult<bool> QuicTransport::processIncomingData() {
    GEODE_UNWRAP(streamcommon::processIncomingData(
        *m_conn, *this, m_recvBuffer, m_messageSizeLimit, m_recvMsgQueue
    ));

    // TODO: temporary
    auto stats = m_conn->connStats();
    log::debug("QUIC: total sent: {}, total received: {}, total data sent: {}, total data received: {}",
        stats.totalSent, stats.totalReceived, stats.totalDataSent, stats.totalDataReceived);

    return Ok(!m_recvMsgQueue.empty());
}

class QuicConnection& QuicTransport::connection() {
    return *m_conn;
}

}
