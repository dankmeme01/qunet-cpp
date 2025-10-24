#include <qunet/protocol/errors.hpp>

namespace qn {

std::string_view messageForConnectionError(int code) {
    switch (code) {
        case 1: return "Fragmentation not allowed";
        case 2: return "Requested QDB chunk is too long";
        case 3: return "Requested QDB chunk is invalid (offset/length are out of bounds)";
        case 4: return "Client requested a QDB chunk but a QDB isn't available";
        case 5: return "Protocol violation: client send a malformed zero-length message";
        case 6: return "Protocol violation: client sent a stream message that exceeds the maximum allowed length";
        case 7: return "Internal server error";
        case 8: return "Server is shutting down";
        default: return "";
    }
}

std::string_view messageForServerCloseError(int code) {
    return messageForConnectionError(code);
}

std::string_view messageForHandshakeError(int code) {
    switch (code) {
        case 1: return "Client qunet version is too old";
        case 2: return "Client qunet version is too new";
        case 3: return "Reconnect failed, unknown connection ID";
        case 4: return "Duplicate connection detected from the same address";
        default: return "";
    }
}

}