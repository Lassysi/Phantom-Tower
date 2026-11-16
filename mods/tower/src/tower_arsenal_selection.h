#pragma once

#include <cstdint>
#include <span>

namespace tower::arsenal {

// Native confirmation replaces one of the backpack's two slots.
inline int ReplacedSlot(std::span<const uint64_t> before, std::span<const uint64_t> after) {
    if (before.size() != 2 || after.size() != 2) return -1;
    const bool first = before[0] != after[0];
    const bool second = before[1] != after[1];
    return first == second ? -1 : first ? 0 : 1;
}

}  // namespace tower::arsenal
