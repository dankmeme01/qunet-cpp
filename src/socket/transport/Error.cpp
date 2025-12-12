#include <qunet/socket/transport/Error.hpp>
#include <qunet/util/visit.hpp>
#include <fmt/format.h>

namespace qn {

std::string_view TransportError::CustomKind::message() const {
    // this crashes clang on macos/android
    // using enum CustomCode;

    switch (code) {
        case CustomCode::NotImplemented: return "Not implemented";
        case CustomCode::ConnectionTimedOut: return "Connection timed out";
        case CustomCode::UnexpectedMessage: return "Unexpected message received";
        case CustomCode::MessageTooLong: return "Message too long";
        case CustomCode::ZeroLengthMessage: return "Zero length message received";
        case CustomCode::NoBufferSpace: return "No buffer space available";
        case CustomCode::CongestionLimited: return "Congestion limited, cannot send data right now";
        case CustomCode::DefragmentationError: return "Defragmentation error, message could not be reassembled";
        case CustomCode::TooUnreliable: return "Transport is too unreliable, too many lost messages";
        case CustomCode::InvalidQunetDatabase: return "Invalid Qunet database, cannot decode";
        case CustomCode::InvalidArgument: return "Invalid argument";
        case CustomCode::ReconnectFailed: return "Reconnect failed, server rejected the attempt";
        case CustomCode::TimedOut: return "Operation timed out";
        case CustomCode::Closed: return "Operation cannot be performed because the connection is already closed";
        case CustomCode::Cancelled: return "Connection cancelled";
        case CustomCode::Other: return "Unknown transport error";
    }

    qn::unreachable();
}

std::string TransportError::message() const {
#define FOR_MSG(t, msg) if (std::holds_alternative<t>(m_kind)) { return fmt::format(msg, std::get<t>(m_kind).message()); } else

    FOR_MSG(qsox::Error, "Socket error: {}")

#ifdef QUNET_QUIC_SUPPORT
    FOR_MSG(QuicError, "QUIC error: {}")
#endif
#ifdef QUNET_TLS_SUPPORT
    FOR_MSG(TlsError, "TLS error: {}")
#endif
    FOR_MSG(ByteReaderError, "Error decoding packet: {}")
    FOR_MSG(ByteWriterError, "Error encoding message: {}")
    FOR_MSG(HandshakeFailure, "Handshake failed: {}")
    FOR_MSG(MessageDecodeError, "Error decoding message: {}")
    FOR_MSG(CompressorError, "Compression error: {}")
    FOR_MSG(DecompressorError, "Decompression error: {}")
    FOR_MSG(CustomKind, "{}")
    /* else */ {
        return "Unknown transport error";
    }
}

}