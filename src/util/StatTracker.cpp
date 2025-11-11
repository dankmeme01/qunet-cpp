#include <qunet/util/StatTracker.hpp>
#include <asp/iter.hpp>

#include <unordered_set>

using namespace asp::time;

namespace qn {

size_t StatSnapshot::avgUpPacketSize() const {
    return bytesUp / packetsUp;
}

size_t StatSnapshot::avgDownPacketSize() const {
    return bytesDown / packetsDown;
}

StatTracker::StatTracker() {}

void StatTracker::setEnabled(bool enable) {
    m_enabled = enable;
}

StatSnapshot StatTracker::snapshot(asp::time::Duration period) const {
    constexpr size_t SMAX = std::numeric_limits<size_t>::max();
    constexpr float FMAX = std::numeric_limits<float>::max();

    auto now = Instant::now();

    // find range of packets/messages to include
    auto pend = m_packets.end();
    auto pstart = m_packets.end();

    if (period.isZero()) {
        pstart = m_packets.begin();
    } else {
        while (pstart != m_packets.begin()) {
            pstart = std::prev(pstart);
            if (now.durationSince(pstart->timestamp) > period) {
                pstart = std::next(pstart);
                break;
            }
        }
    }

    auto mend = m_messages.end();
    auto mstart = m_messages.end();

    if (period.isZero()) {
        mstart = m_messages.begin();
    } else {
        while (mstart != m_messages.begin()) {
            mstart = std::prev(mstart);
            if (now.durationSince(mstart->timestamp) > period) {
                mstart = std::next(mstart);
                break;
            }
        }
    }

    auto packets = std::span(pstart, pend);
    auto messages = std::span(mstart, mend);

    StatSnapshot snap{};
    snap.period = period.isZero() ? now.durationSince(m_startedAt) : period;
    snap.minDownPacketSize = snap.minUpPacketSize = SMAX;

    for (auto& msg : messages) {
        auto& count = msg.up ? snap.messagesUp : snap.messagesDown;
        count++;

        if (msg.compressedBytes && msg.bytes > 0) {
            auto& ccnt = msg.up ? snap.compressedUp : snap.compressedDown;
            auto& avgr = msg.up ? snap.compUpAvgRatio : snap.compDownAvgRatio;
            auto& saved = msg.up ? snap.compUpSavedBytes : snap.compDownSavedBytes;
            auto& savedp = msg.up ? snap.compUpSavedPercent : snap.compDownSavedPercent;

            ccnt++;

            float ratio = (float)*msg.compressedBytes / (float)msg.bytes;

            // will be post processed later
            avgr += ratio;
            saved += (msg.bytes - *msg.compressedBytes);
        }

        if (msg.relHeader) {
            auto& hdr = *msg.relHeader;
            auto& rcnt = msg.up ? snap.upReliable : snap.downReliable;

            rcnt++;
        }
    }

    this->countRelStats(snap, messages);

    for (auto& p : packets) {
        auto& count = p.up ? snap.packetsUp : snap.packetsDown;
        auto& bytes = p.up ? snap.bytesUp : snap.bytesDown;
        auto& min = p.up ? snap.minUpPacketSize : snap.minDownPacketSize;
        auto& max = p.up ? snap.maxUpPacketSize : snap.maxDownPacketSize;

        count++;
        bytes += p.bytes;

        if (p.bytes < min) {
            min = p.bytes;
        } else if (p.bytes > max) {
            max = p.bytes;
        }
    }

    if (snap.minDownPacketSize == SMAX) {
        snap.minDownPacketSize = 0;
    }
    if (snap.minUpPacketSize == SMAX) {
        snap.minUpPacketSize = 0;
    }

    snap.compUpAvgRatio = snap.compUpAvgRatio / snap.compressedUp;
    snap.compDownAvgRatio = snap.compDownAvgRatio / snap.compressedDown;
    snap.compUpSavedPercent = (float)snap.compUpSavedBytes / (float)(snap.bytesUp + snap.compUpSavedBytes);
    snap.compDownSavedPercent = (float)snap.compDownSavedBytes / (float)(snap.bytesDown + snap.compDownSavedBytes);

    return snap;
}

void StatTracker::countRelStats(StatSnapshot& snap, std::span<const Message> messages) const {
    snap.minAckDelay = Duration::infinite();

    static thread_local std::unordered_set<uint16_t> outIds;
    static thread_local std::unordered_map<uint16_t, std::pair<std::optional<Instant>, std::optional<Instant>>> acks;
    size_t ackDelayDenom = 0;

    outIds.clear();
    acks.clear();

    for (auto& msg : messages) {
        if (!msg.relHeader) continue;
        auto& hdr = *msg.relHeader;

        // record up retransmissions and times of sent messages
        if (msg.up && hdr.messageId != 0) {
            auto [_, unique] = outIds.insert(hdr.messageId);

            if (!unique) {
                snap.upRetransmissions++;
            } else {
                acks[hdr.messageId].first = msg.timestamp;
            }
        }

        // record acks to our reliable messages
        if (!msg.up && hdr.ackCount > 0) {
            for (size_t i = 0; i < hdr.ackCount; i++) {
                auto id = hdr.acks[i];
                auto& target = acks[id].second;

                if (!target) {
                    target = msg.timestamp;
                }
            }
        }
    }

    for (auto& [_, pair] : acks) {
        auto& msgAt = pair.first;
        auto& ackAt = pair.second;

        if (!msgAt) {
            // something weird? unknown ack?
            snap.downUnknownAcks++;
        } else if (!ackAt) {
            snap.upUnacked++;
        } else {
            auto delay = ackAt->durationSince(*msgAt);

            if (delay < snap.minAckDelay) {
                snap.minAckDelay = delay;
            }

            if (delay > snap.maxAckDelay) {
                snap.maxAckDelay = delay;
            }

            // will be post processed
            snap.avgAckDelay += delay;
            ackDelayDenom++;
        }
    }

    if (ackDelayDenom == 0) {
        snap.minAckDelay = Duration::zero();
        snap.avgAckDelay = Duration::zero();
    } else {
        snap.avgAckDelay = snap.avgAckDelay / ackDelayDenom;
    }
}

StatWholeSnapshot StatTracker::snapshotFull() const {
    StatWholeSnapshot snap = this->snapshot();

    snap.connectionLifetime = m_startedAt.elapsed();
    if (m_connectedAt) {
        snap.tookToConnect = m_connectedAt->durationSince(m_startedAt);

        if (m_handshakeFinishedAt) {
            snap.tookToHandshake = m_handshakeFinishedAt->durationSince(*m_connectedAt);
        }
    }

    return snap;
}

void StatTracker::onConnected() {
    m_connectedAt = Instant::now();
}

void StatTracker::onUpPacket(size_t bytes) {
    if (!m_enabled) return;

    m_packets.push_back(Packet {
        .timestamp = Instant::now(),
        .bytes = bytes,
        .up = true,
    });
}

void StatTracker::onDownPacket(size_t bytes) {
    if (!m_enabled) return;

    m_packets.push_back(Packet {
        .timestamp = Instant::now(),
        .bytes = bytes,
        .up = false,
    });
}

void StatTracker::onUpMessage(uint8_t header,
                    size_t bytes,
                    std::optional<size_t> compressedBytes,
                    asp::time::Duration compressionTime,
                    std::optional<ReliabilityHeader> relHeader)
{
    this->onMessage(header, true, bytes, compressedBytes, compressionTime, relHeader);
}

void StatTracker::onDownMessage(uint8_t header,
                    size_t bytes,
                    std::optional<size_t> compressedBytes,
                    asp::time::Duration compressionTime,
                    std::optional<ReliabilityHeader> relHeader)
{
    this->onMessage(header, false, bytes, compressedBytes, compressionTime, relHeader);
}

void StatTracker::onMessage(uint8_t header,
                    bool up,
                    size_t bytes,
                    std::optional<size_t> compressedBytes,
                    asp::time::Duration compressionTime,
                    std::optional<ReliabilityHeader> relHeader)
{
    if (!m_enabled) return;

    m_messages.push_back(Message {
        .timestamp = Instant::now(),
        .type = MessageType::Data, // TODO
        .bytes = bytes,
        .up = up,
        .compressedBytes = compressedBytes,
        .compressionTime = compressionTime,
        .relHeader = relHeader,
    });
}

}
