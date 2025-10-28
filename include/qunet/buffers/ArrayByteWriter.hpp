#pragma once

#include "ByteWriter.hpp"

namespace qn {

// A non-growable non-allocating wrapper around a ByteWriter using a char array as storage.
template <size_t N>
class ArrayByteWriter : public ByteWriter<std::array<uint8_t, N>> {
public:
    ArrayByteWriter() {
        this->m_writer = &m_buffer;
    }

    ~ArrayByteWriter() = default;

    ArrayByteWriter(const ArrayByteWriter& other) = default;
    ArrayByteWriter& operator=(const ArrayByteWriter& other) = default;
    ArrayByteWriter(ArrayByteWriter&& other) noexcept = default;
    ArrayByteWriter& operator=(ArrayByteWriter&& other) noexcept = default;

    std::vector<uint8_t> toVector() const {
        return std::vector<uint8_t>(m_buffer.data(), m_buffer.data() + this->m_pos);
    }

    const std::array<uint8_t, N>& data() const {
        return m_buffer;
    }

    std::array<uint8_t, N>& data() {
        return m_buffer;
    }

private:
    std::array<uint8_t, N> m_buffer;
};

}