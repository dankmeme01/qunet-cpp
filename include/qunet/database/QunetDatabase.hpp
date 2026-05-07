#pragma once

#include <dbuf/ByteReader.hpp>
#include <qsox/SocketAddress.hpp>
#include <qunet/util/Result.hpp>
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
    inline DatabaseDecodeError(std::string error) : m_kind(error) {}

    bool operator==(const DatabaseDecodeError& other) const = default;
    bool operator!=(const DatabaseDecodeError& other) const = default;

    std::variant<std::string, CustomKind> m_kind;

    std::string_view message() const;
};
inline auto format_as(const DatabaseDecodeError& err) { return err.message(); }

constexpr uint16_t QUNET_DATABASE_VERSION = 1;

struct QunetDatabase {
    std::optional<std::vector<uint8_t>> zstdDict;
    int zstdLevel = 0;
    std::array<uint8_t, 16> hash;

    static Result<QunetDatabase, DatabaseDecodeError> decode(const std::vector<uint8_t>& data);

    std::array<uint8_t, 16> getHash() const;

private:
    Result<void, DatabaseDecodeError> decodeSection(uint16_t type, size_t size, dbuf::ByteReader<>& reader);
    Result<void, DatabaseDecodeError> decodeZstdDictSection(size_t size, dbuf::ByteReader<>& reader);

    static Result<QunetDatabase, DatabaseDecodeError> decode(dbuf::ByteReader<>& reader);
};

std::optional<QunetDatabase> tryFindQdb(const std::filesystem::path& folder, const qsox::SocketAddress& address);
Result<> saveQdb(
    const std::vector<uint8_t>& data,
    const std::filesystem::path& folder,
    const qsox::SocketAddress& address
);

}