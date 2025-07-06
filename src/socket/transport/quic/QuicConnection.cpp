#include "QuicConnection.hpp"
#include <qunet/Connection.hpp>
#include <qunet/protocol/constants.hpp>
#include <qunet/util/Error.hpp>
#include <qunet/Log.hpp>

#include <ngtcp2/ngtcp2_crypto.h>
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/logging.h>
#include <qsox/Poll.hpp>
#include <asp/time/Duration.hpp>
#include <asp/time/Instant.hpp>
#include <asp/time/sleep.hpp>
#include <asp/time/chrono.hpp>
#include <random>

// TODO i probably should somehow handle a dead server by seeing if packets don't arrive for a while
// TODO: see if this can be optimized further, maybe we send acks too often or something
// TODO: connection migration

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
    })
{
    m_connThread.setLoopFunction([this](auto& stopToken) {
        this->threadFunc(stopToken);
    });

    // defer the connection thread start
}

QuicConnection::~QuicConnection() {
    m_connThread.stopAndWait();

    auto conn = m_conn.lock();
    if (*conn) {
        ngtcp2_conn_del(*conn);
    }
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

ngtcp2_conn* QuicConnection::rawHandle() const {
    return *m_conn.lock();
}

ngtcp2_crypto_conn_ref* QuicConnection::connRef() const {
    return const_cast<ngtcp2_crypto_conn_ref*>(&m_connRef);
}

TransportResult<std::unique_ptr<QuicConnection>> QuicConnection::connect(
    const SocketAddress& address,
    const Duration& timeout,
    const ClientTlsContext* tlsContext,
    const ConnectionDebugOptions* debugOptions
) {
    QN_ASSERT(tlsContext != nullptr && "TLS context must not be null");

    // Create the connection to return
    std::unique_ptr<QuicConnection> ret{new QuicConnection(nullptr)};

    // Set debug options
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

    // Create the UDP socket, connect to the server
    ret->m_socket = GEODE_UNWRAP(UdpSocket::bindAny(address.isV6()));
    GEODE_UNWRAP(ret->m_socket->connect(address));

    // Initialize settings
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
            auto* quicConn = static_cast<QuicConnection*>(user_data);
            QN_ASSERT(stream_id == quicConn->m_mainStream->id() && "Stream ID must match the main stream ID");

            auto res = quicConn->m_mainStream->deliverToRecvBuffer(data, datalen);
            if (!res) {
                log::warn("Failed to deliver data to QUIC stream: {}", res.unwrapErr().message());
                return -1;
            }

            quicConn->m_readableNotify.notifyAll();

            return 0;
        },
        .acked_stream_data_offset = [](ngtcp2_conn* conn, int64_t stream_id, uint64_t offset, uint64_t datalen, void* user_data, void* stream_user_data) {
            auto* quicConn = static_cast<QuicConnection*>(user_data);
            QN_ASSERT(stream_id == quicConn->m_mainStream->id() && "Stream ID must match the main stream ID");

            quicConn->m_mainStream->onAck(offset, datalen);

            // Data acknowledgement may result in buffer space being freed and the stream becoming writable again,
            // so notify any waiters.
            quicConn->m_writableNotify.notifyAll();

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

    auto conn = ret->m_conn.lock();
    QuicError err = ngtcp2_conn_client_new(
        &*conn, &dcid, &scid, &path, NGTCP2_PROTO_VER_V1, &callbacks, &settings, &params, nullptr, ret.get()
    );

    if (!err.ok()) {
        return Err(err);
    }

    // Create the TLS session
    ret->m_tlsSession = GEODE_UNWRAP(ClientTlsSession::create(
        *tlsContext, address, ret.get(), "localhost" // TODO: use actual server name
    ));

    ngtcp2_conn_set_tls_native_handle(*conn, ret->m_tlsSession->nativeHandle());
    ngtcp2_conn_set_keep_alive_timeout(*conn, Duration::fromSecs(30).nanos());

    conn.unlock();

    // Setup poller
    ret->m_poller.addSocket(*ret->m_socket, PollType::Read);
    ret->m_poller.addPipe(ret->m_dataWrittenPipe, PollType::Read);

    // Start the connection thread
    ret->m_connThreadState = ThreadState::Handshaking;
    ret->m_connTimeout = timeout;
    ret->m_connThread.start();
    ret->m_connectionReady.wait();

    // done!! :)

    log::debug("QUIC: connection is ready now");

    GEODE_UNWRAP(std::move(ret->m_handshakeResult));

    return Ok(std::move(ret));
}

TransportResult<> QuicConnection::performHandshake(const asp::time::Duration& timeout) {
    auto startedAt = Instant::now();

    ngtcp2_path_storage_zero(&m_networkPath);

    auto conn = m_conn.lock();

    GEODE_UNWRAP(this->sendNonStreamPacket(true));

    while (ngtcp2_conn_get_handshake_completed(*conn) == 0) {
        auto remaining = timeout - startedAt.elapsed();

        if (remaining.isZero()) {
            return Err(TransportError::ConnectionTimedOut);
        }

        bool has = GEODE_UNWRAP(this->pollReadableSocket(remaining));
        if (!has) {
            continue;
        }

        // read the handshake response, it may be multiple packets (?)
        GEODE_UNWRAP(this->doRecv());
    }

    log::debug("QUIC: handshake completed");

    return Ok();
}

QuicResult<QuicStream> QuicConnection::openBidiStream() {
    int64_t streamId = -1;

    QuicError err = ngtcp2_conn_open_bidi_stream(*m_conn.lock(), &streamId, this);
    if (!err.ok()) {
        return Err(err);
    }

    QN_ASSERT(streamId != -1 && "Stream ID must not be -1");

    return Ok(QuicStream(this, streamId));
}

TransportResult<bool> QuicConnection::pollReadable(const Duration& dur) {
    if (m_closed) {
        return Err(TransportError::Closed);
    }

    if (m_mainStream->readable()) {
        return Ok(true); // stream is readable, no need to wait
    }

    return Ok(m_readableNotify.wait(dur, [&] {
        return m_mainStream->readable();
    }));
}

TransportResult<bool> QuicConnection::pollReadableSocket(const asp::time::Duration& dur) {
    auto res = GEODE_UNWRAP(qsox::pollOne(*m_socket, PollType::Read, dur.millis()));

    return Ok(res == PollResult::Readable);
}

TransportResult<bool> QuicConnection::pollWritable(const Duration& dur) {
    if (m_closed) {
        return Err(TransportError::Closed);
    }

    if (m_mainStream->writable()) {
        return Ok(true); // stream is writable, no need to wait
    }

    return Ok(m_writableNotify.wait(dur, [&] {
        return m_mainStream->writable();
    }));
}

TransportResult<size_t> QuicConnection::send(const uint8_t* data, size_t len) {
    if (m_closed) {
        return Err(TransportError::Closed);
    }

    if (len == 0) {
        return Ok(0); // nothing to send
    }

    len = GEODE_UNWRAP(m_mainStream->write(data, len));

    if (len == 0) {
        return Err(TransportError::NoBufferSpace);
    }

    m_dataWrittenPipe.notify();

    return Ok(len);
}

TransportResult<> QuicConnection::sendAll(const uint8_t* data, size_t len) {
    if (m_closed) {
        return Err(TransportError::Closed);
    }

    while (len) {
        auto res = this->send(data, len);

        if (!res) {
            auto err = res.unwrapErr();
            if (std::get<TransportError::CustomKind>(err.m_kind).code == TransportError::NoBufferSpace) {
                GEODE_UNWRAP(this->pollWritable(Duration{}));
                continue;
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
    if (m_closed) {
        return Err(TransportError::Closed);
    }

    if (len == 0) {
        return Ok(0);
    }

    while (true) {
        if (!GEODE_UNWRAP(this->pollReadable(Duration{}))) {
            continue;
        }

        size_t readBytes = GEODE_UNWRAP(m_mainStream->read(buffer, len));

        if (readBytes > 0) {
            return Ok(readBytes);
        }
    }
}

TransportResult<> QuicConnection::doRecv() {
    ngtcp2_pkt_info pi{};
    uint8_t outBuf[1500];

    m_poller.clearReadiness(*m_socket);

    size_t recvRes = GEODE_UNWRAP(m_socket->recv(outBuf, sizeof(outBuf)));
    log::debug("QUIC: read {} bytes from the socket", recvRes);

    m_totalBytesReceived.fetch_add(recvRes, std::memory_order::relaxed);

    auto conn = m_conn.lock();

    QuicError res = ngtcp2_conn_read_pkt(*conn, &m_networkPath.path, &pi, outBuf, recvRes, timestamp());
    if (!res.ok()) {
        log::warn("QUIC: failed to read the packet: {}", res.message());
        if (res.code == NGTCP2_ERR_CRYPTO) {
            auto tlsErr = m_tlsSession->lastError();
            log::warn("QUIC: last TLS error: {}", tlsErr.message());
            return Err(tlsErr);
        }

        return Err(res);
    }

    return Ok();
}

TransportResult<> QuicConnection::close() {
    if (m_closed) {
        return Err(TransportError::Closed);
    }

    m_terminateCleanly = true;
    m_terminating = true;
    return Ok();
}

bool QuicConnection::finishedClosing() const {
    return m_closed && m_connThreadState == ThreadState::Stopped;
}

void QuicConnection::threadFunc(asp::StopToken<>& stopToken) {
    switch (m_connThreadState) {
        case ThreadState::Idle: {
            asp::time::yield();
        } break;

        case ThreadState::Handshaking: {
            // Wait for the QUIC handshake to complete
            m_handshakeResult = this->performHandshake(m_connTimeout);

            if (m_handshakeResult.isOk()) {
                // Open the main stream
                if (auto stream = this->openBidiStream())  {
                    m_mainStream = std::move(stream).unwrap();
                } else {
                    m_handshakeResult = Err(stream.unwrapErr());
                }
            }

            if (m_handshakeResult.isOk()) {
                m_connThreadState = ThreadState::Running;
            } else {
                m_connThreadState = ThreadState::Stopped;
                log::warn("QUIC: handshake failed: {}", m_handshakeResult.unwrapErr().message());
            }

            m_connectionReady.notifyOne();
        } break;

        case ThreadState::Running: {
            if (m_terminating) {
                m_connThreadState = ThreadState::Stopping;
                m_closed = true; // set closed flag to true, disallowing callers to send/receive data
                return;
            }

            if (m_connThrExpiry == UINT64_MAX) {
                m_connThrExpiry = ngtcp2_conn_get_expiry(*m_conn.lock());
            }

            auto now = timestamp();

            bool wantSendData = false;
            size_t toFlush = m_mainStream->toFlush();

            if (toFlush > 0) {
                // // check if we are allowed to send data
                // auto count = ngtcp2_conn_get_cwnd_left(m_conn);
                // log::debug("QUIC: stream {} has {} bytes to flush, congestion window: {}", m_mainStream->id(), toFlush, count);

                // wantSendData = count > 0;

                wantSendData = true;
            }

            bool didExpire = now >= m_connThrExpiry;

            // only poll if expiry isn't reached yet and there's no data
            bool shouldPoll = !didExpire && !wantSendData;

            bool hasIncomingData = false;

            if (shouldPoll) {
                auto timeout = Duration::fromNanos(m_connThrExpiry - now);
                // log::debug("QUIC: until expiry: {}", timeout.toString());

                if (timeout.millis() > 100) {
                    timeout = Duration::fromMillis(100);
                } else if (timeout.millis() < 2) {
                    timeout = Duration::fromMillis(2);
                }

                // log::debug("QUIC: polling for incoming data, timeout: {}", timeout.toString());

                auto res = this->thrPoll(timeout);
                hasIncomingData = res.sockReadable;
                wantSendData = res.newDataAvail;

                now = timestamp();
                didExpire = now >= m_connThrExpiry;
            }

            // log::debug("QUIC: poll result - hasIncomingData: {}, wantSendData: {}, didExpire: {}", hasIncomingData, wantSendData, didExpire);

            auto conn = m_conn.lock();

            // If the socket is readable, read the data and push to the receive buffer
            if (hasIncomingData) {
                auto res = this->doRecv();

                if (!res) {
                    auto err = res.unwrapErr();
                    log::warn("QUIC: failed to receive data: {}", err.message());
                    this->thrOnFatalError(err);
                    return;
                }
            }

            // handle expired timer
            if (didExpire) {
                auto code = ngtcp2_conn_handle_expiry(*conn, timestamp());
                m_connThrExpiry = ngtcp2_conn_get_expiry(*conn);

                if (code == NGTCP2_ERR_IDLE_CLOSE) {
                    this->thrOnIdleTimeout();
                    return;
                }
            }

            // check if there's any buffered data to send
            if (wantSendData) while (true) {
                auto res = m_mainStream->tryFlush();
                if (res.isOk()) {
                    size_t written = res.unwrap();
                    if (written == 0) {
                        // no more data to send
                        break;
                    }

                    // otherwise, keep trying to send
                } else {
                    conn.unlock();
                    this->thrHandleError(res.unwrapErr());
                    return;
                }
            }

            // write other packets if needed (acks, pings, retransmissions, etc.)
            // TODO: when we have better debugging methods, check if this really needs to be sent every time,
            // rather than only when `didExpire` is true
            auto res = this->sendNonStreamPacket(false);
            if (!res) {
                conn.unlock();
                this->thrHandleError(res.unwrapErr());
            }
        } break;

        case ThreadState::Stopping: {
            auto conn = m_conn.lock();

            // cleanup connection if needed
            if (m_terminateCleanly) {
                TransportResult<> res = Ok();

                // We could gracefully close the stream, but there is no point since the connection is being terminated,
                // it will only add an additional UDP packet to be sent to the server.

                // res = m_mainStream->close();
                // if (!res) {
                //     auto err = res.unwrapErr();
                //     log::warn("QUIC: failed to close main stream: {}", err.message());
                // }

                res = this->sendClosePacket();
                if (!res) {
                    auto err = res.unwrapErr();
                    log::warn("QUIC: failed to send close packet: {}", err.message());
                } else {
                    log::debug("QUIC: sent close packet, connection terminated");
                }
            }

            m_connThreadState = ThreadState::Stopped;

        } break;

        case ThreadState::Stopped: {
            m_terminating = false;
            log::debug("QUIC: connection fully terminated, stopping thread");
            stopToken.stop();
        } break;
    }
}

TransportResult<> QuicConnection::sendNonStreamPacket(bool handshake) {
    uint8_t outBuf[1500];
    ngtcp2_pkt_info pi{};

    auto conn = m_conn.lock();
    auto written = ngtcp2_conn_write_pkt(*conn, handshake ? &m_networkPath.path : nullptr, &pi, outBuf, sizeof(outBuf), timestamp());

    if (written < 0) {
        QuicError err(written);
        log::warn("QUIC: failed to write {} packet: {}", handshake ? "handshake" : "non-stream", err.message());
        return Err(err);
    } else if (written == 0) {
        if (handshake) {
            log::error("QUIC: failed to write handshake packet to buffer, written == 0");
            return Err(TransportError::Other);
        } else {
            return Ok(); // nothing to send
        }
    }

    ngtcp2_conn_update_pkt_tx_time(*conn, timestamp());

    conn.unlock();

    log::debug("QUIC: sending {} packet, size: {}", handshake ? "handshake" : "non-stream", written);

    m_totalBytesSent.fetch_add(written, std::memory_order::relaxed);

    if (!this->shouldLosePacket()) {
        GEODE_UNWRAP(m_socket->send(outBuf, written));
    }

    return Ok();
}

TransportResult<> QuicConnection::sendClosePacket() {
    uint8_t outBuf[1500];
    ngtcp2_pkt_info pi{};

    auto conn = m_conn.lock();

    ngtcp2_ccerr ccerr {
        .type = NGTCP2_CCERR_TYPE_APPLICATION,
        .error_code = 1, // graceful closure
        .frame_type = 0,
        .reason = nullptr,
        .reasonlen = 0,
    };

    auto written = ngtcp2_conn_write_connection_close(*conn, &m_networkPath.path, &pi, outBuf, sizeof(outBuf), &ccerr, timestamp());

    if (written < 0) {
        QuicError err(written);
        return Err(err);
    }

    QN_ASSERT(written > 0 && "Close packet is 0 bytes");

    conn.unlock();

    log::debug("QUIC: sending close packet, size: {}", written);

    m_totalBytesSent.fetch_add(written, std::memory_order::relaxed);

    GEODE_UNWRAP(m_socket->send(outBuf, written));

    return Ok();
}

void QuicConnection::thrOnIdleTimeout() {
    log::warn("QUIC: connection idle timeout reached, closing connection");
    this->thrOnFatalError(TransportError::ConnectionTimedOut);
}

void QuicConnection::thrHandleError(const TransportError& err) {
    if (this->isCongestionRelatedError(err)) {
        log::debug("QUIC: packet blocked due to congestion control, can't send pending data");
        asp::time::yield();
    } else {
        this->thrOnFatalError(err);
    }
}

void QuicConnection::thrOnFatalError(const TransportError& err) {
    m_fatalError = err;
    log::error("QUIC: fatal error, terminating: {}", err.message());
    m_terminateCleanly = false;
    m_terminating = true;
}

QuicConnection::ThrPollResult QuicConnection::thrPoll(const asp::time::Duration& timeout) {
    auto result = m_poller.poll(timeout);
    if (!result) {
        return {};
    }

    ThrPollResult res{};
    if (result->isSocket(*m_socket)) {
        res.sockReadable = true;
    } else if (result->isPipe(m_dataWrittenPipe)) {
        res.newDataAvail = true;
        m_dataWrittenPipe.consume();
    }

    return res;
}

bool QuicConnection::isCongestionRelatedError(const TransportError& err) {
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

QuicConnectionStats QuicConnection::connStats() const {
    return QuicConnectionStats {
        .totalSent = m_totalBytesSent.load(std::memory_order::relaxed),
        .totalReceived = m_totalBytesReceived.load(std::memory_order::relaxed),
        .totalDataSent = m_totalDataBytesSent.load(std::memory_order::relaxed),
        .totalDataReceived = m_totalDataBytesReceived.load(std::memory_order::relaxed),
    };
}

bool QuicConnection::shouldLosePacket() const {
    float sim = std::clamp(m_lossSimulation, 0.0f, 1.0f);
    if (sim <= 0.0f) return false;

    static thread_local std::mt19937 generator(std::random_device{}());
    std::uniform_real_distribution<float> distribution(0.0f, 1.0f);
    float randomValue = distribution(generator);

    if (randomValue < sim) {
        log::debug("QUIC: purposefully dropping packet due to loss simulation");
        return true;
    }

    return false;
}

}