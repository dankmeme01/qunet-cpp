#pragma once

#include <qunet/socket/transport/Error.hpp>
#include <qunet/socket/message/QunetMessage.hpp>
#include <qunet/Log.hpp>

// Common operations for stream-like transport layers (TCP, QUIC, etc.)

namespace qn::streamcommon {

TransportResult<> sendMessage(QunetMessage message, auto&& socket) {
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

// The logic here is pretty much taken from stream transports in the qunet server
TransportResult<QunetMessage> receiveMessage(auto&& socket, std::vector<uint8_t>& readBufferStorage, size_t& readBufferPos, size_t messageSizeLimit) {
    auto readBuffer = std::span<uint8_t>(readBufferStorage.begin(), readBufferStorage.end());

    while (true) {
        // first, try to parse a message from the buffer
        if (readBufferPos >= sizeof(uint32_t)) {
            size_t length = ByteReader{readBuffer.subspan(0, sizeof(uint32_t))}.readU32().unwrap();

            if (length == 0) {
                return Err(TransportError::ZeroLengthMessage);
            } else if (messageSizeLimit && length > messageSizeLimit) {
                log::warn("Received message larger than limit: {} > {}", length, messageSizeLimit);
                return Err(TransportError::MessageTooLong);
            }

            size_t totalLen = sizeof(uint32_t) + length;
            if (readBufferPos >= totalLen) {
                // we have a full message in the buffer
                auto data = readBuffer.subspan(sizeof(uint32_t), length);

                ByteReader reader(data);
                auto msg = QunetMessage::decode(reader);

                // shift leftover bytes in the buffer
                auto leftoverBytes = readBufferPos - totalLen;
                if (leftoverBytes > 0) {
                    std::memmove(readBuffer.data(), readBuffer.data() + totalLen, leftoverBytes);
                }
                readBufferPos = leftoverBytes;

                return Ok(GEODE_UNWRAP(msg));
            }

            // if there is not enough data but we know the length, check if additional space needs to be allocated
            if (totalLen > readBuffer.size()) {
                // geometric growth
                size_t newSize = std::max(totalLen, readBuffer.size() * 2);
                readBufferStorage.resize(newSize);
                readBuffer = std::span<uint8_t>(readBufferStorage.begin(), readBufferStorage.end());
            }
        }

        // read from the socket
        size_t len = GEODE_UNWRAP(socket.receive(readBuffer.data() + readBufferPos, readBuffer.size() - readBufferPos));

        if (len == 0) {
            return Err(qsox::Error{qsox::Error::ConnectionClosed});
        }

        readBufferPos += len;
    }
}

}
