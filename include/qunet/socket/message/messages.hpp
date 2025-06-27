#pragma once
#include <qunet/buffers/ByteWriter.hpp>
#include <qunet/buffers/HeapByteWriter.hpp>
#include <qunet/buffers/ByteReader.hpp>
#include <qunet/protocol/constants.hpp>
#include <qunet/util/assert.hpp>

#define QN_NO_ENCODE(t) MessageEncodeResult encode(auto& writer) const { \
    QN_ASSERT(false && "Message of type " #t " cannot be encoded"); \
}
#define QN_NO_DECODE(t) static MessageDecodeResult<t> decode(ByteReader& writer) { \
    QN_ASSERT(false && "Message of type " # t " cannot be decoded"); \
}

namespace qn {

using MessageEncodeResult = geode::Result<void, ByteWriterError>;
template <typename T>
using MessageDecodeResult = geode::Result<T, ByteReaderError>;

struct PingMessage {};

struct PongMessage {};

struct KeepaliveMessage {};

struct KeepaliveResponseMessage {};

struct HandshakeStartMessage {
    uint16_t majorVersion;
    uint16_t fragLimit;
    std::array<uint8_t, 16> qdbHash;

    MessageEncodeResult encode(auto& writer) const {
        writer.writeU16(majorVersion);
        writer.writeU16(fragLimit);
        writer.writeBytes(qdbHash.data(), qdbHash.size());
        return Ok();
    }

    QN_NO_DECODE(HandshakeStartMessage);
};

struct HandshakeFinishMessage {
    struct QdbData {
        uint32_t uncompressedSize;
        uint32_t compressedSize;
        uint32_t chunkOffset;
        uint32_t chunkSize;
        std::vector<uint8_t> chunkData;
    };

    uint64_t connectionId;
    std::optional<QdbData> qdbData;

    QN_NO_ENCODE(HandshakeFinishMessage);

    static MessageDecodeResult<HandshakeFinishMessage> decode(ByteReader& reader) {
        HandshakeFinishMessage out;

        out.connectionId = GEODE_UNWRAP(reader.readU64());
        if (reader.readBool()) {
            out.qdbData = QdbData{};
            auto& qdb = out.qdbData.value();

            qdb.uncompressedSize = GEODE_UNWRAP(reader.readU32());
            qdb.compressedSize = GEODE_UNWRAP(reader.readU32());
            qdb.chunkOffset = GEODE_UNWRAP(reader.readU32());
            qdb.chunkSize = GEODE_UNWRAP(reader.readU32());

            if (qdb.chunkSize > 0) {
                qdb.chunkData.resize(qdb.chunkSize);
                GEODE_UNWRAP(reader.readBytes(qdb.chunkData.data(), qdb.chunkSize));
            }
        }

        return Ok(std::move(out));
    }
};

struct HandshakeFailureMessage {
    uint32_t errorCode;
    std::string errorMessage;

    std::string_view message() const {
        switch (errorCode) {
            case 0: return errorMessage;
            case 1: return "Client qunet version is too old";
            case 2: return "Client qunet version is too new";
            case 3: return "Reconnect failed, unknown connection ID";
            default: return "Unknown error, invalid error code";
        }
    }

    static MessageDecodeResult<HandshakeFailureMessage> decode(ByteReader& writer) {
        HandshakeFailureMessage out;
        out.errorCode = GEODE_UNWRAP(writer.readU32());
        if (out.errorCode == 0) {
            out.errorMessage = GEODE_UNWRAP(writer.readString());
        }

        return Ok(std::move(out));
    }

    QN_NO_ENCODE(HandshakeFailureMessage);
};

struct ClientCloseMessage {};

struct ServerCloseMessage {};

struct ClientReconnectMessage {};

struct ConnectionErrorMessage {};

struct QdbChunkRequestMessage {};

struct QdbChunkResponseMessage {};

struct QdbgToggleMessage {};

struct QdbgReportMessage {};

struct DataMessage {
    MessageEncodeResult encode(auto& writer) const {
        return Ok();
    }

    static MessageDecodeResult<DataMessage> decode(ByteReader& reader) {
        return Ok(DataMessage{});
    }
};

}