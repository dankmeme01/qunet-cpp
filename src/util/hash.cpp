#include <qunet/util/hash.hpp>
#include <blake3.h>

namespace qn {

Hash<32> blake3Hash(const uint8_t* data, size_t size) {
    return blake3Hash(std::span(data, size));
}

Hash<32> blake3Hash(std::span<const uint8_t> data) {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, data.data(), data.size());

    Hash<32> hash;
    blake3_hasher_finalize(&hasher, hash.data, 32);

    return hash;
}

Hash<32> blake3Hash(const std::string& data) {
    return blake3Hash((const uint8_t*)data.data(), data.size());
}

Hash<32> blake3Hash(const std::vector<uint8_t>& data) {
    return blake3Hash(std::span(data.begin(), data.end()));
}

}