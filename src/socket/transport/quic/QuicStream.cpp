#include "QuicStream.hpp"
#include "QuicConnection.hpp"
#include <qunet/Log.hpp>

using namespace arc;

static const size_t INITIAL_BUFFER_SIZE = 16384; // 16 KiB
static const size_t MAX_BUFFER_SIZE = 1024 * 1024 * 16; // 16 MiB

namespace qn {

QuicStream::QuicStream(class QuicConnection* conn, uint64_t id)
    : m_conn(conn),
      m_streamId(id),
      m_recvBuffer(INITIAL_BUFFER_SIZE),
      m_sendBuffer(INITIAL_BUFFER_SIZE) {}

QuicStream::~QuicStream() {
    if (m_streamId != -1 && !m_closed) {
        this->close();
    }
}

QuicStream::QuicStream(QuicStream&& other) noexcept {
    *this = std::move(other);
}

QuicStream& QuicStream::operator=(QuicStream&& other) noexcept {
    if (this != &other) {
        m_streamId = other.m_streamId;
        other.m_streamId = -1;
        m_closed = other.m_closed;

        m_sendBuffer = std::move(other.m_sendBuffer);
        m_recvBuffer = std::move(other.m_recvBuffer);
    }
    return *this;
}

void QuicStream::close() {
    if (m_closed || m_streamId == -1) {
        return;
    }

    m_closed = true;
    auto conn = m_conn->m_conn;
    ngtcp2_conn_shutdown_stream(conn, 0, m_streamId, 1);
}

int64_t QuicStream::id() const {
    return m_streamId;
}

size_t QuicStream::unreadBytes() const {
    return m_closed ? 0 : m_recvBuffer.size();
}

size_t QuicStream::unflushedBytes() const {
    return m_closed ? 0 : (m_sendBuffer.size() - m_unackedBytes);
}

size_t QuicStream::unackedBytes() const {
    return m_unackedBytes;
}

size_t QuicStream::sendCapacity() const {
    // we cannot grow the buffer if there is data in flight
    return (this->unackedBytes() > 0 ? m_sendBuffer.capacity() : MAX_BUFFER_SIZE) - m_sendBuffer.size();

}

size_t QuicStream::write(const uint8_t* data, size_t len) {
    log::debug(
        "QUIC stream {}: writing {} bytes to write buffer (buffer capacity: {}, free space: {})",
        m_streamId, len, m_sendBuffer.capacity(), m_sendBuffer.capacity() - m_sendBuffer.size()
    );

    size_t maxWrite = this->sendCapacity();
    if (m_sendBuffer.size() + len > maxWrite) {
        log::warn("QUIC stream {}: failed to write to send buffer, no space left!", m_streamId);
        return 0;
    }

    m_sendBuffer.write(data, len);
    return len;
}

size_t QuicStream::read(void* buffer, size_t len) {
    size_t toRead = std::min<size_t>(len, m_recvBuffer.size());
    if (toRead == 0) {
        return 0;
    }

    m_recvBuffer.read(buffer, toRead);

    log::debug("QUIC stream {}: read {} bytes from receive buffer", m_streamId, toRead);
    return toRead;
}

void QuicStream::onReceivedData(const uint8_t* data, size_t len) {
    log::debug("QUIC stream {}: received {} stream bytes", m_streamId, len);

    size_t spaceUntilMax = MAX_BUFFER_SIZE - m_recvBuffer.size();

    if (len < spaceUntilMax) {
        m_recvBuffer.write(data, len);
    } else {
        log::warn("QUIC stream {}: failed to write to receive buffer, no space left!", m_streamId);
    }

    m_readableNotify.notifyAll();
}

void QuicStream::onAck(uint64_t offset, uint64_t ackedBytes) {
    // ngtcp2 guarantees there are no gaps in the acked bytes.
    // once we receive an ack, it's safe to skip these bytes
    m_sendBuffer.read(nullptr, ackedBytes);

    auto unacked = m_unackedBytes -= ackedBytes;
    m_ackOffset = offset + ackedBytes;

    QN_DEBUG_ASSERT(m_ackOffset <= m_streamOffset);
    QN_DEBUG_ASSERT(unacked == m_streamOffset - m_ackOffset);

    log::debug(
        "QUIC stream {}: acknowledged {} bytes @ offset {} (unacked: {}, unsent: {})",
        m_streamId, ackedBytes, offset, unacked, m_sendBuffer.size() - unacked
    );

    // some send capacity may have freed up, notify any waiters
    m_writableNotify.notifyAll();
}

Future<> QuicStream::pollWritable() {
    while (this->sendCapacity() == 0) {
        co_await m_writableNotify.notified();
    }
}

Future<> QuicStream::pollReadable() {
    while (this->unreadBytes() == 0) {
        co_await m_readableNotify.notified();
    }
}

CircularByteBuffer::WrappedRead QuicStream::peekUnsentData() {
    auto wrp = m_sendBuffer.peek(m_sendBuffer.size());
    wrp.skip(m_unackedBytes);
    return wrp;
}

void QuicStream::advanceSentData(size_t len) {
    m_unackedBytes += len;
    m_streamOffset += len;
}

}
