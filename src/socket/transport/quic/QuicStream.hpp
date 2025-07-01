#pragma once

#include <qunet/socket/transport/Error.hpp>
#include <qunet/util/SlidingBuffer.hpp>
#include <qunet/buffers/CircularByteBuffer.hpp>
#include <asp/sync/Mutex.hpp>

namespace qn {

// Represents a bidirectional QUIC stream. This class has interior mutability and is fully thread-safe.
class QuicStream {
public:
    QuicStream(class QuicConnection* conn, int64_t streamId);
    ~QuicStream();

    QuicStream(const QuicStream&) = delete;
    QuicStream& operator=(const QuicStream&) = delete;

    QuicStream(QuicStream&&);
    QuicStream& operator=(QuicStream&&);

    int64_t id() const;

    // Acknowledge the number of bytes that have been successfully received by the peer.
    void onAck(uint64_t offset, uint64_t ackedBytes);

    // Write data to this QUIC stream, returns the number of bytes that were written to the internal buffer.
    // Even if all data has been written, this does not guarantee that the data has actually been sent over the network.
    // Make sure to call `tryFlush()` to send more data and check how many bytes remain to be sent.
    TransportResult<size_t> write(const uint8_t* data, size_t len);

    // Try to send any pending data over the QUIC stream.
    // Returns the number of bytes that remain in the internal buffer that have not been sent yet.
    TransportResult<size_t> tryFlush();

    // Returns the number of bytes that are currently buffered and ready to be sent.
    size_t toFlush();

    // Returns the number of bytes that are currently buffered and ready to be read.
    size_t toReceive() const;

    // Returns whether the send buffer has any free space to write more data.
    bool writable() const;

    // Returns whether the receive buffer has any data that can be read.
    bool readable() const;

    // Read data from the receive buffer, returns the number of bytes that were read.
    // This does not block and does not receive data from the stream, you should ensure
    // `toReceive()` is greater than 0 before calling this method.
    TransportResult<size_t> read(uint8_t* buffer, size_t len);

    TransportResult<> close();

private:
    friend class QuicConnection;

    class QuicConnection* m_conn = nullptr;
    int64_t m_streamId = -1;

    asp::Mutex<void, true> m_mutex;
    asp::Mutex<SlidingBuffer> m_sendBuffer;
    asp::Mutex<CircularByteBuffer> m_recvBuffer;
    size_t m_sendBufferSentPos = 0;

    TransportResult<size_t> doSend(bool fin = false);

    TransportResult<size_t> deliverToRecvBuffer(const uint8_t* data, size_t len);
};

}