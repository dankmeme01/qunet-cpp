#pragma once

#include <stdint.h>

namespace qn {

template <typename T>
constexpr inline T exponentialMovingAverage(T prev, T newv, double alpha) {
    return static_cast<T>(alpha * static_cast<double>(newv) + (1.0 - alpha) * static_cast<double>(prev));
}

}