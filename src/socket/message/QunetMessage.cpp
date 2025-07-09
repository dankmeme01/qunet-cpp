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
MessageEncodeResult QunetMessage::encodeHeader(
    HeapByteWriter& writer,
    uint64_t connectionId
) const {
    // Write the header byte
    std::visit(makeVisitor {
        [&](const PingMessage& msg) {
            return writer.writeU8(MSG_PING);
        },
        [&](const PongMessage& msg) {
            return writer.writeU8(MSG_PONG);
        },
        [&](const KeepaliveMessage& msg) {
            return writer.writeU8(MSG_KEEPALIVE);
        },
        [&](const KeepaliveResponseMessage& msg) {
            return writer.writeU8(MSG_KEEPALIVE_RESPONSE);
        },
        [&](const HandshakeStartMessage& msg) {
            return writer.writeU8(MSG_HANDSHAKE_START);
        },
        [&](const HandshakeFinishMessage& msg) {
            return writer.writeU8(MSG_HANDSHAKE_FINISH);
        },
        [&](const HandshakeFailureMessage& msg) {
            return writer.writeU8(MSG_HANDSHAKE_FAILURE);
        },
        [&](const ClientCloseMessage& msg) {
            return writer.writeU8(MSG_CLIENT_CLOSE);
        },
        [&](const ServerCloseMessage& msg) {
            return writer.writeU8(MSG_SERVER_CLOSE);
        },
        [&](const ClientReconnectMessage& msg) {
            return writer.writeU8(MSG_CLIENT_RECONNECT);
        },
        [&](const ConnectionErrorMessage& msg) {
            return writer.writeU8(MSG_CONNECTION_ERROR);
        },
        [&](const QdbChunkRequestMessage& msg) {
            return writer.writeU8(MSG_QDB_CHUNK_REQUEST);
        },
        [&](const QdbChunkResponseMessage& msg) {
            return writer.writeU8(MSG_QDB_CHUNK_RESPONSE);
        },
        [&](const QdbgToggleMessage& msg) {
            return writer.writeU8(MSG_QDBG_TOGGLE);
        },
        [&](const QdbgReportMessage& msg) {
            return writer.writeU8(MSG_QDBG_REPORT);
        },
        [&](const DataMessage& msg) {
            return writer.writeU8(MSG_DATA);
        }
    }, m_kind);

    // TODO compression header

    if (connectionId != 0) {
        // write the connection ID (udp)
        writer.writeU64(connectionId);
    }

    return Ok();
}
std::string_view QunetMessage::typeStr() const {
    return std::visit(makeVisitor {
        [&](const PingMessage& msg) {
            return "PingMessage";
        },
        [&](const PongMessage& msg) {
            return "PongMessage";
        },
        [&](const KeepaliveMessage& msg) {
            return "KeepaliveMessage";
        },
        [&](const KeepaliveResponseMessage& msg) {
            return "KeepaliveResponseMessage";
        },
        [&](const HandshakeStartMessage& msg) {
            return "HandshakeStartMessage";
        },
        [&](const HandshakeFinishMessage& msg) {
            return "HandshakeFinishMessage";
        },
        [&](const HandshakeFailureMessage& msg) {
            return "HandshakeFailureMessage";
        },
        [&](const ClientCloseMessage& msg) {
            return "ClientCloseMessage";
        },
        [&](const ServerCloseMessage& msg) {
            return "ServerCloseMessage";
        },
        [&](const ClientReconnectMessage& msg) {
            return "ClientReconnectMessage";
        },
        [&](const ConnectionErrorMessage& msg) {
            return "ConnectionErrorMessage";
        },
        [&](const QdbChunkRequestMessage& msg) {
            return "QdbChunkRequestMessage";
        },
        [&](const QdbChunkResponseMessage& msg) {
            return "QdbChunkResponseMessage";
        },
        [&](const QdbgToggleMessage& msg) {
            return "QdbgToggleMessage";
        },
        [&](const QdbgReportMessage& msg) {
            return "QdbgReportMessage";
        },
        [&](const DataMessage& msg) {
            return "DataMessage";
        }
    }, m_kind);
}

geode::Result<QunetMessage, MessageDecodeError> QunetMessage::decodeWithMeta(QunetMessageMeta&& meta) {
    auto msgType = meta.type;

    if (msgType == MSG_DATA) {
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
    }

    return Err(MessageDecodeError::InvalidMessageType);
}

static geode::Result<QunetMessageMeta, MessageDecodeError> decodeMeta(ByteReader& reader) {
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
    bool fragmented = (msgType & MSG_DATA_BIT_FRAGMENTATION) != 0;
    bool reliable = (msgType & MSG_DATA_BIT_RELIABILITY) != 0;
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
}

}