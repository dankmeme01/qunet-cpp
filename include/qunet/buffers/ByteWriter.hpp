#pragma once

#include "Error.hpp"
#include <qunet/util/assert.hpp>

#include <cstring>
#include <vector>
#include <span>
#include <string_view>
#include <stdint.h>

namespace qn {

template <typename T = void>
using ByteWriterResult = geode::Result<T, ByteWriterError>;

template <bool CanExpand>
using WriteBytesResult = std::conditional_t<CanExpand, void, ByteWriterResult<>>;

template <typename T>
struct ByteWriterBuffer {
    static constexpr bool CanExpand = false;

    static WriteBytesResult<CanExpand> writeBytes(T& writer, size_t pos, const uint8_t* data, size_t size) {
        static_assert(!std::is_same_v<T, T>, "ByteWriterBuffer not specialized for this type");
    }

    static WriteBytesResult<CanExpand> writeZeroes(T& writer, size_t pos, size_t size) {
        static_assert(!std::is_same_v<T, T>, "ByteWriterBuffer not specialized for this type");
    }

    static size_t capacity(T& writer) {
        static_assert(!std::is_same_v<T, T>, "ByteWriterBuffer not specialized for this type");
    }

    static std::span<const uint8_t> written(T& writer, size_t pos) {
        static_assert(!std::is_same_v<T, T>, "ByteWriterBuffer not specialized for this type");
    }
};

template <typename Wr>
class ByteWriter {
public:
    template <typename T = void>
    using Result = ByteWriterResult<T>;
    using WBResult = WriteBytesResult<ByteWriterBuffer<Wr>::CanExpand>;
    static constexpr bool CanExpand = ByteWriterBuffer<Wr>::CanExpand;

    ByteWriter(Wr& writer) : m_writer(&writer) {}
    ~ByteWriter() = default;

    ByteWriter(const ByteWriter& other) = default;
    ByteWriter& operator=(const ByteWriter& other) = default;
    ByteWriter(ByteWriter&& other) noexcept = default;
    ByteWriter& operator=(ByteWriter&& other) noexcept = default;

    WBResult writeBytes(const uint8_t* data, size_t size) {
        if constexpr (CanExpand) {
            ByteWriterBuffer<Wr>::writeBytes(*m_writer, m_pos, data, size);
            m_pos += size;
        } else {
            GEODE_UNWRAP(ByteWriterBuffer<Wr>::writeBytes(*m_writer, m_pos, data, size));
            m_pos += size;
            return Ok();
        }
    }

    WBResult writeBytes(std::span<const uint8_t> data) {
        return this->writeBytes(data.data(), data.size());
    }

    WBResult writeBytes(const std::vector<uint8_t>& data) {
        return this->writeBytes(data.data(), data.size());
    }

    WBResult writeZeroes(size_t size) {
        if constexpr (CanExpand) {
            ByteWriterBuffer<Wr>::writeZeroes(*m_writer, m_pos, size);
            m_pos += size;
        } else {
            GEODE_UNWRAP(ByteWriterBuffer<Wr>::writeZeroes(*m_writer, m_pos, size));
            m_pos += size;
            return Ok();
        }
    }

    WBResult writeU8(uint8_t value) {
        return this->writeBytes((const uint8_t*)&value, sizeof(value));
    }

    WBResult writeU16(uint16_t value) {
        return this->writeBytes((const uint8_t*)&value, sizeof(value));
    }

    WBResult writeU32(uint32_t value) {
        return this->writeBytes((const uint8_t*)&value, sizeof(value));
    }

    WBResult writeU64(uint64_t value) {
        return this->writeBytes((const uint8_t*)&value, sizeof(value));
    }

    WBResult writeBool(bool value) {
        return this->writeU8(value ? 1 : 0);
    }

    WBResult writeI8(int8_t value) {
        return this->writeBytes((const uint8_t*)&value, sizeof(value));
    }

    WBResult writeI16(int16_t value) {
        return this->writeBytes((const uint8_t*)&value, sizeof(value));
    }

    WBResult writeI32(int32_t value) {
        return this->writeBytes((const uint8_t*)&value, sizeof(value));
    }

    WBResult writeI64(int64_t value) {
        return this->writeBytes((const uint8_t*)&value, sizeof(value));
    }

    WBResult writeF32(float value) {
        return this->writeBytes((const uint8_t*)&value, sizeof(value));
    }

    WBResult writeF64(double value) {
        return this->writeBytes((const uint8_t*)&value, sizeof(value));
    }

    WBResult writeFloat(float value) {
        return this->writeF32(value);
    }

    WBResult writeDouble(double value) {
        return this->writeF64(value);
    }

    Result<> writeVarInt(int64_t value) {
        QN_ASSERT(false && "varint encoding not implemented yet");
        return Ok();
    }

    Result<> writeVarUint(uint64_t value) {
        size_t written = 0;

        while (true) {
            uint8_t byte = value & 0x7f;
            value >>= 7;

            if (value != 0) {
                // set continuation bit
                byte |= 0x80;
            }

            if constexpr (CanExpand) {
                this->writeU8(byte);
            } else {
                GEODE_UNWRAP(this->writeU8(byte));
            }

            written++;

            if (value == 0) {
                break;
            }
        }

        return Ok();
    }

    WBResult writeStringVar(std::string_view str) {
        if constexpr (CanExpand) {
            this->writeVarUint(str.size());
            this->writeBytes((const uint8_t*)str.data(), str.size());
        } else {
            GEODE_UNWRAP(this->writeVarUint(str.size()));
            GEODE_UNWRAP(this->writeBytes((const uint8_t*)str.data(), str.size()));
            return Ok();
        }
    }

    Result<void> writeStringU8(std::string_view str) {
        if (str.size() > std::numeric_limits<uint8_t>::max()) {
            return Err(ByteWriterError::StringTooLong);
        }

        if constexpr (CanExpand) {
            this->writeU8(str.size());
            this->writeBytes((const uint8_t*)str.data(), str.size());
        } else {
            GEODE_UNWRAP(this->writeU8(str.size()));
            GEODE_UNWRAP(this->writeBytes((const uint8_t*)str.data(), str.size()));
        }

        return Ok();
    }

    Result<void> writeStringU16(std::string_view str) {
        if (str.size() > std::numeric_limits<uint16_t>::max()) {
            return Err(ByteWriterError::StringTooLong);
        }

        if constexpr (CanExpand) {
            this->writeU16(str.size());
            this->writeBytes((const uint8_t*)str.data(), str.size());
        } else {
            GEODE_UNWRAP(this->writeU16(str.size()));
            GEODE_UNWRAP(this->writeBytes((const uint8_t*)str.data(), str.size()));
        }

        return Ok();
    }

    Result<void> writeStringU32(std::string_view str) {
        if (str.size() > std::numeric_limits<uint32_t>::max()) {
            return Err(ByteWriterError::StringTooLong);
        }

        if constexpr (CanExpand) {
            this->writeU32(str.size());
            this->writeBytes((const uint8_t*)str.data(), str.size());
        } else {
            GEODE_UNWRAP(this->writeU32(str.size()));
            GEODE_UNWRAP(this->writeBytes((const uint8_t*)str.data(), str.size()));
        }

        return Ok();
    }

    Result<void> setPosition(size_t pos) {
        if (pos > ByteWriterBuffer<Wr>::capacity(*m_writer)) {
            return Err(ByteWriterError::OutOfBoundsWrite);
        }
        m_pos = pos;
        return Ok();
    }

    size_t position() const {
        return m_pos;
    }

    std::span<const uint8_t> written() const {
        return ByteWriterBuffer<Wr>::written(*m_writer, m_pos);
    }

    Result<void> performAt(size_t pos, auto&& func) {
        if (pos > ByteWriterBuffer<Wr>::capacity(*m_writer)) {
            return Err(ByteWriterError::OutOfBoundsWrite);
        }

        size_t oldPos = m_pos;
        m_pos = pos;
        func(*this);
        m_pos = oldPos;
        return Ok();
    }

protected:
    ByteWriter() : m_writer(nullptr) {}

    Wr* m_writer;
    size_t m_pos = 0;
};

// Buffer specialization for vector<uint8_t>

template <>
struct ByteWriterBuffer<std::vector<uint8_t>> {
    static constexpr bool CanExpand = true;

    static WriteBytesResult<CanExpand> writeBytes(std::vector<uint8_t>& writer, size_t pos, const uint8_t* data, size_t size) {
        size_t end = pos + size;

        if (end > writer.size()) {
            writer.resize(end);
        }

        std::memcpy(writer.data() + pos, data, size);
    }

    static WriteBytesResult<CanExpand> writeZeroes(std::vector<uint8_t>& writer, size_t pos, size_t size) {
        size_t end = pos + size;

        if (end > writer.size()) {
            writer.resize(end);
        }

        std::memset(writer.data() + pos, 0, size);
    }

    static size_t capacity(std::vector<uint8_t>& writer) {
        return writer.size();
    }

    static std::span<const uint8_t> written(std::vector<uint8_t>& writer, size_t pos) {
        return std::span{writer.data(), pos};
    }
};

// Buffer specialization for array<uint8_t, N>

template <size_t N>
struct ByteWriterBuffer<std::array<uint8_t, N>> {
    static constexpr bool CanExpand = false;

    static WriteBytesResult<CanExpand> writeBytes(std::array<uint8_t, N>& writer, size_t pos, const uint8_t* data, size_t size) {
        size_t end = pos + size;

        if (end > writer.size()) {
            return Err(ByteWriterError::OutOfBoundsWrite);
        }

        std::memcpy(writer.data() + pos, data, size);
        return Ok();
    }

    static WriteBytesResult<CanExpand> writeZeroes(std::array<uint8_t, N>& writer, size_t pos, size_t size) {
        size_t end = pos + size;

        if (end > writer.size()) {
            return Err(ByteWriterError::OutOfBoundsWrite);
        }

        std::memset(writer.data() + pos, 0, size);
        return Ok();
    }

    static size_t capacity(std::array<uint8_t, N>& writer) {
        return N;
    }

    static std::span<const uint8_t> written(std::array<uint8_t, N>& writer, size_t pos) {
        return std::span{writer.data(), pos};
    }
};

}