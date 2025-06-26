#include <qunet/socket/transport/BaseTransport.hpp>

namespace qn {

std::string_view TransportError::message() const {
    switch (m_code) {
        case NetworkError: return "Network error";
        case ConnectionClosed: return "Connection closed";
        case EncodingFailed: return "Encoding failed";
        case DecodingFailed: return "Decoding failed";
        case TransportCreationFailed: return "Transport creation failed";
        case ConnectionTimedOut: return "Connection timed out";
        case HandshakeFailed: return "Handshake failed";
        case InvalidMessage: return "Invalid message";
    }

    QN_ASSERT(false);
}

TransportError BaseTransport::makeError(qsox::Error err) {
    // TODO: better error handling
    return TransportError::NetworkError;
}

}