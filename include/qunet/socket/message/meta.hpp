#pragma once

#include <stdint.h>
#include <stddef.h>
#include <optional>
#include <vector>

enum class CompressionType {
    None = 0,
    Zstd = 1,
    Lz4 = 2,
};

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
    std::optional<CompressionHeader> compressionHeader;
    std::optional<ReliabilityHeader> reliabilityHeader;
    std::optional<FragmentationHeader> fragmentationHeader;
    std::vector<uint8_t> data;
};
