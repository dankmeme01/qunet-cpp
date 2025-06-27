#include <qunet/socket/transport/BaseTransport.hpp>

namespace qn {

std::string_view TransportError::message() const {
    switch (m_code) {
        case NetworkError: return "Network error";
        case ConnectionClosed: return "Connection closed";
        case ConnectionRefused: return "Connection refused";
        case EncodingFailed: return "Encoding failed";
        case DecodingFailed: return "Decoding failed";
        case TransportCreationFailed: return "Transport creation failed";
        case ConnectionTimedOut: return "Connection timed out";
        case HandshakeFailed: return "Handshake failed";
        case InvalidMessage: return "Invalid message";
        case ZeroLengthMessage: return "Zero length message";
        case MessageTooLong: return "Message too long";
    }

    QN_ASSERT(false);
}

TransportError BaseTransport::makeError(qsox::Error err) {
    // TODO: better error handling
    using enum qsox::Error::Code;

    switch (err.code()) {
        case ConnectionClosed:
        case ConnectionAborted:
            return TransportError::ConnectionClosed;
        case ConnectionRefused:
            return TransportError::ConnectionRefused;
        default: return TransportError::NetworkError;
    }

    return TransportError::NetworkError;
}

void BaseTransport::setConnectionId(uint64_t connectionId) {
    m_connectionId = connectionId;
}

void BaseTransport::setMessageSizeLimit(size_t limit) {
    m_messageSizeLimit = limit;
}

}