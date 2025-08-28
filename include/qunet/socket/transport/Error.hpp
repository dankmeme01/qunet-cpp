#pragma once

#include <qunet/socket/message/QunetMessage.hpp>
#include <qunet/compression/Error.hpp>
#include <qunet/util/Error.hpp>
#include <qunet/buffers/Error.hpp>
#include <qsox/Error.hpp>

// Various transport errors

namespace qn {

QSOX_MAKE_OPAQUE_ERROR_STRUCT(QuicError, int);
QSOX_MAKE_OPAQUE_ERROR_STRUCT(TlsError, unsigned long);

// Transport error

struct TransportError {
    typedef enum {
        ConnectionTimedOut,
        NotImplemented,
        UnexpectedMessage,
        MessageTooLong,
        ZeroLengthMessage,
        NoBufferSpace,
        CongestionLimited,
        DefragmentationError,
        TooUnreliable,
        InvalidQunetDatabase,
        ReconnectFailed,
        TimedOut,
        Closed,
        Cancelled,
        Other,
    } CustomCode;

    struct CustomKind {
        CustomCode code;

        std::string_view message() const;

        bool operator==(const CustomKind& other) const = default;
        bool operator!=(const CustomKind& other) const = default;
    };

    struct HandshakeFailure {
        std::string reason;

        HandshakeFailure(std::string reason) : reason(std::move(reason)) {}

        std::string_view message() const {
            return reason;
        }

        bool operator==(const HandshakeFailure& other) const = default;
        bool operator!=(const HandshakeFailure& other) const = default;
    };

    TransportError(const qsox::Error& err) : m_kind(err) {}
    TransportError(QuicError err) : m_kind(std::move(err)) {}
    TransportError(TlsError err) : m_kind(std::move(err)) {}
    TransportError(ByteReaderError err) : m_kind(std::move(err)) {}
    TransportError(ByteWriterError err) : m_kind(std::move(err)) {}
    TransportError(HandshakeFailure err) : m_kind(std::move(err)) {}
    TransportError(MessageDecodeError err) : m_kind(std::move(err)) {}
    TransportError(CompressorError err) : m_kind(std::move(err)) {}
    TransportError(DecompressorError err) : m_kind(std::move(err)) {}
    TransportError(CustomCode code) : m_kind(CustomKind{code}) {}

    bool operator==(const TransportError& other) const = default;
    bool operator!=(const TransportError& other) const = default;

    std::variant<
        qsox::Error,
        QuicError,
        TlsError,
        ByteReaderError,
        ByteWriterError,
        HandshakeFailure,
        MessageDecodeError,
        CompressorError,
        DecompressorError,
        CustomKind
    > m_kind;

    std::string message() const;
};

template <typename T = void>
using TransportResult = geode::Result<T, TransportError>;

}