#pragma once

#include <qunet/util/assert.hpp>
#include <qunet/util/visit.hpp>
#include <qunet/util/Error.hpp>
#include <qunet/util/TwoSpanSource.hpp>
#include "messages.hpp"
#include "meta.hpp"
#include <variant>
#include <asp/iter.hpp>

namespace qn {

QN_MAKE_ERROR_STRUCT(MessageDecodeError,
    InvalidMessageType,
    InvalidData,
    InvalidConnCtlCode,
    NotEnoughData,
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
        ClientCloseMessage,
        ServerCloseMessage,
        ClientReconnectMessage,
        ConnectionErrorMessage,
        // QdbChunkRequestMessage,
        // QdbChunkResponseMessage,
        ReconnectSuccessMessage,
        ReconnectFailureMessage,
        ConnectionControlMessage,
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
    QunetMessage(ClientCloseMessage msg) : m_kind(std::move(msg)) {}
    QunetMessage(ServerCloseMessage msg) : m_kind(std::move(msg)) {}
    QunetMessage(ClientReconnectMessage msg) : m_kind(std::move(msg)) {}
    QunetMessage(ConnectionErrorMessage msg) : m_kind(std::move(msg)) {}
    QunetMessage(ConnectionControlMessage msg) : m_kind(std::move(msg)) {}
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

    MessageEncodeResult encodeControlHeader(dbuf::ByteWriter<>& writer, uint64_t connectionId) const;
    MessageEncodeResult encodeControlMsg(dbuf::ByteWriter<>& writer, uint64_t connectionId) const;

    MessageEncodeResult encodeDataHeader(dbuf::ByteWriter<>& writer, uint64_t connectionId, bool omitHeaders) const;

    std::string_view typeStr() const;
    uint8_t headerByte() const;

    static Result<QunetMessage, MessageDecodeError> decodeWithMeta(QunetMessageMeta&& meta);

    /// Decodes message meta from the message header
    static Result<QunetMessageMeta, MessageDecodeError> decodeMeta(std::span<const uint8_t> data, bool udpHeaders = false, bool udpConnId = false);

private:
    VariantTy m_kind;
};

struct QunetUdpMessageIter : asp::iter::Iter<QunetUdpMessageIter, Result<QunetMessageMeta, MessageDecodeError>> {
    // pass whether we are the server
    QunetUdpMessageIter(std::span<const uint8_t> data, bool server);

    using Item = Result<QunetMessageMeta, MessageDecodeError>;

    std::optional<Item> next();

private:
    std::span<const uint8_t> m_data;
    size_t m_pos = 0;
    bool m_eof = false, m_server;
};

}
