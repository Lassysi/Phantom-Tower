// 影塔成长相关的菜单文字（纯函数，界面 tower_ui.cpp 直接用；离线测试检查内容和按钮数）。
// 原生对话框最多 4 个按钮（引擎分配的按钮数组余量 4，实机）。
#pragma once
#include <format>
#include <string>
#include <vector>

#include "tower_growth.h"

namespace tower::growth::text {

inline constexpr int kMaxButtons = 4;

inline std::string Join(const std::vector<std::string>& v, const char* sep = "、") {
    std::string o;
    for (auto& x : v) o += (o.empty() ? "" : sep) + x;
    return o.empty() ? std::string("无") : o;
}
inline std::string WeaponLine(const OwnedWeapon& w) {
    const auto& d = kWeapons[w.index];
    std::string s = std::format("{} {} 级", d.name, w.level);
    if (const int next = NextPerk(w.index, w.level)) s += std::format("（{} 级出下一个特效）", next);
    else if (w.level >= kWeaponCap) s += "（已满）";
    if (w.snapped) s += "·已铸魂";
    return s;
}
// 每层开打前的正文：点数 + 兵器 + 药 + 饰品 + 祝福
inline std::string Prep(const Profile& p, const Blessings& b, int floor, int floors) {
    const FloorPlan next = PlanFor(floor + 1, floors);
    std::vector<std::string> mains, shaqi, meds, accs, bless;
    for (auto& w : p.weapons) (kWeapons[w.index].kind == 0 ? mains : shaqi).push_back(WeaponLine(w));
    for (int m : p.medicines) meds.push_back(std::format("{}×{}", kMedicines[m].name, p.MedUses(m)));
    for (auto& a : p.accessories) accs.push_back(a.refine > 1 ? std::format("{}·精炼{}", kAccessories[a.index].name, a.refine) : std::string(kAccessories[a.index].name));
    for (int i = 0; i < kBlessCount; ++i) if (b.stacks[i]) bless.push_back(std::format("{}×{}", kBlessings[i].name, b.stacks[i]));
    std::string s = std::format("第 {} 层 · {}\n淬炼点 {}　修为点 {}", floor + 1, FloorName(next.kind), p.temper, p.cultivate);
    if (p.soulTokens) s += std::format("　铸魂令 {}", p.soulTokens);
    s += "\n主武器：" + Join(mains) + "\n影之武装：" + Join(shaqi);
    s += std::format("\n药瓶 {} 级：", p.medLevel) + Join(meds);
    s += "\n饰品：" + Join(accs);
    if (b.Any()) s += "\n祝福：" + Join(bless);
    if (p.temper || p.cultivate) s += "\n\n还有点数没用：可以先「升级…」再开打。";
    return s;
}
// 升级菜单正文
inline std::string Upgrade(const Profile& p) {
    std::string s = std::format("淬炼点 {}（1 点 = 兵器 +1 级）\n修为点 {}（能力修行）", p.temper, p.cultivate);
    for (auto& w : p.weapons) s += "\n" + WeaponLine(w);
    int lit = 0;
    for (int i = 0; i < kNodeCount; ++i) lit += p.nodes[i] > 0;
    s += std::format("\n能力：已点亮 {}/{} 个节点", lit, kNodeCount);
    s += "\n\n兵器升级、能力修行用游戏原生界面；回退兵器 / 洗点在这里。";
    return s;
}
inline std::vector<std::string> UpgradeButtons(const Profile& p) {
    std::vector<std::string> b = {"兵器升级", "能力修行"};
    bool any = false;
    for (auto& w : p.weapons) any |= w.level > 1;
    for (int n : p.nodes) any |= n > 0;
    if (any) b.push_back("回退 · 洗点…");
    b.push_back("返回");
    return b;
}
// 本层收获
inline std::string Gains(const FloorPlan& plan, int bossWeapons) {
    std::vector<std::string> g;
    if (plan.temper) g.push_back(std::format("淬炼点 +{}", plan.temper));
    if (plan.cultivate) g.push_back(std::format("修为点 +{}", plan.cultivate));
    if (bossWeapons) g.push_back(std::format("兵器 {} 把", bossWeapons));
    if (plan.soulToken) g.push_back("铸魂令 ×1");
    if (plan.medLevel) g.push_back(std::format("药瓶 → {} 级", plan.medLevel));
    if (plan.medTier) g.push_back("新药可选");
    if (plan.accessoryBox) g.push_back("饰品匣");
    if (plan.refineBox) g.push_back("饰品精炼匣");
    return "本层收获：" + Join(g);
}
inline std::string PickLabel(const PickOffer& o, const char* const* itemNames, int itemCount) {
    switch (o.kind) {
        case Pick::TemperPack: return std::format("淬炼包（淬炼点 +{}）", o.amount);
        case Pick::CultivatePack: return std::format("修为包（修为点 +{}）", o.amount);
        case Pick::Accessory: return o.index >= 0 ? std::format("饰品：{}", kAccessories[o.index].name) : "";
        case Pick::MedicineCharge: return "药引（本层药瓶 +1 次）";
        case Pick::Weapon: return o.index >= 0 ? std::format("兵器：{}", kWeapons[o.index].name) : "";
        case Pick::Item: return o.index >= 0 && o.index < itemCount ? std::format("道具：{}", itemNames[o.index]) : "";
        case Pick::Jade: return std::format("魂玉 +{}", o.amount);
        case Pick::Blessing: {
            static constexpr const char* rank[] = {"", "凡", "灵", "玄"};
            return o.index >= 0 ? std::format("祝福·{}（{}，{} 层）", kBlessings[o.index].name, rank[std::clamp(o.amount, 1, 3)], o.amount) : "";
        }
    }
    return "";
}

}  // namespace tower::growth::text
