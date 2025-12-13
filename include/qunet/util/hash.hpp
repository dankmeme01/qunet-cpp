#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>
#include <span>

namespace qn {

std::string hexEncode(const uint8_t* data, size_t size);

template <size_t N>
struct Hash {
    uint8_t data[N];

    bool operator==(const Hash& other) const;
    bool operator!=(const Hash& other) const;
    bool operator<(const Hash& other) const;

    std::string toString() const {
        return hexEncode(data, N);
    }
};

Hash<32> blake3Hash(const uint8_t* data, size_t size);
Hash<32> blake3Hash(std::span<const uint8_t> data);
Hash<32> blake3Hash(const std::string& data);
Hash<32> blake3Hash(const std::vector<uint8_t>& data);

}