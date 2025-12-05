#include <qunet/socket/transport/BaseTransport.hpp>
#include <qunet/util/algo.hpp>
#include <qunet/Log.hpp>
#include <asp/time/Instant.hpp>
#include <chrono>

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

arc::Future<TransportResult<QunetMessage>> BaseTransport::performHandshake(
    HandshakeStartMessage handshakeStart
) {
    ARC_CO_UNWRAP(co_await this->sendMessage(std::move(handshakeStart), false));
    co_return co_await this->receiveMessage();
}

arc::Future<TransportResult<QunetMessage>> BaseTransport::performReconnect(
    uint64_t connectionId
) {
    ARC_CO_UNWRAP(co_await this->sendMessage(ClientReconnectMessage{ connectionId }, false));
    co_return co_await this->receiveMessage();
}

arc::Future<TransportResult<bool>> BaseTransport::pollTimeout(asp::time::Duration timeout) {
    auto res = co_await arc::timeout(timeout, this->poll());

    if (res.isErr()) {
        co_return Ok(false); // timed out
    } else {
        auto inner = res.unwrap();
        if (inner.isErr()) {
            co_return Err(inner.unwrapErr());
        } else {
            co_return Ok(true);
        }
    }
}

uint64_t BaseTransport::getKeepaliveTimestamp() const {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

Duration BaseTransport::untilTimerExpiry() const {
    return Duration::infinite();
}

arc::Future<TransportResult<>> BaseTransport::handleTimerExpiry() {
    if (m_unackedKeepalives >= 3) {
        co_return Err(TransportError::TimedOut);
    }

    co_return Ok();
}

TransportResult<QunetMessage> BaseTransport::decodePreFinalDataMessage(QunetMessageMeta&& meta) {
    // handle compression...

    std::vector<uint8_t> data;

    if (!meta.compressionHeader) {
        data = std::move(meta.data);
        m_tracker.onDownMessage(MSG_DATA, data.size(), std::nullopt, {}, meta.reliabilityHeader);
    } else {
        size_t uncSize = meta.compressionHeader->uncompressedSize;
        m_tracker.onDownMessage(MSG_DATA, uncSize, meta.data.size(), {}, meta.reliabilityHeader);

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

    return Ok(DataMessage {
        .data = std::move(data),
    });
}

void BaseTransport::onIncomingMessage(const QunetMessage& msg) {
    if (msg.is<KeepaliveResponseMessage>()) {
        auto ts = msg.as<KeepaliveResponseMessage>().timestamp;
        auto now = this->getKeepaliveTimestamp();

        if (now > ts) {
            auto passed = Duration::fromNanos(now - ts);
            this->updateLatency(passed);
        }
    }
}

Duration BaseTransport::getLatency() const {
    return Duration::fromMicros(m_lastRttMicros);
}

void BaseTransport::updateLatency(Duration rtt) {
    if (m_lastRttMicros == 0) {
        m_lastRttMicros = rtt.micros();
    } else {
        m_lastRttMicros = exponentialMovingAverage<uint64_t>(m_lastRttMicros, rtt.micros(), 0.25);
    }
}

void BaseTransport::updateLastActivity() {
    m_lastActivity = Instant::now();
}

void BaseTransport::updateLastKeepalive() {
    m_lastKeepalive = Instant::now();
    m_totalKeepalives++;
    m_unackedKeepalives++;
}

Duration BaseTransport::sinceLastActivity() const {
    if (!m_lastActivity) {
        return Duration::infinite();
    }

    return m_lastActivity->elapsed();
}

Duration BaseTransport::sinceLastKeepalive() const {
    if (!m_lastKeepalive) {
        return Duration::infinite();
    }

    return m_lastKeepalive->elapsed();
}

}