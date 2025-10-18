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
        case CustomCode::ReconnectFailed: return "Reconnect failed, server rejected the attempt";
        case CustomCode::TimedOut: return "Operation timed out";
        case CustomCode::Closed: return "Operation cannot be performed because the connection is already closed";
        case CustomCode::Cancelled: return "Connection cancelled";
        case CustomCode::Other: return "Unknown transport error";
    }

    qn::unreachable();
}

std::string TransportError::message() const {
    return std::visit(makeVisitor {
        [](const qsox::Error& err) {
            return fmt::format("Socket error: {}", err.message());
        },
        [](const QuicError& err) {
            return fmt::format("QUIC error: {}", err.message());
        },
        [](const TlsError& err) {
            return fmt::format("TLS error: {}", err.message());
        },
        [](const ByteReaderError& err) {
            return fmt::format("Error decoding packet: {}", err.message());
        },
        [](const ByteWriterError& err) {
            return fmt::format("Error encoding message: {}", err.message());
        },
        [](const HandshakeFailure& err) {
            return fmt::format("Handshake failed: {}", err.message());
        },
        [](const MessageDecodeError& err) {
            return fmt::format("Error decoding message: {}", err.message());
        },
        [](const CompressorError& err) {
            return fmt::format("Compression error: {}", err.message());
        },
        [](const DecompressorError& err) {
            return fmt::format("Decompression error: {}", err.message());
        },
        [](const CustomKind& kind) {
            return fmt::format("{}", kind.message());
        }
    }, m_kind);
}

}