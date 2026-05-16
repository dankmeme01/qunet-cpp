#pragma once
#include <cstddef>
#include <random>
#include <qunet/protocol/constants.hpp>
#include <dbuf/ByteWriter.hpp>
#include <qunet/util/assert.hpp>

namespace qn {

struct QunetPadRNG {
    QunetPadRNG() {
        std::random_device rd;
        m_x = rd();
        m_y = rd();
        m_z = rd();
    }

    uint64_t next() {
        auto rotl = [](uint64_t x, int k) {
            return (x << k) | (x >> (64 - k));
        };

        uint64_t xp = m_x, yp = m_y, zp = m_z;
        m_x = 15241094284759029579u * zp;
        m_y = yp - xp; m_y = rotl(m_y, 12);
        m_z = zp - yp; m_z = rotl(m_z, 44);
        return xp;
    }

    uint64_t m_x, m_y, m_z;
};


/// Pad all control messages to be at least 64 bytes, so that it's less likely they are dropped by bad middleboxes.
/// 64 - 14 (ethernet) - 20 (ipv4) - 8 (udp) = 22 bytes
static constexpr size_t MINIMUM_UDP_PAYLOAD = 22;

inline void writePaddingBytes(dbuf::ByteWriter<>& writer, size_t bytes) {
    static thread_local QunetPadRNG rng;

    size_t filled = 0;
    while (filled < bytes) {
        uint64_t rand = rng.next();
        size_t toWrite = std::min(sizeof(rand), bytes - filled);
        writer.writeBytes((uint8_t*)&rand, toWrite);
        filled += toWrite;
    }
}

inline void writePadMessage(dbuf::ByteWriter<>& writer, size_t bytes) {
    QN_ASSERT(bytes > 0);

    writer.writeU8(qn::MSG_PADDING);
    bytes--;

    qn::writePaddingBytes(writer, bytes);
}

inline void padMessageToMinimum(dbuf::ByteWriter<>& writer) {
    (void) writer.setPosition(writer.written().size());

    auto pos = writer.position();
    if (pos < MINIMUM_UDP_PAYLOAD) {
        writePadMessage(writer, MINIMUM_UDP_PAYLOAD - pos);
    }
}

}
