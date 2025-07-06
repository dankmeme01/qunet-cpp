#pragma once

#include <qunet/buffers/CircularByteBuffer.hpp>
#include <qunet/socket/transport/Error.hpp>
#include <qunet/socket/message/QunetMessage.hpp>
#include <qunet/Log.hpp>
#include <queue>

// Common operations for stream-like transport layers (TCP, QUIC, etc.)

namespace qn::streamcommon {

inline TransportResult<> sendMessage(QunetMessage message, auto&& socket) {
    HeapByteWriter writer;

    bool isHandshake = message.is<HandshakeStartMessage>();

    // Leave space for message length
    size_t preMessagePos = 0;
    if (!isHandshake) {
        writer.writeU32(0);
        preMessagePos = writer.position();
    }

    GEODE_UNWRAP(message.encodeHeader(writer, 0));

    auto res = message.encode(writer);
    if (!res) {
        log::debug("Failed to encode message: {}", res.unwrapErr().message());
        return Err(res.unwrapErr());
    }

    // Write message length
    uint32_t messageSize = writer.position() - preMessagePos;

    if (!isHandshake) {
        writer.performAt(preMessagePos - sizeof(uint32_t), [&](auto& writer) {
            writer.writeU32(messageSize);
        }).unwrap();
    }

    auto data = writer.written();

    GEODE_UNWRAP(socket.sendAll(data.data(), data.size()));

    return Ok();
}

// The logic here is similar to the one in rust implementation
inline TransportResult<> processIncomingData(
    auto&& socket,
    CircularByteBuffer& buffer,
    size_t messageSizeLimit,
    std::queue<QunetMessage>& msgQueue
) {
    // read from the socket if applicable
    auto wnd = buffer.writeWindow();
    if (wnd.size() < 2048) {
        buffer.reserve(2048);
        wnd = buffer.writeWindow();
    }

    size_t len = GEODE_UNWRAP(socket.receive(wnd.data(), wnd.size()));

    if (len == 0) {
        return Err(TransportError::Closed);
    }

    buffer.advanceWrite(len);

    // see if we can decode a message from the buffer
    if (buffer.size() < sizeof(uint32_t)) {
        // not enough data to read the length
        return Ok();
    }

    uint8_t lenbuf[sizeof(uint32_t)];
    buffer.peek(lenbuf, sizeof(uint32_t));

    size_t length = ByteReader{lenbuf, sizeof(uint32_t)}.readU32().unwrap();

    if (length == 0) {
        return Err(TransportError::ZeroLengthMessage);
    } else if (messageSizeLimit && length > messageSizeLimit) {
        log::warn("Received message larger than limit: {} > {}", length, messageSizeLimit);
        return Err(TransportError::MessageTooLong);
    }

    size_t totalLen = sizeof(uint32_t) + length;
    if (buffer.size() >= totalLen) {
        // we have a full message in the buffer
        buffer.skip(sizeof(uint32_t));
        auto wrpread = buffer.peek(length);
        buffer.skip(length);

        ByteReader reader = ByteReader::withTwoSpans(wrpread.first, wrpread.second);
        auto msg = QunetMessage::decode(reader);

        msgQueue.push(GEODE_UNWRAP(msg));
    }

    return Ok();
}

}
