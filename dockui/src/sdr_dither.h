#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace docky::color {

inline float QuantizeThreshold(unsigned x, unsigned y) {
    x = (x ^ y) & 7u;
    y &= 7u;
    const unsigned rank = ((x & 1u) << 5) | ((y & 1u) << 4) |
                          ((x & 2u) << 2) | ((y & 2u) << 1) |
                          ((x & 4u) >> 1) | ((y & 4u) >> 2);
    return (static_cast<float>(rank) + 0.5f) / 64.0f - 0.5f;
}

inline unsigned Quantize8(float levels, unsigned x, unsigned y) {
    return static_cast<unsigned>(std::clamp(std::floor(levels + QuantizeThreshold(x, y) + 0.5f), 0.f, 255.f));
}

}  // namespace docky::color
