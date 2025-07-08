#include <qunet/socket/transport/UdpTransport.hpp>
#include <qunet/buffers/HeapByteWriter.hpp>
#include <qunet/protocol/constants.hpp>
#include <qunet/socket/message/meta.hpp>
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

    uint8_t hdbyte = buffer[0];

    // not a data message, cannot be compressed/fragmented/reliable
    if ((hdbyte & MSG_DATA_MASK) == 0) {
        ByteReader reader(buffer, bytesRead);
        auto msg = GEODE_UNWRAP(QunetMessage::decode(reader));

        m_recvMsgQueue.push(std::move(msg));

        return Ok(true);
    }

    // data message, parse headers
    bool fragmented = (hdbyte & MSG_DATA_BIT_FRAGMENTATION) != 0;
    bool reliable = (hdbyte & MSG_DATA_BIT_RELIABILITY) != 0;
    CompressionType compressionType = (CompressionType) ((hdbyte >> MSG_DATA_BIT_COMPRESSION_1) & 0b11);

    std::optional<CompressionHeader> compHeader;
    std::optional<ReliabilityHeader> relHeader;
    std::optional<FragmentationHeader> fragHeader;

    ByteReader reader(buffer + 1, bytesRead - 1);

    switch (compressionType) {
        case CompressionType::Zstd: {
            compHeader = CompressionHeader {
                .type = CompressionType::Zstd,
                .uncompressedSize = GEODE_UNWRAP(reader.readU32()),
            };
        } break;

        case CompressionType::Lz4: {
            compHeader = CompressionHeader {
                .type = CompressionType::Lz4,
                .uncompressedSize = GEODE_UNWRAP(reader.readU32()),
            };
        } break;

        default: break;
    }

    if (reliable) {
        ReliabilityHeader relHdr;
        relHdr.messageId = GEODE_UNWRAP(reader.readU16());
        relHdr.ackCount = GEODE_UNWRAP(reader.readU16());

        for (size_t i = 0; i < std::min<size_t>(relHdr.ackCount, 8); i++) {
            relHdr.acks[i] = GEODE_UNWRAP(reader.readU16());
        }

        relHeader = relHdr;
    }

    if (fragmented) {
        FragmentationHeader fragHdr;
        fragHdr.messageId = GEODE_UNWRAP(reader.readU16());
        fragHdr.fragmentIndex = GEODE_UNWRAP(reader.readU16());

        // top bit of fragmentIndex indicates if this is the last fragment
        fragHdr.lastFragment = (fragHdr.fragmentIndex & (uint16_t)0x8000) != 0;
        fragHdr.fragmentIndex &= (uint16_t)0x7FFF;

        fragHeader = fragHdr;
    }

    QunetMessageMeta meta {
        .compressionHeader = compHeader,
        .reliabilityHeader = relHeader,
        .fragmentationHeader = fragHeader,
        .data = reader.readToEnd(),
    };

    // handle fragmented / reliable messages

    if (fragmented) {
        auto recvf = GEODE_UNWRAP(m_fragStore.processFragment(std::move(meta)));

        // if a message wasn't completed, just return false
        if (!recvf) {
            return Ok(false);
        }

        // otherwise, keep processing
        meta = std::move(*recvf);
    }

    QN_ASSERT(!meta.fragmentationHeader.has_value());

    reliable = meta.reliabilityHeader.has_value();

    if (reliable) {
        if (!m_relStore.handleIncoming(meta)) {
            // duplicate message or otherwise invalid
            return Ok(false);
        }
    }

    GEODE_UNWRAP(this->pushPreFinalDataMessage(std::move(meta)));

    // if this was a reliable message, it is possible that messages that previously were received out of order are
    // now available to be properly processed. we want to push these messages too, but strictly *after* this one.
    if (reliable) {
        while (m_relStore.hasDelayedMessage()) {
            auto msg = m_relStore.popDelayedMessage();
            QN_DEBUG_ASSERT(msg.has_value());

            GEODE_UNWRAP(this->pushPreFinalDataMessage(std::move(*msg)));
        }
    }

    return Ok(true);
}

TransportResult<> UdpTransport::pushPreFinalDataMessage(QunetMessageMeta&& meta) {
    // handle compression...

    std::vector<uint8_t> data;

    if (!meta.compressionHeader) {
        data = std::move(meta.data);
    } else {
        size_t uncSize = meta.compressionHeader->uncompressedSize;
        if (uncSize > m_messageSizeLimit) {
            return Err(TransportError::MessageTooLong);
        }

        data.resize(uncSize);

        auto ty = meta.compressionHeader->type;

        switch (ty) {
            case CompressionType::Zstd: {
                GEODE_UNWRAP(m_zstdDecompressor.decompress(
                    meta.data.data(), meta.data.size(),
                    data.data(), uncSize
                ));

                data.resize(uncSize);
            } break;

            case CompressionType::Lz4: {
                return Err(TransportError::NotImplemented);
            } break;

            default: {
                // how did we get here?
                QN_ASSERT(false && "Unknown compression type");
            };
        }
    }

    // oh my god is it finally over

    m_recvMsgQueue.push(DataMessage {
        .data = std::move(data),
    });

    return Ok();
}

}
