#include "QuicConnection.hpp"
#include <qunet/Log.hpp>
#include <qunet/util/Error.hpp>
#include <qunet/protocol/constants.hpp>

#include <ngtcp2/ngtcp2_crypto.h>
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/logging.h>
#include <qsox/Poll.hpp>
#include <asp/time/Duration.hpp>
#include <asp/time/Instant.hpp>
#include <asp/time/sleep.hpp>
#include <random>

using namespace asp::time;
using namespace qsox;

static bool secureRandom(uint8_t* buf, size_t len) {
    thread_local WC_RNG rng;
    thread_local bool initialized = false;

    if (!initialized) {
        if (wc_InitRng(&rng) != 0) {
            qn::log::error("Failed to initialize random number generator");
            return false;
        }

        initialized = true;
    }

    if (wc_RNG_GenerateBlock(&rng, buf, len) != 0) {
        qn::log::error("Failed to generate random bytes");
        return false;
    }

    return true;
}

static void fillRandom(uint8_t* dest, size_t len) {
    if (!secureRandom(dest, len)) {
        // fallback to a less secure method
        static std::random_device rd;
        static std::mt19937_64 generator(rd());

        while (len > 8) {
            uint64_t value = generator();
            std::memcpy(dest, &value, sizeof(value));
            dest += sizeof(value);
            len -= sizeof(value);
        }

        // less than 8 bytes left
        while (len > 0) {
            uint8_t value = static_cast<uint8_t>(generator() % 256);
            *dest++ = value;
            --len;
        }
    }
}

template <std::integral T>
static T fillRandom() {
    T value;

    fillRandom(reinterpret_cast<uint8_t*>(&value), sizeof(T));

    return value;
}

namespace qn {

bool QuicError::ok() const {
    return code == 0;
}

std::string_view QuicError::message() const {
    return ngtcp2_strerror(code);
}

QuicConnection::QuicConnection(ngtcp2_conn* conn)
    : m_conn(conn),
    m_connRef(ngtcp2_crypto_conn_ref {
        .get_conn = [](ngtcp2_crypto_conn_ref* ref) -> ngtcp2_conn* {
            auto* conn = static_cast<QuicConnection*>(ref->user_data);
            return conn->rawHandle();
        },
        .user_data = this,
}) {}

QuicConnection::~QuicConnection() {
    if (m_conn) {
        ngtcp2_conn_del(m_conn);
    }
}

static void logQuic(const char* format, va_list args) {
    va_list argsCopy;
    va_copy(argsCopy, args);

    int len = std::vsnprintf(nullptr, 0, format, argsCopy);
    va_end(argsCopy);

    std::string buffer(len, '\0');
    std::vsnprintf(buffer.data(), buffer.size() + 1, format, args);

    qn::log::info("(ngtcp2) {}", buffer);
}

static void logPrintfCallback(void*, const char* format, ...) {
#ifdef QUNET_DEBUG
    // use vsnprintf to format the message
    va_list args;
    va_start(args, format);

    logQuic(format, args);

    va_end(args);
#endif
}

ngtcp2_conn* QuicConnection::rawHandle() const {
    return m_conn;
}

ngtcp2_crypto_conn_ref* QuicConnection::connRef() const {
    return const_cast<ngtcp2_crypto_conn_ref*>(&m_connRef);
}

TransportResult<std::unique_ptr<QuicConnection>> QuicConnection::connect(
    const SocketAddress& address,
    const Duration& timeout,
    const ClientTlsContext* tlsContext
) {
    QN_ASSERT(tlsContext != nullptr && "TLS context must not be null");

    // wolfSSL_Debugging_ON();

    // Create the connection to return
    std::unique_ptr<QuicConnection> ret{new QuicConnection(nullptr)};

    // Create the UDP socket, connect to the server
    ret->m_socket = GEODE_UNWRAP(UdpSocket::bindAny(address.isV6()));
    GEODE_UNWRAP(ret->m_socket->connect(address));

    // Initialize settings
    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.log_printf = &logPrintfCallback;
    settings.cc_algo = NGTCP2_CC_ALGO_CUBIC;
    settings.initial_ts = timestamp();
    settings.handshake_timeout = timeout.nanos();
    settings.initial_pkt_num = fillRandom<decltype(settings.initial_pkt_num)>();
    if (settings.initial_pkt_num > INT32_MAX) {
        settings.initial_pkt_num -= INT32_MAX;
    }

    // Initialize transport parameters
    ngtcp2_transport_params params;
    ngtcp2_transport_params_default(&params);
    params.initial_max_stream_data_bidi_local = 1024 * 1024 * 8; // 8 MiB
    params.initial_max_stream_data_bidi_remote = 1024 * 1024 * 8; // 8 MiB
    params.initial_max_stream_data_uni = 1024 * 1024 * 8; // 8 MiB
    params.initial_max_data = 1024 * 1024 * 16; // 16 MiB
    params.initial_max_streams_bidi = 2; // we only need 1 stream really
    params.initial_max_streams_uni = 1;
    params.max_idle_timeout = Duration::fromSecs(60).nanos();
    params.active_connection_id_limit = 4; // idk?
    params.grease_quic_bit = 1;

    // Initialize connection IDs
    ngtcp2_cid scid, dcid;
    scid.datalen = 17; // value used by ngtcp2 example
    fillRandom(scid.data, scid.datalen);
    dcid.datalen = 18; // ditto
    fillRandom(dcid.data, dcid.datalen);

    // Initialize callbacks
    ngtcp2_callbacks callbacks {
        .client_initial = &ngtcp2_crypto_client_initial_cb,
        .recv_crypto_data = &ngtcp2_crypto_recv_crypto_data_cb,
        .encrypt = &ngtcp2_crypto_encrypt_cb,
        .decrypt = &ngtcp2_crypto_decrypt_cb,
        .hp_mask = &ngtcp2_crypto_hp_mask_cb,
        .recv_stream_data = [](ngtcp2_conn* conn, uint32_t flags,
                                int64_t stream_id, uint64_t offset,
                                const uint8_t* data, size_t datalen,
                                void* user_data, void* stream_user_data
        ) {
            auto* quicConn = static_cast<QuicConnection*>(user_data);
            QN_ASSERT(stream_id == quicConn->m_mainStream->id() && "Stream ID must match the main stream ID");

            auto res = quicConn->m_mainStream->deliverToRecvBuffer(data, datalen);
            if (!res) {
                log::warn("Failed to deliver data to QUIC stream: {}", res.unwrapErr().message());
                return -1;
            }

            return 0;
        },
        .acked_stream_data_offset = [](ngtcp2_conn* conn, int64_t stream_id, uint64_t offset, uint64_t datalen, void* user_data, void* stream_user_data) {
            auto* quicConn = static_cast<QuicConnection*>(user_data);
            QN_ASSERT(stream_id == quicConn->m_mainStream->id() && "Stream ID must match the main stream ID");

            quicConn->m_mainStream->onAck(offset, datalen);

            return 0;
        },
        .recv_retry = &ngtcp2_crypto_recv_retry_cb,
        .rand = [](uint8_t* dest, size_t destlen, const ngtcp2_rand_ctx* rand_ctx) {
            fillRandom(dest, destlen);
        },
        .get_new_connection_id = [](ngtcp2_conn* conn, ngtcp2_cid* cid, uint8_t* token, size_t cidlen, void* user_data) {
            fillRandom(cid->data, cidlen);
            cid->datalen = cidlen;

            fillRandom(token, NGTCP2_STATELESS_RESET_TOKENLEN);

            return 0;
        },
        .update_key = ngtcp2_crypto_update_key_cb,
        .delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb,
        .delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
        .get_path_challenge_data = ngtcp2_crypto_get_path_challenge_data_cb,
        .version_negotiation = ngtcp2_crypto_version_negotiation_cb,
    };

    // Initialize path

    // TODO: idk if this is correct
    auto localAddr = GEODE_UNWRAP(ret->m_socket->localAddress());

    ngtcp2_path path {};
    sockaddr_storage localStorage, remoteStorage;

    if (localAddr.isV6()) {
        localAddr.toV6().toSockAddr((sockaddr_in6&) localStorage);
        address.toV6().toSockAddr((sockaddr_in6&) remoteStorage);

        path.local.addrlen = sizeof(sockaddr_in6);
        path.remote.addrlen = sizeof(sockaddr_in6);
    } else {
        localAddr.toV4().toSockAddr((sockaddr_in&) localStorage);
        address.toV4().toSockAddr((sockaddr_in&) remoteStorage);

        path.local.addrlen = sizeof(sockaddr_in);
        path.remote.addrlen = sizeof(sockaddr_in);
    }

    path.local.addr = (sockaddr*)&localStorage;
    path.remote.addr = (sockaddr*)&remoteStorage;

    // Initialize the ngtcp2 connection

    QuicError err = ngtcp2_conn_client_new(
        &ret->m_conn, &dcid, &scid, &path, NGTCP2_PROTO_VER_V1, &callbacks, &settings, &params, nullptr, ret.get()
    );

    if (!err.ok()) {
        ret->m_conn = nullptr;
        return Err(err);
    }

    // Create the TLS session
    ret->m_tlsSession = GEODE_UNWRAP(ClientTlsSession::create(
        *tlsContext, address, ret.get(), "localhost" // TODO: use actual server name
    ));

    ngtcp2_conn_set_tls_native_handle(ret->m_conn, ret->m_tlsSession->nativeHandle());

    // Wait for the QUIC handshake to complete
    GEODE_UNWRAP(ret->performHandshake(timeout));

    // Open the main stream
    ret->m_mainStream = GEODE_UNWRAP(ret->openBidiStream());

    // done!! :)

    return Ok(std::move(ret));
}

TransportResult<> QuicConnection::performHandshake(const asp::time::Duration& timeout) {
    auto startedAt = Instant::now();

    ngtcp2_path_storage_zero(&m_networkPath);

    ngtcp2_pkt_info pi{};
    uint8_t outBuf[1500];

    size_t nwrite = ngtcp2_conn_write_pkt(m_conn, &m_networkPath.path, &pi, outBuf, sizeof(outBuf), timestamp());
    if (nwrite > 0) {
        log::debug("QUIC handshake packet written, size: {}", nwrite);

        GEODE_UNWRAP(m_socket->send(outBuf, nwrite));
    }

    ngtcp2_conn_update_pkt_tx_time(m_conn, timestamp());

    while (ngtcp2_conn_get_handshake_completed(m_conn) == 0) {
        auto remaining = timeout - startedAt.elapsed();

        if (remaining.isZero()) {
            return Err(TransportError::ConnectionTimedOut);
        }

        bool has = GEODE_UNWRAP(this->pollReadable(remaining));
        if (!has) {
            continue;
        }

        // read the handshake response, it may be multiple packets (?)
        GEODE_UNWRAP(this->doRecv());
    }

    log::debug("QUIC handshake completed");

    return Ok();
}

QuicResult<QuicStream> QuicConnection::openBidiStream() {
    int64_t streamId = -1;
    QuicError err = ngtcp2_conn_open_bidi_stream(m_conn, &streamId, this);
    if (!err.ok()) {
        return Err(err);
    }

    QN_ASSERT(streamId != -1 && "Stream ID must not be -1");

    return Ok(QuicStream(this, streamId));
}

TransportResult<bool> QuicConnection::pollReadable(const asp::time::Duration& dur) {
    auto res = GEODE_UNWRAP(qsox::pollOne(*m_socket, PollType::Read, dur.millis()));

    return Ok(res == PollResult::Readable);
}

TransportResult<> QuicConnection::waitUntilWritable(int64_t streamId) {
    log::warn("I don't know what to do, I am sleeping! (stream blocked due to control flow!)");
    asp::time::sleep(Duration::fromMillis(100));
    return Ok();
}

TransportResult<> QuicConnection::waitForBufferSpace(int64_t streamId) {
    log::warn("I don't know what to do, I am sleeping! (no space in buffer!)");
    asp::time::sleep(Duration::fromMillis(100));
    return Ok();
}

TransportResult<size_t> QuicConnection::send(const uint8_t* data, size_t len) {
    if (len == 0) {
        return Ok(0); // nothing to send
    }

    len = GEODE_UNWRAP(m_mainStream->write(data, len));

    if (len == 0) {
        return Err(TransportError::NoBufferSpace);
    }

    while (true) {
        auto res = m_mainStream->tryFlush();

        if (res) {
            if (res.unwrap() == 0) {
                break;
            } else {
                // continue flushing
                continue;
            }
        } else {
            auto err = res.unwrapErr();

            if (std::get<QuicError>(err.m_kind).code == NGTCP2_ERR_STREAM_DATA_BLOCKED) {
                // stream is blocked, wait a little and try again
                GEODE_UNWRAP(this->waitUntilWritable(m_mainStream->id()));
            } else {
                // some other error occurred
                return Err(err);
            }
        }
    }

    return Ok(len);
}

TransportResult<> QuicConnection::sendAll(const uint8_t* data, size_t len) {
    while (len) {
        auto res = this->send(data, len);
        if (!res) {
            auto err = res.unwrapErr();
            if (std::get<TransportError::CustomKind>(err.m_kind).code == TransportError::NoBufferSpace) {
                GEODE_UNWRAP(this->waitForBufferSpace(m_mainStream->id()));
            } else {
                return Err(err);
            }
        }

        size_t written = res.unwrap();
        data += written;
        len -= written;
    }

    return Ok();
}

TransportResult<size_t> QuicConnection::receive(uint8_t* buffer, size_t len) {
    while (m_mainStream->toReceive() == 0){
        bool res = GEODE_UNWRAP(this->pollReadable(Duration::fromSecs(1)));

        if (res) {
            GEODE_UNWRAP(this->doRecv());
        }
    }

    return m_mainStream->read(buffer, len);
}

TransportResult<> QuicConnection::doRecv() {
    ngtcp2_pkt_info pi{};
    uint8_t outBuf[1500];

    size_t recvRes = GEODE_UNWRAP(m_socket->recv(outBuf, sizeof(outBuf)));
    log::debug("Read {} bytes from the QUIC socket", recvRes);

    QuicError res = ngtcp2_conn_read_pkt(m_conn, &m_networkPath.path, &pi, outBuf, recvRes, timestamp());
    if (!res.ok()) {
        log::warn("Failed to read QUIC packet: {}", res.message());
        if (res.code == NGTCP2_ERR_CRYPTO) {
            auto tlsErr = m_tlsSession->lastError();
            log::warn("Last TLS error: {}", tlsErr.message());
            return Err(tlsErr);
        }

        return Err(res);
    }

    return Ok();
}


}