#pragma once

#ifdef QUNET_QUIC_SUPPORT

#include <stdint.h>
#include <qunet/socket/transport/Error.hpp>
#include <qunet/util/SlidingBuffer.hpp>
#include <qunet/buffers/CircularByteBuffer.hpp>
#include <arc/sync/Notify.hpp>
#include <asp/sync/Mutex.hpp>

namespace qn {

class QuicStream {
public:
    QuicStream(class QuicConnection* conn, uint64_t id);
    ~QuicStream();

    QuicStream(const QuicStream&) = delete;
    QuicStream& operator=(const QuicStream&) = delete;
    QuicStream(QuicStream&&) noexcept;
    QuicStream& operator=(QuicStream&&) noexcept;

    int64_t id() const;

    /// Returns amount of buffered bytes that have not been read by the application yet.
    size_t unreadBytes() const;
    /// Returns amount of bytes that have been written to the buffer but not yet sent.
    size_t unflushedBytes() const;
    /// Returns amount of bytes that have been sent but not yet acknowledged.
    size_t unackedBytes() const;
    size_t sendCapacity() const;
    size_t sendCapacity(CircularByteBuffer& sendBuffer) const;

    void close();

    size_t write(const uint8_t* data, size_t len);
    size_t read(void* buffer, size_t len);

    arc::Future<> pollWritable();
    arc::Future<> pollReadable();

    std::pair<CircularByteBuffer::WrappedRead, asp::MutexGuard<CircularByteBuffer>> peekUnsentData();
    void advanceSentData(size_t len);

private:
    friend class QuicConnection;

    QuicConnection* m_conn = nullptr;
    int64_t m_streamId = -1;
    bool m_closed = false;
    arc::Notify m_writableNotify, m_readableNotify;

    asp::Mutex<CircularByteBuffer> m_sendBuffer;
    asp::Mutex<CircularByteBuffer> m_recvBuffer;
    uint64_t m_ackOffset = 0;
    std::atomic<uint64_t> m_unackedBytes = 0;
    std::atomic<uint64_t> m_streamOffset = 0;

    void onReceivedData(const uint8_t* data, size_t len);
    void onAck(uint64_t offset, uint64_t ackedBytes);
};

}

#endif
