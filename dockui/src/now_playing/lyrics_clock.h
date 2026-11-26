#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace nowplaying::lyrics {

constexpr int64_t kTicksPerSecond = 10000000;

// GSMTC's position describes the timestamp of that observation. A player that
// publishes whole seconds only constrains the estimate to a one-second interval.
inline int64_t CorrectPlaybackPosition(int64_t estimate, int64_t observed, bool wholeSeconds, bool reanchor) {
    const int64_t upper = observed + (wholeSeconds ? kTicksPerSecond - 1 : 0);
    if (reanchor) return observed;
    const int64_t correction = std::clamp(estimate, observed, upper) - estimate;
    if (std::llabs(correction) > 3 * kTicksPerSecond / 2) return estimate + correction;
    return estimate + correction / 4;
}

inline uint64_t OffsetKey(std::wstring_view track, int durationMs) {
    uint64_t hash = 14695981039346656037ull;
    for (wchar_t ch : track) {
        hash ^= static_cast<uint32_t>(ch);
        hash *= 1099511628211ull;
    }
    hash ^= static_cast<uint64_t>((int64_t(durationMs) + 500) / 1000);
    return hash * 1099511628211ull;
}

}  // namespace nowplaying::lyrics
