#pragma once

#include <qunet/socket/message/QunetMessage.hpp>
#include <qunet/compression/Error.hpp>
#include <qunet/util/Error.hpp>
#include <qsox/Error.hpp>
#include <arc/time/Timeout.hpp>
#ifdef QUNET_TLS_SUPPORT
# include <xtls/Base.hpp>
#endif

// Various transport errors

namespace qn {

QSOX_MAKE_OPAQUE_ERROR_STRUCT(QuicError, int);

#ifdef QUNET_TLS_SUPPORT
using xtls::TlsError;
using xtls::TlsResult;
#endif

#ifdef QUNET_WS_SUPPORT
class WsError {
    std::string error;
public:
    WsError(std::string error) : error(std::move(error)) {}

    std::string_view message() const {
        return error;
    }

    bool operator==(const WsError& other) const = default;
    bool operator!=(const WsError& other) const = default;
};
#endif

// Transport error

struct TransportError {
    typedef enum {
        ConnectionTimedOut,
        HandshakeTimedOut,
        NotImplemented,
        UnexpectedMessage,
        MessageTooLong,
        ZeroLengthMessage,
        NoBufferSpace,
        CongestionLimited,
        DefragmentationError,
        TooUnreliable,
        InvalidQunetDatabase,
        InvalidArgument,
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
#ifdef QUNET_QUIC_SUPPORT
    TransportError(QuicError err) : m_kind(std::move(err)) {}
#endif
#ifdef QUNET_TLS_SUPPORT
    TransportError(TlsError err) : m_kind(std::move(err)) {}
#endif
    TransportError(std::string err) : m_kind(std::move(err)) {}
    TransportError(HandshakeFailure err) : m_kind(std::move(err)) {}
    TransportError(MessageDecodeError err) : m_kind(std::move(err)) {}
    TransportError(CompressorError err) : m_kind(std::move(err)) {}
    TransportError(DecompressorError err) : m_kind(std::move(err)) {}
    TransportError(CustomCode code) : m_kind(CustomKind{code}) {}
    TransportError(arc::TimedOut) : m_kind(qsox::Error{qsox::Error::TimedOut}) {}
#ifdef QUNET_WS_SUPPORT
    TransportError(WsError err) : m_kind(std::move(err)) {}
#endif

    bool operator==(const TransportError& other) const = default;
    bool operator!=(const TransportError& other) const = default;

    std::variant<
        qsox::Error,
#ifdef QUNET_QUIC_SUPPORT
        QuicError,
#endif
#ifdef QUNET_TLS_SUPPORT
        TlsError,
#endif
#ifdef QUNET_WS_SUPPORT
        WsError,
#endif
        std::string,
        HandshakeFailure,
        MessageDecodeError,
        CompressorError,
        DecompressorError,
        CustomKind
    > m_kind;

    std::string message() const;
};
inline auto format_as(const TransportError& err) { return err.message(); }

template <typename T = void>
using TransportResult = geode::Result<T, TransportError>;

}