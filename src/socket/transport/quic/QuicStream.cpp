#include "QuicStream.hpp"
#include "QuicConnection.hpp"

#include <ngtcp2/ngtcp2.h>
#include <qunet/util/assert.hpp>
#include <qunet/Log.hpp>
#include <cstring>

static const size_t INITIAL_BUFFER_SIZE = 16384; // 16 KiB
static const size_t MAX_BUFFER_SIZE = 1024 * 1024 * 4; // 4 MiB

namespace qn {

QuicStream::QuicStream(QuicConnection* conn, int64_t streamId) : m_conn(conn), m_streamId(streamId) {
    m_sendBuffer.resize(INITIAL_BUFFER_SIZE);
    m_recvBuffer.resize(INITIAL_BUFFER_SIZE);
}

QuicStream::~QuicStream() {
    if (m_streamId != -1) {
        ngtcp2_conn_shutdown_stream(m_conn->rawHandle(), 0, m_streamId, 1);
    }
}

QuicStream::QuicStream(QuicStream&& other) {
    *this = std::move(other);
}

QuicStream& QuicStream::operator=(QuicStream&& other) {
    if (this != &other) {
        auto _lock1 = m_mutex.lock();
        auto _lock2 = other.m_mutex.lock();

        m_conn = other.m_conn;
        m_streamId = other.m_streamId;
        m_sendBuffer = std::move(other.m_sendBuffer);
        m_sendBufferPos = other.m_sendBufferPos;
        m_sendBufferWritePos = other.m_sendBufferWritePos;
        m_sendBufferAckPos = other.m_sendBufferAckPos;
        m_sendStreamOffset = other.m_sendStreamOffset;
        m_recvBuffer = std::move(other.m_recvBuffer);
        m_recvBufferPos = other.m_recvBufferPos;
        m_recvBufferWritePos = other.m_recvBufferWritePos;

        other.m_conn = nullptr;
        other.m_streamId = -1;
    }

    return *this;
}

void QuicStream::onAck(uint64_t offset, uint64_t ackedBytes) {
    auto _lock = m_mutex.lock();

    log::debug("QUIC stream {}: acknowledged bytes {}-{}", m_streamId, offset, ackedBytes);

    m_sendBufferAckPos += ackedBytes;
    m_sendStreamOffset = offset + ackedBytes;

    QN_DEBUG_ASSERT(m_sendBufferAckPos <= m_sendBufferPos && "Ack position must not exceed send position");

    // if all the data that has been sent so far has been acked, we can reset the buffer
    if (m_sendBufferAckPos == m_sendBufferPos) {
        m_sendBufferPos = 0;
        m_sendBufferAckPos = 0;
    }
}

TransportResult<size_t> QuicStream::write(const uint8_t* data, size_t len) {
    auto _lock = m_mutex.lock();

    size_t remSpace = m_sendBuffer.size() - m_sendBufferPos;

    if (len > remSpace) {
        // we don't have enough space, see if we can grow the buffer
        size_t nextSize = this->calculateNextBufferSize(m_sendBuffer.size(), m_sendBufferPos + len);

        if (nextSize > MAX_BUFFER_SIZE) {
            // can't grow, write what we can
            len = remSpace;
        } else {
            m_sendBuffer.resize(nextSize);
        }
    }

    if (len == 0) {
        return Ok(0);
    }

    // copy the data into the buffer
    std::memcpy(m_sendBuffer.data() + m_sendBufferPos, data, len);
    m_sendBufferPos += len;

    return Ok(len);
}

TransportResult<size_t> QuicStream::tryFlush() {
    auto _lock = m_mutex.lock();

    size_t toSend = this->toFlush();

    if (toSend == 0) {
        // nothing to flush
        return Ok(0);
    }

    GEODE_UNWRAP(this->doSend(toSend));

    return Ok(this->toFlush());
}

size_t QuicStream::toFlush() {
    auto _lock = m_mutex.lock();

    return m_sendBufferPos - m_sendBufferWritePos;

}

TransportResult<size_t> QuicStream::doSend(size_t count) {
    uint8_t outBuf[1500];
    ngtcp2_pkt_info pi{};
    ngtcp2_ssize datalen;

    auto src = m_sendBuffer.data() + m_sendBufferWritePos;

    auto written = ngtcp2_conn_write_stream(
        m_conn->rawHandle(),
        nullptr,
        &pi,
        outBuf,
        sizeof(outBuf),
        &datalen,
        0,
        m_streamId,
        src,
        count,
        timestamp()
    );

    if (written < 0) {
        return Err(QuicError(written));
    }

    ngtcp2_conn_update_pkt_tx_time(m_conn->rawHandle(), timestamp());

    GEODE_UNWRAP(m_conn->m_socket->send(outBuf, written));

    m_sendBufferWritePos += datalen;

    return Ok(datalen);
}

TransportResult<size_t> QuicStream::read(uint8_t* buffer, size_t len) {
    auto _lock = m_mutex.lock();

    return Ok(this->fillFromRecvBuffer(buffer, len));
}

size_t QuicStream::calculateNextBufferSize(size_t current, size_t needed) {
    QN_ASSERT(needed < (1ull << 31) && "Needed size must be less than 2 GiB");

    while (current < needed) {
        current *= 2;
    }

    return current;
}

TransportResult<size_t> QuicStream::deliverToRecvBuffer(const uint8_t* data, size_t len) {
    if (len == 0) {
        return Ok(0);
    }

    auto _lock = m_mutex.lock();

    size_t remSpace = m_recvBuffer.size() - m_recvBufferWritePos;

    if (len > remSpace) {
        size_t nextSize = this->calculateNextBufferSize(m_recvBuffer.size(), m_recvBufferWritePos + len);

        if (nextSize > MAX_BUFFER_SIZE) {
            return Err(TransportError::NoBufferSpace);
        }

        m_recvBuffer.resize(nextSize);
    }

    std::memcpy(m_recvBuffer.data() + m_recvBufferWritePos, data, len);
    m_recvBufferWritePos += len;

    return Ok(len);
}

size_t QuicStream::fillFromRecvBuffer(uint8_t* buffer, size_t len) {
    auto _lock = m_mutex.lock();

    size_t toRead = std::min(len, this->toReceive());

    if (toRead == 0) {
        return 0;
    }

    std::memcpy(buffer, m_recvBuffer.data() + m_recvBufferPos, toRead);
    m_recvBufferPos += toRead;

    if (m_recvBufferPos == m_recvBufferWritePos) {
        // reset the buffer if we have read everything
        m_recvBufferPos = 0;
        m_recvBufferWritePos = 0;
    }

    return toRead;
}

size_t QuicStream::toReceive() const {
    auto _lock = m_mutex.lock();

    return m_recvBufferWritePos - m_recvBufferPos;
}

int64_t QuicStream::id() const {
    return m_streamId;
}

}