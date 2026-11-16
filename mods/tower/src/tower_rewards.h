// 影塔奖励 / 商店 / 准备道具 / 快捷栏（纯逻辑，可离线测试；用户 2026-11-01 要求，notes/tower-rewards-shop-quickslots-20261101.md）。
// 本文件只是新模型的规则层：还没有接进引擎结算、界面和局存档（下一步），价格 / 概率 / 权重都是待用户审稿的初稿。
//
// 核心约束（逐条来自用户要求）：
//   - 装备按"塔区进度门槛"进池：奖励、刷新、商店共用一张表（Catalog）；第一区（1–10 层，含第 10 层与刷新、商店）只出凡品；
//     门槛未核实的物品（zone = -1，"待核"）一律不进池，不按名字猜前后期。
//   - 三格：成长 / 装备 / 灵活；一组不重复；刷新优先避开刚显示的三项、但绝不为凑数越级。
//   - 已持有 / 封顶 / 无实际收益的选项过滤；祝福按实际能加的层数标品质（截断后的凡品不显示成玄品）。
//   - 商店用魂玉；买入是一笔交易：发放成功并读回才扣钱，旧界面重复请求（版本号不对）不成交；货架状态随塔档保存。
//   - 准备道具：整备里预装（每层最多 3 种），下层"成功开战"才消耗，取消 / 生成失败保留；没有战斗中即时使用的道具（用户 2026-11-01）。
#pragma once
#include "tower_balance.h"
#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <format>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "progression_catalog.generated.h"
#include "tower_economy.h"
#include "tower_growth.h"

namespace tower::rewards {

using growth::Xs;

// ---------------------------------------------------------------- 装备进度表（奖励与商店共用）
enum class Cat { Main, Shaqi, Accessory, Medicine };
// Native descriptions for these image-only prototypes are absent or placeholders.
// Keep IDs and inventory indices intact for old saves, but never offer them anew.
inline bool RetiredWeapon(int i) {
    if (i < 0 || i >= growth::kWeaponCount) return false;
    const auto id = growth::kWeapons[i].id;
    return id == "ZhuGuangBaoQi" || id == "MoQie" || id == "XueSeLingDang" ||
           id == "BaWangLianQiu" || id == "MoShou";
}
struct Gate {
    Cat cat;
    int index;          // kWeapons / kAccessories / kMedicines 下标
    int zone;           // 首次进池的塔区 0–4；-1 = 待核（不进池）
    bool bossLinked;    // Boss 关联兵器：主要靠 Boss 掉落，默认不上货架
    bool reward, shop;  // 能否进三选一 / 商店
};
inline constexpr int kZones = 5;
inline int ZoneOf(int floor, int floors) { return floors < 10 ? 0 : std::clamp((floor - 1) / 10, 0, kZones - 1); }

struct Catalog {
    std::vector<Gate> gates;
    const Gate* Find(Cat c, int index) const {
        for (auto& g : gates) if (g.cat == c && g.index == index) return &g;
        return nullptr;
    }
    void Set(const Gate& g) {
        for (auto& x : gates) if (x.cat == g.cat && x.index == g.index) { x = g; return; }
        gates.push_back(g);
    }
    // 默认表：只放有依据的——原生药按现有一 / 二 / 三档与稀有药规则；兵器 / 饰品全部"待核"，
    // 等 Codex 的装备来源对照表（mods/tower/data/，按 ParseCatalog 的格式覆盖）。起手套装兵器已持有，不影响抽取
    static Catalog Default() {
        Catalog c;
        auto med = [&](std::string_view id, int zone) { if (const int m = growth::MedicineIndex(id); m >= 0) c.Set({Cat::Medicine, m, zone, false, true, true}); };
        for (auto id : growth::kMedTier1) med(id, 0);
        for (auto id : growth::kMedTier2) med(id, 1);
        for (auto id : growth::kMedTier3) med(id, 2);
        med(growth::kMedRare, 3);  // 第 35 层的稀有药规则
        for (int i = 0; i < growth::kWeaponCount; ++i) c.Set({growth::kWeapons[i].kind == 1 ? Cat::Shaqi : Cat::Main, i, -1, false, true, true});
        for (int i = 0; i < growth::kAccessoryCount; ++i) if (growth::AccessoryInPool(i)) c.Set({Cat::Accessory, i, -1, false, true, true});
        return c;
    }
};
// 覆盖格式（一行一件，# 注释）：<main|shaqi|acc|med> <原生 ID> <区 0–4 或 -1> <boss 0/1> <reward 0/1> <shop 0/1>
// 未知 ID / 坏行跳过并计数（bad），不让一行错误弄坏整张表
inline Catalog ParseCatalog(const std::string& text, Catalog base, int* bad = nullptr);
// 引擎实际用的表：Codex 装备进度表合同（构建时由 tools/gen_progression_catalog.py 生成、随 DLL 发布）覆盖默认表。
// 合同里待核 / 排除的项一律区 -1（不进奖励、不进商店），已批准的只按 first_tower_zone 开放
inline Catalog Contract(int* bad = nullptr) { return ParseCatalog(kProgressionCatalog, Catalog::Default(), bad); }
inline Catalog ParseCatalog(const std::string& text, Catalog base, int* bad) {
    std::istringstream in(text);
    std::string line;
    int errors = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#' || line[0] == '\r') continue;
        std::istringstream ls(line);
        std::string cat, id;
        int zone = -2, boss = 0, reward = 1, shop = 1;
        if (!(ls >> cat >> id >> zone >> boss >> reward >> shop) || zone < -1 || zone >= kZones) { ++errors; continue; }
        Cat c; int index = -1;
        if (cat == "main") { c = Cat::Main; index = growth::WeaponIndex(id, 0); }
        else if (cat == "shaqi") { c = Cat::Shaqi; index = growth::WeaponIndex(id, 1); }
        else if (cat == "acc") { c = Cat::Accessory; index = growth::AccessoryIndex(id); if (index >= 0 && !growth::AccessoryInPool(index)) index = -1; }  // 付费 / 剔除饰品永不进池
        else if (cat == "med") { c = Cat::Medicine; index = growth::MedicineIndex(id); }
        else { ++errors; continue; }
        if (index < 0) { ++errors; continue; }
        base.Set({c, index, zone, boss != 0, reward != 0, shop != 0});
    }
    if (bad) *bad = errors;
    return base;
}

// ---------------------------------------------------------------- 品质
// 1 凡 / 2 灵 / 3 玄。第一区（1–10 层）只有凡；之后沿用祝福稀有度的区间表（初稿，后四区概率另做平衡）
inline int Quality(Xs& r, int floor, int floors) {
    const int zone = ZoneOf(floor, floors);
    if (zone == 0) return 1;
    return growth::BlessRarity(r, zone);
}

// ---------------------------------------------------------------- 奖励项
enum class Kind { Blessing, TemperPack, CultivatePack, Weapon, Accessory, Refine, Medicine, Item, Jade, Attr, SoulToken };  // SoulToken：铸魂令（v15，只从奖励来）  // Attr：v14 属性奖励（index = AttrKind，amount = 千分比）
struct Offer {
    Kind kind = Kind::Jade;
    int index = -1;    // 祝福 / 兵器 / 饰品 / 药 / 道具下标
    int amount = 0;    // 祝福层数（= 显示品质）/ 点数 / 魂玉 / 道具个数
    int quality = 1;
    bool operator==(const Offer& o) const { return kind == o.kind && index == o.index; }  // 同一物品 / 同一效果
};
inline bool RetiredOffer(const Offer& o) { return o.kind == Kind::Weapon && RetiredWeapon(o.index); }
// 随机局只按已击败 Boss 的掉落记录解锁，不按原固定层表推测。
inline std::vector<Offer> BossDropOffers(const std::vector<int>& earned) {
    std::vector<Offer> out;
    for (int w : earned) {
        if (w < 0 || w >= tower::kWeaponDropCount) continue;
        const auto& d = tower::kWeaponDrops[w];
        if (!d.verified || tower::NotBossWeapon(d.weapon)) continue;
        const int wi = growth::WeaponIndex(d.weapon, d.shaqi ? 1 : 0);
        if (wi < 0 || RetiredWeapon(wi)) continue;
        const Offer offer{Kind::Weapon, wi, 1, 9};
        if (std::find(out.begin(), out.end(), offer) == out.end()) out.push_back(offer);
    }
    return out;
}
inline int JadeBase(int floor) { return static_cast<int>(std::lround(balance::kRewardJade * balance::J(balance::ShownOfBoss(floor)))); }  // v3：奖励页魂玉 1.5 J(s)
inline constexpr int kItemStackCap = 9;  // 单种消耗品库存上限（初稿）

// ---------------------------------------------------------------- v14 属性奖励（用户 2026-11-05）
// 名称用游戏术语：攻击 / 破防能力 / 生命 / 杀气。按区给档位（千分比），整轮累计封顶；到顶的属性不再进候选。
// 各档为简单 / 困难的实际百分比，累计作用于原生属性；生命不封顶。
enum class AttrKind { Attack, Break, Life, Shachi, Defense, LifeSteal,
                      ResistFire, ResistFrost, ResistLightning, ResistToxin, ResistBleed, ResistChaos };  // 只能末尾追加：下标同 kAttrs、存档 gr.attr 的顺序
inline constexpr int kNoCap = 1000000;  // 不封顶（生命）
// 3.0.0+（用户 2026-11-13）：除伤害减免（影塔合计 80%，艾卡西亚 90%）外数值不封顶；攻击 / 破防 / 杀气 / 吸血不再有上限
// （杀气原来 +150% 封顶 = 原生 400 × 2.5 = 1000，攻击 +200%）。减伤属性本身给到 80%，再由合计下限截住；抗性仍 45%
struct AttrDef { AttrKind kind; std::string_view id, name, what; int cap; int step[5]; int hard[5]; int weight; };
inline constexpr AttrDef kAttrs[] = {
    // v3（用户 2026-11-12）：打完 100 层大约攻击力 +150–200%、破防 ≈ +100%、减伤 ≤ +30%，其余留给海克斯。
    // step = 简单；困难 = 简单 × 1.2（hard 列由下面的 Hard() 算，表里保留同值只为格式）
    {AttrKind::Attack, "atk", "攻击力", "攻击力", kNoCap, {40, 50, 60, 70, 80}, {48, 60, 72, 84, 96}, 35},
    {AttrKind::Break, "break", "武器破防", "所有武器的破防能力", kNoCap, {25, 30, 35, 40, 45}, {30, 36, 42, 48, 54}, 22},
    {AttrKind::Life, "life", "生命上限", "生命上限", kNoCap, {60, 75, 90, 105, 120}, {72, 90, 108, 126, 144}, 40},
    {AttrKind::Shachi, "shachi", "杀气上限", "杀气上限", kNoCap, {80, 90, 100, 110, 120}, {96, 108, 120, 132, 144}, 12},
    {AttrKind::Defense, "guard", "伤害减免", "减免受到的伤害", 800, {15, 15, 20, 20, 25}, {18, 18, 24, 24, 30}, 5},
    {AttrKind::LifeSteal, "leech", "生命吸取", "按敌方掉血比例回复生命", kNoCap, {10, 10, 10, 10, 10}, {12, 12, 12, 12, 12}, 1},
    {AttrKind::ResistFire, "res_fire", "火焰抗性", "减少火焰伤害与点燃积累", 450, {15, 15, 15, 15, 15}, {18, 18, 18, 18, 18}, 2},
    {AttrKind::ResistFrost, "res_frost", "冰霜抗性", "减少冰霜伤害与冰冻积累", 450, {15, 15, 15, 15, 15}, {18, 18, 18, 18, 18}, 2},
    {AttrKind::ResistLightning, "res_thunder", "雷电抗性", "减少雷电伤害与感电积累", 450, {15, 15, 15, 15, 15}, {18, 18, 18, 18, 18}, 2},
    {AttrKind::ResistToxin, "res_toxin", "毒素抗性", "减少毒素伤害与中毒积累", 450, {15, 15, 15, 15, 15}, {18, 18, 18, 18, 18}, 2},
    {AttrKind::ResistBleed, "res_bleed", "流血抗性", "减少血伤害与出血、血爆积累", 450, {15, 15, 15, 15, 15}, {18, 18, 18, 18, 18}, 2},
    {AttrKind::ResistChaos, "res_chaos", "混沌抗性", "减少混沌伤害", 450, {15, 15, 15, 15, 15}, {18, 18, 18, 18, 18}, 2},
};
// 抗性 → 原生属性名后缀：受伤倍率 TakenVulnerabilityMultiplier_<damage>，异常积累 TakenVulnerabilityMultiplier_<acc>Accumulation
struct ResistMap { AttrKind kind; const char* damage; const char* acc[2]; };
inline constexpr ResistMap kResists[] = {
    {AttrKind::ResistFire, "Fire", {"Ignite", nullptr}},
    {AttrKind::ResistFrost, "Ice", {"Frost", nullptr}},
    {AttrKind::ResistLightning, "Lightning", {"Electrified", nullptr}},
    {AttrKind::ResistToxin, "Toxin", {"Poison", nullptr}},
    {AttrKind::ResistBleed, "Blood", {"Bleed", "BloodBurst"}},
    {AttrKind::ResistChaos, "Chaos", {nullptr, nullptr}},
};
inline constexpr int kAttrCount = static_cast<int>(std::size(kAttrs));
// 3.0.0（用户 2026-11-12）：无尽层每 30 层一档（显示 100–129 第 1 档、130–159 第 2 档…），属性上限按档往上抬
// （攻击 +50% / 档、破防 +25%、杀气上限 +25%；减伤 / 吸血 / 抗性不抬），每次奖励的档位值也 +25% / 档；锻造同一套候选
inline int CapBonus(AttrKind k) { return k == AttrKind::Attack ? 500 : k == AttrKind::Break || k == AttrKind::Shachi ? 250 : 0; }
inline constexpr int kMaxEndlessTier = 60;
inline int AttrCapAt(int i, int tier) {
    if (i < 0 || i >= static_cast<int>(std::size(kAttrs))) return 0;
    return kAttrs[i].cap >= kNoCap ? kNoCap : kAttrs[i].cap + std::clamp(tier, 0, kMaxEndlessTier) * CapBonus(kAttrs[i].kind);
}
inline int EndlessTier(int floor) { return floor <= 50 ? 0 : (floor + 49 - 100) / 30 + 1; }  // 内部层号 → 档（51 = 显示 100）
struct AttrTotals {
    std::array<int, kAttrCount> pm{};  // 本轮累计（千分比）
    mutable int tier = 0;              // 当前无尽档（不存档；每次取 Ctx 时按层号设置）
    int Room(int i) const { return i >= 0 && i < kAttrCount ? std::max(0, AttrCapAt(i, tier) - pm[i]) : 0; }
    // 加到上限为止，返回实际加了多少
    int Add(int i, int amount) { const int add = std::clamp(amount, 0, Room(i)); if (add > 0) pm[i] += add; return add; }
    double Mul(AttrKind k) const { return 1.0 + pm[static_cast<int>(k)] / 1000.0; }
};

// 抽取时看的本局状态
struct Context {
    int modPurchaseLimit = 1; // Per-item purchases in the current shop visit.
    int weaponPricePct = 100; // 武库扩建：兵器价格百分比
    // 驿站每次上架数量（用户 2026-11-11）：武器 / 药 / 饰品三个池各 4 个；武库扩建武器 +2，钱生钱每类 +2
    int shopPerCategory = 4, shopExtraWeapons = 0, shopExtraAll = 0;
    bool shopBothPoints = false;  // 钱生钱：淬炼点、修为点同时上架
    int shopModItems = 2;         // MOD 道具每次上架几个（钱生钱 3 个）
    // v3（用户 2026-11-12）：本局各类已买次数，价格按 (1 + 涨幅)^次数 上涨；MOD 道具按每一种分别计
    const std::array<int, balance::kBuyCount>* buys = nullptr;
    const std::array<int, kItemCount>* itemBuys = nullptr;
    int floor = 1, floors = 50;
    int attributeFloor = 0; // Optional later-stage attribute values without unlocking later equipment.
    int difficulty = 0;  // 0 困难，1 简单；由局存档恢复
    const growth::Profile* profile = nullptr;
    const growth::Blessings* blessings = nullptr;
    const Catalog* catalog = nullptr;
    std::array<int, kItemCount> items{};  // 塔内消耗品库存
    std::vector<Offer> avoid;             // 刷新：刚显示的三项（尽量避开）
    std::set<std::pair<int, int>> seen;   // 本局展示过的装备（kind, index）：降低重复曝光
    const AttrTotals* attrs = nullptr;    // v14 属性累计（空 = 旧模型）
    std::vector<Offer> lastFloor;         // 上一层最终那一组（避免相邻两层整组相同）
    const std::vector<Offer>* returned = nullptr;  // N9i：回收过的东西，回到驿站随机池（含 Boss 兵器 / 未开放区的），没持有才进
    bool soulFloor = false;               // v15：本层奖励里固定有一张铸魂令（SoulFloor）
};

// 道具的使用时机（决定能否进三选一、末层是否有用、进哪个栏位）
enum class Timing { Retired, Prep, Instant, Passive, Settlement, Use };  // Use：放 MOD 背包，层间点了立即生效（3.1.0 魂玉袋 / 经验丹）
inline Timing ItemTiming(Item i) {
    switch (i) {
        // 准备道具：开层前预装，下一层正式开战时扣并生效（用户 2026-11-01：不再有战斗中即时使用的道具）
        case Item::Sunder: case Item::Freeze: case Item::Slow: case Item::Shachi: case Item::Guard: case Item::Edge: case Item::Break: case Item::Leech:
        case Item::Rage: case Item::Calm: case Item::Bounty: return Timing::Prep;
        case Item::Revive: case Item::Rebirth: case Item::Foresight: return Timing::Passive;
        case Item::JadeBag: case Item::ExpPill: return Timing::Use;
        case Item::Reroll: return Timing::Settlement;
        default: return Timing::Retired;               // 回春丹（用户剔除）、情报 / 借将令（玩法未接）
    }
}

namespace detail {
inline bool Owned(const growth::Profile& p, Cat c, int i) {
    if (c == Cat::Main || c == Cat::Shaqi) return p.Find(i) >= 0 || p.Banned(i);  // 3.1.0：熔掉的兵器当作已有，不再出现
    if (c == Cat::Medicine) return p.HasMedicine(i);
    return std::any_of(p.accessories.begin(), p.accessories.end(), [i](auto& a) { return a.index == i; });
}
inline int RefineLevel(const growth::Profile& p, int i) {
    for (auto& a : p.accessories) if (a.index == i) return a.refine;
    return -1;
}
inline Kind KindOf(Cat c) { return c == Cat::Medicine ? Kind::Medicine : c == Cat::Accessory ? Kind::Accessory : Kind::Weapon; }
// 一个装备候选（含饰品精炼的"下一档"）。weight：没见过的 3、见过的 1；前三区非 Boss 兵器 ×2（补非 Boss 兵器的来源）
struct Cand { Offer offer; int weight; };
// 3.4.0（用户：无尽层武器 / 药 / 饰品越来越少，但玩家根本没拿全）：进入无尽层（显示 100 层起）后，全部能正常发放的兵器都进驿站和奖励页的候选池——
// 包括 Boss 关联兵器（以前只靠打过对应 Boss 才补上架，随机 Boss 模式更少）。废案 / 未接入 / 已回收禁用的照旧不放
inline bool EndlessAll(const Context& c) { return c.floors >= 50 && c.floor > 50; }
inline bool GrantableWeapon(int index) {
    if (index < 0 || index >= growth::kWeaponCount || RetiredWeapon(index)) return false;
    const auto id = growth::kWeapons[index].id;
    return !tower::NotBossWeapon(id) && std::any_of(std::begin(tower::kWeaponDrops), std::end(tower::kWeaponDrops), [&](const auto& d) { return d.weapon == id && d.verified; });
}
inline std::vector<Cand> Equipment(const Context& c, bool forShop, Cat only, bool anyCat) {
    std::vector<Cand> out;
    const int zone = ZoneOf(c.floor, c.floors);
    for (auto& g : c.catalog->gates) {
        if (!anyCat && g.cat != only) continue;
        if ((g.cat == Cat::Main || g.cat == Cat::Shaqi) && RetiredWeapon(g.index)) continue;
        const bool allWeapon = (g.cat == Cat::Main || g.cat == Cat::Shaqi) && EndlessAll(c) && GrantableWeapon(g.index) && !c.profile->Banned(g.index);
        if (!allWeapon) {
        if (g.zone < 0 || g.zone > zone || !(forShop ? g.shop : g.reward)) continue;
        if (forShop && g.bossLinked) continue;  // 货架默认只放非 Boss 关联
        // Boss 必掉的兵器 / 影之武装只走固定掉落，不进三选一（用户 2026-11-05：从第 4 层开局，三选一出了彭猛的巨凶斧）
        if (g.bossLinked && (g.cat == Cat::Main || g.cat == Cat::Shaqi)) continue;
        }
        Offer o{KindOf(g.cat), g.index, 1, 1};
        // 3.1.0（用户：奖励页饰品带「精炼」后缀没必要，饰品有打磨升级渠道）：已持有的饰品不再以精炼形式出现
        if (Owned(*c.profile, g.cat, g.index)) continue;  // 已持有的兵器 / 药 / 饰品都不当"新物品"
        int w = c.seen.count({static_cast<int>(o.kind), o.index}) ? 1 : 3;
        if (o.kind == Kind::Weapon && !g.bossLinked && zone <= 2) w *= 2;
        out.push_back({o, w});
    }
    return out;
}
inline bool Taken(const std::vector<Offer>& v, const Offer& o) { return std::find(v.begin(), v.end(), o) != v.end(); }
// 加权抽一个；先避开 avoid，没有了再放宽（候选不足时仍按合法池）
inline bool Pick(Xs& r, const std::vector<Cand>& all, const std::vector<Offer>& group, const std::vector<Offer>& avoid, Offer* out) {
    for (int pass = 0; pass < 2; ++pass) {
        int total = 0;
        for (auto& c : all) if (!Taken(group, c.offer) && (pass || !Taken(avoid, c.offer))) total += c.weight;
        if (total <= 0) continue;
        int x = r.Below(total);
        for (auto& c : all) {
            if (Taken(group, c.offer) || (!pass && Taken(avoid, c.offer))) continue;
            if ((x -= c.weight) < 0) { *out = c.offer; return true; }
        }
    }
    return false;
}
// 有实际收益的祝福：按能加的层数标品质
inline std::vector<Cand> Blessings(Xs& r, const Context& c) {
    std::vector<Cand> out;
    for (int i = 0; i < growth::kBlessCount; ++i) {
        const int room = growth::kBlessings[i].ready ? c.blessings->Room(i) : 0;
        if (room <= 0) continue;
        const int q = std::min(Quality(r, c.floor, c.floors), room);
        out.push_back({{Kind::Blessing, i, q, q}, 1});
    }
    return out;
}
inline std::vector<Cand> Items(const Context& c) {
    std::vector<Cand> out;
    const bool last = c.floor >= c.floors;
    for (int i = 0; i < kItemCount; ++i) {
        if (!ItemInRewardPool(i)) continue;
        const auto t = ItemTiming(kItems[i].item);
        if (t == Timing::Retired) continue;
        if (last && t != Timing::Passive) continue;  // 末层：之后没有"下一层 / 下一次三选一"，这些用不上（复活符末层仍有用）
        if (c.items[i] >= kItemStackCap) continue;   // 库存满：领了也放不下
        out.push_back({{Kind::Item, i, 1, 1}, std::max(1, kItems[i].weight)});
    }
    return out;
}
}  // namespace detail

// 通关三选一：成长 / 装备 / 灵活。每格没有合法项时由其他合法类别补位，最后才是魂玉；所有选项都真实可领
inline std::array<Offer, 3> Draw(Xs& r, const Context& c) {
    using namespace detail;
    std::vector<Offer> group;
    const bool points = c.floor <= growth::kGrowthEndsAfter && c.floors >= 10;
    auto equip = Equipment(c, false, Cat::Main, true);
    auto bless = Blessings(r, c);
    auto items = Items(c);
    // 魂玉兜底：每份用不同的 index（-1、-2…），一组里不会出现两个"同一项"
    auto jade = [&](int mul) {
        const int n = static_cast<int>(std::count_if(group.begin(), group.end(), [](auto& g) { return g.kind == Kind::Jade; }));
        return Offer{Kind::Jade, -1 - n, std::max(1, JadeBase(c.floor) * mul / 2), 1};
    };
    Offer o;
    // 成长格：祝福或点数包（36 层起只剩祝福；都没了换装备）
    {
        std::vector<Cand> growthPool = bless;
        if (points) { growthPool.push_back({{Kind::TemperPack, -1, 2, 1}, 2}); growthPool.push_back({{Kind::CultivatePack, -1, 2, 1}, 2}); }
        if (Pick(r, growthPool, group, c.avoid, &o) || Pick(r, equip, group, c.avoid, &o)) group.push_back(o);
        else group.push_back(jade(3));
    }
    // 装备格
    if (Pick(r, equip, group, c.avoid, &o) || Pick(r, items, group, c.avoid, &o)) group.push_back(o);
    else group.push_back(jade(3));
    // 灵活格：装备 / 道具 / 魂玉，偶尔祝福
    {
        // 祝福在灵活格只是"偶尔"（15%），不扩成常见永久 Buff
        if (r.Below(100) < 15 && Pick(r, bless, group, c.avoid, &o)) { group.push_back(o); return {group[0], group[1], group[2]}; }
        std::vector<Cand> flex = equip;
        for (auto& x : items) flex.push_back(x);
        const Offer j = jade(3);
        if (j.index == -1) flex.push_back({j, 2});  // 组里还没有魂玉时，魂玉也是一个正常选项
        if (!Pick(r, flex, group, c.avoid, &o) && !Pick(r, bless, group, c.avoid, &o)) o = j;
        group.push_back(o);
    }
    return {group[0], group[1], group[2]};
}

// ---------------------------------------------------------------- v14 普通奖励抽取（用户 2026-11-05）
// 三个普通候选，每个按类型权重抽：属性约 50%、MOD 道具约 30%、原生道具约 20%（某类没有合法项时按剩余类型重新归一）；
// 同组不重复；尽量避开刷新前那一组（avoid）与上一层整组相同（lastFloor）。刷新令单独判定（约 30%），固定在第四位，不占普通位
// v15（用户 2026-11-05）：大部分是慢慢升角色属性，其次点数，偶尔武器 / 饰品 / 药，MOD 道具稀有；刷新令 5%
inline constexpr int kTypeCount = 5;
// 1.1.0 用户 2026-11-07：普通奖励以属性为主，原生装备再压一点（饰品和药主要从额外奖励和驿站来）
inline constexpr int kTypeWeight[kTypeCount] = {65, 10, 0, 25, 4};  /* 3.6.0（用户）：加「魂玉」项，权重 4（低于属性） */  /* v3（用户 2026-11-12）：属性 65 / MOD 10 / 原生装备 0（只走驿站和额外挑战）/ 点数 25 */ //  /* 1.4.0：点数 26 → 18，让给属性 */  // 属性 / MOD 道具 / 原生装备（武器、饰品、药）/ 点数（淬炼、修为）
inline constexpr int kRefreshChance = 5;
inline constexpr int kSoulFloors = 15;  // 铸魂令：整轮随机约 15 层的奖励里各出一张，不选就没了
// 哪些层的奖励里带铸魂令（同一种子固定；1–50 层里随机挑 kSoulFloors 层）
// 用户 2026-11-12：铸魂令溢出（给铸魂令相关海克斯让路）→ 每层奖励页约 7% 出现，前 50 层与无尽层同一规则
inline constexpr int kSoulRewardPct = 7, kSoulShopPct = 5;
inline bool SoulFloor(uint64_t seed, int floor, int floors) {
    if (floors < 10 || floor < 1) return false;
    uint64_t x = (seed ^ 0x50D1F00Dull) * 0x9E3779B97F4A7C15ull + static_cast<uint64_t>(floor) * 0xD6E8FEB86659FD93ull;
    x ^= x >> 29; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 32;
    return static_cast<int>(x % 100) < kSoulRewardPct;
}
inline bool SoulFloorLegacy(uint64_t seed, int floor, int floors) {
    if (floors < 10 || floor < 1 || floor > std::min(50, floors)) return false;
    const int n = std::min(50, floors);
    std::array<int, 50> order{};
    for (int i = 0; i < n; ++i) order[i] = i + 1;
    uint64_t x = ((seed ^ 0x50D1F00Dull) * 0x9E3779B97F4A7C15ull) | 1;
    for (int i = n - 1; i > 0; --i) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; std::swap(order[i], order[x % static_cast<uint64_t>(i + 1)]); }
    for (int i = 0; i < std::min(kSoulFloors, n); ++i) if (order[i] == floor) return true;
    return false;
}
namespace detail {
inline std::vector<Cand> Attrs(const Context& c) {
    std::vector<Cand> out;
    if (!c.attrs) return out;
    const int attributeFloor = c.attributeFloor > 0 ? c.attributeFloor : c.floor;
    const int zone = ZoneOf(attributeFloor, c.floors);
    for (int i = 0; i < kAttrCount; ++i) {
        // Full tower alternates Boss and waves: Boss 36 displays floor 71.
        if (kAttrs[i].kind == AttrKind::LifeSteal && (c.floors < 50 || attributeFloor < 31)) continue;  // v3：显示 61 层起
        const int room = c.attrs->Room(i);
        if (room <= 0) continue;
        const int z = std::clamp(zone, 0, kZones - 1);
        const int tier = EndlessTier(attributeFloor);
        const int step = (c.difficulty == 1 ? kAttrs[i].step[z] : kAttrs[i].hard[z]) * (100 + 25 * tier) / 100;  // 无尽档 +25% / 档
        if (kAttrs[i].kind == AttrKind::Life) {  // 少见档为常见档的 1.5 倍
            // 1.0.1（玩家反馈后面层几乎全是 +45%）：少见档只占"生命上限"这一项出现次数的 10%（权重 9 : 1，生命总权重不变）
            out.push_back({{Kind::Attr, i, std::min(step, room), 1}, kAttrs[i].weight * 9 / 10});
            out.push_back({{Kind::Attr, i, std::min(step * 3 / 2, room), 2}, std::max(1, kAttrs[i].weight / 10)});
            continue;
        }
        const int amount = std::min(step, room);
        out.push_back({{Kind::Attr, i, amount, 1}, kAttrs[i].weight});
    }
    return out;
}
inline std::vector<Cand> ModItems(const Context& c) {
    std::vector<Cand> out;
    for (int i = 0; i < kItemCount; ++i) {
        if (!ItemInRewardPool(i) || (ItemTiming(kItems[i].item) != Timing::Prep && ItemTiming(kItems[i].item) != Timing::Use)) continue;  // 战斗准备道具 + 点用道具（复活 / 刷新令不进普通位）
        if (c.items[i] >= kItemStackCap) continue;
        out.push_back({{Kind::Item, i, 1, 1}, std::max(1, kItems[i].weight)});
    }
    return out;
}
// 点数：淬炼点 +5（用户 2026-11-06：试过 +3 后改回 +5）、修为点 +1；每层通过另给 1 个修为点、不给淬炼点
inline std::vector<Cand> Points(const Context&) {
    return {{{Kind::TemperPack, -1, 2, 1}, 3}, {{Kind::CultivatePack, -2, 1, 1}, 2}};  // 1.4.0 用户 2026-11-08：淬炼 +5 → +2（玩家十几层就满 30 级）
}
inline bool SameSet(const std::vector<Offer>& a, const std::vector<Offer>& b) {
    if (a.size() != b.size() || a.empty()) return false;
    for (auto& x : a) if (!Taken(b, x)) return false;
    return true;
}
}  // namespace detail
struct Group { std::array<Offer, 3> normal{}; bool refresh = false; };
inline int ShopPrice(const Offer& o, const Context& c);
// 3.6.0（用户 2026-11-14）：奖励页魂玉项 = 驿站当前淬炼点、修为点售价中较低的那个；选中后进本层魂玉账单（吃全部倍率）
inline int RewardJade(const Context& c) {
    return std::max(1, std::min(ShopPrice(Offer{Kind::TemperPack, 0, 3, 1}, c), ShopPrice(Offer{Kind::CultivatePack, 0, 1, 1}, c)));
}
inline constexpr int kRewardJadeIndex = -9;
inline Group Draw14(Xs& r, const Context& c) {
    using namespace detail;
    const std::vector<Cand> pools[kTypeCount] = {Attrs(c), ModItems(c), Equipment(c, false, Cat::Main, true), Points(c),
                                                 {{{Kind::Jade, kRewardJadeIndex, RewardJade(c), 1}, 1}}};
    auto once = [&](Xs& rr) {
        std::vector<Offer> group;
        if (c.soulFloor) group.push_back(Offer{Kind::SoulToken, -1, 1, 1});  // 铸魂令层：固定占一位
        for (int slot = static_cast<int>(group.size()); slot < 3; ++slot) {
            Offer o;
            bool got = false;
            bool tried[kTypeCount] = {};
            for (int attempt = 0; attempt < kTypeCount && !got; ++attempt) {
                int total = 0;
                for (int t = 0; t < kTypeCount; ++t) if (!tried[t] && !pools[t].empty()) total += kTypeWeight[t];
                if (total <= 0) break;
                int x = rr.Below(total), type = -1;
                for (int t = 0; t < kTypeCount; ++t) if (!tried[t] && !pools[t].empty() && (x -= kTypeWeight[t]) < 0) { type = t; break; }
                if (type < 0) break;
                tried[type] = true;
                got = Pick(rr, pools[type], group, c.avoid, &o);
            }
            if (!got) {  // 全部类型都没有能放进这一组的了：魂玉兜底（index 互不相同）
                const int n = static_cast<int>(std::count_if(group.begin(), group.end(), [](auto& g) { return g.kind == Kind::Jade; }));
                o = Offer{Kind::Jade, -1 - n, std::max(1, JadeBase(c.floor)), 1};
            }
            group.push_back(o);
        }
        return group;
    };
    std::vector<Offer> group = once(r);
    for (int k = 0; k < 6 && (SameSet(group, c.lastFloor) || SameSet(group, c.avoid)); ++k) group = once(r);
    Group g;
    for (int i = 0; i < 3; ++i) g.normal[i] = group[i];
    g.refresh = r.Below(100) < kRefreshChance;
    return g;
}

// 领取前再核一次（存档里的旧选项、刷新期间状态变化）：仍然合法且有实际收益
// 3.2.0（用户：抽不到抗性饰品，打对应 Boss 很难）：辟火 / 避雷 / 防冰 / 凝血 / 防腐五个抗异常饰品在驿站常驻，
// 不占随机饰品格、不按进度开放；价格照饰品走（随层数和购买次数上涨），买下后下架
// 3.2.1（用户）：驿站武器 / 药 / 饰品各多 1 个「珍品位」，从全部已核验的兵器 / 药 / 饰品里随机（不按进度，前期也能抽到后期的），
// 3.3.1（用户）：价格 = 该类正常价不超过 500 时固定 500；超过 500 后按正常价 ×1.4。offer.quality = kRareQuality 标记
inline constexpr int kRareQuality = 8, kRareFloorPrice = 500;
inline constexpr int kShelfFillWeapons = 4;  // 3.6.0：主武器 / 影之武装可上架少于这个数时补 Boss 关联武器
inline int RarePrice(int normal) { return normal <= kRareFloorPrice ? kRareFloorPrice : static_cast<int>(int64_t{normal} * 7 / 5); }
inline bool ResistCharm(int accessory) {
    if (accessory < 0 || accessory >= growth::kAccessoryCount) return false;
    const auto id = growth::kAccessories[accessory].id;
    return id == "AC_ResistFire" || id == "AC_ResistThunder" || id == "AC_ResistIce" || id == "AC_ResistBleed" || id == "AC_ResistPoison";
}
inline bool StillValid(const Offer& o, const Context& c) {
    using namespace detail;
    if (RetiredOffer(o)) return false;
    if (o.kind == Kind::Weapon && (o.index < 0 || o.index >= growth::kWeaponCount)) return false;
    const int zone = ZoneOf(c.floor, c.floors);
    switch (o.kind) {
        case Kind::Weapon: case Kind::Medicine: case Kind::Accessory: {
            const Cat cat = o.kind == Kind::Medicine ? Cat::Medicine : o.kind == Kind::Accessory ? Cat::Accessory
                          : growth::kWeapons[o.index].kind == 1 ? Cat::Shaqi : Cat::Main;
            const Gate* g = c.catalog->Find(cat, o.index);
            if (o.quality == 9) return !Owned(*c.profile, cat, o.index);  // 驿站补充渠道（Boss 关联、缺原生获取渠道）：不按奖励门槛
            if (o.quality == kRareQuality) return !Owned(*c.profile, cat, o.index) && !(o.kind == Kind::Weapon && c.profile->Banned(o.index));  // 3.2.1 珍品位：不按进度
            if (o.kind == Kind::Accessory && ResistCharm(o.index)) return !Owned(*c.profile, cat, o.index);  // 3.2.0 常驻抗异常饰品：不按进度
            if (o.kind == Kind::Weapon && EndlessAll(c) && GrantableWeapon(o.index)) return !Owned(*c.profile, cat, o.index) && !c.profile->Banned(o.index);  // 3.4.0 无尽层全兵器
            return g && g->zone >= 0 && g->zone <= zone && !Owned(*c.profile, cat, o.index);
        }
        case Kind::Refine: { const int lv = RefineLevel(*c.profile, o.index); return lv >= 0 && lv + 1 == o.amount && o.amount <= growth::kAccessories[o.index].refine; }
        case Kind::Blessing: return o.index >= 0 && o.index < growth::kBlessCount && c.blessings->Room(o.index) >= 1;
        case Kind::TemperPack: case Kind::CultivatePack: return o.amount > 0;  // v15：点数整轮都能出（每层只给 1 + 1）
        case Kind::SoulToken: return o.amount > 0;
        case Kind::Item: return o.index >= 0 && o.index < kItemCount && c.items[o.index] < (o.index == static_cast<int>(Item::Foresight) ? std::clamp(c.modPurchaseLimit, 1, 3) : kItemStackCap);
        case Kind::Jade: return o.amount > 0;
        case Kind::Attr: return c.attrs && o.index >= 0 && o.index < kAttrCount && o.amount > 0 && c.attrs->Room(o.index) >= o.amount;
    }
    return false;
}

// 下标落在对应表内（读档 / 旧界面传来的数据先过这一关，界面和发放才不会越界）
inline bool Sane(const Offer& o) {
    switch (o.kind) {
        case Kind::Blessing: return o.index >= 0 && o.index < growth::kBlessCount && o.amount > 0;
        case Kind::TemperPack: case Kind::CultivatePack: case Kind::Jade: return o.amount > 0;
        case Kind::Weapon: return o.index >= 0 && o.index < growth::kWeaponCount;
        case Kind::Accessory: case Kind::Refine: return o.index >= 0 && o.index < growth::kAccessoryCount;
        case Kind::Medicine: return o.index >= 0 && o.index < growth::kMedicineCount;
        case Kind::Item: return o.index >= 0 && o.index < kItemCount;
        case Kind::Attr: return o.index >= 0 && o.index < kAttrCount && o.amount > 0;
        case Kind::SoulToken: return o.amount > 0;
    }
    return false;
}
// 空货位 / 无效项（商店没有合法货时 index = -1）：32ffb7d 实机 3 次 C++ 异常——状态序列化对空货位按负下标取名字
inline bool Empty(const Offer& o) {
    if (o.kind == Kind::Jade || o.kind == Kind::TemperPack || o.kind == Kind::CultivatePack || o.kind == Kind::SoulToken) return false;
    if (o.kind == Kind::Attr) return o.index < 0 || o.index >= kAttrCount;
    const int n = o.kind == Kind::Blessing ? growth::kBlessCount : o.kind == Kind::Weapon ? growth::kWeaponCount
                : o.kind == Kind::Accessory || o.kind == Kind::Refine ? growth::kAccessoryCount : o.kind == Kind::Medicine ? growth::kMedicineCount : kItemCount;
    return o.index < 0 || o.index >= n;
}
// 界面文字（品质只在祝福上显示：装备没有原生的凡 / 灵 / 玄，不伪造）
inline std::string Label(const Offer& o) {
    static constexpr std::string_view q[] = {"", "凡", "灵", "玄"};
    if (Empty(o)) return "（空）";
    switch (o.kind) {
        case Kind::Blessing: return std::format("祝福·{}（{}）×{}", growth::kBlessings[o.index].name, q[std::clamp(o.quality, 1, 3)], o.amount);
        case Kind::TemperPack: return std::format("淬炼点 +{}", o.amount);
        case Kind::CultivatePack: return std::format("修为点 +{}", o.amount);
        case Kind::Weapon: return std::format("{}：{}", growth::kWeapons[o.index].kind == 1 ? "影之武装" : "主兵器", growth::kWeapons[o.index].name);
        case Kind::Accessory: return std::format("饰品：{}", growth::kAccessories[o.index].name);
        case Kind::Refine: return std::format("饰品精炼：{} → {} 级", growth::kAccessories[o.index].name, o.amount);
        case Kind::Medicine: return std::format("新药：{}", growth::kMedicines[o.index].name);
        case Kind::Item: return std::format("{}：{}", kItems[o.index].name, kItems[o.index].text);
        case Kind::Jade: return std::format("魂玉 +{}", o.amount);
        case Kind::Attr: return std::format("{} +{:g}%", kAttrs[o.index].name, o.amount / 10.0);
        case Kind::SoulToken: return std::format("铸魂令 ×{}", o.amount);
    }
    return "?";
}

// 313987b 用户：按钮只放短名称；效果另起一行写清范围（全局 / 当前兵器 / 本层）、数值、时机与持续
// 3.2.1（用户）：珍品位是抽奖——货架上只显示「随机武器 / 药 / 饰品」，买下后才知道是什么（实际物品仍按种子 + 本次驿站定好，同种子同结果）
inline std::string RareName(const Offer& o) {
    return o.kind == Kind::Weapon ? "珍·随机武器" : o.kind == Kind::Medicine ? "珍·随机药" : "珍·随机饰品";
}
inline bool Mystery(const Offer& o) { return o.quality == kRareQuality && (o.kind == Kind::Weapon || o.kind == Kind::Medicine || o.kind == Kind::Accessory); }
inline std::string Name(const Offer& o) {
    static constexpr std::string_view q[] = {"", "凡", "灵", "玄"};
    if (Empty(o)) return "（空）";
    if (Mystery(o)) return RareName(o);
    switch (o.kind) {
        case Kind::Blessing: return std::format("{}（{}）", growth::kBlessings[o.index].name, q[std::clamp(o.quality, 1, 3)]);
        case Kind::TemperPack: return std::format("淬炼点 +{}", o.amount);
        case Kind::CultivatePack: return std::format("修为点 +{}", o.amount);
        case Kind::Weapon: return std::string(growth::kWeapons[o.index].name);
        case Kind::Accessory: return std::string(growth::kAccessories[o.index].name);
        case Kind::Refine: return std::format("{} 精炼", growth::kAccessories[o.index].name);
        case Kind::Medicine: return std::string(growth::kMedicines[o.index].name);
        case Kind::Item: return o.amount > 1 ? std::format("{}×{}", kItems[o.index].name, o.amount) : std::string(kItems[o.index].name);
        case Kind::Jade: return std::format("魂玉 +{}", o.amount);
        case Kind::Attr: return std::format("{} +{:g}%", kAttrs[o.index].name, o.amount / 10.0);
        case Kind::SoulToken: return std::format("铸魂令 ×{}", o.amount);
    }
    return "?";
}
inline std::string Effect(const Offer& o) {
    if (Empty(o)) return "";
    if (Mystery(o)) return o.kind == Kind::Weapon ? "购买后随机获得一件武器或影之武装" : o.kind == Kind::Medicine ? "购买后随机获得一种药" : "购买后随机获得一件饰品";
    switch (o.kind) {
        case Kind::Blessing: {
            const auto& b = growth::kBlessings[o.index];
            return std::format("本局一直生效：{}，本次 {} 层，最多 {} 层", b.text, o.amount, b.maxStack);
        }
        case Kind::TemperPack: return "整备里升级兵器用，每级 1 点";
        case Kind::CultivatePack: return "整备里点能力节点用";
        case Kind::Weapon: return std::format("{}，1 级进塔档背包；Esc → 角色装备后生效，只在塔内", growth::kWeapons[o.index].kind == 1 ? "影之武装" : "主兵器");
        case Kind::Accessory: return std::format("饰品：{}；Esc → 角色装上后生效，本局有效", growth::kAccessories[o.index].text);
        case Kind::Refine: return std::format("精炼到 {} 级：{}", o.amount, growth::kAccessories[o.index].text);
        case Kind::Medicine: return std::format("新药：{}；装进药槽后战斗中使用", growth::kMedicines[o.index].text);
        case Kind::Item: return ItemTiming(kItems[o.index].item) == Timing::Prep
            ? std::format("{}；放入 MOD 背包，层间准备，下一层开战时生效", kItems[o.index].text)
            : ItemTiming(kItems[o.index].item) == Timing::Use ? std::format("{}；放入 MOD 背包，随时点用", kItems[o.index].text)
            : std::string(kItems[o.index].text);
        case Kind::Jade: return "驿站购物用";
        case Kind::Attr:
            return std::format("{} +{:g}%", kAttrs[o.index].what, o.amount / 10.0);
        case Kind::SoulToken: return "整备 → 铸魂里为武器铸魂用";
    }
    return "";
}
// 正文里的一项：短名称一行，效果缩进另起一行（对话框正文会在区域内换行；按钮横排不换行，只放名称）
inline std::string Entry(int n, const std::string& name, const std::string& effect) {
    return std::format("{}. {}\n　　{}", n, name, effect);
}

// ---------------------------------------------------------------- 驿站商店（魂玉）
// 价格初稿（待 50 层预算模拟后交用户审稿）：随区上涨
inline int Price(Kind k, int zone) {
    // 2.0.0 用户 2026-11-08：前两区武器降价，保证前期有武器玩；后三区不变
    if (k == Kind::Weapon) { constexpr int kWeaponPrice[] = {100, 200, 396, 504, 612}; return kWeaponPrice[std::clamp(zone, 0, 4)]; }
    // 2.0.0 用户 2026-11-08：驿站每层都开、Boss 兵器也靠买，全部涨价（×1.5）
    const int base = k == Kind::Weapon ? 180 : k == Kind::Accessory ? 135 : k == Kind::Refine ? 105 : k == Kind::Medicine ? 120 : k == Kind::Item ? 180 : k == Kind::TemperPack ? 25 : k == Kind::CultivatePack ? 15 : 0;  // v15：MOD 道具 ×3（用户 2026-11-05）
    return base * (10 + 6 * std::clamp(zone, 0, kZones - 1)) / 10;
}
// v3：刷新 0.12 × 2J(s)，本店每次 ×1.5（floor = 内部层，即本店对应的 Boss 层）
inline int RefreshCost(int refreshes, int floor = 1) { return balance::RefreshPrice(balance::ShownOfBoss(std::max(1, floor)), refreshes); }
inline balance::Buy BuyOf(const Offer& o) {
    switch (o.kind) {
        case Kind::Weapon: return balance::Buy::Weapon;
        case Kind::Accessory: return balance::Buy::Accessory;
        case Kind::Medicine: return balance::Buy::Medicine;
        case Kind::Refine: return balance::Buy::Refine;
        case Kind::TemperPack: return balance::Buy::Temper;
        case Kind::CultivatePack: return balance::Buy::Cultivate;
        default: return balance::Buy::Mod;
    }
}
// 基准价格按购买次数上涨；武器再乘武库扩建折扣；重生符 / 吸血符 / 透视眼保留原倍数。
inline int ShopPrice(const Offer& o, const Context& c) {
    const auto b = BuyOf(o);
    int bought = 0;
    if (b == balance::Buy::Mod) bought = c.itemBuys && o.index >= 0 && o.index < kItemCount ? (*c.itemBuys)[static_cast<size_t>(o.index)] : 0;
    else bought = c.buys ? (*c.buys)[static_cast<size_t>(b)] : 0;
    int p = balance::Price(b, balance::ShownOfBoss(std::max(1, c.floor)), bought);
    if (b == balance::Buy::Weapon) p = std::max(1, static_cast<int>(int64_t{p} * std::clamp(c.weaponPricePct, 10, 100) / 100));
    if (o.kind == Kind::Item) {
        if (o.index == static_cast<int>(Item::Rebirth) || o.index == static_cast<int>(Item::Revive)) p *= 3;
        // 3.1.0（用户）：魂玉袋便宜（当层武器价 15%，用时给 30%：越往高层用越赚）；经验丹打六折
        else if (o.index == static_cast<int>(Item::JadeBag)) p = std::max(1, balance::Price(balance::Buy::Weapon, balance::ShownOfBoss(std::max(1, c.floor)), 0) * 15 / 100);
        else if (o.index == static_cast<int>(Item::ExpPill)) p = std::max(1, p * 3 / 5);
        else if (o.index == static_cast<int>(Item::Leech)) p = p * 27 / 10;
        else if (o.index == static_cast<int>(Item::Foresight)) p = p * 12 / 5;
        else if (o.index == static_cast<int>(Item::Bounty)) p = std::min(p, BountyPriceCap(c.floor));  // 3.6.0 价格跟层走
    }
    if (o.kind == Kind::Accessory && ResistCharm(o.index)) p = std::max(1, p * 6 / 5);
    if (o.quality == kRareQuality && (o.kind == Kind::Weapon || o.kind == Kind::Medicine || o.kind == Kind::Accessory))
        p = RarePrice(p);  // 3.2.0（用户）：常驻抗异常饰品比普通饰品贵 20%，同样随层数 / 购买次数涨
    if (o.kind == Kind::SoulToken) p = balance::Price(balance::Buy::Weapon, balance::ShownOfBoss(std::max(1, c.floor)), 0) * 6 / 5;  // 驿站铸魂令：比首件武器还贵 20%
    return p;
}

struct Ware { Offer offer; int price = 0; bool sold = false; };
struct Shop {
    int floor = -1;           // 本店对应的层（-1 = 没有开店）
    uint32_t version = 0;     // 货架版本：界面只能按当前版本成交
    int refreshes = 0;
    uint64_t seed = 0;        // 下一次补货用的随机状态（读档不免费换货）
    std::vector<Ware> shelf;  // 6 格：主兵器 1、影武 1、饰品 2、药 1、消耗品 1；没有合法货就空格（index -1）
    std::vector<Offer> bought;
    bool Open() const { return floor >= 0; }
};
// 续档时按当前规则更新报价，保留同一站的货品、已买记录和刷新随机状态。
inline bool RepriceShelf(Shop& s, const Context& c) {
    if (!s.Open()) return false;
    bool changed = false;
    for (auto& w : s.shelf) {
        if (w.sold || w.price <= 0) continue;
        const int price = ShopPrice(w.offer, c);
        if (w.price != price) { w.price = price; changed = true; }
    }
    if (changed) ++s.version;
    return changed;
}
// 补货：按本区合法库存；不展示超进度装备凑数；本次到店已买过的不再上架
inline void Stock(Shop& s, const Context& base) {
    using namespace detail;
    Xs r{s.seed ? s.seed : 0x9E3779B97F4A7C15ull};
    Context c = base;
    const int zone = ZoneOf(c.floor, c.floors);
    s.shelf.clear();
    std::vector<Offer> group = s.bought;
    auto put = [&](const std::vector<Cand>& pool, Kind kind) {
        Offer o;
        if (Pick(r, pool, group, {}, &o)) { group.push_back(o); s.shelf.push_back({o, ShopPrice(o, c), false}); }
        else s.shelf.push_back({{kind, -1, 0, 1}, 0, true});  // 空格（标成已售，不可买）
    };
    put(Equipment(c, true, Cat::Main, false), Kind::Weapon);
    put(Equipment(c, true, Cat::Shaqi, false), Kind::Weapon);
    auto acc = Equipment(c, true, Cat::Accessory, false);
    put(acc, Kind::Accessory);
    put(acc, Kind::Accessory);
    put(Equipment(c, true, Cat::Medicine, false), Kind::Medicine);
    put(Items(c), Kind::Item);
    s.seed = r.s;
    ++s.version;
}
namespace detail {
// 本次驿站的随机键：同一局不同层的驿站不同，同一次驿站（买东西、读档后重排）不变
inline uint64_t Mix64(uint64_t n) { n ^= n >> 30; n *= 0xBF58476D1CE4E5B9ull; n ^= n >> 27; n *= 0x94D049BB133111EBull; n ^= n >> 31; return n; }
inline uint64_t VisitKey(const Shop& s) { return Mix64((s.seed ? s.seed : 1) ^ (0xD1B54A32D192ED03ull * static_cast<uint64_t>(s.floor + 7))); }
inline uint64_t WareKey(uint64_t visit, const Offer& o) { return Mix64(visit ^ (static_cast<uint64_t>(o.kind) << 40) ^ (static_cast<uint64_t>(static_cast<uint32_t>(o.index)) * 0x9E3779B97F4A7C15ull)); }
}
// v14 驿站（用户 2026-11-05）：大列表。本轮已开放的原生武器 / 药 / 饰品（按进度区逐步加入、非 Boss 关联、未持有）
// 一直在架直到买下——买不起也不会在下次刷新时消失；supplement = 50 层流程里缺原生获取渠道的 Boss 关联武器（引擎给）；
// MOD 消耗道具常备。顺序：MOD 道具 → 原生（按开放区、表序）→ 补充渠道
inline void Stock14(Shop& s, const Context& c, const std::vector<Offer>& supplement) {
    using namespace detail;
    const int zone = ZoneOf(c.floor, c.floors);
    s.shelf.clear();
    // 2.0.0 用户 2026-11-08：每层不再固定给点数，每次驿站可以各买一次：淬炼点 +3、修为点 +1
    auto boughtKind = [&](Kind k) { return std::any_of(s.bought.begin(), s.bought.end(), [k](const Offer& o) { return o.kind == k; }); };
    // 用户 2026-11-11：每次驿站随机上架淬炼点或修为点其中一个；持有钱生钱两个都上架
    const uint64_t visit = detail::VisitKey(s);
    const bool temperToday = c.shopBothPoints || (visit & 1) == 0, cultivateToday = c.shopBothPoints || (visit & 1) == 1;
    if (temperToday && !boughtKind(Kind::TemperPack)) { const Offer o{Kind::TemperPack, 0, 3, 1}; s.shelf.push_back({o, ShopPrice(o, c), false}); }
    if (cultivateToday && !boughtKind(Kind::CultivatePack)) { const Offer o{Kind::CultivatePack, 0, 1, 1}; s.shelf.push_back({o, ShopPrice(o, c), false}); }
    // 用户 2026-11-12：约 5% 的驿站在属性（点数）这一类里上架 1 枚铸魂令，价格昂贵；本次买过就不再上
    if (detail::Mix64(visit ^ 0x50D1F00Dull) % 100 < static_cast<uint64_t>(kSoulShopPct) && !boughtKind(Kind::SoulToken)) {
        const Offer o{Kind::SoulToken, -1, 1, 1}; s.shelf.push_back({o, ShopPrice(o, c), false});
    }
    // 2.0.0 用户 2026-11-08：每次驿站随机 2 个不同的 MOD 道具（买了哪个，本次哪个就没有了，另一个不换）。重生符权重最低；复活符前 100 层不卖（只来自额外挑战奖励），无尽层上架、权重最低
    {
        auto mods = ModItems(c);
        const int rb = static_cast<int>(Item::Rebirth);
        if (c.items[rb] < kItemStackCap) mods.push_back({{Kind::Item, rb, 1, 1}, kItems[rb].weight});
        // 3.1.0（用户）：显示 100 层后（无尽层）复活符也上架 MOD 道具，权重最低（1，其余都 ≥ 2），价格同重生符 ×3
        const int rv = static_cast<int>(Item::Revive);
        if (c.floor > 50 && c.floors >= 50 && c.items[rv] < kItemStackCap) mods.push_back({{Kind::Item, rv, 1, 1}, 1});
        const int sight = static_cast<int>(Item::Foresight);
        const int leech = static_cast<int>(Item::Leech);
        const Offer leechOffer{Kind::Item, leech, 1, 1};
        const bool leechBoughtHere = std::find(s.bought.begin(), s.bought.end(), leechOffer) != s.bought.end();
        // Keep the original draw stable if buying the ninth charm filled the bag.
        if (c.items[leech] < kItemStackCap || leechBoughtHere) mods.push_back({leechOffer, kItems[leech].weight});
        const Offer sightOffer{Kind::Item, sight, 1, 1};
        const bool boughtHere = std::find(s.bought.begin(), s.bought.end(), sightOffer) != s.bought.end();
        if (c.floor < c.floors && (c.items[sight] == 0 || boughtHere)) mods.push_back({sightOffer, kItems[sight].weight});
        Xs mr{(s.seed ^ (0x9E3779B97F4A7C15ull * static_cast<uint64_t>(s.floor + 3))) | 1};
        mr.Next();
        std::vector<Offer> drawn;
        for (int k = 0; k < std::clamp(c.shopModItems, 0, 6); ++k) {  // 用户 2026-11-11：默认 2 个，钱生钱 3 个
            Offer one;
            if (!Pick(mr, mods, drawn, {}, &one)) break;
            drawn.push_back(one);
            if (std::count(s.bought.begin(), s.bought.end(), one) < std::clamp(c.modPurchaseLimit, 1, 3))
                s.shelf.push_back({one, ShopPrice(one, c), false});
        }
    }
    // 用户 2026-11-11：武器（打过的 Boss 兵器 + 随进度开放的非 Boss 兵器）、药、饰品分成三个池，随流程动态流入；
    // 每次驿站每个池随机上架 4 个（武库扩建武器 +2，钱生钱每类 +2）。同一次驿站里的抽选固定：
    // 每件按（本次驿站, 货品）算一个随机键，取键最小的 N 件——本次买下的也参与排名，所以买完不会补上新货、其余不变
    struct PoolWare { Offer offer; int price; bool bought; };
    std::vector<PoolWare> pools[3];  // 0 武器 1 药 2 饰品
    auto inShelf = [&](const Offer& o) { for (const auto& p : pools) for (const auto& w : p) if (w.offer == o) return true; return false; };
    auto add = [&](int pool, const Offer& o, int price, Cat cat) {
        if (inShelf(o)) return;  // Boss 兵器已作为补充渠道进池的不重复
        if (o.kind == Kind::Accessory && ResistCharm(o.index)) return;  // 抗异常饰品单独常驻，不进随机池
        const bool boughtHere = std::find(s.bought.begin(), s.bought.end(), o) != s.bought.end();
        if (!boughtHere && Owned(*c.profile, cat, o.index)) return;
        pools[pool].push_back({o, price, boughtHere});
    };
    const int weaponPct = std::clamp(c.weaponPricePct, 10, 100);
    for (auto& o : supplement) {
        if (o.kind != Kind::Weapon || o.index < 0 || o.index >= growth::kWeaponCount || RetiredWeapon(o.index)) continue;
        add(0, o, ShopPrice(o, c), growth::kWeapons[o.index].kind == 1 ? Cat::Shaqi : Cat::Main);
    }
    std::vector<std::pair<int, const Gate*>> native;
    for (auto& g : c.catalog->gates) {
        if ((g.cat == Cat::Main || g.cat == Cat::Shaqi) && RetiredWeapon(g.index)) continue;
        if (g.zone < 0 || g.zone > zone || !g.shop || g.bossLinked) continue;
        native.push_back({g.zone, &g});
    }
    // N9i（用户 2026-11-13）：回收的东西进池子（不是必上架），和其它货一起随机抽
    if (c.returned) for (const auto& o : *c.returned) {
        if (o.index < 0) continue;
        if (o.kind == Kind::Weapon) {
            if (o.index >= growth::kWeaponCount || RetiredWeapon(o.index) || c.profile->Banned(o.index)) continue;
            add(0, o, ShopPrice(o, c), growth::kWeapons[o.index].kind == 1 ? Cat::Shaqi : Cat::Main);
        } else if (o.kind == Kind::Medicine) add(1, o, ShopPrice(o, c), Cat::Medicine);
        else if (o.kind == Kind::Accessory) add(2, o, ShopPrice(o, c), Cat::Accessory);
    }
    if (detail::EndlessAll(c)) for (const auto& g : c.catalog->gates) {  // 3.4.0 无尽层：全部可发放的兵器进武器池（已在池里的不重复）
        if ((g.cat != Cat::Main && g.cat != Cat::Shaqi) || !detail::GrantableWeapon(g.index) || c.profile->Banned(g.index)) continue;
        const Offer o{Kind::Weapon, g.index, 1, 1};
        bool dup = false; for (const auto& w : pools[0]) dup = dup || (w.offer.kind == Kind::Weapon && w.offer.index == g.index);
        if (!dup) add(0, o, ShopPrice(o, c), g.cat);
    }
    std::stable_sort(native.begin(), native.end(), [](auto& a, auto& b) { return a.first < b.first; });
    for (auto& [z, g] : native) {
        const Offer o{KindOf(g->cat), g->index, 1, 1};
        const int pool = g->cat == Cat::Medicine ? 1 : g->cat == Cat::Accessory ? 2 : 0;
        add(pool, o, ShopPrice(o, c), g->cat);
        // v3：饰品精炼（原 40 层精炼匣）进饰品池：已持有、未满精炼的饰品。3.1.0 用户：去掉精炼（饰品走打磨），这段停用
        if (false && g->cat == Cat::Accessory && Owned(*c.profile, g->cat, g->index)) {
            const int lv = RefineLevel(*c.profile, g->index);
            if (lv < growth::kAccessories[g->index].refine) {
                const Offer refine{Kind::Refine, g->index, lv + 1, 1};
                if (!inShelf(refine)) pools[2].push_back({refine, ShopPrice(refine, c), std::find(s.bought.begin(), s.bought.end(), refine) != s.bought.end()});
            }
        }
    }
    // 3.6.0（用户 2026-11-15：很多玩家反映驿站不上架武器 / 影之武装）：非 Boss 武器总共只有主 9 / 影 9 把，买齐或拿了拾荒者后就空了。
    // 主武器或影之武装能上架的不足 4 件时，补进本区及之前区的 Boss 关联武器（没持有的、有已核验发放记录的），quality 9 不走进度门槛
    for (const Cat want : {Cat::Main, Cat::Shaqi}) {
        int have = 0;
        for (const auto& w : pools[0])
            if (!w.bought && w.offer.kind == Kind::Weapon && w.offer.index >= 0 && w.offer.index < growth::kWeaponCount &&
                (growth::kWeapons[w.offer.index].kind == 1) == (want == Cat::Shaqi)) ++have;
        if (have >= kShelfFillWeapons) continue;
        for (const auto& g : c.catalog->gates) {
            if (g.cat != want || !g.bossLinked || g.zone < 0 || g.zone > zone || RetiredWeapon(g.index) || c.profile->Banned(g.index)) continue;
            const auto id = growth::kWeapons[g.index].id;
            if (tower::NotBossWeapon(id) || std::none_of(std::begin(tower::kWeaponDrops), std::end(tower::kWeaponDrops), [&](const auto& d) { return d.weapon == id && d.verified; })) continue;
            const Offer o{Kind::Weapon, g.index, 1, 9};
            add(0, o, ShopPrice(o, c), want);
        }
    }
    for (int pool = 0; pool < 3; ++pool) {
        auto& p = pools[pool];
        // v3：武器 2、药 1、饰品（含精炼）2；武库扩建武器 +2；钱生钱每类 +1
        const int base = c.shopPerCategory != 4 ? c.shopPerCategory  /* 显式覆盖（测试 / 开发：整池上架） */
                       : pool == 0 ? balance::kShelfWeapon : pool == 1 ? balance::kShelfMedicine : balance::kShelfAccessory;
        const int n = std::max(0, base + c.shopExtraAll + (pool == 0 ? c.shopExtraWeapons : 0));
        std::vector<size_t> order(p.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        auto key = [&](size_t i) { return detail::WareKey(visit, p[i].offer); };
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return key(a) < key(b); });
        if (static_cast<int>(order.size()) > n) order.resize(static_cast<size_t>(n));
        std::sort(order.begin(), order.end());  // 上架后按原顺序显示（Boss 兵器在前、按开放区）
        for (size_t i : order) if (!p[i].bought) s.shelf.push_back({p[i].offer, p[i].price, false});
    }
    // 3.2.1 珍品位：武器（含影之武装）/ 药 / 饰品各 1 个，全图鉴随机；同一次驿站固定（买下后不补）
    auto rare = [&](std::initializer_list<Cat> cats, Kind kind, uint64_t salt) {
        int best = -1; uint64_t bestKey = ~0ull; bool bestBought = false;
        for (const auto& g : c.catalog->gates) {
            if (std::find(cats.begin(), cats.end(), g.cat) == cats.end() || g.zone < 0 || !(g.reward || g.shop)) continue;
            if (kind == Kind::Weapon && (RetiredWeapon(g.index) || c.profile->Banned(g.index))) continue;
            // 只放能正常发放的：进度表里已核验（区号 ≥ 0、能进奖励或商店）；Boss 关联兵器还要有已核验的掉落 / 发放记录（废案、未接入的不放）
            if (kind == Kind::Weapon && g.bossLinked && !g.shop) {
                const auto id = growth::kWeapons[g.index].id;
                if (tower::NotBossWeapon(id) || std::none_of(std::begin(tower::kWeaponDrops), std::end(tower::kWeaponDrops), [&](const auto& d) { return d.weapon == id && d.verified; })) continue;
            }
            if (kind == Kind::Accessory && ResistCharm(g.index)) continue;
            const Offer o{kind, g.index, 1, kRareQuality};
            const bool boughtHere = std::find(s.bought.begin(), s.bought.end(), o) != s.bought.end();
            if (!boughtHere && Owned(*c.profile, g.cat, g.index)) continue;
            if (!boughtHere && std::any_of(s.shelf.begin(), s.shelf.end(), [&](const Ware& w) { return w.offer.kind == kind && w.offer.index == g.index; })) continue;
            const uint64_t key = detail::WareKey(visit ^ salt, o);
            if (key < bestKey) { bestKey = key; best = g.index; bestBought = boughtHere; }
        }
        if (best < 0 || bestBought) return;
        const Offer o{kind, best, 1, kRareQuality};
        s.shelf.push_back({o, ShopPrice(o, c), false});
    };
    rare({Cat::Main, Cat::Shaqi}, Kind::Weapon, 0x5EA1A11Dull);
    rare({Cat::Medicine}, Kind::Medicine, 0x3ED1C1E5ull);
    rare({Cat::Accessory}, Kind::Accessory, 0xACCE550Bull);
    for (int i = 0; i < growth::kAccessoryCount; ++i) {  // 抗异常饰品常驻（没持有就上架）
        if (!ResistCharm(i) || Owned(*c.profile, Cat::Accessory, i)) continue;
        const Offer o{Kind::Accessory, i, 1, 1};
        if (std::find(s.bought.begin(), s.bought.end(), o) != s.bought.end()) continue;
        s.shelf.push_back({o, ShopPrice(o, c), false});
    }
    ++s.version;
}
// 2.0.0 用户 2026-11-08：第 1 层 Boss 后的三选一是随机开局武器：全部主武器和影之武装等概率，不按进度（紫冥剑也可能出）。
// 铸魂形态不单独发，已持有的不出。quality = 9：不走奖励的进度门槛（StillValid）
inline std::array<Offer, 3> DrawStarterWeapons(Xs& r, const Context& c) {
    std::vector<int> pool;
    for (int i = 0; i < growth::kWeaponCount; ++i)
        if (c.profile->Find(i) < 0 && !c.profile->Banned(i) && !RetiredWeapon(i) && !NotBossWeapon(growth::kWeapons[i].id)) pool.push_back(i);
    std::array<Offer, 3> out{};
    for (size_t k = 0; k < out.size() && !pool.empty(); ++k) {
        const size_t i = static_cast<size_t>(r.Below(static_cast<int>(pool.size())));
        out[k] = Offer{Kind::Weapon, pool[i], 1, 9};
        pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(i));
    }
    return out;
}
// 额外挑战奖励：一件符合进度的非 Boss 关联原生武器 / 药 / 饰品（未持有）；没有返回 false
inline bool DrawBonusNative(Xs& r, const Context& c, Offer* out) {
    using namespace detail;
    auto pool = Equipment(c, false, Cat::Main, true);
    std::erase_if(pool, [](const Cand& x) { return x.offer.kind == Kind::Refine; });
    return Pick(r, pool, {}, {}, out);
}
// 1.0.1 用户 2026-11-07：额外挑战成功 → 额外三选一。池 = 非 Boss 关联的原生武器 / 药 / 饰品（未持有、进度内）
// + MOD 道具（准备道具、复活符、重生符）。三项互不相同；池不够就少于三项
// 1.1.0 用户 2026-11-07：游戏里大部分饰品和药靠任务拿，爬塔很难拿到 → 额外奖励按类别抽（不再按件数堆）：
// 饰品 38%、药 27%、非 Boss 武器 / 影之武装 20%、MOD 准备道具 10%、复活符 / 重生符 5%；已持有的不再出现。
// v3（用户 2026-11-12）：装备主要靠驿站。额外挑战三选一按类别：魂玉包（3 × J(s)）35、点数 25、MOD 准备道具 20、
// 非 Boss 装备（武器 / 饰品 / 药 / 精炼）15、复活符 / 重生符 5；同类可多次被抽中但同一组不重复
enum class BonusCat { Jade, Points, Mod, Equip, Charm, Count };
inline constexpr int kBonusWeight[static_cast<int>(BonusCat::Count)] = {35, 25, 20, 15, 5};
inline std::vector<Offer> DrawBonus3(Xs& r, const Context& c) {
    using namespace detail;
    constexpr int n = static_cast<int>(BonusCat::Count);
    std::vector<Cand> pools[n];
    const int jade = static_cast<int>(std::lround(balance::kBonusJade * balance::J(balance::ShownOfBoss(std::max(1, c.floor)))));
    pools[static_cast<int>(BonusCat::Jade)].push_back({{Kind::Jade, -1, jade, 1}, 1});
    pools[static_cast<int>(BonusCat::Points)] = Points(c);
    pools[static_cast<int>(BonusCat::Mod)] = ModItems(c);
    for (auto& e : Equipment(c, false, Cat::Main, true)) pools[static_cast<int>(BonusCat::Equip)].push_back(e);
    for (Item it : {Item::Revive, Item::Rebirth}) {
        const int i = static_cast<int>(it);
        if (c.items[i] < kItemStackCap) pools[static_cast<int>(BonusCat::Charm)].push_back({{Kind::Item, i, 1, 1}, 1});
    }
    std::vector<Offer> out;
    for (int k = 0; k < 3; ++k) {
        bool tried[n] = {}, got = false;
        Offer o;
        for (int attempt = 0; attempt < n && !got; ++attempt) {
            int total = 0;
            for (int t = 0; t < n; ++t) if (!tried[t] && !pools[t].empty()) total += kBonusWeight[t];
            if (total <= 0) break;
            int x = r.Below(total), type = -1;
            for (int t = 0; t < n; ++t) if (!tried[t] && !pools[t].empty() && (x -= kBonusWeight[t]) < 0) { type = t; break; }
            if (type < 0) break;
            tried[type] = true;
            got = Pick(r, pools[type], out, {}, &o);  // 这一类已被本组选光就换别的类
        }
        if (!got) break;
        out.push_back(o);
    }
    return out;
}
inline void OpenShop(Shop& s, const Context& c, uint64_t seed) {
    s = {};
    s.floor = c.floor;
    s.seed = seed ? seed : 1;
    Stock(s, c);
}
enum class Buy { Ok, Stale, Sold, NoJade, Invalid, GrantFailed };
// 3.1.0（玩家反馈：魂玉够却买不了）：说清楚一件货为什么现在不能买；能买返回空。与 StillValid 同一套条件
inline std::string_view WhyNot(const Offer& o, const Context& c) {
    using namespace detail;
    if (StillValid(o, c)) return {};
    if (RetiredOffer(o)) return "已下架";
    switch (o.kind) {
        case Kind::Weapon: case Kind::Medicine: case Kind::Accessory: {
            if (o.kind == Kind::Weapon && (o.index < 0 || o.index >= growth::kWeaponCount)) return "已下架";
            const Cat cat = o.kind == Kind::Medicine ? Cat::Medicine : o.kind == Kind::Accessory ? Cat::Accessory : growth::kWeapons[o.index].kind == 1 ? Cat::Shaqi : Cat::Main;
            if (cat != Cat::Medicine && cat != Cat::Accessory && c.profile->Banned(o.index)) return "已熔铸移除";
            if (Owned(*c.profile, cat, o.index)) return "已拥有";
            return "尚未解锁";
        }
        case Kind::Item: return "已达持有上限";
        case Kind::Refine: return "精炼等级已变化";
        case Kind::Attr: return "已达上限";
        default: return "暂不可买";
    }
}
inline std::string_view BuyText(Buy b) {
    switch (b) {
        case Buy::Ok: return "";
        case Buy::Stale: return "货架已变化，请重新打开商店";
        case Buy::Sold: return "已售出";
        case Buy::NoJade: return "魂玉不足";
        case Buy::Invalid: return "这件现在不能买";
        case Buy::GrantFailed: return "发放失败，未扣魂玉";
    }
    return "?";
}
// 一笔交易：版本 → 库存 → 合法性 → 余额 → 发放（grant 返回 true = 已发放并读回）→ 扣钱。任何一步失败都不扣钱、不改货架
template <class Grant>
inline Buy Purchase(Shop& s, int slot, uint32_t version, int& jade, const Context& c, Grant grant) {
    if (!s.Open() || version != s.version) return Buy::Stale;
    if (slot < 0 || slot >= static_cast<int>(s.shelf.size())) return Buy::Invalid;
    Ware& w = s.shelf[slot];
    if (w.sold || Empty(w.offer)) return Buy::Sold;  // 3.1.0（玩家：驿站铸魂令"交易未确认"）：铸魂令没有下标（-1），原来 index < 0 一律当已售出
    if (w.offer.kind == Kind::Item && std::count(s.bought.begin(), s.bought.end(), w.offer) >= std::clamp(c.modPurchaseLimit, 1, 3)) return Buy::Sold;
    if (!StillValid(w.offer, c)) return Buy::Invalid;
    if (jade < w.price) return Buy::NoJade;
    if (!grant(w.offer)) return Buy::GrantFailed;
    jade -= w.price;
    w.sold = true;
    s.bought.push_back(w.offer);
    ++s.version;  // 旧界面的重复请求不再成交
    return Buy::Ok;
}
inline Buy Refresh(Shop& s, uint32_t version, int& jade, const Context& c) {
    if (!s.Open() || version != s.version) return Buy::Stale;
    const int cost = RefreshCost(s.refreshes, s.floor);
    if (jade < cost) return Buy::NoJade;
    jade -= cost;
    ++s.refreshes;
    Stock(s, c);
    return Buy::Ok;
}
inline void CloseShop(Shop& s) { s = {}; }

// ---------------------------------------------------------------- 准备道具（下一层生效）
// 初版一个待发动槽。Arm 只登记不扣；下层"倒计时结束正式开战"时 Fire 扣 1 并返回要施加的道具；取消 / 生成失败什么都不做（保留）
// 准备道具：每层最多预装 3 种不同的（同一种一层只装一个，不叠加），预装只登记不扣；
// 下一层正式开战（读秒结束）那一刻才扣库存并生效，取消入场 / 生成失败 / 暂离都不扣、预装保留；效果作用到本层结束
inline constexpr int kPrepSlots = kItemCount;  // 3.6.0（用户 2026-11-15）：不同种类可以一起用，同一种一层只能用一张（Armed 拦重复）；原 v14 每层一件
struct Prep {
    std::vector<int> armed;   // 待发动（kItems 下标，互不相同）
    std::vector<int> active;  // 本层已生效（层结束 / 出塔清掉）
    bool Armed(int i) const { return std::find(armed.begin(), armed.end(), i) != armed.end(); }
    bool Active(int i) const { return std::find(active.begin(), active.end(), i) != active.end(); }
    // 拒绝原因（空 = 可以装）
    std::string_view Why(int i, const std::array<int, kItemCount>& have) const {
        if (i < 0 || i >= kItemCount || ItemTiming(kItems[i].item) != Timing::Prep) return "这个道具不能预装";
        if (have[i] <= 0) return "没有库存";
        if (Armed(i)) return "已经预装了";
        if (static_cast<int>(armed.size()) >= kPrepSlots) return "已准备的道具太多";
        return "";
    }
    bool Arm(int i, const std::array<int, kItemCount>& have) {
        if (!Why(i, have).empty()) return false;
        armed.push_back(i);
        return true;
    }
    bool Disarm(int i) {
        auto it = std::find(armed.begin(), armed.end(), i);
        if (it == armed.end()) return false;
        armed.erase(it);
        return true;
    }
    void DisarmAll() { armed.clear(); }
    // 正式开战：有库存的才扣并生效；返回本次生效的
    std::vector<int> Fire(std::array<int, kItemCount>& have) {
        active.clear();
        for (int i : armed)
            if (i >= 0 && i < kItemCount && have[i] > 0) { --have[i]; active.push_back(i); }
        armed.clear();
        return active;
    }
    // Undo only this wave's already-fired preparations before a retry.
    // Fail closed if another pending preparation would be overwritten.
    bool RearmFired(std::array<int, kItemCount>& have) {
        if (active.empty()) return true;
        if (!armed.empty() || active.size() > static_cast<size_t>(kPrepSlots)) return false;
        std::vector<int> seen;
        for (int i : active) {
            if (i < 0 || i >= kItemCount || ItemTiming(kItems[i].item) != Timing::Prep ||
                std::find(seen.begin(), seen.end(), i) != seen.end() || have[i] < 0 || have[i] >= INT_MAX) return false;
            seen.push_back(i);
        }
        for (int i : active) ++have[i];
        armed = active; active.clear();
        return true;
    }
    void FloorEnded() { active.clear(); }
};

// ---------------------------------------------------------------- 战斗即时道具快捷栏（已停用：只为读旧局存档保留，用户 2026-11-01 取消即时道具）
inline constexpr int kQuickSlots = 3;
struct QuickBar {
    std::array<int, kQuickSlots> slot{-1, -1, -1};  // kItems 下标
    int selected = 0;
    bool Assign(int s, int item) {
        if (s < 0 || s >= kQuickSlots) return false;
        if (item >= 0 && (item >= kItemCount || ItemTiming(kItems[item].item) != Timing::Instant)) return false;  // 只装即时类，避免误用准备道具
        for (auto& x : slot) if (x == item && item >= 0) x = -1;  // 一种道具只占一格
        slot[s] = item;
        return true;
    }
    void Cycle(int dir) { selected = ((selected + dir) % kQuickSlots + kQuickSlots) % kQuickSlots; }  // 不自动轮转到别的道具
    // 用当前格：apply 返回 true = 效果已成功施加，这时才扣 1；没有库存 / 条件不满足（apply 返回 false）不扣
    template <class Apply>
    bool Use(std::array<int, kItemCount>& have, Apply apply) {
        const int i = slot[selected];
        if (i < 0 || have[i] <= 0) return false;
        if (!apply(i)) return false;
        --have[i];
        return true;
    }
};

// ---------------------------------------------------------------- 存读档（随塔档 / 局存档保存）
inline std::string SaveOffer(const Offer& o) { return std::format("{},{},{},{}", static_cast<int>(o.kind), o.index, o.amount, o.quality); }
inline bool LoadOffer(std::istream& in, Offer* o) {
    int k = 0; char c1, c2, c3;
    if (!(in >> k >> c1 >> o->index >> c2 >> o->amount >> c3 >> o->quality) || c1 != ',' || c2 != ',' || c3 != ',' || k < 0 || k > static_cast<int>(Kind::SoulToken)) return false;
    o->kind = static_cast<Kind>(k);
    return true;
}
// "shop <floor> <version> <refreshes> <seed> <n> [offer price sold]… <m> [bought]… prep <item> <active> quick <s0> <s1> <s2> <sel>"
inline std::string Save(const Shop& s, const Prep& p, const QuickBar& q) {
    std::string out = std::format("shop {} {} {} {} {}", s.floor, s.version, s.refreshes, s.seed, s.shelf.size());
    for (auto& w : s.shelf) out += std::format(" {} {} {}", SaveOffer(w.offer), w.price, w.sold ? 1 : 0);
    out += std::format(" {}", s.bought.size());
    for (auto& b : s.bought) out += " " + SaveOffer(b);
    out += std::format(" prep2 {}", p.armed.size());
    for (int i : p.armed) out += std::format(" {}", i);
    out += std::format(" {}", p.active.size());
    for (int i : p.active) out += std::format(" {}", i);
    out += std::format(" quick {} {} {} {}", q.slot[0], q.slot[1], q.slot[2], q.selected);
    return out;
}
inline bool Load(const std::string& text, Shop* s, Prep* p, QuickBar* q) {
    std::istringstream in(text);
    std::string tag;
    Shop ns; Prep np; QuickBar nq;
    size_t n = 0, m = 0;
    if (!(in >> tag >> ns.floor >> ns.version >> ns.refreshes >> ns.seed >> n) || tag != "shop" || n > 256) return false;  // v14 大列表货架
    for (size_t i = 0; i < n; ++i) {
        Ware w; int sold = 0;
        if (!LoadOffer(in, &w.offer) || !(in >> w.price >> sold)) return false;
        if (w.offer.index >= 0 && !Sane(w.offer)) return false;  // 空格（index -1）照样读
        w.sold = sold != 0;
        ns.shelf.push_back(w);
    }
    if (!(in >> m) || m > 64) return false;
    for (size_t i = 0; i < m; ++i) { Offer o; if (!LoadOffer(in, &o)) return false; ns.bought.push_back(o); }
    // 旧格式 "prep <item> <active>"（单件）照读；新格式 "prep2 <n> … <m> …"
    if (!(in >> tag)) return false;
    auto valid = [](int i) { return i >= 0 && i < kItemCount && ItemTiming(kItems[i].item) == Timing::Prep; };
    if (tag == "prep") {
        int item = -1, active = -1;
        if (!(in >> item >> active)) return false;
        if (item >= kItemCount || active >= kItemCount) return false;
        if (valid(item)) np.armed.push_back(item);
        if (valid(active)) np.active.push_back(active);
    } else if (tag == "prep2") {
        for (auto* list : {&np.armed, &np.active}) {
            size_t k = 0;
            if (!(in >> k) || k > static_cast<size_t>(kItemCount)) return false;
            for (size_t j = 0; j < k; ++j) {
                int i = -1;
                if (!(in >> i) || i < 0 || i >= kItemCount) return false;
                if (valid(i) && std::find(list->begin(), list->end(), i) == list->end() && (list != &np.armed || np.armed.size() < static_cast<size_t>(kPrepSlots))) list->push_back(i);
            }
        }
    } else return false;
    if (!(in >> tag >> nq.slot[0] >> nq.slot[1] >> nq.slot[2] >> nq.selected) || tag != "quick") return false;
    if (nq.selected < 0 || nq.selected >= kQuickSlots) return false;
    for (int x : nq.slot) if (x >= kItemCount) return false;
    *s = ns; *p = np; *q = nq;
    return true;
}

}  // namespace tower::rewards
