#include "QuicConnection.hpp"
#include <qunet/Connection.hpp>
#include <qunet/protocol/constants.hpp>
#include <qunet/util/Error.hpp>
#include <qunet/util/rng.hpp>
#include <qunet/Log.hpp>

#include <ngtcp2/ngtcp2_crypto.h>
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/logging.h>
#include <arc/time/Timeout.hpp>
#include <asp/time/Instant.hpp>
#include <random>

using namespace asp::time;
using namespace arc;


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
        static thread_local std::mt19937_64 generator(std::random_device{}());

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

static void logQuic(const char* format, va_list args) {
    va_list argsCopy;
    va_copy(argsCopy, args);

    int len = std::vsnprintf(nullptr, 0, format, argsCopy);
    va_end(argsCopy);

    std::string buffer(len, '\0');
    std::vsnprintf(buffer.data(), buffer.size() + 1, format, args);

    qn::log::debug("(ngtcp2) {}", buffer);
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

static void initPathStorage(
    ngtcp2_path_storage& storage,
    const qsox::SocketAddress& local,
    const qsox::SocketAddress& remote
) {
    ngtcp2_path_storage_zero(&storage);

    if (local.isV4()) {
        local.toV4().toSockAddr(storage.local_addrbuf.in);
    } else {
        local.toV6().toSockAddr(storage.local_addrbuf.in6);
    }

    if (remote.isV4()) {
        remote.toV4().toSockAddr(storage.remote_addrbuf.in);
    } else {
        remote.toV6().toSockAddr(storage.remote_addrbuf.in6);
    }
}

namespace qn {

bool QuicError::ok() const {
    return code == 0;
}

std::string_view QuicError::message() const {
    return ngtcp2_strerror(code);
}


static bool isCongestionRelatedError(const TransportError& err) {
    if (std::holds_alternative<QuicError>(err.m_kind)) {
        auto& quicErr = std::get<QuicError>(err.m_kind);
        return quicErr.code == NGTCP2_ERR_STREAM_DATA_BLOCKED ||
               quicErr.code == NGTCP2_ERR_FLOW_CONTROL;
    } else if (std::holds_alternative<TransportError::CustomKind>(err.m_kind)) {
        auto& customErr = std::get<TransportError::CustomKind>(err.m_kind);
        return customErr.code == TransportError::NoBufferSpace ||
               customErr.code == TransportError::CongestionLimited;
    }

    return false;
}

QuicConnection::QuicConnection(ngtcp2_conn* conn)
    : m_conn(conn),
      m_connRef(ngtcp2_crypto_conn_ref {
        .get_conn = [](ngtcp2_crypto_conn_ref* ref) -> ngtcp2_conn* {
            auto* conn = static_cast<QuicConnection*>(ref->user_data);
            return conn->rawHandle();
        },
        .user_data = this,
      })
{
}

QuicConnection::~QuicConnection() {
    ngtcp2_conn_del(m_conn);
}

ngtcp2_conn* QuicConnection::rawHandle() const {
    return m_conn;
}

ngtcp2_crypto_conn_ref* QuicConnection::connRef() const {
    return const_cast<ngtcp2_crypto_conn_ref*>(&m_connRef);
}

Future<TransportResult<std::unique_ptr<QuicConnection>>> QuicConnection::connect(
    const qsox::SocketAddress& address,
    const Duration& timeout,
    const ClientTlsContext* tlsContext,
    const ConnectionOptions* connOptions
) {
    QN_ASSERT(tlsContext != nullptr && "TLS context must not be null");
    auto debugOptions = connOptions ? &connOptions->debug : nullptr;

    // create the connection early to make use of raii
    std::unique_ptr<QuicConnection> ret(new QuicConnection(nullptr));
    ret->m_connectDeadline = Instant::now() + timeout;
    ret->m_nextExpiry = Instant::now();

    // set debug options
    if (debugOptions) {
        if (debugOptions->verboseSsl) {
            wolfSSL_SetLoggingCb([](int logLevel, const char* logMessage) {
                log::debug("(wolfSSL) {}", logMessage);
            });

            wolfSSL_Debugging_ON();
        } else {
            wolfSSL_Debugging_OFF();
        }

        if (debugOptions->packetLossSimulation != 0.0f) {
            ret->m_lossSimulation = debugOptions->packetLossSimulation;
        }
    }

    // create the new udp socket and connect it to the server
    ret->m_socket = ARC_CO_UNWRAP(co_await UdpSocket::bindAny(address.isV6()));
    ARC_CO_UNWRAP(ret->m_socket->connect(address));

    // initialize quic settings
    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.log_printf = (debugOptions && debugOptions->verboseQuic) ? &logPrintfCallback : nullptr;
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
            auto qc = static_cast<QuicConnection*>(user_data);
            auto it = qc->m_streams.find(stream_id);
            if (it == qc->m_streams.end()) {
                log::warn("Received data for unknown QUIC stream {}", stream_id);
                return 0;
            }

            auto& stream = it->second;
            stream.onReceivedData(data, datalen);

            return 0;
        },
        .acked_stream_data_offset = [](ngtcp2_conn* conn, int64_t stream_id, uint64_t offset, uint64_t datalen, void* user_data, void* stream_user_data) {
            auto qc = static_cast<QuicConnection*>(user_data);
            auto it = qc->m_streams.find(stream_id);
            if (it == qc->m_streams.end()) {
                log::warn("Received ack for unknown QUIC stream {}", stream_id);
                return 0;
            }

            auto& stream = it->second;
            stream.onAck(offset, datalen);

            return 0;
        },
        .stream_open = [](ngtcp2_conn* conn, int64_t stream_id, void*) {
            return 0;
        },
        .stream_close = [](ngtcp2_conn*, uint32_t flags, int64_t stream_id, uint64_t app_error_code, void* user_data, void*) {
            log::debug("QUIC stream {}: closed by peer (error code: {})", stream_id, app_error_code);
            auto qc = static_cast<QuicConnection*>(user_data);
            qc->m_streams.erase(stream_id);
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
        .stream_reset = [](ngtcp2_conn*, int64_t stream_id, uint64_t final_size, uint64_t app_error_code, void*, void*) {
            log::debug("QUIC stream {}: received reset (final size: {}, error code: {})", stream_id, final_size, app_error_code);
            return 0;
        },
    };

    // Initialize path
    auto localAddr = ARC_CO_UNWRAP(ret->m_socket->localAddress());

    ngtcp2_path_storage path{};
    initPathStorage(path, localAddr, address);

    // Initialize the ngtcp2 connection

    QuicError err = ngtcp2_conn_client_new(
        &ret->m_conn, &dcid, &scid, &path.path, NGTCP2_PROTO_VER_V1, &callbacks, &settings, &params, nullptr, ret.get()
    );

    if (!err.ok()) {
        co_return Err(err);
    }

    // Create the TLS session
    ret->m_tls = ARC_CO_UNWRAP(ClientTlsSession::create(
        *tlsContext, address, ret.get(), "localhost" // TODO: use actual server name
    ));

    ngtcp2_conn_set_tls_native_handle(ret->m_conn, ret->m_tls->nativeHandle());
    ngtcp2_conn_set_keep_alive_timeout(ret->m_conn, Duration::fromSecs(30).nanos());

    // Start the handshake and wait for it to complete
    ARC_CO_UNWRAP(co_await ret->performHandshake());

    // open the main stream
    ret->m_mainStreamId = ARC_CO_UNWRAP(ret->openStream());

    log::info("QUIC: connection is ready now!");

    co_return Ok(std::move(ret));
}

QuicResult<int64_t> QuicConnection::openStream() {
    int64_t id = -1;
    QuicError err = ngtcp2_conn_open_bidi_stream(m_conn, &id, this);
    if (!err.ok()) {
        return Err(err);
    }

    QN_ASSERT(id != -1);

    m_streams.emplace(id, QuicStream(this, id));

    return Ok(id);
}

TransportResult<> QuicConnection::closeStream(int64_t id) {
    auto it = m_streams.find(id);

    if (it == m_streams.end()) {
        log::warn("Attempting to close invalid QUIC stream {}", id);
        return Err(TransportError::InvalidArgument);
    }

    it->second.close();
    return Ok();
}

Future<TransportResult<>> QuicConnection::performHandshake() {
    ngtcp2_path_storage_zero(&m_networkPath);

    ARC_CO_UNWRAP(co_await this->sendHandshakePacket());

    while (ngtcp2_conn_get_handshake_completed(m_conn) == 0) {
        auto tres = co_await arc::timeoutAt(
            m_connectDeadline,
            this->receivePacket()
        );

        if (!tres) {
            co_return Err(TransportError::ConnectionTimedOut);
        }

        ARC_CO_UNWRAP(std::move(tres).unwrap());
    }

    log::debug("QUIC: handshake completed");

    co_return Ok();
}

TransportResult<size_t> QuicConnection::wrapWritePacket(uint8_t* buf, size_t size, bool handshake) {
    auto written = ngtcp2_conn_write_pkt(m_conn, handshake ? &m_networkPath.path : nullptr, nullptr, buf, size, timestamp());

    if (written < 0) {
        QuicError err(written);
        log::warn("QUIC: failed to write{} packet: {}", handshake ? " handshake" : "", err.message());
        return Err(err);
    }

    return Ok(written);
}

Future<TransportResult<>> QuicConnection::sendHandshakePacket() {
    uint8_t buf[1500];

    size_t written = ARC_CO_UNWRAP(this->wrapWritePacket(buf, sizeof(buf), true));
    QN_ASSERT(written != 0);

    co_return co_await this->sendPacket(buf, written);
}

Future<TransportResult<>> QuicConnection::sendNonStreamPacket() {
    uint8_t buf[1500];

    size_t written = ARC_CO_UNWRAP(this->wrapWritePacket(buf, sizeof(buf), false));

    if (written != 0) {
        ARC_CO_UNWRAP(co_await this->sendPacket(buf, written));
    }

    co_return Ok();
}

Future<TransportResult<>> QuicConnection::sendClosePacket() {
    uint8_t buf[1500];
    ngtcp2_pkt_info pi{};

    ngtcp2_ccerr ccerr {
        .type = NGTCP2_CCERR_TYPE_APPLICATION,
        .error_code = 1, // graceful closure
        .frame_type = 0,
        .reason = nullptr,
        .reasonlen = 0
    };

    auto written = ngtcp2_conn_write_connection_close(
        m_conn,
        &m_networkPath.path,
        nullptr,
        buf,
        sizeof(buf),
        &ccerr,
        timestamp()
    );

    if (written < 0) {
        co_return Err(QuicError(written));
    }

    QN_ASSERT(written > 0 && "close packet is 0 bytes");

    log::debug("QUIC: sending close packet");
    co_return co_await this->sendPacket(buf, written);
}

Future<TransportResult<>> QuicConnection::sendPacket(const uint8_t* buf, size_t size) {
    log::debug("QUIC: sending packet, size: {}", size);

    if (!this->shouldLosePacket()) {
        ARC_CO_UNWRAP(co_await m_socket->send(buf, size));
    }

    ngtcp2_conn_update_pkt_tx_time(m_conn, timestamp());

    m_lastSendAttempt = Instant::now();
    m_totalBytesSent.fetch_add(size, std::memory_order::relaxed);

    co_return Ok();
}

Future<TransportResult<>> QuicConnection::sendStreamData(QuicStream& stream, bool fin) {
    auto wrp = stream.peekUnsentData();
    if (wrp.size() == 0 && !fin) {
        co_return Ok();
    }

    uint8_t outBuf[1500];
    ngtcp2_pkt_info pi{};
    ngtcp2_ssize streamDataWritten = -1;

    uint32_t flags = 0;

    if (fin) {
        flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
        log::debug("QUIC stream {}: sending FIN packet", stream.m_streamId);
    } else {
        log::debug("QUIC stream {}: sending data packet ({} bytes)", stream.m_streamId, wrp.size());
    }

    ngtcp2_vec vecs[2];
    size_t vecCount = 0;

    if (wrp.first.size() > 0) {
        vecs[vecCount++] = ngtcp2_vec{const_cast<uint8_t*>(wrp.first.data()), wrp.first.size()};
    }

    if (wrp.second.size() > 0) {
        vecs[vecCount++] = ngtcp2_vec{const_cast<uint8_t*>(wrp.second.data()), wrp.second.size()};
    }

    auto written = ngtcp2_conn_writev_stream(
        m_conn,
        nullptr,
        &pi,
        outBuf,
        sizeof(outBuf),
        &streamDataWritten,
        flags,
        stream.m_streamId,
        vecs,
        vecCount,
        timestamp()
    );

    if (written < 0) {
        QuicError err(written);
        log::warn("QUIC stream {}: failed to write stream data: {}", stream.m_streamId, err.message());
        co_return Err(err);
    } else if (written == 0) {
        log::debug("QUIC stream {}: failed to write stream data due to congestion/flow control", stream.m_streamId);
        co_return Err(TransportError::CongestionLimited);
    }

    log::debug("QUIC stream {}: sending datagram size {} ({} stream bytes)", stream.m_streamId, written, streamDataWritten);

    auto res = co_await this->sendPacket(outBuf, written);
    if (res) {
        stream.advanceSentData(streamDataWritten);
    }

    co_return res;
}

Future<TransportResult<>> QuicConnection::receivePacket() {
    uint8_t outBuf[1500];

    size_t bytes = ARC_CO_UNWRAP(co_await m_socket->recv(outBuf, sizeof(outBuf)));
    log::debug("QUIC: read {} bytes from the socket", bytes);

    m_totalBytesReceived.fetch_add(bytes, std::memory_order::relaxed);

    QuicError res = ngtcp2_conn_read_pkt(m_conn, &m_networkPath.path, nullptr, outBuf, bytes, timestamp());
    if (res.ok()) co_return Ok();

    log::warn("QUIC: failed to read the packet: {}", res.message());
    if (res.code == NGTCP2_ERR_CRYPTO) {
        auto tlsErr = m_tls->lastError();
        log::warn("QUIC: last TLS error: {}", tlsErr.message());
        co_return Err(tlsErr);
    }
    co_return Err(res);
}

Future<TransportResult<>> QuicConnection::close() {
    m_closed = true;
    return this->sendClosePacket();
}

TransportResult<> QuicConnection::closeSync() {
    // don't do anything, just soft close
    m_closed = true;
    return Ok();
}

bool QuicConnection::isClosed() const {
    return m_closed;
}

Future<TransportResult<>> QuicConnection::pollReadable() {
    return this->pollReadable(m_mainStreamId);
}

Future<TransportResult<>> QuicConnection::pollReadable(int64_t streamId) {
    ARC_CO_UNWRAP_INTO(auto& stream, this->getStream(streamId));
    co_await stream.pollReadable();
    co_return Ok();
}

Future<TransportResult<>> QuicConnection::pollWritable() {
    return this->pollWritable(m_mainStreamId);
}

Future<TransportResult<>> QuicConnection::pollWritable(int64_t streamId) {
    ARC_CO_UNWRAP_INTO(auto& stream, this->getStream(streamId));
    co_await stream.pollWritable();
    co_return Ok();
}

Future<TransportResult<size_t>> QuicConnection::send(const void* data, size_t len) {
    return this->send(m_mainStreamId, data, len);
}

Future<TransportResult<size_t>> QuicConnection::send(int64_t streamId, const void* data, size_t len) {
    if (m_closed) {
        co_return Err(TransportError::Closed);
    }

    if (len == 0) {
        co_return Ok(0);
    }

    ARC_CO_UNWRAP_INTO(auto& stream, this->getStream(streamId));

    size_t written = 0;
    while (written == 0) {
        co_await stream.pollWritable();
        written = stream.write((const uint8_t*)data, len);
    }

    co_return Ok(written);
}

Future<TransportResult<>> QuicConnection::sendAll(const void* data, size_t len) {
    return this->sendAll(m_mainStreamId, data, len);
}

Future<TransportResult<>> QuicConnection::sendAll(int64_t streamId, const void* vdata, size_t len) {
    const uint8_t* data = static_cast<const uint8_t*>(vdata);

    while (len) {
        size_t bytes = ARC_CO_UNWRAP(co_await this->send(streamId, data, len));
        data += bytes;
        len -= bytes;
    }

    co_return Ok();
}

Future<TransportResult<size_t>> QuicConnection::receive(void* buf, size_t bufSize) {
    return this->receive(m_mainStreamId, buf, bufSize);
}

Future<TransportResult<size_t>> QuicConnection::receive(int64_t streamId, void* buf, size_t bufSize) {
    if (m_closed) {
        co_return Err(TransportError::Closed);
    }
    if (bufSize == 0) {
        co_return Ok(0);
    }

    ARC_CO_UNWRAP_INTO(auto& stream, this->getStream(streamId));

    while (true) {
        size_t read = stream.read(buf, bufSize);

        if (read > 0) {
            co_return Ok(read);
        } else {
            ARC_CO_UNWRAP(co_await this->receivePacket());
        }
    }
}

Duration QuicConnection::untilTimerExpiry() const {
    return m_nextExpiry.durationSince(Instant::now());
}

Future<TransportResult<>> QuicConnection::handleTimerExpiry() {
    auto now = Instant::now();
    auto ngtcp2Expiry = Instant::fromRawNanos(ngtcp2_conn_get_expiry(m_conn));
    auto nextExpiry = Instant::farFuture();

    // handle ngtcp2 expiry
    if (ngtcp2Expiry <= now) {
        auto code = ngtcp2_conn_handle_expiry(m_conn, now.rawNanos());
        ngtcp2Expiry = Instant::fromRawNanos(ngtcp2_conn_get_expiry(m_conn));

        if (code == NGTCP2_ERR_IDLE_CLOSE) {
            // TODO: maybe handle other way
            co_return Err(TransportError::ConnectionTimedOut);
        }

        nextExpiry = std::min(nextExpiry, ngtcp2Expiry);
    }

    // try to send any pending data
    for (auto& [id, stream] : m_streams) {
        if (stream.unflushedBytes() == 0) continue;

        ARC_CO_UNWRAP(co_await this->sendStreamData(stream));
    }

    // try to send a non-stream packet
    ARC_CO_UNWRAP(co_await this->sendNonStreamPacket());

    m_nextExpiry = nextExpiry;

    co_return Ok();
}

TransportResult<QuicStream&> QuicConnection::getStream(int64_t streamId) {
    auto it = m_streams.find(streamId);
    if (it == m_streams.end()) {
        log::warn("Attempting to get invalid QUIC stream {}", streamId);
        return Err(TransportError::InvalidArgument);
    }

    return Ok(it->second);
}

bool QuicConnection::shouldLosePacket() const {
    float sim = std::clamp(m_lossSimulation, 0.0f, 1.0f);

    if (qn::randomChance(sim)) {
        log::debug("QUIC: purposefully dropping packet due to loss simulation");
        return true;
    }

    return false;
}

}
