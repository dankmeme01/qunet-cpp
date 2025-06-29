#pragma once

#include <qunet/util/assert.hpp>
#include <qunet/util/visit.hpp>
#include "messages.hpp"
#include <variant>

namespace qn {

QN_MAKE_ERROR_STRUCT(MessageDecodeError,
    InvalidMessageType,
    InvalidData
);

class QunetMessage {
    using VariantTy = std::variant<
        // PingMessage,
        // PongMessage,
        // KeepaliveMessage,
        // KeepaliveResponseMessage,
        HandshakeStartMessage,
        HandshakeFinishMessage,
        HandshakeFailureMessage,
        // ClientCloseMessage,
        // ServerCloseMessage,
        // ClientReconnectMessage,
        // ConnectionErrorMessage,
        // QdbChunkRequestMessage,
        // QdbChunkResponseMessage,
        // QdbgToggleMessage,
        // QdbgReportMessage,
        DataMessage
    >;

public:
    // oh well
    // QunetMessage(PingMessage msg) : m_kind(std::move(msg)) {}
    // QunetMessage(PongMessage msg) : m_kind(std::move(msg)) {}
    // QunetMessage(KeepaliveMessage msg) : m_kind(std::move(msg)) {}
    // QunetMessage(KeepaliveResponseMessage msg) : m_kind(std::move(msg)) {}
    QunetMessage(HandshakeStartMessage msg) : m_kind(std::move(msg)) {}
    QunetMessage(HandshakeFinishMessage msg) : m_kind(std::move(msg)) {}
    // QunetMessage(HandshakeFailureMessage msg) : m_kind(std::move(msg)) {}
    // QunetMessage(ClientCloseMessage msg) : m_kind(std::move(msg)) {}
    // QunetMessage(ServerCloseMessage msg) : m_kind(std::move(msg)) {}
    // QunetMessage(ClientReconnectMessage msg) : m_kind(std::move(msg)) {}
    // QunetMessage(ConnectionErrorMessage msg) : m_kind(std::move(msg)) {}
    // QunetMessage(QdbChunkRequestMessage msg) : m_kind(std::move(msg)) {}
    // QunetMessage(QdbChunkResponseMessage msg) : m_kind(std::move(msg)) {}
    // QunetMessage(QdbgToggleMessage msg) : m_kind(std::move(msg)) {}
    // QunetMessage(QdbgReportMessage msg) : m_kind(std::move(msg)) {}
    QunetMessage(DataMessage msg) : m_kind(std::move(msg)) {}

    QunetMessage(VariantTy kind) : m_kind(std::move(kind)) {}

    QunetMessage(const QunetMessage& other) = default;
    QunetMessage& operator=(const QunetMessage& other) = default;
    QunetMessage(QunetMessage&& other) noexcept = default;
    QunetMessage& operator=(QunetMessage&& other) noexcept = default;

    template <typename T>
    bool is() const {
        return std::holds_alternative<T>(m_kind);
    }

    template <typename T>
    T& as() {
        return std::get<T>(m_kind);
    }

    template <typename T>
    const T& as() const {
        return std::get<T>(m_kind);
    }

    // writer here can be both ByteWriter and HeapByteWriter
    MessageEncodeResult encode(auto& writer) const {
        return std::visit([&writer](const auto& msg) {
            if constexpr (requires { msg.encode(writer); }) {
                return msg.encode(writer);
            } else {
                QN_ASSERT(false && "This message does not support encoding");
            }
        }, m_kind);
    }

    MessageEncodeResult encodeHeader(
        HeapByteWriter& writer,
        uint64_t connectionId
    ) const {
        // Write the header byte
        std::visit(makeVisitor {
            [&](PingMessage msg) {
                return writer.writeU8(MSG_PING);
            },
            [&](PongMessage msg) {
                return writer.writeU8(MSG_PONG);
            },
            [&](KeepaliveMessage msg) {
                return writer.writeU8(MSG_KEEPALIVE);
            },
            [&](KeepaliveResponseMessage msg) {
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
            [&](ClientCloseMessage msg) {
                return writer.writeU8(MSG_CLIENT_CLOSE);
            },
            [&](ServerCloseMessage msg) {
                return writer.writeU8(MSG_SERVER_CLOSE);
            },
            [&](ClientReconnectMessage msg) {
                return writer.writeU8(MSG_CLIENT_RECONNECT);
            },
            [&](ConnectionErrorMessage msg) {
                return writer.writeU8(MSG_CONNECTION_ERROR);
            },
            [&](QdbChunkRequestMessage msg) {
                return writer.writeU8(MSG_QDB_CHUNK_REQUEST);
            },
            [&](QdbChunkResponseMessage msg) {
                return writer.writeU8(MSG_QDB_CHUNK_RESPONSE);
            },
            [&](QdbgToggleMessage msg) {
                return writer.writeU8(MSG_QDBG_TOGGLE);
            },
            [&](QdbgReportMessage msg) {
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

    static geode::Result<QunetMessage, MessageDecodeError> decode(ByteReader& reader);

private:
    VariantTy m_kind;
};

}
