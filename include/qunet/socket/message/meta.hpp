#pragma once

#include <stdint.h>
#include <stddef.h>
#include <optional>
#include <vector>
#include <fmt/format.h>

enum class CompressionType {
    None = 0,
    Zstd = 1,
    ZstdNoDict = 2,
    Lz4 = 3,
};

inline auto format_as(const CompressionType& ct) -> std::string_view {
    switch (ct) {
        case CompressionType::None: return "None";
        case CompressionType::Zstd: return "Zstd";
        case CompressionType::ZstdNoDict: return "Zstd (no dict)";
        case CompressionType::Lz4: return "Lz4";
        default: return "Unknown";
    }
}

struct CompressionHeader {
    CompressionType type;
    uint32_t uncompressedSize;
};

struct ReliabilityHeader {
    uint16_t messageId;
    size_t ackCount;
    uint16_t acks[8];
};

struct FragmentationHeader {
    uint16_t messageId;
    uint16_t fragmentIndex;
    bool lastFragment;
};

struct QunetMessageMeta {
    uint8_t type; // message type, for data messages this does not include options and will be MSG_DATA
    std::optional<CompressionHeader> compressionHeader;
    std::optional<ReliabilityHeader> reliabilityHeader;
    std::optional<FragmentationHeader> fragmentationHeader;
    std::vector<uint8_t> data;
};
