#include "QuicStream.hpp"
#include "QuicConnection.hpp"

#include <ngtcp2/ngtcp2.h>
#include <qunet/util/assert.hpp>
#include <qunet/Log.hpp>
#include <cstring>

static const size_t INITIAL_BUFFER_SIZE = 16384; // 16 KiB
static const size_t MAX_BUFFER_SIZE = 1024 * 1024 * 4; // 4 MiB

namespace qn {

QuicStream::QuicStream(QuicConnection* conn, int64_t streamId)
    : m_conn(conn),
      m_streamId(streamId),
      m_recvBuffer(asp::Mutex<SlidingBuffer>({INITIAL_BUFFER_SIZE, MAX_BUFFER_SIZE})),
      m_sendBuffer(asp::Mutex<SlidingBuffer>({INITIAL_BUFFER_SIZE, MAX_BUFFER_SIZE}))
      {}

QuicStream::~QuicStream() {
    if (m_streamId != -1) {
        ngtcp2_conn_shutdown_stream(m_conn->rawHandle(), 0, m_streamId, 1);
    }
}

QuicStream::QuicStream(QuicStream&& other)
    : m_conn(other.m_conn),
      m_streamId(other.m_streamId),
      m_recvBuffer(std::move(*other.m_recvBuffer.lock())),
      m_sendBuffer(std::move(*other.m_sendBuffer.lock())),
      m_sendBufferSentPos(other.m_sendBufferSentPos)

{
    other.m_conn = nullptr;
    other.m_streamId = -1;
}

QuicStream& QuicStream::operator=(QuicStream&& other) {
    if (this != &other) {
        auto _lock1 = m_mutex.lock();
        auto _lock2 = other.m_mutex.lock();

        m_conn = other.m_conn;
        m_streamId = other.m_streamId;
        other.m_conn = nullptr;
        other.m_streamId = -1;

        *m_recvBuffer.lock() = std::move(*other.m_recvBuffer.lock());
        *m_sendBuffer.lock() = std::move(*other.m_sendBuffer.lock());
        m_sendBufferSentPos = other.m_sendBufferSentPos;
    }

    return *this;
}

void QuicStream::onAck(uint64_t offset, uint64_t ackedBytes) {
    auto sndbuf = m_sendBuffer.lock();
    sndbuf->read(nullptr, ackedBytes);

    // if all data has been acknowledged, the send buffer should reset write pos and read pos to 0,
    // and we should reset the sent position accordingly
    if (sndbuf->writePos() == sndbuf->readPos()) {
        m_sendBufferSentPos = 0;
    }

    log::debug(
        "QUIC stream {}: acknowledged {} bytes @ offset {} (unacked: {}, unsent: {})",
        m_streamId, ackedBytes, offset, m_sendBufferSentPos - sndbuf->readPos(), sndbuf->writePos() - m_sendBufferSentPos
    );
}

TransportResult<size_t> QuicStream::write(const uint8_t* data, size_t len) {
    auto sndbuf = m_sendBuffer.lock();

    log::debug(
        "QUIC stream {}: writing {} bytes to send buffer (buffer space: {}, grow limit: {})",
        m_streamId, len, sndbuf->canWrite(), sndbuf->canWriteMax()
    );

    return Ok(sndbuf->write(data, len) ? len : 0);
}

TransportResult<size_t> QuicStream::tryFlush() {
    GEODE_UNWRAP(this->doSend());

    return Ok(this->toFlush());
}

size_t QuicStream::toFlush() {
    return m_sendBuffer.lock()->writePos() - m_sendBufferSentPos;
}

TransportResult<size_t> QuicStream::doSend(bool fin) {
    auto _lock = m_mutex.lock();
    auto sndbuf = m_sendBuffer.lock();

    size_t count = sndbuf->writePos() - m_sendBufferSentPos;
    if (count == 0 && !fin) {
        return Ok(0);
    }

    uint8_t outBuf[1500];
    ngtcp2_pkt_info pi{};
    ngtcp2_ssize datalen;

    uint32_t flags = 0;

    if (!fin) {
        log::debug("QUIC stream {}: trying to send {} bytes", m_streamId, count);
    } else {
        log::debug("QUIC stream {}: sending FIN packet", m_streamId);
        flags = NGTCP2_WRITE_STREAM_FLAG_FIN;
    }

    auto src = sndbuf->data() + m_sendBufferSentPos;

    auto written = ngtcp2_conn_write_stream(
        m_conn->rawHandle(),
        nullptr,
        &pi,
        outBuf,
        sizeof(outBuf),
        &datalen,
        flags,
        m_streamId,
        src,
        count,
        timestamp()
    );

    if (written < 0) {
        QuicError err(written);
        log::warn("QUIC stream {}: failed to write stream data: {}", m_streamId, err.message());
        return Err(err);
    } else if (written == 0) {
        log::debug("QUIC stream {}: failed to write stream data due to congestion control", m_streamId);
        return Err(TransportError::CongestionLimited);
    }

    log::debug("QUIC stream {}: sending datagram size {} ({} stream bytes)", m_streamId, written, datalen);

    ngtcp2_conn_update_pkt_tx_time(m_conn->rawHandle(), timestamp());

    GEODE_UNWRAP(m_conn->m_socket->send(outBuf, written));

    if (datalen == -1) {
        return Ok(0);
    }

    m_sendBufferSentPos += datalen;

    return Ok(datalen);
}

TransportResult<size_t> QuicStream::read(uint8_t* buffer, size_t len) {
    size_t read = m_recvBuffer.lock()->read(buffer, len);

    log::debug("QUIC stream {}: read {} bytes from receive buffer", m_streamId, read);

    return Ok(read);
}

TransportResult<> QuicStream::close() {
    GEODE_UNWRAP(this->doSend(true)); // send FIN
    return Ok();
}

TransportResult<size_t> QuicStream::deliverToRecvBuffer(const uint8_t* data, size_t len) {
    auto rcvbuf = m_recvBuffer.lock();

    log::debug("QUIC stream {}: received {} stream bytes", m_streamId, len);

    if (rcvbuf->write(data, len)) {
        return Ok(len);
    } else {
        log::warn("QUIC stream {}: failed to write to receive buffer, no space left!", m_streamId);
        return Err(TransportError::NoBufferSpace);
    }
}

size_t QuicStream::toReceive() const {
    return m_recvBuffer.lock()->canRead();
}

bool QuicStream::writable() const {
    return m_sendBuffer.lock()->canWriteMax() > 0;
}

bool QuicStream::readable() const {
    return this->toReceive() > 0;
}

int64_t QuicStream::id() const {
    return m_streamId;
}

}