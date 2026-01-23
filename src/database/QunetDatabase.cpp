#include <qunet/database/QunetDatabase.hpp>
#include <qunet/util/visit.hpp>
#include <qunet/util/hash.hpp>
#include <qunet/Log.hpp>

#include <fmt/format.h>
#include <asp/fs.hpp>
#include <cstring>


namespace qn {

std::string_view DatabaseDecodeError::CustomKind::message() const {
    // this crashes clang on macos/android
    // using enum CustomCode;

    switch (code) {
        case CustomCode::UnsupportedVersion: return "Unsupported Qunet database version";
        case CustomCode::InvalidHeader: return "Invalid Qunet database header";
        case CustomCode::SectionSizeInvalid: return "Section size is invalid";
        case CustomCode::ZstdDictTooLarge: return "Zstd dictionary section is too large";
    }

    qn::unreachable();
}

std::string_view DatabaseDecodeError::message() const {
    return std::visit([](const auto& err) -> std::string_view {
        return err.message();
    }, m_kind);
}

geode::Result<QunetDatabase, DatabaseDecodeError> QunetDatabase::decode(const std::vector<uint8_t>& data) {
    ByteReader reader(data);
    auto qdb = GEODE_UNWRAP(decode(reader));

    // compute hash
    auto hash = qn::blake3Hash(data);
    std::memcpy(qdb.hash.data(), hash.data, 16);

    return Ok(std::move(qdb));
}

struct SectionHeader {
    uint16_t type;
    uint16_t options;
    uint32_t offset;
    uint32_t size;
};

static size_t roundUpTo16(size_t value) {
    return (value + 15) & ~(size_t)(15);
}

geode::Result<QunetDatabase, DatabaseDecodeError> QunetDatabase::decode(ByteReader& reader) {
    constexpr uint8_t MAGIC[] = { 0xa3, 0xdb, 0xdb, 0x11 };
    uint8_t magic[sizeof(MAGIC)];

    GEODE_UNWRAP(reader.readBytes(magic, sizeof(magic)));

    if (std::memcmp(magic, MAGIC, sizeof(MAGIC)) != 0) {
        return Err(DatabaseDecodeError::InvalidHeader);
    }

    auto version = GEODE_UNWRAP(reader.readU16());
    if (version != QUNET_DATABASE_VERSION) {
        return Err(DatabaseDecodeError::UnsupportedVersion);
    }

    auto sectionCount = GEODE_UNWRAP(reader.readU16());

    std::vector<SectionHeader> headers;

    for (size_t i = 0; i < sectionCount; ++i) {
        SectionHeader header;
        header.type = GEODE_UNWRAP(reader.readU16());
        header.options = GEODE_UNWRAP(reader.readU16());
        header.offset = GEODE_UNWRAP(reader.readU32());
        header.size = GEODE_UNWRAP(reader.readU32());

        if (header.size == 0 || header.offset == 0) {
            return Err(DatabaseDecodeError::SectionSizeInvalid);
        }

        headers.push_back(header);
    }

    size_t headerSize = 4 + 2 + 2 + (sectionCount * 12);

    size_t padding = roundUpTo16(headerSize) - headerSize;
    GEODE_UNWRAP(reader.skip(padding));

    size_t sectionsBegin = headerSize + padding;

    // reserve data in the buffer to fit all sections

    std::vector<uint8_t> dataBuffer;

    for (auto& section : headers) {
        size_t sectionEnd = section.offset + section.size - sectionsBegin;

        if (sectionEnd > dataBuffer.size()) {
            dataBuffer.resize(sectionEnd);
        }
    }

    // read all sections into the buffer
    GEODE_UNWRAP(reader.readBytes(dataBuffer.data(), dataBuffer.size()));

    QunetDatabase db;

    for (auto& section : headers) {
        size_t begin = section.offset - sectionsBegin;
        size_t end = begin + section.size;

        auto sectionData = std::span<const uint8_t>{dataBuffer}.subspan(begin, end - begin);
        ByteReader reader(sectionData);

        GEODE_UNWRAP(db.decodeSection(section.type, section.size, reader));
    }

    return Ok(std::move(db));
}

geode::Result<void, DatabaseDecodeError> QunetDatabase::decodeSection(uint16_t type, size_t size, ByteReader& reader) {
    switch (type) {
        case 3: {
            return this->decodeZstdDictSection(size, reader);
        } break;

        default: {
            // skip unknown section
            return Ok();
        } break;
    }
}

geode::Result<void, DatabaseDecodeError> QunetDatabase::decodeZstdDictSection(size_t size, ByteReader& reader) {
    if (size > 1024 * 1024) {
        return Err(DatabaseDecodeError::ZstdDictTooLarge);
    }

    zstdLevel = GEODE_UNWRAP(reader.readI32());
    zstdDict = reader.readToEnd();

    return Ok();
}

std::array<uint8_t, 16> QunetDatabase::getHash() const {
    return this->hash;
}

static std::filesystem::path pathForQdb(const std::filesystem::path& folder, const qsox::SocketAddress& address) {
    // use address hash as filename
    auto hash = qn::blake3Hash(address.toString());
    auto hexstr = hash.toString();
    hexstr.resize(32);

    return folder / (hexstr + ".qdb");
}

std::optional<QunetDatabase> tryFindQdb(const std::filesystem::path& folder, const qsox::SocketAddress& address) {
    auto qdbPath = pathForQdb(folder, address);

    if (!asp::fs::exists(qdbPath)) {
        return std::nullopt;
    }

    auto res = asp::fs::read(qdbPath);
    if (!res) {
        log::warn("Failed to read QDB file {}: {}", qdbPath.string(), res.unwrapErr());
        return std::nullopt;
    }

    auto qdb = QunetDatabase::decode(*res);
    if (!qdb) {
        log::warn("Failed to decode QDB file {}: {}", qdbPath.string(), qdb.unwrapErr().message());
        return std::nullopt;
    }

    return std::move(qdb).unwrap();
}

geode::Result<> saveQdb(
    const std::vector<uint8_t>& data,
    const std::filesystem::path& folder,
    const qsox::SocketAddress& address
) {
    auto qdbPath = pathForQdb(folder, address);
    if (!asp::fs::exists(folder)) {
        (void) asp::fs::createDirAll(folder);
    }

    return asp::fs::write(qdbPath, data);
}

}