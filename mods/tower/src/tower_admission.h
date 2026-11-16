// Boss 准入（正式池）：只放实机验证过能正常战斗的内容（有原生血条、能锁定、正常击败）。
// 静态分类（A / scene=false）不是白名单；生成成功、有血量也不算。2026-10-31 实机依据：
//   - 铁面杀手 IronMaskMan_Boss：ABI5 四组对照里多次正常击败；
//   - "玉琮"：混沌 YuCong_HunDun：3c501e6 第 1 层呈现正常、击败进入奖励；
//   - 魂（镜像）Hun：bea24d6 第 1 层无血条、不能锁定 → 内置禁用。
// 本机文件可增补（Codex 实测后）：%LOCALAPPDATA%\PBZTower\boss-verified.txt（每行一个类路径，# 注释）、
// boss-denied.txt（每行 类路径<Tab>原因；玩家在塔内报"Boss 有问题"也写这里）。禁用优先于验证。
#pragma once
#include <string>
#include <string_view>

namespace tower {
// 类名里是否含这个 Boss 身份（按 "_" 分隔的完整片段比较："Hun" 不匹配 BP_Monster_YuCong_HunDun_C，
// "ChiLian" 匹配 BP_Monster_ChiLian_P2_C）。身份本身可以带 "_"（AHua_GuiTouDao）
inline bool IdentityIn(std::string_view cls, std::string_view id) {
    if (id.empty()) return false;
    for (size_t at = cls.find(id); at != std::string_view::npos; at = cls.find(id, at + 1)) {
        const bool left = at == 0 || cls[at - 1] == '_';
        const size_t end = at + id.size();
        const bool right = end == cls.size() || cls[end] == '_';
        if (left && right) return true;
    }
    return false;
}
}  // namespace tower
namespace tower::admission {

inline constexpr std::string_view kVerified[] = {
    "/Game/RenewalContent/Actors/Mobs/Normal/BP_Monster_IronMaskMan_Boss.BP_Monster_IronMaskMan_Boss_C",
    "/PBZCombatAsset/ActorData/Mobs/YuCong/BP_Monster_YuCong_HunDun.BP_Monster_YuCong_HunDun_C",
    "/PBZCombatAsset/ActorData/Mobs/042_AHua/BP_Monster_AHua_GuiTouDao.BP_Monster_AHua_GuiTouDao_C",  // 开发机 boss-verified.txt（2026-10-31 实测）内置进发布版
};
inline constexpr std::string_view kDenied[] = {
    "/PBZCombatAsset/ActorData/Mobs/000_Hun/BP_Monster_Hun.BP_Monster_Hun_C",
};
// 固定测试入口（开发验收用，不代表 50 层内容已验收）
inline constexpr std::string_view kAcceptanceBoss = kVerified[0];

inline bool InList(std::string_view path, const std::string_view* list, size_t n) {
    for (size_t i = 0; i < n; ++i) if (list[i] == path) return true;
    return false;
}
// extraVerified / extraDenied：本机文件内容（一行一项，制表符后是原因）
inline bool InText(std::string_view path, std::string_view text) {
    size_t a = 0;
    while (a < text.size()) {
        size_t b = text.find('\n', a);
        if (b == std::string_view::npos) b = text.size();
        auto line = text.substr(a, b - a);
        if (const size_t t = line.find('\t'); t != std::string_view::npos) line = line.substr(0, t);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (!line.empty() && line[0] != '#' && line == path) return true;
        a = b + 1;
    }
    return false;
}
// audit：开发审计模式（全静态池，只排除禁用）；正式模式只放已验证
inline bool Admitted(std::string_view path, bool audit, std::string_view extraVerified, std::string_view extraDenied) {
    if (InList(path, kDenied, std::size(kDenied)) || InText(path, extraDenied)) return false;
    return audit || InList(path, kVerified, std::size(kVerified)) || InText(path, extraVerified);
}

}  // namespace tower::admission
