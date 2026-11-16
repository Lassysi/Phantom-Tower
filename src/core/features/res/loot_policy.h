#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace pbz::res::loot {
enum class Origin { Unknown, Enemy, Pickup, Chest };
struct Quantity {
    int32_t amount = 0;
    bool changed = false;
};

// FName is an index + an optional stored numeric suffix, not just an index.
// Number=1 means _0. Names with literal leading-zero suffixes may remain wholly
// in the comparison string, so Number=0 must not alter them.
inline std::string FullItemName(std::string base, uint32_t number) {
    if (base.empty() || base == "None") return {};
    if (number) base += "_" + std::to_string(number - 1);
    return base;
}

inline bool ValidMultiplier(double multiplier) {
    return std::isfinite(multiplier) && multiplier >= 0 && multiplier <= 1000 &&
           std::trunc(multiplier) == multiplier;
}

// GetItemNumInfo.MaxNum is HasMaxAmount in this build, NOT a capacity:
// AddPropsToBackPack increments both CurrentAmount and HasMaxAmount. Guard
// both counters against overflow, and additionally honor a separately proven
// native capacity when one becomes available. Never clamp against the total.
inline Quantity ScaleGrant(int32_t input, double multiplier, int32_t current,
                           int32_t acquired, int32_t nativeCapacity = INT32_MAX) {
    Quantity result{input, false};
    if (input <= 0 || !ValidMultiplier(multiplier) || current < 0 || acquired < current ||
        nativeCapacity < current || multiplier == 1) return result;
    const int64_t room = (std::min)(int64_t(nativeCapacity) - current,
                                  int64_t(INT32_MAX) - acquired);
    // Leave an already out-of-policy native request alone. Do not shrink a
    // legitimate grant merely because a queried limit is smaller than input.
    if (input > room) return result;
    // Integer-only resource multipliers: no per-grant rounding or fractional
    // carry. The largest accepted product (INT32_MAX * 1000) fits in int64_t.
    const int64_t requested = int64_t(input) * static_cast<int64_t>(multiplier);
    result.amount = static_cast<int32_t>((std::min)(requested, room));
    result.changed = result.amount != input;
    return result;
}

}  // namespace pbz::res::loot
