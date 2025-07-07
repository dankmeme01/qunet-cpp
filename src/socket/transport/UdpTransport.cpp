#include <qunet/socket/transport/UdpTransport.hpp>
#include <qunet/buffers/HeapByteWriter.hpp>
#include <qunet/protocol/constants.hpp>
#include <qunet/Log.hpp>

#include <asp/time/Instant.hpp>
#include <qsox/Poll.hpp>
#include <algorithm>

#define MAP_UNWRAP(x) GEODE_UNWRAP((x).mapErr([](const auto& err) { return TransportError::EncodingFailed; }))

using namespace qsox;
using namespace asp::time;

namespace qn {

UdpTransport::UdpTransport(qsox::UdpSocket socket) : m_socket(std::move(socket)) {}

UdpTransport::~UdpTransport() {}

NetResult<UdpTransport> UdpTransport::connect(const SocketAddress& address) {
    auto socket = GEODE_UNWRAP(UdpSocket::bindAny(address.isV6()));
    GEODE_UNWRAP(socket.connect(address));

    return Ok(UdpTransport(std::move(socket)));
}

TransportResult<> UdpTransport::close()  {
    // udp, of course, does not have any cleanup
    m_closed = true;
    return Ok();
}

bool UdpTransport::isClosed() const {
    return m_closed;
}

TransportResult<QunetMessage> UdpTransport::performHandshake(
    HandshakeStartMessage handshakeStart,
    const std::optional<asp::time::Duration>& timeout
) {
    auto startedAt = Instant::now();

    GEODE_UNWRAP(this->sendMessage(handshakeStart));
    auto lastSentHandshake = Instant::now();
    size_t sentAttempts = 1;

    std::vector<HandshakeFinishMessage> chunks;
    std::queue<QunetMessage> unexpectedMessages;

    HandshakeFinishMessage outMessage{};

    while (true) {
        // UDP is an unreliable protocol, so this function may retransmit the handshake message if needed.
        // Polls are limited to 750ms, if no message arrives within that time, the handshake message is sent again.
        // This repeats until a message is received or the timeout is reached.
        auto fullTimeout = timeout ? *timeout - startedAt.elapsed() : Duration::infinite();
        auto remTimeout = std::min(fullTimeout, Duration::fromMillis(750) - lastSentHandshake.elapsed());

        // if full timeout expired, return a timeout error
        if (fullTimeout.isZero()) {
            return Err(TransportError::TimedOut);
        }

        // if rem timeout expired, resend the handshake message
        if (remTimeout.isZero()) {
            GEODE_UNWRAP(this->sendMessage(handshakeStart));
            lastSentHandshake = Instant::now();
            sentAttempts++;
            continue;
        }

        bool hasData = GEODE_UNWRAP(this->poll(remTimeout));
        bool hasMessage = false;

        if (hasData) {
            hasMessage = GEODE_UNWRAP(this->processIncomingData());
        } else {
            hasMessage = this->messageAvailable();
        }

        if (!hasMessage) {
            continue; // no message available, keep polling
        }

        auto chunkmsg = GEODE_UNWRAP(this->receiveMessage());

        if (chunkmsg.is<HandshakeFailureMessage>()) {
            // if it's a failure message, return it directly
            return Ok(std::move(chunkmsg));
        } else if (!chunkmsg.is<HandshakeFinishMessage>()) {
            // since this is UDP, messages may arrive out of order, store unexpected messages for later
            unexpectedMessages.push(std::move(chunkmsg));
            continue;
        }

        // we got (either an entire or a chunk of) a handshake finish message
        auto& msg = chunkmsg.as<HandshakeFinishMessage>();

        // if there's no qdb data, we can return the message immediately
        if (!msg.qdbData) {
            outMessage = std::move(msg);
            break;
        }

        // if this is the first chunk, set some initial values
        if (chunks.empty()) {
            outMessage.connectionId = msg.connectionId;
            outMessage.qdbData = HandshakeFinishMessage::QdbData {
                .uncompressedSize = msg.qdbData->uncompressedSize,
                .compressedSize = msg.qdbData->compressedSize,
            };
            chunks.push_back(std::move(msg));

            if (outMessage.qdbData->compressedSize > 1024 * 1024) {
                return Err(TransportError::HandshakeFailure{"Qdb data is too large"});
            }
        } else {
            // otherwise, check if connection ID matches and check if this is a duplicate chunk
            if (msg.connectionId != outMessage.connectionId) {
                return Err(TransportError::HandshakeFailure{"Mismatch between connection IDs in handshake chunks"});
            }

            auto offset = msg.qdbData->chunkOffset;
            if (std::find_if(chunks.begin(), chunks.end(), [&](const auto& c) { return c.qdbData->chunkOffset == offset; }) != chunks.end()) {
                // this chunk is a duplicate, skip it
                continue;
            }

            // add the chunk to the list
            chunks.push_back(std::move(msg));
        }

        // check if we have all chunks
        size_t sizeSum = 0;
        for (auto& c : chunks) {
            sizeSum += c.qdbData->chunkSize;
        }

        if (sizeSum != outMessage.qdbData->compressedSize) {
            continue;
        }

        // we now must have all chunks and we are ready to combine them
        std::vector<uint8_t> combinedData(outMessage.qdbData->compressedSize);

        for (auto& c : chunks) {
            QN_ASSERT(c.qdbData->chunkData.size() == c.qdbData->chunkSize);

            // check for out of bounds
            if (c.qdbData->chunkOffset > outMessage.qdbData->compressedSize || c.qdbData->chunkOffset + c.qdbData->chunkSize > outMessage.qdbData->compressedSize) {
                return Err(TransportError::HandshakeFailure{"Qdb chunk data out of bounds"});
            }

            std::memcpy(
                combinedData.data() + c.qdbData->chunkOffset,
                c.qdbData->chunkData.data(),
                c.qdbData->chunkSize
            );
        }

        // note: for a milicious server, it is still possible to send a malformed response, where part of the qdb data is uninitialized.
        // this is not a concern to us, since the contents of the qdb data are validated later on when it's parsed.
        outMessage.qdbData->chunkData = std::move(combinedData);
        outMessage.qdbData->chunkOffset = 0;
        outMessage.qdbData->chunkSize = outMessage.qdbData->compressedSize;

        break;
    }

    // push back any unexpected messages to the queue
    while (!unexpectedMessages.empty()) {
        m_recvMsgQueue.push(std::move(unexpectedMessages.front()));
        unexpectedMessages.pop();
    }

    // done!

    return Ok(std::move(outMessage));
}

TransportResult<> UdpTransport::sendMessage(QunetMessage message) {
    HeapByteWriter writer;

    GEODE_UNWRAP(message.encodeHeader(writer, m_connectionId));
    GEODE_UNWRAP(message.encode(writer));

    auto data = writer.written();
    auto cres = GEODE_UNWRAP(m_socket.send(data.data(), data.size()));

    return Ok();
}

TransportResult<bool> UdpTransport::poll(const std::optional<Duration>& dur) {
    int timeout = dur ? dur->millis() : -1;

    auto res = GEODE_UNWRAP(qsox::pollOne(m_socket, PollType::Read, timeout));

    return Ok(res == PollResult::Readable);
}

TransportResult<bool> UdpTransport::processIncomingData() {
    uint8_t buffer[UDP_PACKET_LIMIT];

    auto bytesRead = GEODE_UNWRAP(m_socket.recv(buffer, sizeof(buffer)));

    if (bytesRead == 0) {
        return Err(TransportError::ZeroLengthMessage);
    }

    ByteReader reader(buffer, bytesRead);
    auto msg = GEODE_UNWRAP(QunetMessage::decode(reader));

    m_recvMsgQueue.push(std::move(msg));

    return Ok(true);
}

}
