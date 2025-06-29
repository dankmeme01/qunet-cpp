#pragma once

#include <vector>
#include <cstring>
#include <stdint.h>
#include <stddef.h>

namespace qn {

class SlidingBuffer {
public:
    SlidingBuffer(size_t initialSize, size_t maxSize) {
        m_data.resize(initialSize);
        m_maxSize = maxSize;
    }

    SlidingBuffer(const SlidingBuffer& other) = default;
    SlidingBuffer& operator=(const SlidingBuffer& other) = default;
    SlidingBuffer(SlidingBuffer&& other) noexcept = default;
    SlidingBuffer& operator=(SlidingBuffer&& other) noexcept = default;

    // Writes data into this buffer, returns false if there is not enough space
    bool write(const void* data, size_t len) {
        if (len == 0) return true;

        size_t remSpace = m_data.size() - m_writePos;
        if (len > remSpace) {
            if (!this->resizeToFit(m_writePos + len)) {
                return false; // Not enough space
            }
        }

        std::memcpy(m_data.data() + m_writePos, data, len);
        m_writePos += len;

        return true;
    }

    // Returns the number of bytes that are available to read
    size_t canRead() const {
        return m_writePos - m_readPos;
    }

    // Returns the number of bytes that can be written to the buffer
    size_t canWrite() const {
        return m_data.size() - m_writePos;
    }

    // Returns the maximum number of bytes that can be written to the buffer
    size_t canWriteMax() const {
        return m_maxSize - m_writePos;
    }

    // Reads data from the buffer into the provided pointer (or skips bytes if null), returns the number of bytes read
    size_t read(void* buffer, size_t len) {
        size_t toRead = std::min(len, this->canRead());

        if (toRead == 0) {
            return 0;
        }

        if (buffer) {
            std::memcpy(buffer, m_data.data() + m_readPos, toRead);
        }

        m_readPos += toRead;

        // we can reset the buffer if everything was read
        if (m_readPos == m_writePos) {
            m_readPos = 0;
            m_writePos = 0;
        }

        return toRead;
    }

    const uint8_t* readPtr() const {
        return m_data.data() + m_readPos;
    }

    const uint8_t* writePtr() const {
        return m_data.data() + m_writePos;
    }

    const uint8_t* data() const {
        return m_data.data();
    }

    size_t size() const {
        return m_data.size();
    }

    size_t writePos() const {
        return m_writePos;
    }

    size_t readPos() const {
        return m_readPos;
    }

private:
    std::vector<uint8_t> m_data;
    size_t m_maxSize = 0;
    size_t m_writePos = 0;
    size_t m_readPos = 0;

    bool resizeToFit(size_t needed) {
        if (needed >= (1 << 31)) {
            // never allow buffers >= 2 GiB, prevent integer overflow
            return false;
        }

        size_t current = m_data.size();

        while (current < needed) {
            current *= 2;
        }

        if (current > m_maxSize) {
            return false;
        }

        m_data.resize(current);

        return true;
    }
};

}