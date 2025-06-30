#pragma once
#include <qunet/buffers/ByteWriter.hpp>
#include <qunet/buffers/HeapByteWriter.hpp>
#include <qunet/buffers/ByteReader.hpp>
#include <qunet/protocol/constants.hpp>
#include <qunet/protocol/errors.hpp>
#include <qunet/util/assert.hpp>
#include <fmt/format.h>

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

struct KeepaliveMessage {
    uint64_t timestamp;

    MessageEncodeResult encode(auto& writer) const {
        writer.writeU64(timestamp);
        writer.writeU8(0);
        return Ok();
    }

    QN_NO_DECODE(KeepaliveMessage);
};

struct KeepaliveResponseMessage {
    uint64_t timestamp;
    std::vector<uint8_t> data;

    QN_NO_ENCODE(KeepaliveResponseMessage);

    static MessageDecodeResult<KeepaliveResponseMessage> decode(ByteReader& reader) {
        KeepaliveResponseMessage out;

        out.timestamp = GEODE_UNWRAP(reader.readU64());
        auto dataSize = GEODE_UNWRAP(reader.readU16());

        if (dataSize > 0) {
            out.data.resize(dataSize);
            GEODE_UNWRAP(reader.readBytes(out.data.data(), dataSize));
        }

        return Ok(std::move(out));
    }
};

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
        if (errorCode == 0) {
            return errorMessage;
        }

        auto err = messageForHandshakeError(errorCode);
        if (err.empty()) {
            const_cast<std::string&>(errorMessage) = fmt::format("Unknown handshake error code: {}", errorCode);
            return errorMessage;
        } else {
            return err;
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

struct ServerCloseMessage {
    uint32_t errorCode;
    std::string errorMessage;

    std::string_view message() const {
        if (errorCode == 0) {
            return errorMessage;
        }

        auto err = messageForServerCloseError(errorCode);
        if (err.empty()) {
            const_cast<std::string&>(errorMessage) = fmt::format("Unknown server close error code: {}", errorCode);
            return errorMessage;
        } else {
            return err;
        }
    }

    static MessageDecodeResult<ServerCloseMessage> decode(ByteReader& writer) {
        ServerCloseMessage out;
        out.errorCode = GEODE_UNWRAP(writer.readU32());
        if (out.errorCode == 0) {
            out.errorMessage = GEODE_UNWRAP(writer.readString());
        }

        return Ok(std::move(out));
    }

    QN_NO_ENCODE(ServerCloseMessage);
};

struct ClientReconnectMessage {};

struct ConnectionErrorMessage {
    uint32_t errorCode;

    std::string_view message() const {
        auto err = messageForConnectionError(errorCode);
        if (err.empty()) {
            static thread_local auto message = fmt::format("Unknown server close error code: {}", errorCode);
            return message;
        } else {
            return err;
        }
    }

    MessageEncodeResult encode(auto& writer) const {
        writer.writeU32(errorCode);
        return Ok();
    }

    static MessageDecodeResult<ConnectionErrorMessage> decode(ByteReader& writer) {
        return Ok(ConnectionErrorMessage {
            .errorCode = GEODE_UNWRAP(writer.readU32()),
        });
    }
};

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