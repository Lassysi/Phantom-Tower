// Item-editor invariants; no game pointers and no state mutation.
#pragma once
#include <cstdint>
#include <limits>
#include "inventory.catalog.generated.h"

namespace pbz::inv::policy {
inline constexpr int32_t QuantityLimit = 99999;
inline constexpr int32_t DustLimit = 999999999;
inline bool KindMatches(const catalog::Item& item, const std::string& kind) {
    if (item.component) return false;
    if (kind == "weapon_max") return item.kind == catalog::Kind::Weapon || item.kind == catalog::Kind::Shaqi;
    if (kind == "grant") return item.quantity && item.grant;
    if (kind == "stack") return item.quantity || item.observed;
    if (kind == "add_stack") return (item.quantity && !item.grant) || item.observed;
    switch (item.kind) {
    case catalog::Kind::Weapon: return kind == "weapon";
    case catalog::Kind::Shaqi: return kind == "shaqi_weapon";
    case catalog::Kind::Accessory: return kind == "accessory";
    case catalog::Kind::Medicine: return kind == "medicine";
    case catalog::Kind::Skin: return kind == "skin";
    default: return kind == "item" && !item.quantity;
    }
}
inline bool Counts(int32_t current, int32_t acquired) { return current >= 0 && acquired >= current; }
inline int32_t TargetLimit(int32_t current, int32_t acquired) {
    if (!Counts(current, acquired)) return -1;
    const int64_t room = int64_t(INT32_MAX) - acquired + current;
    return static_cast<int32_t>((std::min)(int64_t(QuantityLimit), room));
}
inline bool Quantity(int64_t target, int32_t current, int32_t acquired) {
    return Counts(current, acquired) && target >= 0 && target <= TargetLimit(current, acquired);
}
inline bool Grant(int64_t count, int32_t current, int32_t acquired) {
    return Counts(current, acquired) && count > 0 && count <= QuantityLimit && count <= int64_t(INT32_MAX) - acquired;
}
inline int32_t AdditionLimit(int32_t current, int32_t acquired) {
    if (!Counts(current, acquired)) return 0;
    return static_cast<int32_t>((std::min)(int64_t(QuantityLimit), int64_t(INT32_MAX) - acquired));
}
inline bool Addition(int64_t count, int32_t current, int32_t acquired) {
    return count > 0 && count <= AdditionLimit(current, acquired);
}
}  // namespace pbz::inv::policy
