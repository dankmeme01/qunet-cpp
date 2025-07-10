#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <span>

namespace qn {

template <size_t N>
struct Hash {
    uint8_t data[N];

    bool operator==(const Hash& other) const;
    bool operator!=(const Hash& other) const;
    bool operator<(const Hash& other) const;

    std::string toString() const {
        std::string str;
        str.reserve(N * 2);

        constexpr char hexChars[] = "0123456789abcdef";

        for (size_t i = 0; i < N; ++i) {
            str.push_back(hexChars[(data[i] >> 4) & 0x0F]);
            str.push_back(hexChars[data[i] & 0x0F]);
        }

        return str;
    }
};

Hash<32> blake3Hash(const uint8_t* data, size_t size);
Hash<32> blake3Hash(std::span<const uint8_t> data);

}