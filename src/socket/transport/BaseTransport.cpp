#include <qunet/socket/transport/BaseTransport.hpp>
#include <qunet/util/algo.hpp>
#include <asp/time/Instant.hpp>

using namespace asp::time;

namespace qn {

void BaseTransport::setConnectionId(uint64_t connectionId) {
    m_connectionId = connectionId;
}

void BaseTransport::setMessageSizeLimit(size_t limit) {
    m_messageSizeLimit = limit;
}

TransportResult<> BaseTransport::initCompressors(const QunetDatabase* qdb) {
    if (qdb && qdb->zstdDict) {
        auto& dict = *qdb->zstdDict;
        GEODE_UNWRAP(m_zstdCompressor.initWithDictionary(dict.data(), dict.size(), qdb->zstdLevel));
        GEODE_UNWRAP(m_zstdDecompressor.initWithDictionary(dict.data(), dict.size()));
    } else {
        GEODE_UNWRAP(m_zstdCompressor.init(MSG_ZSTD_COMPRESSION_LEVEL));
        GEODE_UNWRAP(m_zstdDecompressor.init());
    }

    return Ok();
}

TransportResult<QunetMessage> BaseTransport::performHandshake(
    HandshakeStartMessage handshakeStart,
    const std::optional<asp::time::Duration>& timeout
) {
    auto startedAt = Instant::now();

    GEODE_UNWRAP(this->sendMessage(std::move(handshakeStart), false));

    while (true) {
        auto remTimeout = timeout ? std::optional(*timeout - startedAt.elapsed()) : std::nullopt;
        if (remTimeout && remTimeout->isZero()) {
            return Err(TransportError::TimedOut);
        }

        if (!GEODE_UNWRAP(this->poll(remTimeout))) {
            continue;
        }

        bool msgAvailable = GEODE_UNWRAP(this->processIncomingData());
        if (msgAvailable) {
            break;
        }
    }

    QN_DEBUG_ASSERT(!m_recvMsgQueue.empty());

    return this->receiveMessage();
}

TransportResult<QunetMessage> BaseTransport::receiveMessage() {
    if (!m_recvMsgQueue.empty()) {
        auto msg = std::move(m_recvMsgQueue.front());
        m_recvMsgQueue.pop();
        return Ok(std::move(msg));
    }

    // block until a message is available
    while (!GEODE_UNWRAP(this->processIncomingData()));

    return this->receiveMessage();
}

bool BaseTransport::messageAvailable() {
    return !m_recvMsgQueue.empty();
}

Duration BaseTransport::untilTimerExpiry() const {
    return Duration::infinite();
}

TransportResult<> BaseTransport::handleTimerExpiry() {
    return Ok();
}

TransportResult<> BaseTransport::_pushPreFinalDataMessage(QunetMessageMeta&& meta) {
    return this->pushPreFinalDataMessage(std::move(meta));
}

void BaseTransport::_pushFinalControlMessage(QunetMessage&& meta) {
    m_recvMsgQueue.push(std::move(meta));
}

TransportResult<> BaseTransport::pushPreFinalDataMessage(QunetMessageMeta&& meta) {
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

    // zero-sized data messages are special, they can be used for reliable ACKs, but are not shown to the user
    if (data.empty()) {
        return Ok();
    }

    m_recvMsgQueue.push(DataMessage {
        .data = std::move(data),
    });

    return Ok();
}

Duration BaseTransport::getLatency() const {
    return Duration::fromMicros(m_lastRttMicros);
}

void BaseTransport::updateLatency(Duration rtt) {
    m_lastRttMicros = exponentialMovingAverage<uint64_t>(m_lastRttMicros, rtt.micros(), 0.25);
}

void BaseTransport::updateLastActivity() {
    m_lastActivity = Instant::now();
}

Duration BaseTransport::sinceLastActivity() const {
    if (!m_lastActivity) {
        return Duration::infinite();
    }

    return m_lastActivity->elapsed();
}

}