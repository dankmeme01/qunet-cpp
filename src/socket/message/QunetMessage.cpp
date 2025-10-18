#include <qunet/socket/message/QunetMessage.hpp>
#include <qunet/protocol/constants.hpp>

#define MAP_UNWRAP(x) GEODE_UNWRAP((x).mapErr([](const auto& err) { return MessageDecodeError::InvalidData; }))

namespace qn {

std::string_view MessageDecodeError::message() const {
    switch (m_code) {
        case InvalidMessageType: return "Invalid message type";
        case InvalidData: return "Invalid data in message";
    }
}
MessageEncodeResult QunetMessage::encodeControlHeader(
    HeapByteWriter& writer,
    uint64_t connectionId
) const {
#define FOR_MSG(t, v) if (this->is<t>()) { writer.writeU8(v); } else

    // Write the header byte
    // FOR_MSG(PingMessage, MSG_PING)
    // FOR_MSG(PongMessage, MSG_PONG)
    FOR_MSG(KeepaliveMessage, MSG_KEEPALIVE)
    FOR_MSG(KeepaliveResponseMessage, MSG_KEEPALIVE_RESPONSE)
    FOR_MSG(HandshakeStartMessage, MSG_HANDSHAKE_START)
    FOR_MSG(HandshakeFinishMessage, MSG_HANDSHAKE_FINISH)
    FOR_MSG(HandshakeFailureMessage, MSG_HANDSHAKE_FAILURE)
    // FOR_MSG(ClientCloseMessage, MSG_CLIENT_CLOSE)
    FOR_MSG(ServerCloseMessage, MSG_SERVER_CLOSE)
    FOR_MSG(ClientReconnectMessage, MSG_CLIENT_RECONNECT)
    FOR_MSG(ConnectionErrorMessage, MSG_CONNECTION_ERROR)
    // FOR_MSG(QdbChunkRequestMessage, MSG_QDB_CHUNK_REQUEST)
    // FOR_MSG(QdbChunkResponseMessage, MSG_QDB_CHUNK_RESPONSE)
    // FOR_MSG(ReconnectSuccessMessage, MSG_RECONNECT_SUCCESS)
    // FOR_MSG(ReconnectFailureMessage, MSG_RECONNECT_FAILURE)
    // FOR_MSG(QdbgToggleMessage, MSG_QDBG_TOGGLE)
    // FOR_MSG(QdbgReportMessage, MSG_QDBG_REPORT)
    /* else */ {
        QN_ASSERT(false && "unsupported message in encodeHeader");
    }
#undef FOR_MSG

    if (connectionId != 0) {
        // write the connection ID (udp)
        writer.writeU64(connectionId);
    }

    return Ok();
}

MessageEncodeResult QunetMessage::encodeControlMsg(HeapByteWriter& writer, uint64_t connectionId) const {
    GEODE_UNWRAP(this->encodeControlHeader(writer, connectionId));
    return this->encode(writer);
}

MessageEncodeResult QunetMessage::encodeDataHeader(HeapByteWriter& writer, uint64_t connectionId, bool omitHeaders) const {
    QN_ASSERT(this->is<DataMessage>());

    auto& msg = this->as<DataMessage>();

    uint8_t hdr = MSG_DATA;

    if (!omitHeaders) {
        // set compression bits
        if (msg.compHeader.has_value()) {
            hdr |= (uint8_t)msg.compHeader->type << MSG_DATA_BIT_COMPRESSION_1;
        }

        // set reliability bit
        if (msg.relHeader.has_value()) {
            hdr |= MSG_DATA_RELIABILITY_MASK;
        }
    }

    // write the header byte
    writer.writeU8(hdr);

    if (!omitHeaders) {
        // write compression header
        if (msg.compHeader.has_value()) {
            writer.writeU32(msg.compHeader->uncompressedSize);
        }

        // write connection ID
        if (connectionId != 0) {
            writer.writeU64(connectionId);
        }

        // write reliability header
        if (msg.relHeader.has_value()) {
            auto& relHdr = msg.relHeader.value();
            writer.writeU16(relHdr.messageId);
            writer.writeU16(relHdr.ackCount);

            for (size_t i = 0; i < relHdr.ackCount && i < 8; i++) {
                writer.writeU16(relHdr.acks[i]);
            }
        }
    } else {
        // write connection ID
        if (connectionId != 0) {
            writer.writeU64(connectionId);
        }
    }

    return Ok();
}

std::string_view QunetMessage::typeStr() const {
#define FOR_MSG(t) if (this->is<t>()) { return #t; } else

    // FOR_MSG(PingMessage)
    // FOR_MSG(PongMessage)
    FOR_MSG(KeepaliveMessage)
    FOR_MSG(KeepaliveResponseMessage)
    FOR_MSG(HandshakeStartMessage)
    FOR_MSG(HandshakeFinishMessage)
    FOR_MSG(HandshakeFailureMessage)
    // FOR_MSG(ClientCloseMessage)
    FOR_MSG(ServerCloseMessage)
    FOR_MSG(ClientReconnectMessage)
    FOR_MSG(ConnectionErrorMessage)
    // FOR_MSG(QdbChunkRequestMessage)
    // FOR_MSG(QdbChunkResponseMessage)
    FOR_MSG(ReconnectSuccessMessage)
    FOR_MSG(ReconnectFailureMessage)
    // FOR_MSG(QdbgToggleMessage)
    // FOR_MSG(QdbgReportMessage)
    /* else */ {
        return "UnknownMessageType";
    }
#undef FOR_MSG
}

geode::Result<QunetMessage, MessageDecodeError> QunetMessage::decodeWithMeta(QunetMessageMeta&& meta) {
    auto msgType = meta.type;

    if (msgType & MSG_DATA_MASK) {
        QN_DEBUG_ASSERT(false && "QunetMessage::decodeWithMeta does not handle data messages");
        return Err(MessageDecodeError::InvalidMessageType);
    }

    auto reader = ByteReader{meta.data};

    switch (msgType) {
        case MSG_KEEPALIVE: {
            return Ok(MAP_UNWRAP(KeepaliveMessage::decode(reader)));
        } break;

        case MSG_KEEPALIVE_RESPONSE: {
            return Ok(MAP_UNWRAP(KeepaliveResponseMessage::decode(reader)));
        } break;

        case MSG_HANDSHAKE_FINISH: {
            return Ok(MAP_UNWRAP(HandshakeFinishMessage::decode(reader)));
        } break;

        case MSG_HANDSHAKE_FAILURE: {
            return Ok(MAP_UNWRAP(HandshakeFailureMessage::decode(reader)));
        } break;

        case MSG_SERVER_CLOSE: {
            return Ok(MAP_UNWRAP(ServerCloseMessage::decode(reader)));
        } break;

        case MSG_CONNECTION_ERROR: {
            return Ok(MAP_UNWRAP(ConnectionErrorMessage::decode(reader)));
        } break;

        case MSG_RECONNECT_SUCCESS: {
            return Ok(MAP_UNWRAP(ReconnectSuccessMessage::decode(reader)));
        } break;

        case MSG_RECONNECT_FAILURE: {
            return Ok(MAP_UNWRAP(ReconnectFailureMessage::decode(reader)));
        } break;
    }

    return Err(MessageDecodeError::InvalidMessageType);
}

geode::Result<QunetMessageMeta, MessageDecodeError> QunetMessage::decodeMeta(ByteReader& reader) {
    auto msgType = MAP_UNWRAP(reader.readU8());

    if ((msgType & MSG_DATA_MASK) == 0) {
        // not a data message
        return Ok(QunetMessageMeta {
            .type = msgType,
            .data = reader.readToEnd()
        });
    }

    // data message, parse headers
    QunetMessageMeta meta;
    meta.type = MSG_DATA;
    bool fragmented = (msgType & MSG_DATA_FRAGMENTATION_MASK) != 0;
    bool reliable = (msgType & MSG_DATA_RELIABILITY_MASK) != 0;
    CompressionType compressionType = (CompressionType) ((msgType >> MSG_DATA_BIT_COMPRESSION_1) & 0b11);

    switch (compressionType) {
        case CompressionType::Zstd: {
            meta.compressionHeader = CompressionHeader {
                .type = CompressionType::Zstd,
                .uncompressedSize = MAP_UNWRAP(reader.readU32()),
            };
        } break;

        case CompressionType::Lz4: {
            meta.compressionHeader = CompressionHeader {
                .type = CompressionType::Lz4,
                .uncompressedSize = MAP_UNWRAP(reader.readU32()),
            };
        } break;

        default: break;
    }

    if (reliable) {
        ReliabilityHeader relHdr;
        relHdr.messageId = MAP_UNWRAP(reader.readU16());
        relHdr.ackCount = MAP_UNWRAP(reader.readU16());

        for (size_t i = 0; i < std::min<size_t>(relHdr.ackCount, 8); i++) {
            relHdr.acks[i] = MAP_UNWRAP(reader.readU16());
        }

        meta.reliabilityHeader = relHdr;
    }

    if (fragmented) {
        FragmentationHeader fragHdr;
        fragHdr.messageId = MAP_UNWRAP(reader.readU16());
        fragHdr.fragmentIndex = MAP_UNWRAP(reader.readU16());

        // top bit of fragmentIndex indicates if this is the last fragment
        fragHdr.lastFragment = (fragHdr.fragmentIndex & (uint16_t)0x8000) != 0;
        fragHdr.fragmentIndex &= (uint16_t)0x7FFF;

        meta.fragmentationHeader = fragHdr;
    }

    meta.data = reader.readToEnd();

    return Ok(std::move(meta));
}

}