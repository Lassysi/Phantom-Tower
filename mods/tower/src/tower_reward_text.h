#pragma once
#include <string>
#include <string_view>

// 只处理影塔过层奖励文案；信息页、商店和原生对话框均不改。
namespace tower::rewardui {
inline bool IsReward(std::string_view s) {
    return s.find("层通过 · 三选一") != s.npos;
}
inline void Replace(std::string& s, std::string_view from, std::string_view to) {
    for (size_t p = 0; (p = s.find(from, p)) != s.npos; p += to.size()) s.replace(p, from.size(), to);
}
inline std::string Compact(std::string s) {
    if (!IsReward(s)) return s;
    // 兼容本局已加载的旧 DLL：去掉累计数值/祝福，保留后面的奖励。
    for (const auto marker : {"\n\n本局加成", "\n\n当前加成"}) {
        const auto start = s.find(marker);
        if (start == s.npos) continue;
        auto end = s.find("\n\n三选一：", start);
        const auto reroll = s.find("\n\n本次三选一：", start);
        if (end == s.npos || (reroll != s.npos && reroll < end)) end = reroll;
        if (end != s.npos) s.erase(start, end - start);
    }
    Replace(s, "（开层前在背包里换上）", "");
    for (const auto kind : {"主兵器", "影之武装"})
        Replace(s, std::string("\n　　") + kind + "，1 级进塔档背包；Esc → 角色装备后生效，只在塔内", "");
    Replace(s, "全局 · 本局一直生效：", "本局：");
    Replace(s, "（每层）；本次 ", "；本次 ");
    Replace(s, "；装进药槽后战斗中使用", "");
    Replace(s, "\n\n\n", "\n\n");
    return s;
}
} // namespace tower::rewardui
