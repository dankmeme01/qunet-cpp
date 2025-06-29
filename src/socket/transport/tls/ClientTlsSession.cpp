#include "ClientTlsSession.hpp"
#include "../quic/QuicConnection.hpp"
#include <utility>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <ngtcp2/ngtcp2.h>

namespace qn {

ClientTlsSession::ClientTlsSession(ClientTlsSession&& other) {
    *this = std::move(other);
}

ClientTlsSession& ClientTlsSession::operator=(ClientTlsSession&& other) {
    if (this != &other) {
        m_ssl = other.m_ssl;
        other.m_ssl = nullptr;
    }

    return *this;
}

ClientTlsSession::ClientTlsSession(WOLFSSL* m_ssl) : m_ssl(m_ssl) {}

ClientTlsSession::~ClientTlsSession() {
    if (m_ssl) {
        wolfSSL_free(m_ssl);
    }
}

TlsResult<ClientTlsSession> ClientTlsSession::create(
    const ClientTlsContext& context,
    const qsox::SocketAddress& address,
    QuicConnection* quicConn,
    const std::string& serverName
) {
    auto ctx = context.m_ctx;
    auto ssl = wolfSSL_new(ctx);

    if (!ssl) {
        return Err(lastError());
    }

    // can be used as a raii guard here
    ClientTlsSession session(ssl);

    TlsError err = wolfSSL_set_app_data(ssl, quicConn->connRef());
    if (!err.ok()) {
        return Err(err);
    }

    wolfSSL_set_connect_state(ssl);

    const uint8_t alpn[] = "\x06qunet1";
    err = wolfSSL_set_alpn_protos(ssl, alpn, sizeof(alpn) - 1);
    if (!err.ok()) {
        return Err(err);
    }

    if (!serverName.empty()) {
        TlsError err = wolfSSL_UseSNI(ssl, WOLFSSL_SNI_HOST_NAME, serverName.c_str(), serverName.size());
        if (!err.ok()) {
            return Err(err);
        }
    }

    // use quic v1
    wolfSSL_set_quic_transport_version(ssl, 0x39);

    // TODO: a bunch of scary session ticket stuff
    // https://github.com/ngtcp2/ngtcp2/blob/5fea5f0e4abd751a0c2686aca823aeb7554c5b5a/examples/tls_client_session_wolfssl.cc#L96

    return Ok(std::move(session));
}

void* ClientTlsSession::nativeHandle() const {
    return m_ssl;
}

TlsError ClientTlsSession::lastError() {
    return TlsError(wolfSSL_ERR_get_error());
}

}