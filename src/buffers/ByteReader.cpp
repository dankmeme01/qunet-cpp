#include <qunet/buffers/ByteReader.hpp>
#include <cstring>

template <typename T = void>
using Result = qn::ByteReader::Result<T>;

namespace qn {

ByteReader::ByteReader(std::span<const uint8_t> data) : m_data(data) {}
ByteReader::ByteReader(const uint8_t* data, size_t size) : m_data(data, data + size) {}
ByteReader::ByteReader(const std::vector<uint8_t>& data) : m_data(data.begin(), data.end()) {}

Result<void> ByteReader::readBytes(uint8_t* data, size_t size) {
    auto span = GEODE_UNWRAP(this->readBytes(size));
    std::memcpy(data, span.data(), size);
    return Ok();
}

Result<std::span<const uint8_t>> ByteReader::readBytes(size_t size) {
    if (m_pos + size > m_data.size()) {
        return Err(ByteReaderError::OutOfBoundsRead);
    }

    auto begin = m_data.begin() + m_pos;
    auto end = begin + size;
    m_pos += size;

    return Ok(std::span{begin, end});
}

Result<void> ByteReader::skip(size_t size) {
    if (m_pos + size > m_data.size())  {
        return Err(ByteReaderError::OutOfBoundsRead);
    }

    m_pos += size;

    return Ok();
}

Result<uint8_t> ByteReader::readU8() {
    return this->readBytesAs<uint8_t>();
}

Result<uint16_t> ByteReader::readU16() {
    return this->readBytesAs<uint16_t>();
}

Result<uint32_t> ByteReader::readU32() {
    return this->readBytesAs<uint32_t>();
}

Result<uint64_t> ByteReader::readU64() {
    return this->readBytesAs<uint64_t>();
}

Result<bool> ByteReader::readBool() {
    return this->readU8().map([](uint8_t v) { return v != 0; });
}

Result<int8_t> ByteReader::readI8() {
    return this->readBytesAs<int8_t>();
}

Result<int16_t> ByteReader::readI16() {
    return this->readBytesAs<int16_t>();
}

Result<int32_t> ByteReader::readI32() {
    return this->readBytesAs<int32_t>();
}

Result<int64_t> ByteReader::readI64() {
    return this->readBytesAs<int64_t>();
}

Result<float> ByteReader::readF32() {
    return this->readBytesAs<float>();
}

Result<double> ByteReader::readF64() {
    return this->readBytesAs<double>();
}

Result<float> ByteReader::readFloat() {
    return this->readF32();
}

Result<double> ByteReader::readDouble() {
    return this->readF64();
}

Result<int64_t> ByteReader::readVarInt() {
    // TODO
    return Err(ByteReaderError::VarintOverflow);
}
Result<uint64_t> ByteReader::readVarUint() {
    // TODO
    return Err(ByteReaderError::VarintOverflow);
}

Result<std::string> ByteReader::readStringVar() {
    auto len = GEODE_UNWRAP(this->readVarUint());
    return this->readFixedString(len);
}

Result<std::string> ByteReader::readStringU8() {
    auto len = GEODE_UNWRAP(this->readU8());
    return this->readFixedString(len);
}

Result<std::string> ByteReader::readStringU16() {
    auto len = GEODE_UNWRAP(this->readU16());
    return this->readFixedString(len);
}

Result<std::string> ByteReader::readFixedString(size_t len) {
    if (len > 1024 * 1024) {
        return Err(ByteReaderError::StringTooLong);
    }

    std::string out(len, '\0');
    GEODE_UNWRAP(this->readBytes((uint8_t*)out.data(), len));

    return Ok(std::move(out));
}

std::span<const uint8_t> ByteReader::remaining() const {
    return std::span{m_data.begin() + m_pos, m_data.end()};
}

size_t ByteReader::remainingSize() const {
    return m_data.size() - m_pos;
}

size_t ByteReader::position() const {
    return m_pos;
}

void ByteReader::setPosition(size_t pos) {
    m_pos = pos;
}

}