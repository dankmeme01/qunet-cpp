#pragma once

#include <qunet/util/assert.hpp>
#include <qunet/util/visit.hpp>
#include "messages.hpp"
#include "meta.hpp"
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
        KeepaliveMessage,
        KeepaliveResponseMessage,
        HandshakeStartMessage,
        HandshakeFinishMessage,
        HandshakeFailureMessage,
        // ClientCloseMessage,
        ServerCloseMessage,
        ClientReconnectMessage,
        ConnectionErrorMessage,
        // QdbChunkRequestMessage,
        // QdbChunkResponseMessage,
        ReconnectSuccessMessage,
        ReconnectFailureMessage,
        // QdbgToggleMessage,
        // QdbgReportMessage,
        DataMessage
    >;

public:
    // oh well
    // QunetMessage(PingMessage msg) : m_kind(std::move(msg)) {}
    // QunetMessage(PongMessage msg) : m_kind(std::move(msg)) {}
    QunetMessage(KeepaliveMessage msg) : m_kind(std::move(msg)) {}
    QunetMessage(KeepaliveResponseMessage msg) : m_kind(std::move(msg)) {}
    QunetMessage(HandshakeStartMessage msg) : m_kind(std::move(msg)) {}
    QunetMessage(HandshakeFinishMessage msg) : m_kind(std::move(msg)) {}
    QunetMessage(HandshakeFailureMessage msg) : m_kind(std::move(msg)) {}
    // QunetMessage(ClientCloseMessage msg) : m_kind(std::move(msg)) {}
    QunetMessage(ServerCloseMessage msg) : m_kind(std::move(msg)) {}
    QunetMessage(ClientReconnectMessage msg) : m_kind(std::move(msg)) {}
    QunetMessage(ConnectionErrorMessage msg) : m_kind(std::move(msg)) {}
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

    MessageEncodeResult encodeControlHeader(HeapByteWriter& writer, uint64_t connectionId) const;
    MessageEncodeResult encodeControlMsg(HeapByteWriter& writer, uint64_t connectionId) const;

    MessageEncodeResult encodeDataHeader(HeapByteWriter& writer, uint64_t connectionId, bool omitHeaders) const;

    std::string_view typeStr() const;

    static geode::Result<QunetMessage, MessageDecodeError> decodeWithMeta(QunetMessageMeta&& meta);

    /// Decodes message meta from the message header
    static geode::Result<QunetMessageMeta, MessageDecodeError> decodeMeta(ByteReader& reader);

private:
    VariantTy m_kind;
};

}
