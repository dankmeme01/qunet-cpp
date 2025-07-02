#include "QuicStream.hpp"
#include "QuicConnection.hpp"

#include <ngtcp2/ngtcp2.h>
#include <qunet/util/assert.hpp>
#include <qunet/Log.hpp>
#include <cstring>

static const size_t INITIAL_BUFFER_SIZE = 16384; // 16 KiB
static const size_t MAX_BUFFER_SIZE = 1024 * 1024 * 16; // 16 MiB

namespace qn {

QuicStream::QuicStream(QuicConnection* conn, int64_t streamId)
    : m_conn(conn),
      m_streamId(streamId),
      m_recvBuffer(asp::Mutex<CircularByteBuffer>(INITIAL_BUFFER_SIZE)),
      m_writeBuffer(asp::Mutex<CircularByteBuffer>(INITIAL_BUFFER_SIZE))
      {}

QuicStream::~QuicStream() {
    if (m_streamId != -1) {
        auto conn = m_conn->m_conn.lock();
        ngtcp2_conn_shutdown_stream(*conn, 0, m_streamId, 1);
    }
}

QuicStream::QuicStream(QuicStream&& other)
    : m_conn(other.m_conn),
      m_streamId(other.m_streamId),
      m_recvBuffer(std::move(*other.m_recvBuffer.lock())),
      m_writeBuffer(std::move(*other.m_writeBuffer.lock()))
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
        *m_writeBuffer.lock() = std::move(*other.m_writeBuffer.lock());
    }

    return *this;
}

void QuicStream::onAck(uint64_t offset, uint64_t ackedBytes) {
    // ngtcp2 guarantees there are no gaps in the acked bytes.
    // once we receive an ack, it's safe to skip these bytes

    auto wrbuf = m_writeBuffer.lock();
    wrbuf->read(nullptr, ackedBytes);

    auto unacked = m_unackedBytes -= ackedBytes;
    m_ackOffset = offset + ackedBytes;

    QN_DEBUG_ASSERT(m_ackOffset <= m_streamOffset);
    QN_DEBUG_ASSERT(unacked == m_streamOffset - m_ackOffset);

    log::debug(
        "QUIC stream {}: acknowledged {} bytes @ offset {} (unacked: {}, unsent: {})",
        m_streamId, ackedBytes, offset, unacked, wrbuf->size() - unacked
    );
}

TransportResult<size_t> QuicStream::write(const uint8_t* data, size_t len) {
    auto sndbuf = m_writeBuffer.lock();

    log::debug(
        "QUIC stream {}: writing {} bytes to write buffer (buffer capacity: {}, free space: {})",
        m_streamId, len, sndbuf->capacity(), sndbuf->capacity() - sndbuf->size()
    );

    // we cannot grow the buffer if there is data in flight
    size_t maxWrite = (this->hasDataInFlight() ? sndbuf->capacity() : MAX_BUFFER_SIZE) - sndbuf->size();

    if (sndbuf->size() + len > maxWrite) {
        log::warn("QUIC stream {}: failed to write to send buffer, no space left!", m_streamId);
        return Err(TransportError::NoBufferSpace);
    }

    sndbuf->write(data, len);

    return Ok(len);
}

TransportResult<size_t> QuicStream::tryFlush() {
    GEODE_UNWRAP(this->doSend());

    return Ok(this->toFlush());
}

size_t QuicStream::toFlush() {
    return m_writeBuffer.lock()->size() - m_unackedBytes;
}

TransportResult<size_t> QuicStream::doSend(bool fin) {
    auto _lock = m_mutex.lock();
    auto wrbuf = m_writeBuffer.lock();

    size_t unwrittenOffset = m_unackedBytes;

    size_t count = wrbuf->size() - unwrittenOffset;
    if (count == 0 && !fin) {
        return Ok(0);
    }

    uint8_t outBuf[1500];
    ngtcp2_pkt_info pi{};
    ngtcp2_ssize streamDataWritten = -1;

    uint32_t flags = 0;

    if (!fin) {
        log::debug("QUIC stream {}: trying to send {} bytes", m_streamId, count);
    } else {
        log::debug("QUIC stream {}: sending FIN packet", m_streamId);
        flags = NGTCP2_WRITE_STREAM_FLAG_FIN;
    }

    ngtcp2_vec vecs[2];
    size_t vecCount = 0;

    auto wrp = wrbuf->peek(wrbuf->size());
    wrp.skip(unwrittenOffset); // skip to unwritten data
    QN_DEBUG_ASSERT(wrp.size() == count);

    if (wrp.first.size() > 0) {
        vecs[vecCount++] = ngtcp2_vec{const_cast<uint8_t*>(wrp.first.data()), wrp.first.size()};
    }

    if (wrp.second.size() > 0) {
        vecs[vecCount++] = ngtcp2_vec{const_cast<uint8_t*>(wrp.second.data()), wrp.second.size()};
    }

    auto conn = m_conn->m_conn.lock();
    auto written = ngtcp2_conn_writev_stream(
        *conn,
        nullptr,
        &pi,
        outBuf,
        sizeof(outBuf),
        &streamDataWritten,
        flags,
        m_streamId,
        vecs,
        vecCount,
        timestamp()
    );

    if (written < 0) {
        QuicError err(written);
        log::warn("QUIC stream {}: failed to write stream data: {}", m_streamId, err.message());
        return Err(err);
    } else if (written == 0) {
        log::debug("QUIC stream {}: failed to write stream data due to congestion/flow control", m_streamId);
        return Err(TransportError::CongestionLimited);
    }

    log::debug("QUIC stream {}: sending datagram size {} ({} stream bytes)", m_streamId, written, streamDataWritten);

    ngtcp2_conn_update_pkt_tx_time(*conn, timestamp());

    conn.unlock();

    if (!m_conn->shouldLosePacket()) {
        GEODE_UNWRAP(m_conn->m_socket->send(outBuf, written));
    }

    // log how many bytes we sent
    m_conn->m_totalBytesSent.fetch_add(written, std::memory_order::relaxed);

    if (streamDataWritten == -1) {
        return Ok(0);
    }

    m_conn->m_totalDataBytesSent.fetch_add(streamDataWritten, std::memory_order::relaxed);
    m_unackedBytes += streamDataWritten;
    m_streamOffset += streamDataWritten;

    return Ok(streamDataWritten);
}

TransportResult<size_t> QuicStream::read(uint8_t* buffer, size_t len) {
    auto buf = m_recvBuffer.lock();

    size_t read = std::min<size_t>(len, buf->size());
    buf->read(buffer, read);

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

    m_conn->m_totalDataBytesReceived.fetch_add(len, std::memory_order::relaxed);

    size_t spaceUntilMax = MAX_BUFFER_SIZE - rcvbuf->size();

    if (len < spaceUntilMax) {
        rcvbuf->write(data, len);
        return Ok(len);
    } else {
        log::warn("QUIC stream {}: failed to write to receive buffer, no space left!", m_streamId);
        return Err(TransportError::NoBufferSpace);
    }
}

size_t QuicStream::toReceive() const {
    return m_recvBuffer.lock()->size();
}

bool QuicStream::writable() const {
    auto buf = m_writeBuffer.lock();
    return (this->hasDataInFlight() ? buf->capacity() : MAX_BUFFER_SIZE) - buf->size() > 0;
}

bool QuicStream::readable() const {
    return this->toReceive() > 0;
}

bool QuicStream::hasDataInFlight() const {
    return m_unackedBytes > 0;
}

int64_t QuicStream::id() const {
    return m_streamId;
}

}