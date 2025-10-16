#pragma once

#include <qunet/socket/transport/BaseTransport.hpp>
#include <qunet/buffers/CircularByteBuffer.hpp>
#include <qunet/socket/transport/Error.hpp>
#include <qunet/socket/message/QunetMessage.hpp>
#include <qunet/Log.hpp>
#include <queue>

// Common operations for stream-like transport layers (TCP, QUIC, etc.)

namespace qn::streamcommon {

inline TransportResult<> sendMessage(QunetMessage message, auto&& socket, BaseTransport& transport) {
    HeapByteWriter writer;

    bool hasLength = !(message.is<HandshakeStartMessage>() || message.is<ClientReconnectMessage>());

    // Leave space for message length
    size_t preMessagePos = 0;
    if (hasLength) {
        writer.writeU32(0);
        preMessagePos = writer.position();
    }

    auto prependLength = [&] {
        if (hasLength) {
            uint32_t messageSize = writer.position() - preMessagePos;

            writer.performAt(preMessagePos - sizeof(uint32_t), [&](auto& writer) {
                writer.writeU32(messageSize);
            }).unwrap();
        }
    };

    // non-data messages cannot be compressed, fragmented or reliable, so steps are simple here
    if (!message.is<DataMessage>()) {
        GEODE_UNWRAP(message.encodeControlMsg(writer, 0));

        // write message length
        prependLength();

        auto data = writer.written();
        GEODE_UNWRAP(socket.sendAll(data.data(), data.size()));

        transport._tracker().onUpMessage(data[0], data.size());
        transport._tracker().onUpPacket(data.size());

        return Ok();
    }

    auto& msg = message.as<DataMessage>();

    // unlike udp, we don't have fragmentation or reliability worries here, only compression
    QN_DEBUG_ASSERT(!msg.relHeader && "message must not have reliability header for stream transports");

    message.encodeDataHeader(writer, 0, false).unwrap();
    writer.writeBytes(msg.data);

    // write message length
    prependLength();

    auto data = writer.written();
    GEODE_UNWRAP(socket.sendAll(data.data(), data.size()));

    transport._tracker().onUpMessage(data[0], msg.data.size());
    transport._tracker().onUpPacket(data.size());

    return Ok();
}

// The logic here is similar to the one in rust implementation
inline TransportResult<> processIncomingData(
    auto&& socket,
    BaseTransport& transport,
    CircularByteBuffer& buffer,
    size_t messageSizeLimit,
    std::queue<QunetMessage>& msgQueue,
    size_t& unackedKeepalives
) {
    // read from the socket if applicable
    auto wnd = buffer.writeWindow();
    if (wnd.size() < 2048) {
        // reserve more space if needed, but error if too much space is already reserved
        if (buffer.capacity() >= 1024 * 1024) {
            log::warn("processIncomingData: too much space reserved in buffer, capacity: {}, write window size: {}", buffer.capacity(), wnd.size());
            return Err(TransportError::NoBufferSpace);
        }

        buffer.reserve(2048);
        wnd = buffer.writeWindow();
    }

    size_t len = GEODE_UNWRAP(socket.receive(wnd.data(), wnd.size()));

    if (len == 0) {
        return Err(TransportError::Closed);
    }

    transport._tracker().onDownPacket(len);

    buffer.advanceWrite(len);

    // decode messages until we have nothing left
    while (true) {
        if (buffer.size() < sizeof(uint32_t)) {
            // not enough data to read the length
            break;
        }

        uint8_t lenbuf[sizeof(uint32_t)];
        buffer.peek(lenbuf, sizeof(uint32_t));

        size_t length = ByteReader{lenbuf, sizeof(uint32_t)}.readU32().unwrap();

        if (length == 0) {
            // TODO: idk if its worth erroring here?
            // return Err(TransportError::ZeroLengthMessage);
            buffer.skip(sizeof(uint32_t));
            continue;
        } else if (messageSizeLimit && length > messageSizeLimit) {
            log::warn("Received message larger than limit: {} > {}", length, messageSizeLimit);
            return Err(TransportError::MessageTooLong);
        }

        size_t totalLen = sizeof(uint32_t) + length;
        if (buffer.size() < totalLen) {
            break; // not enough data
        }

        // we have a full message in the buffer
        buffer.skip(sizeof(uint32_t));
        auto wrpread = buffer.peek(length);

        ByteReader reader = ByteReader::withTwoSpans(wrpread.first, wrpread.second);
        auto dec = [&]() -> TransportResult<> {
            auto meta = GEODE_UNWRAP(QunetMessage::decodeMeta(reader));

            if (meta.type != MSG_DATA) {
                auto msg = GEODE_UNWRAP(QunetMessage::decodeWithMeta(std::move(meta)));

                if (msg.is<KeepaliveResponseMessage>()) {
                    unackedKeepalives = 0;
                }

                transport._tracker().onDownMessage(wrpread.first[0], length);
                transport._pushFinalControlMessage(std::move(msg));
                return Ok();
            }

            return transport._pushPreFinalDataMessage(std::move(meta));
        };

        auto res = dec();
        buffer.skip(length);
        GEODE_UNWRAP(res);
    }

    return Ok();
}

}
