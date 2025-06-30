#include <qunet/socket/message/QunetMessage.hpp>
#include <qunet/protocol/constants.hpp>

#define MAP_UNWRAP(x) GEODE_UNWRAP((x).mapErr([](const auto& err) { return MessageDecodeError::InvalidData; }))

namespace qn {

std::string_view MessageDecodeError::message() const {
    switch (m_code) {
        case InvalidMessageType: return "Invalid message type";
        case InvalidData: return "Invalid data in message";
    }
}

geode::Result<QunetMessage, MessageDecodeError> QunetMessage::decode(ByteReader& reader) {
    // TODO, just like in the server, split this into `parseHeader` and `decode`
    auto msgType = MAP_UNWRAP(reader.readU8());

    if (msgType & MSG_DATA_MASK) {
        // data message
        // TODO!
        return Err(MessageDecodeError::InvalidMessageType);
    }

    switch (msgType) {
        case MSG_KEEPALIVE: {
            return Ok(MAP_UNWRAP(KeepaliveMessage::decode(reader)));
        } break;

        case MSG_KEEPALIVE_RESPONSE: {
            return Ok(MAP_UNWRAP(KeepaliveResponseMessage::decode(reader)));
        } break;

        case MSG_HANDSHAKE_FINISH: {
            return Ok(MAP_UNWRAP(HandshakeFinishMessage::decode(reader)));
        } break;

        case MSG_HANDSHAKE_FAILURE: {
            return Ok(MAP_UNWRAP(HandshakeFailureMessage::decode(reader)));
        } break;

        case MSG_SERVER_CLOSE: {
            return Ok(MAP_UNWRAP(ServerCloseMessage::decode(reader)));
        } break;

        case MSG_CONNECTION_ERROR: {
            return Ok(MAP_UNWRAP(ConnectionErrorMessage::decode(reader)));
        } break;
    }

    return Err(MessageDecodeError::InvalidMessageType);
}

}