#include <qunet/buffers/HeapByteWriter.hpp>
#include <qunet/util/assert.hpp>
#include <cstring>

template <typename T = void>
using Result = qn::HeapByteWriter::Result<T>;

namespace qn {

HeapByteWriter::HeapByteWriter() {
    m_writer = &m_buffer;
}

HeapByteWriter::HeapByteWriter(const std::vector<uint8_t>& data) : m_buffer(data) {
    m_writer = &m_buffer;
}

HeapByteWriter::HeapByteWriter(std::vector<uint8_t>&& data) : m_buffer(std::move(data)) {
    m_writer = &m_buffer;
}

HeapByteWriter::HeapByteWriter(std::span<const uint8_t> data) : m_buffer(data.begin(), data.end()) {
    m_writer = &m_buffer;
}

HeapByteWriter::HeapByteWriter(const uint8_t* data, size_t size) : m_buffer(data, data + size) {
    m_writer = &m_buffer;
}

std::vector<uint8_t> HeapByteWriter::toVector() const {
    return m_buffer;
}

std::vector<uint8_t> HeapByteWriter::intoVector() && {
    std::vector<uint8_t> result = std::move(m_buffer);
    m_buffer.clear();
    m_pos = 0;
    return result;
}

}