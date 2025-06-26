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
    auto msgType = MAP_UNWRAP(reader.readU8());

    if (msgType & MSG_DATA_MASK) {
        // data message
        // TODO!
        return Err(MessageDecodeError::InvalidMessageType);
    }

    switch (msgType) {
        case MSG_HANDSHAKE_FINISH: {
            return Ok(MAP_UNWRAP(HandshakeFinishMessage::decode(reader)));
        } break;
    }

    return Err(MessageDecodeError::InvalidMessageType);
}

}