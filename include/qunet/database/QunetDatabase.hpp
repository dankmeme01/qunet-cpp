#pragma once

#include <qunet/buffers/ByteReader.hpp>
#include <qunet/buffers/Error.hpp>
#include <qsox/SocketAddress.hpp>
#include <stdint.h>
#include <stddef.h>
#include <optional>
#include <vector>
#include <filesystem>

namespace qn {

struct DatabaseDecodeError {
    typedef enum {
        UnsupportedVersion,
        InvalidHeader,
        SectionSizeInvalid,
        ZstdDictTooLarge,
    } CustomCode;

    struct CustomKind {
        CustomCode code;

        std::string_view message() const;

        bool operator==(const CustomKind& other) const = default;
        bool operator!=(const CustomKind& other) const = default;
    };

    inline DatabaseDecodeError(CustomCode code) : m_kind(CustomKind{code}) {}
    inline DatabaseDecodeError(ByteReaderError error) : m_kind(error) {}

    bool operator==(const DatabaseDecodeError& other) const = default;
    bool operator!=(const DatabaseDecodeError& other) const = default;

    std::variant<ByteReaderError, CustomKind> m_kind;

    std::string_view message() const;
};

constexpr uint16_t QUNET_DATABASE_VERSION = 1;

struct QunetDatabase {
    std::optional<std::vector<uint8_t>> zstdDict;
    std::optional<std::vector<uint8_t>> lz4Dict;
    int zstdLevel;
    int lz4Level;
    std::array<uint8_t, 16> hash;

    static geode::Result<QunetDatabase, DatabaseDecodeError> decode(const std::vector<uint8_t>& data);

    std::array<uint8_t, 16> getHash() const;

private:
    geode::Result<void, DatabaseDecodeError> decodeSection(uint16_t type, size_t size, ByteReader& reader);
    geode::Result<void, DatabaseDecodeError> decodeZstdDictSection(size_t size, ByteReader& reader);

    static geode::Result<QunetDatabase, DatabaseDecodeError> decode(ByteReader& reader);
};

std::optional<QunetDatabase> tryFindQdb(const std::filesystem::path& folder, const qsox::SocketAddress& address);
geode::Result<> saveQdb(
    const std::vector<uint8_t>& data,
    const std::filesystem::path& folder,
    const qsox::SocketAddress& address
);

}