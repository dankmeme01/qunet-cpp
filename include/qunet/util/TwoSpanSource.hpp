#pragma once
#include <dbuf/ByteReader.hpp>
#include <cstring>

namespace qn {

struct TwoSpanSource {
    TwoSpanSource(std::span<const uint8_t> first, std::span<const uint8_t> second) : m_first(first), m_second(second) {}

    geode::Result<void> read(uint8_t* buf, size_t size) {
        if (m_pos + size > this->totalSize()) {
            return geode::Err("Not enough data to read");
        }

        if (m_pos < m_first.size()) {
            size_t firstSize = std::min(size, m_first.size() - m_pos);
            std::memcpy(buf, m_first.data() + m_pos, firstSize);
            m_pos += firstSize;
            buf += firstSize;
            size -= firstSize;
        }

        if (size > 0) {
            size_t off = m_pos - m_first.size();
            std::memcpy(buf, m_second.data() + off, size);
            m_pos += size;
        }

        return geode::Ok();
    }

    geode::Result<void> skip(size_t size) {
        if (m_pos + size > this->totalSize()) {
            return geode::Err("Not enough data to skip");
        }
        m_pos += size;
        return geode::Ok();
    }

    size_t position() const { return m_pos; }
    size_t totalSize() const { return m_first.size() + m_second.size(); }

    geode::Result<void> setPosition(size_t pos) {
        if (pos >= this->totalSize()) {
            return geode::Err("Position out of bounds");
        }
        m_pos = pos;
        return geode::Ok();
    }

private:
    std::span<const uint8_t> m_first;
    std::span<const uint8_t> m_second;
    size_t m_pos = 0;
};

}