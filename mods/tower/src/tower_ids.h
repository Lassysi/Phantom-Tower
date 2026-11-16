#pragma once
#include <string_view>
#include <vector>
#include <string>
#include "tower_json.h"

// Game item identifiers are FNames: ASCII case does not identify another item.
// JSON field names remain case sensitive; use this only inside ID-keyed maps.
namespace tower::ids {
constexpr char Fold(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }
constexpr bool Equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (Fold(a[i]) != Fold(b[i])) return false;
    return true;
}
inline bool Equal(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (!Equal(a[i], b[i])) return false;
    return true;
}
// 装备槽数组：原生空槽是 FName None（索引 0），从内存读回是空字符串；存档 JSON 写的是 "None"。
// 两种写法是同一个空槽；格数和顺序仍逐格比（a7c1c2d 首次进塔：影之武装 [UnnamedBow, None] 被读成 [UnnamedBow, ""]，核验三次拒绝）
inline bool EmptySlot(std::string_view s) { return s.empty() || Equal(s, "None"); }
inline bool SameSlots(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (EmptySlot(a[i]) ? !EmptySlot(b[i]) : !Equal(a[i], b[i])) return false;
    return true;
}
struct Lookup {
    const json::Value* value = nullptr;
    bool ambiguous = false;
};
inline Lookup Find(const json::Value* object, std::string_view id) {
    Lookup out;
    if (!object || object->kind != json::Value::Object) return out;
    for (const auto& [key, v] : object->fields) if (Equal(key, id)) {
        if (out.value) return {nullptr, true}; // Never choose silently between duplicate FNames.
        out.value = &v;
    }
    return out;
}
} // namespace tower::ids
