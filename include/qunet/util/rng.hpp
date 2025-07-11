#pragma once

#include <random>

namespace qn {

inline std::mt19937_64& rng() {
    static thread_local std::mt19937_64 generator(std::random_device{}());
    return generator;
}

inline bool randomChance(float chance) {
    auto& generator = rng();
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    return dist(generator) < chance;
}

}