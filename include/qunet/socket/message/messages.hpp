#pragma once
#include "meta.hpp"
#include <dbuf/ByteWriter.hpp>
#include <dbuf/ByteReader.hpp>
#include <qunet/protocol/constants.hpp>
#include <qunet/protocol/errors.hpp>
#include <qunet/util/assert.hpp>
#include <fmt/format.h>
#include <array>

#define QN_NO_ENCODE(t) MessageEncodeResult encode(auto& writer) const { \
    QN_ASSERT(false && "Message of type " #t " cannot be encoded"); \
}
#define QN_NO_DECODE(t) static MessageDecodeResult<t> decode(auto& writer) { \
    QN_ASSERT(false && "Message of type " # t " cannot be decoded"); \
}

namespace qn {

using MessageEncodeResult = geode::Result<void>;
template <typename T>
using MessageDecodeResult = geode::Result<T>;

struct PingMessage {};

struct PongMessage {};

struct KeepaliveMessage {
    uint64_t timestamp;

    MessageEncodeResult encode(auto& writer) const {
        writer.writeU64(timestamp);
        writer.writeU8(0);
        return geode::Ok();
    }

    QN_NO_DECODE(KeepaliveMessage);
};

struct KeepaliveResponseMessage {
    uint64_t timestamp;
    std::vector<uint8_t> data;

    QN_NO_ENCODE(KeepaliveResponseMessage);

    template <typename S>
    static MessageDecodeResult<KeepaliveResponseMessage> decode(dbuf::ByteReader<S>& reader) {
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
    ProtocolVersion protocolVersion;
    uint16_t fragLimit;
    std::array<uint8_t, 16> qdbHash;

    MessageEncodeResult encode(auto& writer) const {
        writer.writeU16(protocolVersion.major);
        writer.writeU16(protocolVersion.minor);
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

    static MessageDecodeResult<HandshakeFinishMessage> decode(dbuf::ByteReader<>& reader) {
        HandshakeFinishMessage out;

        out.connectionId = GEODE_UNWRAP(reader.readU64());
        if (GEODE_UNWRAP(reader.readBool())) {
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

    static MessageDecodeResult<HandshakeFailureMessage> decode(dbuf::ByteReader<>& writer) {
        HandshakeFailureMessage out;
        out.errorCode = GEODE_UNWRAP(writer.readU32());
        if (out.errorCode == 0) {
            out.errorMessage = GEODE_UNWRAP(writer.readString());
        }

        return Ok(std::move(out));
    }

    QN_NO_ENCODE(HandshakeFailureMessage);
};

struct ClientCloseMessage {
    MessageEncodeResult encode(auto& writer) const {
        uint8_t flags = 0;
        writer.writeU8(flags);
        return Ok();
    }

    QN_NO_DECODE(HandshakeStartMessage);
};

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

    static MessageDecodeResult<ServerCloseMessage> decode(dbuf::ByteReader<>& writer) {
        ServerCloseMessage out;
        out.errorCode = GEODE_UNWRAP(writer.readU32());
        if (out.errorCode == 0) {
            out.errorMessage = GEODE_UNWRAP(writer.readString());
        }

        return Ok(std::move(out));
    }

    QN_NO_ENCODE(ServerCloseMessage);
};

struct ClientReconnectMessage {
    uint64_t connectionId;

    MessageEncodeResult encode(auto& writer) const {
        writer.writeU64(connectionId);
        return Ok();
    }

    QN_NO_DECODE(ClientReconnectMessage);
};

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

    static MessageDecodeResult<ConnectionErrorMessage> decode(dbuf::ByteReader<>& writer) {
        return Ok(ConnectionErrorMessage {
            .errorCode = GEODE_UNWRAP(writer.readU32()),
        });
    }
};

struct QdbChunkRequestMessage {};

struct QdbChunkResponseMessage {};

struct ReconnectSuccessMessage {
    static MessageDecodeResult<ReconnectSuccessMessage> decode(dbuf::ByteReader<>& writer) {
        return Ok(ReconnectSuccessMessage{});
    }

    QN_NO_ENCODE(ReconnectSuccessMessage);
};

struct ReconnectFailureMessage {
    static MessageDecodeResult<ReconnectFailureMessage> decode(dbuf::ByteReader<>& writer) {
        return Ok(ReconnectFailureMessage{});
    }

    QN_NO_ENCODE(ReconnectFailureMessage);
};

struct QdbgToggleMessage {};

struct QdbgReportMessage {};

struct DataMessage {
    std::vector<uint8_t> data;
    std::optional<CompressionHeader> compHeader;
    std::optional<ReliabilityHeader> relHeader;

    MessageEncodeResult encode(auto& writer) const {
        writer.writeBytes(data.data(), data.size());
        return Ok();
    }

    static MessageDecodeResult<DataMessage> decode(dbuf::ByteReader<>& reader) {
        return Ok(DataMessage{ reader.readToEnd() });
    }
};

}