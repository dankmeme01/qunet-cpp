#include <qunet/socket/transport/QuicTransport.hpp>
#include "quic/QuicConnection.hpp"
#include "Common.hpp"

#define MAP_UNWRAP(x) GEODE_UNWRAP((x).mapErr([](const auto& err) { return TransportError::EncodingFailed; }))

using namespace qsox;
using namespace asp::time;

namespace qn {

QuicTransport::QuicTransport(std::unique_ptr<QuicConnection> conn) : m_conn(std::move(conn)) {
    m_readBuffer.resize(512);
}

QuicTransport::QuicTransport(QuicTransport&&) = default;

QuicTransport& QuicTransport::operator=(QuicTransport&&) = default;

QuicTransport::~QuicTransport() {}

TransportResult<QuicTransport> QuicTransport::connect(
    const SocketAddress& address,
    const Duration& timeout,
    const ClientTlsContext* tlsContext
) {
    auto conn = GEODE_UNWRAP(QuicConnection::connect(address, timeout, tlsContext));

    QN_DEBUG_ASSERT(conn != nullptr);

    return Ok(QuicTransport(std::move(conn)));
}

TransportResult<> QuicTransport::sendMessage(QunetMessage message) {
    return streamcommon::sendMessage(std::move(message), *m_conn);
}

TransportResult<bool> QuicTransport::poll(const Duration& dur) {
    return Ok(GEODE_UNWRAP(m_conn->pollReadable(dur)));
}

// The logic here is pretty much taken from QuicTransport in the qunet server
TransportResult<QunetMessage> QuicTransport::receiveMessage() {
    return streamcommon::receiveMessage(*m_conn, m_readBuffer, m_readBufferPos, m_messageSizeLimit);
}

}
