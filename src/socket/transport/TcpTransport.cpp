#include <qunet/socket/transport/TcpTransport.hpp>
#include <qunet/buffers/HeapByteWriter.hpp>
#include <qunet/protocol/constants.hpp>
#include <qunet/Log.hpp>

#include <qsox/Poll.hpp>

#define MAP_UNWRAP(x) GEODE_UNWRAP((x).mapErr([](const auto& err) { return TransportError::EncodingFailed; }))

using namespace qsox;
using namespace asp::time;

namespace qn {

TcpTransport::TcpTransport(qsox::TcpStream socket) : m_socket(std::move(socket)) {
    m_readBuffer.resize(512);
}

TcpTransport::~TcpTransport() {}

NetResult<TcpTransport> TcpTransport::connect(const SocketAddress& address) {
    auto socket = GEODE_UNWRAP(TcpStream::connect(address));

    return Ok(TcpTransport(std::move(socket)));
}

TransportResult<> TcpTransport::sendMessage(QunetMessage message) {
    HeapByteWriter writer;

    bool isHandshake = message.is<HandshakeStartMessage>();

    // Leave space for message length
    size_t preMessagePos = 0;
    if (!isHandshake) {
        writer.writeU32(0);
        preMessagePos = writer.position();
    }

    MAP_UNWRAP(message.encodeHeader(writer, 0));

    auto res = message.encode(writer);
    if (!res) {
        log::warn("Failed to encode message: {}", res.unwrapErr().message());
        return Err(TransportError::EncodingFailed);
    }

    // Write message length
    uint32_t messageSize = writer.position() - preMessagePos;

    if (!isHandshake) {
        writer.performAt(preMessagePos - sizeof(uint32_t), [&](auto& writer) {
            writer.writeU32(messageSize);
        }).unwrap();
    }

    auto data = writer.written();
    auto cres = (m_socket.sendAll(data.data(), data.size()));

    if (!cres) {
        auto err = cres.unwrapErr();
        return Err(this->makeError(err));
    }

    return Ok();
}

TransportResult<bool> TcpTransport::poll(const Duration& dur) {
    auto res = qsox::pollOne(m_socket.handle(), PollType::Read, dur.millis());
    if (!res) {
        return Err(this->makeError(res.unwrapErr()));
    }

    return Ok(res.unwrap() == PollResult::Readable);
}

// The logic here is pretty much taken from TcpTransport in the qunet server
TransportResult<QunetMessage> TcpTransport::receiveMessage() {
    auto readBuffer = std::span<uint8_t>(m_readBuffer.data(), m_readBuffer.size());

    while (true) {
        // first, try to parse a message from the buffer
        if (m_readBufferPos >= sizeof(uint32_t)) {
            size_t length = ByteReader{readBuffer.subspan(0, sizeof(uint32_t))}.readU32().unwrap();

            if (length == 0) {
                return Err(TransportError::ZeroLengthMessage);
            } else if (length > m_messageSizeLimit) {
                log::warn("Received message larger than limit: {} > {}", length, m_messageSizeLimit);
                return Err(TransportError::MessageTooLong);
            }

            size_t totalLen = sizeof(uint32_t) + length;
            if (m_readBufferPos >= totalLen) {
                // we have a full message in the buffer
                auto data = readBuffer.subspan(sizeof(uint32_t), length);

                ByteReader reader(data);
                auto msg = QunetMessage::decode(reader);

                // shift leftover bytes in the buffer
                auto leftoverBytes = m_readBufferPos - totalLen;
                if (leftoverBytes > 0) {
                    std::memmove(m_readBuffer.data(), m_readBuffer.data() + totalLen, leftoverBytes);
                }
                m_readBufferPos = leftoverBytes;

                if (!msg) {
                    return Err(TransportError::DecodingFailed);
                } else {
                    return Ok(std::move(msg).unwrap());
                }
            }

            // if there is not enough data but we know the length, check if additional space needs to be allocated
            if (totalLen > m_readBuffer.size()) {
                // geometric growth
                size_t newSize = std::max(totalLen, m_readBuffer.size() * 2);
                m_readBuffer.resize(newSize);
                readBuffer = std::span<uint8_t>(m_readBuffer.data(), m_readBuffer.size());
            }
        }

        // read from the socket
        auto res = m_socket.receive(readBuffer.data() + m_readBufferPos, readBuffer.size() - m_readBufferPos);
        if (!res) {
            return Err(this->makeError(res.unwrapErr()));
        }

        size_t len = res.unwrap();
        if (len == 0) {
            return Err(TransportError::ConnectionClosed);
        }

        m_readBufferPos += len;
    }
}

}
