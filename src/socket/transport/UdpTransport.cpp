#include <qunet/socket/transport/UdpTransport.hpp>
#include <qunet/buffers/HeapByteWriter.hpp>
#include <qunet/protocol/constants.hpp>
#include <qunet/socket/message/meta.hpp>
#include <qunet/Connection.hpp>
#include <qunet/util/rng.hpp>
#include <qunet/Log.hpp>

#include <asp/time/Instant.hpp>
#include <qsox/Poll.hpp>
#include <algorithm>

#define MAP_UNWRAP(x) GEODE_UNWRAP((x).mapErr([](const auto& err) { return TransportError::EncodingFailed; }))

using namespace qsox;
using namespace asp::time;

namespace qn {

UdpTransport::UdpTransport(qsox::UdpSocket socket, size_t mtu, float lossSim) : m_socket(std::move(socket)), m_mtu(mtu), m_lossSim(lossSim) {}

UdpTransport::~UdpTransport() {}

NetResult<UdpTransport> UdpTransport::connect(
    const SocketAddress& address,
    const struct ConnectionDebugOptions* debugOptions
) {
    auto socket = GEODE_UNWRAP(UdpSocket::bindAny(address.isV6()));
    GEODE_UNWRAP(socket.connect(address));

    return Ok(UdpTransport(
        std::move(socket),
        UDP_PACKET_LIMIT,
        debugOptions ? debugOptions->packetLossSimulation : 0.0f
    ));
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

    GEODE_UNWRAP(this->sendMessage(handshakeStart, false));
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
            GEODE_UNWRAP(this->sendMessage(handshakeStart, false));
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

TransportResult<> UdpTransport::sendMessage(QunetMessage message, bool reliable) {
    // non-data messages cannot be compressed, fragmented or reliable, so steps are simple here
    if (!message.is<DataMessage>()) {
        if (this->shouldLosePacket()) {
            return Ok();
        }

        HeapByteWriter writer;
        GEODE_UNWRAP(message.encodeControlMsg(writer, m_connectionId));
        auto data = writer.written();
        auto cres = GEODE_UNWRAP(m_socket.send(data.data(), data.size()));

        this->updateLastActivity();

        return Ok();
    }

    // data messages are more interesting

    auto& msg = message.as<DataMessage>();

    // always try to ack some messages, even if this isn't a reliable message
    ReliabilityHeader relHdr{};
    relHdr.messageId = 0;
    m_relStore.setOutgoingAcks(relHdr);

    if (reliable) {
        relHdr.messageId = m_relStore.nextMessageId();
    }

    // only assign the reliability header if this is a reliable message or if there's any acks
    if (relHdr.messageId != 0 || relHdr.ackCount > 0) {
        msg.relHeader = std::move(relHdr);
    } else {
        msg.relHeader.reset();
    }

    return this->doSendUnfragmentedData(message, false);
}

TransportResult<> UdpTransport::doSendUnfragmentedData(QunetMessage& message, bool retransmission) {
    auto& msg = message.as<DataMessage>();
    HeapByteWriter writer;

    bool isReliable = msg.relHeader.has_value() && msg.relHeader->messageId != 0 && !retransmission;

    size_t relHdrSize = msg.relHeader.has_value() ? 4 + msg.relHeader->ackCount * 2 : 0; // 2 for message ID + 2 for ack count, 2 for each ACK
    size_t compHdrSize = msg.compHeader.has_value() ? 4 : 0;

    size_t unfragTotalSize = relHdrSize + compHdrSize + msg.data.size();

    if (unfragTotalSize <= m_mtu) {
        // no fragmentation :)
        message.encodeDataHeader(writer, m_connectionId, false).unwrap();

        writer.writeBytes(msg.data);

        auto data = writer.written();

        if (!this->shouldLosePacket()) {
            auto cres = GEODE_UNWRAP(m_socket.send(data.data(), data.size()));
        }

        if (isReliable) {
            m_relStore.pushLocalUnacked(std::move(message));
        }

        this->updateLastActivity();

        return Ok();
    }

    // fragmentation is needed :(

    // determine the maximum size of the payload for each fragment
    // first fragment must include reliability and compression headers if they are present, rest don't have to

    size_t fragHdrSize = 4;
    size_t firstPayloadSize = m_mtu - relHdrSize - compHdrSize - fragHdrSize;
    size_t restPayloadSize = m_mtu - fragHdrSize;

    uint16_t fragMessageId = m_fragStore.nextMessageId();

    size_t offset = 0;
    size_t fragmentIndex = 0;

    std::span<uint8_t> data{msg.data.begin(), msg.data.end()};

    while (offset < msg.data.size()) {
        bool isFirst = (fragmentIndex == 0);
        size_t payloadSize = isFirst ? firstPayloadSize : restPayloadSize;
        bool isLast = (offset + payloadSize >= msg.data.size());

        auto chunk = data.subspan(offset, std::min(payloadSize, data.size() - offset));

        HeapByteWriter fwriter;

        // write the header, omit headers for all but the first fragment
        message.encodeDataHeader(fwriter, m_connectionId, !isFirst).unwrap();

        // but always add the fragmentation header
        uint8_t hdrbyte = fwriter.written()[0] | MSG_DATA_FRAGMENTATION_MASK;
        fwriter.performAt(0, [&](auto& writer) { writer.writeU8(hdrbyte); }).unwrap();
        fwriter.writeU16(fragMessageId);
        fwriter.writeU16((uint16_t) (fragmentIndex | (isLast ? MSG_DATA_LAST_FRAGMENT_MASK : 0)));
        fwriter.writeBytes(chunk);

        offset += chunk.size();
        fragmentIndex++;

        auto data = fwriter.written();

        if (!this->shouldLosePacket()) {
            auto cres = GEODE_UNWRAP(m_socket.send(data.data(), data.size()));
        }
    }

    if (isReliable) {
        m_relStore.pushLocalUnacked(std::move(message));
    }

    this->updateLastActivity();

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
    auto meta = GEODE_UNWRAP(QunetMessage::decodeMeta(reader));

    // not a data message, cannot be compressed/fragmented/reliable
    if (meta.type != MSG_DATA) {
        auto msg = GEODE_UNWRAP(QunetMessage::decodeWithMeta(std::move(meta)));

        if (msg.is<KeepaliveResponseMessage>()) {
            m_unackedKeepalive = false;
        }

        m_recvMsgQueue.push(std::move(msg));

        return Ok(true);
    }

    // handle fragmented / reliable messages

    if (meta.fragmentationHeader) {
        auto recvf = GEODE_UNWRAP(m_fragStore.processFragment(std::move(meta)));

        // if a message wasn't completed, just return false
        if (!recvf) {
            return Ok(false);
        }

        // otherwise, keep processing
        meta = std::move(*recvf);
    }

    QN_ASSERT(!meta.fragmentationHeader.has_value());

    bool reliable = meta.reliabilityHeader.has_value();
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

    return Ok(m_recvMsgQueue.size() > 0);
}

Duration UdpTransport::untilTimerExpiry() const {
    return std::min(
        m_relStore.untilTimerExpiry(),
        this->untilKeepalive()
    );
}

Duration UdpTransport::untilKeepalive() const {
    if (m_unackedKeepalive) {
        // if a keepalive is in flight but has not been acknowledged (lost?), send another one after a bit
        return Duration::fromSecs(3) - this->sinceLastActivity();
    } else {
        // if no keepalives sent in a while, send one
        // timeout is different depending on how many keepalives we have sent so far,
        // we send more at the start to figure out the latency

        switch (m_totalKeepalives) {
            case 0:
            case 1:
                return Duration::fromSecs(3) - this->sinceLastKeepalive();
            case 2:
                return Duration::fromSecs(8) - this->sinceLastKeepalive();
            case 3:
                return Duration::fromSecs(12) - this->sinceLastKeepalive();
            case 4:
                return Duration::fromSecs(20) - this->sinceLastKeepalive();
            default:
                return Duration::fromSecs(30) - this->sinceLastActivity();
        }
    }
}

TransportResult<> UdpTransport::handleTimerExpiry() {
    while (auto msg = m_relStore.maybeRetransmit()) {
        // if we have a message to retransmit, send it
        GEODE_UNWRAP(this->doSendUnfragmentedData(*msg, true));
    }

    // if we have not sent any data messages recently that we could tag acks onto,
    // we might need to send an explicit ack message with no data.
    if (m_relStore.hasUrgentOutgoingAcks()) {
        ReliabilityHeader relHdr{};
        relHdr.messageId = 0;
        m_relStore.setOutgoingAcks(relHdr);

        QN_DEBUG_ASSERT(relHdr.ackCount > 0);

        QunetMessage msg{DataMessage {
            .data = {},
            .compHeader = std::nullopt,
            .relHeader = std::move(relHdr),
        }};

        GEODE_UNWRAP(this->doSendUnfragmentedData(msg, false));
    }

    // if we haven't sent any messages in a while, we should send a keepalive message
    if (this->untilKeepalive().isZero()) {
        GEODE_UNWRAP(this->sendMessage(KeepaliveMessage{}, false));
        this->updateLastKeepalive();
        m_unackedKeepalive = true;
    }

    return Ok();
}

bool UdpTransport::shouldLosePacket() {
    return qn::randomChance(std::clamp(m_lossSim, 0.f, 1.f));
}

}
