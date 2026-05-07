#include <qunet/socket/message/QunetMessage.hpp>
#include <qunet/protocol/constants.hpp>

#define MAP_UNWRAP(x) GEODE_UNWRAP((x).mapErr([](const auto& err) { return MessageDecodeError::InvalidData; }))

namespace qn {

std::string_view MessageDecodeError::message() const {
    switch (m_code) {
        case InvalidMessageType: return "Invalid message type";
        case InvalidData: return "Invalid data in message";
        case InvalidConnCtlCode: return "Invalid connection control code";
        case NotEnoughData: return "Not enough data to decode message";
    }

    return "Unknown error";
}
MessageEncodeResult QunetMessage::encodeControlHeader(
    dbuf::ByteWriter<>& writer,
    uint64_t connectionId
) const {
    writer.writeU8(this->headerByte());

    if (connectionId != 0) {
        // write the connection ID (udp)
        writer.writeU64(connectionId);
    }

    return Ok();
}

MessageEncodeResult QunetMessage::encodeControlMsg(dbuf::ByteWriter<>& writer, uint64_t connectionId) const {
    GEODE_UNWRAP(this->encodeControlHeader(writer, connectionId));
    return this->encode(writer);
}

MessageEncodeResult QunetMessage::encodeDataHeader(dbuf::ByteWriter<>& writer, uint64_t connectionId, bool omitHeaders) const {
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
    FOR_MSG(ClientCloseMessage)
    FOR_MSG(ServerCloseMessage)
    FOR_MSG(ClientReconnectMessage)
    FOR_MSG(ConnectionErrorMessage)
    // FOR_MSG(QdbChunkRequestMessage)
    // FOR_MSG(QdbChunkResponseMessage)
    FOR_MSG(ReconnectSuccessMessage)
    FOR_MSG(ReconnectFailureMessage)
    FOR_MSG(ConnectionControlMessage)
    // FOR_MSG(QdbgToggleMessage)
    // FOR_MSG(QdbgReportMessage)
    FOR_MSG(DataMessage)
    /* else */ {
        return "UnknownMessageType";
    }
#undef FOR_MSG
}

uint8_t QunetMessage::headerByte() const {
#define FOR_MSG(t, v) if (this->is<t>()) { return v; } else
    FOR_MSG(DataMessage, MSG_DATA)
    // FOR_MSG(PingMessage, MSG_PING)
    // FOR_MSG(PongMessage, MSG_PONG)
    FOR_MSG(KeepaliveMessage, MSG_KEEPALIVE)
    FOR_MSG(KeepaliveResponseMessage, MSG_KEEPALIVE_RESPONSE)
    FOR_MSG(HandshakeStartMessage, MSG_HANDSHAKE_START)
    FOR_MSG(HandshakeFinishMessage, MSG_HANDSHAKE_FINISH)
    FOR_MSG(HandshakeFailureMessage, MSG_HANDSHAKE_FAILURE)
    FOR_MSG(ClientCloseMessage, MSG_CLIENT_CLOSE)
    FOR_MSG(ServerCloseMessage, MSG_SERVER_CLOSE)
    FOR_MSG(ClientReconnectMessage, MSG_CLIENT_RECONNECT)
    FOR_MSG(ConnectionErrorMessage, MSG_CONNECTION_ERROR)
    // FOR_MSG(QdbChunkRequestMessage, MSG_QDB_CHUNK_REQUEST)
    // FOR_MSG(QdbChunkResponseMessage, MSG_QDB_CHUNK_RESPONSE)
    // FOR_MSG(ReconnectSuccessMessage, MSG_RECONNECT_SUCCESS)
    // FOR_MSG(ReconnectFailureMessage, MSG_RECONNECT_FAILURE)
    // FOR_MSG(QdbgToggleMessage, MSG_QDBG_TOGGLE)
    // FOR_MSG(QdbgReportMessage, MSG_QDBG_REPORT)
    FOR_MSG(ConnectionControlMessage, MSG_CONNECTION_CONTROL)
    /* else */ {
        QN_ASSERT(false && "unsupported message in headerByte");
    }
#undef FOR_MSG
}

Result<QunetMessage, MessageDecodeError> QunetMessage::decodeWithMeta(QunetMessageMeta&& meta) {
    auto msgType = meta.type;

    if (msgType & MSG_DATA_MASK) {
        QN_DEBUG_ASSERT(false && "QunetMessage::decodeWithMeta does not handle data messages");
        return Err(MessageDecodeError::InvalidMessageType);
    }

    auto reader = dbuf::ByteReader{meta.data};

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

        case MSG_CONNECTION_CONTROL: {
            return Ok(MAP_UNWRAP(ConnectionControlMessage::decode(reader)));
        } break;

        case MSG_PADDING: {
            return Ok(PaddingMessage{});
        } break;
    }

    return Err(MessageDecodeError::InvalidMessageType);
}

static Result<size_t, MessageDecodeError> lengthForNonDataMessage(dbuf::ByteReader<>& reader, uint8_t headerByte) {
    QN_ASSERT(PROTOCOL_VERSION.major == 1 && PROTOCOL_VERSION.minor == 2);

    switch (headerByte) {
        case MSG_PING: return Ok(5);
        case MSG_PONG: {
            size_t total = 0;

            total += 4;
            MAP_UNWRAP(reader.skip(4));

            size_t protocols = MAP_UNWRAP(reader.readU8());
            total += 1 + 3 * protocols;
            MAP_UNWRAP(reader.skip(3 * protocols));

            auto dataLen = MAP_UNWRAP(reader.readU16());
            total += 2 + dataLen;
            MAP_UNWRAP(reader.skip(dataLen));

            return Ok(total);
        } break;

        case MSG_KEEPALIVE: return Ok(9);
        case MSG_KEEPALIVE_RESPONSE: {
            size_t total = 0;

            total += 8;
            MAP_UNWRAP(reader.skip(8));

            auto dataLen = MAP_UNWRAP(reader.readU16());
            total += 2 + dataLen;
            MAP_UNWRAP(reader.skip(dataLen));

            return Ok(total);
        } break;

        case MSG_HANDSHAKE_START: return Ok(2 + 2 + 2 + 16);

        case MSG_HANDSHAKE_FINISH: {
            size_t total = 0;

            total += 8;
            MAP_UNWRAP(reader.skip(8));

            total += 1;
            if (MAP_UNWRAP(reader.readBool())) {
                total += 12;
                MAP_UNWRAP(reader.skip(12));

                auto len = MAP_UNWRAP(reader.readU32());
                total += 4 + len;
                MAP_UNWRAP(reader.skip(len));
            }

            return Ok(total);
        } break;

        case MSG_HANDSHAKE_FAILURE: {
            MAP_UNWRAP(reader.skip(4));
            auto len = MAP_UNWRAP(reader.readU16());
            MAP_UNWRAP(reader.skip(len));

            return Ok(4 + 2 + len);
        } break;

        case MSG_CLIENT_CLOSE: return Ok(1);

        case MSG_SERVER_CLOSE: {
            MAP_UNWRAP(reader.skip(4));
            auto len = MAP_UNWRAP(reader.readU16());
            MAP_UNWRAP(reader.skip(len));

            return Ok(4 + 2 + len);
        } break;

        case MSG_CLIENT_RECONNECT: return Ok(8);
        case MSG_CONNECTION_ERROR: return Ok(4);
        case MSG_QDB_CHUNK_REQUEST: return Ok(8);
        case MSG_QDB_CHUNK_RESPONSE: {
            MAP_UNWRAP(reader.skip(4));
            auto len = MAP_UNWRAP(reader.readU32());
            MAP_UNWRAP(reader.skip(len));

            return Ok(4 + 4 + len);
        } break;

        case MSG_RECONNECT_SUCCESS: return Ok(0);
        case MSG_RECONNECT_FAILURE: return Ok(0);

        case MSG_CONNECTION_CONTROL: {
            auto code = MAP_UNWRAP(reader.readU16());

            size_t msgSize = 0;
            switch (code) {
                case QUNET_CONNCTL_SET_MTU: {
                    msgSize = 2;
                    MAP_UNWRAP(reader.skip(2));
                } break;

                case QUNET_CONNCTL_PMTUD_PROBE: {
                    auto len = MAP_UNWRAP(reader.readU16());
                    msgSize = 2 + len;
                    MAP_UNWRAP(reader.skip(len));
                } break;

                default: return Err(MessageDecodeError::InvalidConnCtlCode);
            }

            return Ok(2 + msgSize);
        } break;

        case MSG_PADDING: return Ok(reader.remainingSize());
    }

    return Err(MessageDecodeError::InvalidMessageType);
}

Result<QunetMessageMeta, MessageDecodeError> QunetMessage::decodeMeta(std::span<const uint8_t> data, bool udpHeaders, bool udpConnId) {
    QN_ASSERT(!data.empty());

    auto msgType = data[0];
    size_t dataStart = udpConnId ? 9 : 1;

    if (data.size() < dataStart) {
        return Err(MessageDecodeError::NotEnoughData);
    }

    data = data.subspan(dataStart);
    dbuf::ByteReader<> reader(data);

    if ((msgType & MSG_DATA_MASK) == 0) {
        // control message
        auto dataSize = GEODE_UNWRAP(lengthForNonDataMessage(reader, msgType));

        if (data.size() < dataSize) {
            return Err(MessageDecodeError::NotEnoughData);
        }
        data = data.subspan(0, dataSize);

        // not a data message
        std::vector<uint8_t> msgData;
        msgData.assign_range(data);
        return Ok(QunetMessageMeta {
            .type = msgType,
            .data = std::move(msgData),
            .dataOffset = dataStart,
        });
    }

    // data message, parse headers
    QunetMessageMeta meta;
    meta.dataOffset = dataStart;
    meta.type = MSG_DATA;
    bool fragmented = (msgType & MSG_DATA_FRAGMENTATION_MASK) != 0;
    bool reliable = (msgType & MSG_DATA_RELIABILITY_MASK) != 0;
    bool hasBoundary = (msgType & MSG_DATA_BOUNDARY_MASK) != 0;
    CompressionType compressionType = (CompressionType) ((msgType >> MSG_DATA_BIT_COMPRESSION_1) & 0b11);

    switch (compressionType) {
        case CompressionType::Zstd:
        case CompressionType::ZstdNoDict:
        case CompressionType::Lz4: {
            meta.compressionHeader = CompressionHeader {
                .type = compressionType,
                .uncompressedSize = MAP_UNWRAP(reader.readU32()),
            };

            meta.dataOffset += 4;
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
        meta.dataOffset += 4 + relHdr.ackCount * 2;
    }

    if (fragmented) {
        FragmentationHeader fragHdr;
        fragHdr.messageId = MAP_UNWRAP(reader.readU16());
        fragHdr.fragmentIndex = MAP_UNWRAP(reader.readU16());

        // top bit of fragmentIndex indicates if this is the last fragment
        fragHdr.lastFragment = (fragHdr.fragmentIndex & (uint16_t)0x8000) != 0;
        fragHdr.fragmentIndex &= (uint16_t)0x7FFF;

        meta.fragmentationHeader = fragHdr;
        meta.dataOffset += 4;
    }

    std::optional<uint16_t> boundary;
    if (hasBoundary) {
        boundary = MAP_UNWRAP(reader.readU16());
        meta.dataOffset += 2;
    }

    meta.data = reader.readToEnd();

    if (boundary) {
        if (meta.data.size() < *boundary) {
            return Err(MessageDecodeError::InvalidData);
        }

        meta.data.resize(*boundary);
    }

    return Ok(std::move(meta));
}

QunetUdpMessageIter::QunetUdpMessageIter(std::span<const uint8_t> data, bool server) : m_data(data), m_server(server) {}

std::optional<QunetUdpMessageIter::Item> QunetUdpMessageIter::next() {
    if (m_eof || m_pos >= m_data.size()) {
        return std::nullopt;
    }

    auto data = m_data.subspan(m_pos);
    bool expectConnId = m_pos == 0 && m_server;
    auto meta = GEODE_UNWRAP(QunetMessage::decodeMeta(data, true, expectConnId));

    m_pos += meta.dataOffset + meta.data.size();

    return Ok(std::move(meta));
}

}