// 3.1.0 驿站【交易】（用户 2026-11-13 定稿）：回收 / 以旧换新 / 熔铸，纯逻辑（引擎负责原生删 / 发兵器，这里只管规则与成长档）。
//   回收：交出 1 把，返还已升等级的淬炼点再 +1，魂玉 = 当前层武器价 80%，铸过魂的返还 1 个铸魂令
//   以旧换新：交出 2 把，换货架上 1 把兵器（不花魂玉），交出的等级照样返还淬炼点
//   熔铸：牺牲 1 把强化另 1 把——被强化的攻击 / 破防各 +15%（最多 10 次）；牺牲品从本局武器池永久移除，等级返还淬炼点；
//         本局累计熔掉 15 把送 1 个铸魂令
// 三种交易每次驿站各限 1 次。不能交出：被熔铸强化过的（防误操作丢加成）、交出后该类兵器一把不剩的
#pragma once
#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include "tower_growth.h"

namespace tower::trade {

enum Op { Recycle = 0, Exchange = 1, Melt = 2, OpCount };
inline constexpr std::string_view kOpName[OpCount] = {"回收", "以旧换新", "熔铸"};
inline constexpr double kMeltStep = 0.15;
inline constexpr int kMeltMax = 10, kMeltSoulEvery = 15, kRecycleJadePct = 80;  // 3.1.0 N7（用户）：回收 10% → 80%

// 每次驿站各限 1 次：按开店的层记
struct Visit {
    int floor = -1;
    unsigned used = 0;
    bool Used(int shopFloor, Op op) const { return floor == shopFloor && (used & (1u << op)); }
    void Mark(int shopFloor, Op op) { if (floor != shopFloor) { floor = shopFloor; used = 0; } used |= 1u << op; }
};

inline double MeltMul(int count) { return 1.0 + kMeltStep * std::clamp(count, 0, kMeltMax); }
inline int KindCount(const growth::Profile& p, int kind) {
    int n = 0;
    for (auto& w : p.weapons) n += growth::kWeapons[w.index].kind == kind;
    return n;
}
// 交出这把（回收 / 以旧换新 / 熔铸的牺牲品）为什么不行；空 = 可以。equipped：当前装备的兵器（含武库扩建第 3、4 把）
inline std::string GiveBlocked(const growth::Profile& p, int w, const std::vector<int>& equipped) {
    if (w < 0 || w >= growth::kWeaponCount || p.Find(w) < 0) return "没有这把兵器";
    (void)equipped;  // 3.1.0 N7（用户）：正在装备的也能交出，交出后从背包消失
    // 3.1.0 N9g（用户 2026-11-13）：任何兵器都能回收 / 熔铸（原生、熔铸过的都行），只保底每类留一把
    if (KindCount(p, growth::kWeapons[w].kind) <= 1) return growth::kWeapons[w].kind == 1 ? "至少留一把影之武装" : "至少留一把主武器";
    return {};
}
// 熔铸的强化对象为什么不行
inline std::string MeltTargetBlocked(const growth::Profile& p, int w) {
    if (w < 0 || w >= growth::kWeaponCount || p.Find(w) < 0) return "没有这把兵器";
    if (p.MeltCount(w) >= kMeltMax) return "熔铸已满";
    return {};
}
// 交出一把能拿回的淬炼点（已升的等级）
// 3.4.0：按这把实际花掉的淬炼点退（游戏背包认领、带等级获得的不算花过）；旧档没记的按等级倒推（淬炼大师按一半）
inline int Refund(const growth::Profile& p, int w) {
    const int s = p.Find(w);
    if (s < 0) return 0;
    const auto& o = p.weapons[s];
    return o.spent >= 0 ? o.spent : p.LevelRefund(std::max(0, o.level - 1));
}

struct Gain { int temper = 0, soul = 0, jade = 0; };
// 3.4.0（用户定稿）：回收的淬炼点 = 交出兵器的等级合计 ÷ 2（向下取整），至少 1——60 级 = 30、30 级 = 15、1 级 = 1；多把一起先加再除。
// 防刷：等级按「这把实际花的点能升到的等级」封顶（1 + 2 × 花费；游戏背包认领、带等级获得没花点的只算 1 级）；旧档没记花费的按等级
inline int RecycleLevel(const growth::Profile& p, int w) {
    const int s = p.Find(w);
    if (s < 0) return 0;
    const auto& o = p.weapons[s];
    return std::max(1, o.spent >= 0 ? std::min(o.level, 1 + 2 * o.spent) : o.level);
}
inline int WeaponsTemper(const growth::Profile& p, const std::vector<int>& ws) {
    // 3.4.0（用户）：1 级的每把单独给 1 点（不参与加和，否则多选时 1 级的等于白交）；2 级及以上先加和再 ÷ 2
    int sum = 0, ones = 0, n = 0;
    for (int w : ws) if (p.Find(w) >= 0) { const int lv = RecycleLevel(p, w); if (lv <= 1) ++ones; else sum += lv; ++n; }
    return n ? ones + (sum > 0 ? std::max(1, sum / 2) : 0) : 0;
}
// 单把的魂玉 / 铸魂令（淬炼点整单另算，见 WeaponsTemper）
inline Gain RecycleExtras(const growth::Profile& p, int w, int weaponPrice) {
    const int s = p.Find(w);
    Gain g;
    if (s < 0) return g;
    g.soul = p.weapons[s].snapped ? 1 : 0;
    g.jade = std::max(1, weaponPrice * kRecycleJadePct / 100);
    return g;
}
inline Gain RecycleGain(const growth::Profile& p, int w, int weaponPrice) { Gain g = RecycleExtras(p, w, weaponPrice); g.temper = WeaponsTemper(p, {w}); return g; }
inline void Remove(growth::Profile& p, int w) {
    std::erase_if(p.weapons, [w](const growth::OwnedWeapon& o) { return o.index == w; });
    std::erase_if(p.melt, [w](const auto& m) { return m.first == w; });  // N9g：熔铸过的交出去，加成跟着没了
}
// 以下三个在原生已删 / 已发成功后调用，只改成长档；返回实际所得
inline Gain ApplyRecycle(growth::Profile& p, int w, int weaponPrice) {
    const Gain g = RecycleGain(p, w, weaponPrice);
    p.temper += g.temper;   // 返还的不算新获得（3.4.0 起不再额外 +1）
    p.soulTokens += g.soul;
    Remove(p, w);
    return g;
}
inline Gain ApplyExchange(growth::Profile& p, int a, int b, int target) {
    Gain g; g.temper = Refund(p, a) + Refund(p, b);
    for (int w : {a, b}) { const int s = p.Find(w); if (s >= 0 && p.weapons[s].snapped) ++g.soul; }
    p.temper += g.temper; p.soulTokens += g.soul;
    Remove(p, a); Remove(p, b);
    p.AddWeapon(target);
    return g;
}
inline Gain ApplyMelt(growth::Profile& p, int target, int sacrifice) {
    Gain g; g.temper = Refund(p, sacrifice);
    if (const int s = p.Find(sacrifice); s >= 0 && p.weapons[s].snapped) ++g.soul;
    Remove(p, sacrifice);
    if (!p.Banned(sacrifice)) p.banned.push_back(sacrifice);
    bool found = false;
    for (auto& [w, n] : p.melt) if (w == target) { n = std::min(kMeltMax, n + 1); found = true; }
    if (!found) p.melt.push_back({target, 1});
    ++p.meltTotal;
    if (p.meltTotal % kMeltSoulEvery == 0) ++g.soul;
    p.temper += g.temper; p.soulTokens += g.soul;
    return g;
}

// 3.1.0 N8（用户）：回收、熔铸可多选——左边多选，一次结算（仍算本次驿站的 1 次回收 / 1 次熔铸）。
// 按顺序在副本上逐把检查（交到最后一把的那一类会被挡下），全部能交才成交
inline std::string GiveManyBlocked(const growth::Profile& p, const std::vector<int>& ws, const std::vector<int>& equipped) {
    if (ws.empty()) return "没有选兵器";
    auto q = p;
    for (size_t i = 0; i < ws.size(); ++i) {
        for (size_t j = 0; j < i; ++j) if (ws[j] == ws[i]) return "选了重复的兵器";
        if (auto why = GiveBlocked(q, ws[i], equipped); !why.empty()) return why;
        Remove(q, ws[i]);
    }
    return {};
}
inline Gain RecycleManyGain(const growth::Profile& p, const std::vector<int>& ws, int weaponPrice) {
    Gain g; g.temper = WeaponsTemper(p, ws);
    for (int w : ws) { const auto x = RecycleExtras(p, w, weaponPrice); g.soul += x.soul; g.jade += x.jade; }
    return g;
}
inline Gain ApplyRecycleMany(growth::Profile& p, const std::vector<int>& ws, int weaponPrice) {
    const Gain g = RecycleManyGain(p, ws, weaponPrice);
    p.temper += g.temper; p.soulTokens += g.soul;
    for (int w : ws) Remove(p, w);
    return g;
}
// ---- N9i（用户 2026-11-13）：回收也收药和饰品，各按本层驿站同类价的 80% 给魂玉；饰品不论几级都再给 1 淬炼点，药只给魂玉。
// 回收清单用编码：兵器 = 兵器下标；药 = kMedCode + 药下标；饰品 = kAccCode + 饰品下标
inline constexpr int kMedCode = 1000, kAccCode = 2000;
enum class Cat { Main, Shaqi, Med, Acc };
inline Cat CatOf(int code) { return code >= kAccCode ? Cat::Acc : code >= kMedCode ? Cat::Med : growth::kWeapons[std::clamp(code, 0, growth::kWeaponCount - 1)].kind == 1 ? Cat::Shaqi : Cat::Main; }
inline bool IsWeapon(int code) { return code >= 0 && code < kMedCode; }
struct Prices { int weapon = 0, med = 0, acc = 0; };
inline int AccSlot(const growth::Profile& p, int a) { for (size_t i = 0; i < p.accessories.size(); ++i) if (p.accessories[i].index == a) return static_cast<int>(i); return -1; }
inline bool Owns(const growth::Profile& p, int code) {
    if (code < 0) return false;
    if (IsWeapon(code)) return code < growth::kWeaponCount && p.Find(code) >= 0;
    if (code >= kAccCode) return code - kAccCode < growth::kAccessoryCount && AccSlot(p, code - kAccCode) >= 0;
    return code - kMedCode < growth::kMedicineCount && std::find(p.medicines.begin(), p.medicines.end(), code - kMedCode) != p.medicines.end();
}
inline std::string_view CodeName(int code) {
    if (code < 0) return "?";
    if (IsWeapon(code)) return code < growth::kWeaponCount ? growth::kWeapons[code].name : std::string_view("?");
    if (code >= kAccCode) return code - kAccCode < growth::kAccessoryCount ? growth::kAccessories[code - kAccCode].name : std::string_view("?");
    return code - kMedCode < growth::kMedicineCount ? growth::kMedicines[code - kMedCode].name : std::string_view("?");
}
inline Gain ItemGain(const growth::Profile& p, int code, const Prices& pr) {
    Gain g;
    if (code >= kAccCode) {
        g.jade = std::max(1, pr.acc * kRecycleJadePct / 100);
        g.temper = AccSlot(p, code - kAccCode) >= 0 ? 1 : 0;
    } else g.jade = std::max(1, pr.med * kRecycleJadePct / 100);
    return g;
}
inline void RemoveCode(growth::Profile& p, int code) {
    if (IsWeapon(code)) { Remove(p, code); return; }
    if (code >= kAccCode) { const int a = code - kAccCode; std::erase_if(p.accessories, [a](const auto& o) { return o.index == a; }); return; }
    std::erase(p.medicines, code - kMedCode);
}
inline std::string RecycleMixBlocked(const growth::Profile& p, const std::vector<int>& codes, const std::vector<int>& equipped) {
    if (codes.empty()) return "没有选东西";
    std::vector<int> ws;
    for (size_t i = 0; i < codes.size(); ++i) {
        for (size_t j = 0; j < i; ++j) if (codes[j] == codes[i]) return "选了重复的东西";
        if (!Owns(p, codes[i])) return std::string(CodeName(codes[i])) + "：已经没有了";
        if (IsWeapon(codes[i])) ws.push_back(codes[i]);
    }
    return ws.empty() ? std::string() : GiveManyBlocked(p, ws, equipped);
}
inline Gain RecycleMixGain(const growth::Profile& p, const std::vector<int>& codes, const Prices& pr) {
    Gain g; std::vector<int> ws;
    for (int c : codes) {
        if (IsWeapon(c)) { ws.push_back(c); const auto x = RecycleExtras(p, c, pr.weapon); g.soul += x.soul; g.jade += x.jade; }
        else { const auto x = ItemGain(p, c, pr); g.temper += x.temper; g.jade += x.jade; }
    }
    g.temper += WeaponsTemper(p, ws);
    return g;
}
inline Gain ApplyRecycleMix(growth::Profile& p, const std::vector<int>& codes, const Prices& pr) {
    const Gain g = RecycleMixGain(p, codes, pr);
    std::vector<int> ws; int itemTemper = 0;
    for (int c : codes) { if (IsWeapon(c)) ws.push_back(c); else itemTemper += ItemGain(p, c, pr).temper; }
    p.temper += g.temper - itemTemper; if (itemTemper > 0) p.Earn(itemTemper, 0);
    p.soulTokens += g.soul;
    for (int c : codes) RemoveCode(p, c);
    return g;
}
// 熔铸最多还能放几把牺牲品（被强化的满 10 次为止）
inline int MeltRoom(const growth::Profile& p, int target) { return target < 0 ? kMeltMax : std::max(0, kMeltMax - p.MeltCount(target)); }
inline std::string MeltManyBlocked(const growth::Profile& p, int target, const std::vector<int>& sacrifices, const std::vector<int>& equipped) {
    if (auto why = MeltTargetBlocked(p, target); !why.empty()) return "不能强化：" + why;
    if (std::find(sacrifices.begin(), sacrifices.end(), target) != sacrifices.end()) return "不能熔铸自己";
    if (static_cast<int>(sacrifices.size()) > MeltRoom(p, target)) return std::format("这把最多再熔 {} 把", MeltRoom(p, target));
    return GiveManyBlocked(p, sacrifices, equipped);
}
inline Gain MeltManyGain(const growth::Profile& p, int target, const std::vector<int>& sacrifices) {
    auto q = p; Gain g;
    for (int s : sacrifices) { const auto x = ApplyMelt(q, target, s); g.temper += x.temper; g.soul += x.soul; }
    return g;
}
inline Gain ApplyMeltMany(growth::Profile& p, int target, const std::vector<int>& sacrifices) {
    Gain g;
    for (int s : sacrifices) { const auto x = ApplyMelt(p, target, s); g.temper += x.temper; g.soul += x.soul; }
    return g;
}

}  // namespace tower::trade
