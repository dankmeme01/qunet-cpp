#ifdef QUNET_QUIC_SUPPORT

#include <qunet/socket/transport/QuicTransport.hpp>
#include <qunet/Connection.hpp>
#include "quic/QuicConnection.hpp"
#include "Common.hpp"

#define MAP_UNWRAP(x) GEODE_UNWRAP((x).mapErr([](const auto& err) { return TransportError::EncodingFailed; }))

using namespace arc;
using namespace asp::time;

namespace qn {

QuicTransport::QuicTransport(std::shared_ptr<QuicConnection> conn) : m_conn(std::move(conn)), m_recvBuffer(512) {}

QuicTransport::QuicTransport(QuicTransport&&) = default;

QuicTransport& QuicTransport::operator=(QuicTransport&&) = default;

QuicTransport::~QuicTransport() {
    if (m_conn) (void) m_conn->closeSync();
}

Future<TransportResult<>> QuicTransport::close() {
    return m_conn->close();
}

TransportResult<> QuicTransport::closeSync() {
    return m_conn->closeSync();
}

bool QuicTransport::isClosed() const {
    return m_conn->isClosed();
}

Future<TransportResult<QuicTransport>> QuicTransport::connect(
    const qsox::SocketAddress& address,
    const Duration& timeout,
    const ClientTlsContext* tlsContext,
    const ConnectionOptions* connOptions,
    const std::string& hostname
) {
    auto conn = ARC_CO_UNWRAP(co_await QuicConnection::connect(
        address, timeout, tlsContext, connOptions, hostname
    ));

    QN_DEBUG_ASSERT(conn != nullptr);

    co_return Ok(QuicTransport(std::move(conn)));
}

Future<TransportResult<>> QuicTransport::sendMessage(QunetMessage message, bool reliable) {
    return streamcommon::sendMessage(std::move(message), *m_conn, *this);
}

Future<TransportResult<>> QuicTransport::poll() {
    return m_conn->pollReadable();
}

Future<TransportResult<QunetMessage>> QuicTransport::receiveMessage() {
    return streamcommon::receiveMessage(
        *m_conn, *this, m_recvBuffer, m_messageSizeLimit, m_unackedKeepalives
    );
}

Duration QuicTransport::untilTimerExpiry() const  {
    return m_conn->untilTimerExpiry();
}

arc::Future<TransportResult<>> QuicTransport::handleTimerExpiry() {
    return m_conn->handleTimerExpiry();
}

class QuicConnection& QuicTransport::connection() {
    return *m_conn;
}

}

#endif
