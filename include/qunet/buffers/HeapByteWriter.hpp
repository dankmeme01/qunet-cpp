#pragma once

#include "ByteWriter.hpp"

namespace qn {

// A simple wrapper around a ByteWriter using a growable std::vector<uint8_t> as backing storage.
class HeapByteWriter : public ByteWriter<std::vector<uint8_t>> {
public:

    HeapByteWriter();
    ~HeapByteWriter() = default;

    HeapByteWriter(const std::vector<uint8_t>& data);
    HeapByteWriter(std::vector<uint8_t>&& data);
    HeapByteWriter(std::span<const uint8_t> data);
    HeapByteWriter(const uint8_t* data, size_t size);

    HeapByteWriter(const HeapByteWriter& other) = default;
    HeapByteWriter& operator=(const HeapByteWriter& other) = default;
    HeapByteWriter(HeapByteWriter&& other) noexcept = default;
    HeapByteWriter& operator=(HeapByteWriter&& other) noexcept = default;

    std::vector<uint8_t> toVector() const;
    std::vector<uint8_t> intoVector() &&;

private:
    std::vector<uint8_t> m_buffer;
};

}