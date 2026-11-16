// 影塔塔内成长（纯逻辑，不碰游戏）：设计见 notes/tower-design-v2.md。
// 成长走游戏原生系统（兵器等级 / 铸魂、能力修行、药瓶、饰品）；这里只管"塔档"——玩家这一局在塔里拥有什么、几级、还剩多少点，
// 以及每层给什么。把塔档落到游戏里由 tower_session.h 负责。
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "floors.generated.h"
#include "growth_data.generated.h"
#include "tower_ids.h"

namespace tower::growth {

// ---------------------------------------------------------------- 查表
inline int WeaponIndex(std::string_view id, int kind) {
    for (int i = 0; i < kWeaponCount; ++i) if (ids::Equal(kWeapons[i].id, id) && kWeapons[i].kind == kind) return i;
    return -1;
}
inline int NodeIndex(std::string_view id) {
    for (int i = 0; i < kNodeCount; ++i) if (ids::Equal(kNodes[i].id, id)) return i;
    return -1;
}
inline int MedicineIndex(std::string_view id) {
    for (int i = 0; i < kMedicineCount; ++i) if (ids::Equal(kMedicines[i].id, id)) return i;
    return -1;
}
inline int AccessoryIndex(std::string_view id) {
    for (int i = 0; i < kAccessoryCount; ++i) if (ids::Equal(kAccessories[i].id, id)) return i;
    return -1;
}
// 下一个特效在几级（已过 30 级或没有特效返回 0）
inline int NextPerk(int weapon, int level) {
    for (int p : kWeapons[weapon].perks) if (p > level) return p;
    return 0;
}
inline int PerksUnlocked(int weapon, int level) {
    int n = 0;
    for (int p : kWeapons[weapon].perks) if (p && p <= level) ++n;
    return n;
}

// ---------------------------------------------------------------- 塔的规则常数
inline constexpr int kWeaponCap = 30;         // 塔内兵器上限：30 级后原生只加攻击，不再有特效
inline constexpr int kStatNodeCap = 20;       // 三个加点节点（锋芒 / 凝神 / 易筋）塔内上限（原生 30）
inline constexpr int kMedLevelMax = 6;
// 饰品槽（用户 2026-11-05）：到第 11 / 21 / 31 层各开一格、第 41 层开满（基础 2 格 + 4 = 6 格）。nextFloor = 将要打 / 正在打的层
inline constexpr int kAccessorySlotFloors[4] = {11, 21, 31, 41};
inline int AccessorySlotUnlocks(int nextFloor) {
    int n = 0;
    for (int f : kAccessorySlotFloors) n += nextFloor >= f;
    return n;
}
inline constexpr int kGrowthEndsAfter = 35;   // 第 36 层起淬炼 / 修为点减半（v14：后期仍会拿到新武器，不能完全断档）
// 塔档标准起点（起手装：黑伤 / 黑伤双刃 / 羽弓 1 级、药葫芦 / 凝锋丹、能力全关、无饰品）的原生属性，
// 2026-11-05 本机全新开局实测（游戏 26266538）：生命 850、攻击 100、杀气上限 400。高于这个的部分来自原档的永久加成，进塔压回
inline constexpr double kStartHp = 850, kStartAttack = 100, kStartShachi = 400;
// 1.0.1（用户 2026-11-06：个别玩家进塔血量和别人不一样）：塔档世界模板的实测起点（模板角色自带加成后 935，用户确认保留）。
// 新局在第 0 层实测，高于它 2% 以上（原档永久加成 / 周目 / DLC / 增益 / 任何没列到的情况）就按比例压回，整局固定
inline constexpr double kPoolStartHp = 935, kPoolStartShachi = 400;
inline constexpr std::string_view kStarterMain = "Tang";  // 黑伤
inline constexpr std::string_view kStarterMedicine = "Hulu";
inline constexpr std::array<std::string_view, 3> kStarterShaqi = {"UnnamedBow", "XueDiZi", "CuiXinPiPa"};  // 羽弓 / 血滴子 / 催心琵琶（用户 2026-10-30）
// 新局起手：三把主武器入包，前两把默认装备；弓和巨凶斧装备在两个影之武装槽。
// 原生 InitMedicines 写的是 Hulu + "Beiming"，药品表里没有 Beiming（疑似试玩版遗留 ID），第二瓶取原流程最早拿到的凝锋丹
inline constexpr std::array<std::string_view, 3> kStarterKitMain = {"Tang", "ZhanMaBlade", "Double"};  // 黑伤 / 亡命斩马刀 / 黑伤双刃（背包）
inline constexpr std::array<std::string_view, 2> kStarterKitShaqi = {"UnnamedBow", "BrutalAxe"};  // 羽弓 / 巨凶斧
inline constexpr std::array<std::string_view, 2> kStarterKitMeds = {"Hulu", "NingFengDan"}; // 药葫芦 / 凝锋丹
// 起手影之武装固定为第一章最先拿到的羽弓 + 巨凶斧（用户 2026-10-31）；血滴子留给之后的塔层，具体层数未定

// 药档（用户 2026-10-30 定的方案第 3 节）
inline constexpr std::array<std::string_view, 4> kMedTier1 = {"Medicine_Recover", "Medicine_Duration", "Medicine_AdditionalRecover", "KuYe"};
inline constexpr std::array<std::string_view, 6> kMedTier2 = {"GuiShen", "NingFengDan", "Medicine_Attack", "FeiJiu", "Medicine_ResistanceElement", "Medicine_Element"};
inline constexpr std::array<std::string_view, 5> kMedTier3 = {"Medicine_Interrupt", "Medicine_Ghost", "Medicine_ShachiAttack", "JinJiHeJi", "RongQi"};
inline constexpr std::string_view kMedRare = "Medicine_UltimateCharge";  // 长生饮
// 在塔里没意义的饰品（加尘埃掉落、潜行、对骑兵）不进池
inline constexpr std::array<std::string_view, 1> kAccessoryExcluded = {"AC_Loot"};
inline bool AccessoryInPool(int i) {
    return std::find(kAccessoryExcluded.begin(), kAccessoryExcluded.end(), kAccessories[i].id) == kAccessoryExcluded.end();
}

// 能力节点在塔里的花费：三条线前 4 个 1 点、后 3 个 2 点、加点节点每级 1 点；榕树之心 2 点
inline bool IsStatNode(int n) { return kNodes[n].maxLevel > 1; }
inline int NodeCap(int n) { return IsStatNode(n) ? std::min(kNodes[n].maxLevel, kStatNodeCap) : kNodes[n].maxLevel; }
// 末列（锋芒 / 凝神 / 易筋，原生要轮回次数）：1.1.1 用户 2026-11-07——每通过 15 层得 1 个轮回点，解锁一个末列能力，
// 前提是同一行前面的节点全部点满；15 / 30 / 45 层各 1 个，45 层三个全开。解锁后照常 1 修为点 1 级（塔内上限 kStatNodeCap）
inline constexpr int kCycleFloors = 15;
inline bool IsFinalNode(int n) { return kNodes[n].line <= 2 && IsStatNode(n); }
inline int CyclePoints(int clearedFloors) { return std::max(0, clearedFloors) / kCycleFloors; }
// 3.0.0（用户 2026-11-12）：无尽层（内部 51 层 = 显示 100 起）每 15 层再给 1 个轮回点，用来让末列能力越过塔内上限 20 继续升级
// （每点 +1 级，照常花 1 修为点，到原生上限 30 为止；三个末列合计最多 +30 级）
inline int EndlessCycles(int bossStage) { return std::max(0, bossStage - 50) / kCycleFloors; }
struct CycleProgress { int current, next, remaining, available; };
inline CycleProgress GetCycleProgress(int bossStage, int finalsUnlocked, int finalExtra = 0) {
    const int current = std::max(0, bossStage);
    const int unlocked = std::clamp(finalsUnlocked, 0, 3);
    if (current >= 50) {  // 无尽：下一个轮回点在第 50 + 15k 层
        const int got = EndlessCycles(current), next = 50 + (got + 1) * kCycleFloors;
        return {current, next, next - current, std::max(0, std::min(3 - unlocked, CyclePoints(current) - unlocked)) + std::max(0, got - finalExtra)};
    }
    const int next = current < 3 * kCycleFloors ? (current / kCycleFloors + 1) * kCycleFloors : 0;
    return {current, next, next ? next - current : 0, std::min(3 - unlocked, std::max(0, CyclePoints(current) - unlocked))};
}
inline int NodeCost(int n) {
    (void)n;
    return 1;  // 用户 2026-11-05：一个修为点升一级能力，不分线路 / 列
}
inline int AllNodesCost() {
    int s = 0;
    for (int i = 0; i < kNodeCount; ++i) s += NodeCost(i) * NodeCap(i);
    return s;
}

// ---------------------------------------------------------------- 每层安排
enum class Floor { Normal, Duo, Trio, TwoPhase, Gauntlet, Group, RuleTrueBlade, RuleBulletTime, RuleNoPotion, RuleExecuteOnly, RuleMix, Timed, Affix, AffixDouble, ZoneBoss, Final };
inline std::string_view FloorName(Floor f) {
    switch (f) {
        case Floor::Normal: return "1v1";
        case Floor::Duo: return "1v2";
        case Floor::Trio: return "1v3";
        case Floor::TwoPhase: return "两阶段";
        case Floor::Gauntlet: return "车轮战";
        case Floor::Group: return "成组";
        case Floor::RuleTrueBlade: return "规则·真剑";
        case Floor::RuleBulletTime: return "规则·子弹时间";
        case Floor::RuleNoPotion: return "规则·禁药";
        case Floor::RuleExecuteOnly: return "规则·只能处决";
        case Floor::RuleMix: return "规则·三选一";
        case Floor::Timed: return "限时";
        case Floor::Affix: return "词缀";
        case Floor::AffixDouble: return "双词缀";
        case Floor::ZoneBoss: return "区首领";
        case Floor::Final: return "塔主";
    }
    return "?";
}
struct FloorPlan {
    Floor kind = Floor::Normal;
    int temper = 0, cultivate = 0;  // 本层通过给的淬炼 / 修为点
    int medTier = 0, medChoices = 0;  // 新药：档位（1–3，4 = 长生饮 + 三档）与可选数量；0 无
    int medLevel = 0;               // 药瓶升到几级（0 不变）
    bool accessoryBox = false;      // 饰品匣（3 选 1）
    bool refineBox = false;         // 饰品精炼匣
    bool soulToken = false;         // 铸魂令
    int jadeMul = 1;                // 魂玉倍数（1v3 ×2）
    bool hard() const { return kind != Floor::Normal && kind != Floor::ZoneBoss && kind != Floor::Final; }
};
// 50 层编排（方案第 6 节；团战 / 观看属于 M2，这里排的是 M1 的替代层型）。
// bosses：本层实际打的 Boss 数（-1 = 按编排预览）。编排是多 Boss 层、实际只打了一只（正式池拒绝独立 Boss 拼组后常见）时
// 按普通层给固定收获（7c2668e 实测：第二层单铁面仍按旧 Duo 给各 +3，Codex 2026-11-01）
inline FloorPlan PlanFor(int floor, int floors, int bosses = -1) {
    FloorPlan p;
    if (floors < 10) {  // 短局（试玩）：第 2 层 1v2，最后一层当区首领
        p.kind = floor == floors ? Floor::ZoneBoss : floors >= 3 && floor == 2 ? Floor::Duo : Floor::Normal;
    } else {
        // v14：层型按实际逐层方案（floors.generated.h），不再按旧的"每区第 3 / 5 / 7 / 8 / 9 层"模板
        const floorplan::Floor* def = nullptr;
        for (auto& f : floorplan::kFloors) if (f.floor == floor) def = &f;
        if (def) {
            switch (def->mode) {
                case floorplan::Mode::Duo: p.kind = Floor::Duo; break;
                case floorplan::Mode::Trio: p.kind = Floor::Trio; break;
                case floorplan::Mode::Relay: p.kind = Floor::Gauntlet; break;
                case floorplan::Mode::Brawl: p.kind = Floor::Group; break;
                case floorplan::Mode::Replay: p.kind = Floor::TwoPhase; break;
                case floorplan::Mode::Zone: case floorplan::Mode::ZoneRelay: p.kind = Floor::ZoneBoss; break;
                case floorplan::Mode::Final: p.kind = Floor::Final; break;
                default: p.kind = Floor::Normal;
            }
        } else if (floor > 50) p.kind = Floor::Trio;  // 第 51 层（隐藏 1v3）
        if (floor % 10 == 0 && floor < 50 && p.kind == Floor::Normal) p.kind = Floor::ZoneBoss;  // 区末层按区首领给
    }
    if (bosses >= 0 && bosses <= 1 && (p.kind == Floor::Duo || p.kind == Floor::Trio)) p.kind = Floor::Normal;
    if (bosses == 2 && p.kind == Floor::Trio) p.kind = Floor::Duo;
    // 点数（v14 复核）：普通 2、难层（重放两阶段 / 车轮 / 双人）3、区首领 / 塔主 4；第 36 层起减半（1 / 2），第 51 层起不给
    // v15（用户 2026-11-05）：每层通过固定 1 淬炼 + 1 修为（含第 51 层）；更多点数从奖励里选
    // 用户 2026-11-06：每层固定只给 1 个修为点，不给淬炼点（淬炼点只从奖励来；武器能免费回退，一轮约升满一个半武器）
    // 1.3.0（用户 2026-11-07：淬炼点全靠奖励刷，刷不出来武器就升不了）：每通过 3 层固定 +1 淬炼点（第 3 / 6 / 9 … 层），修为点每层 1 个不变
    // 2.0.0（用户 2026-11-08：100 层每层都给，成长堆得太快）：正式局每层不再固定给点数；点数来自 Boss 后的奖励页和驿站（短局不变）
    p.temper = 0; p.cultivate = floors >= 10 ? 0 : 1;
    if (floors < 10) return p;  // 短局不排固定奖励（开局送试玩套装，见 TrialKit）
    switch (floor) {  // 固定奖励
        case 5: p.medTier = 1; p.medChoices = 2; break;
        case 10: p.medLevel = 2; break;  // v15：铸魂令不再按层发，只从奖励来（SoulFloor）
        case 15: p.medTier = 2; p.medChoices = 3; break;
        case 20: p.medLevel = 3; break;
        case 25: p.medTier = 3; p.medChoices = 3; p.medLevel = 4; break;
        case 30: p.medLevel = 5; break;
        case 35: p.medTier = 4; p.medChoices = 2; p.medLevel = 6; break;
        case 40: p.refineBox = true; break;
        default: break;
    }
    // v3（用户 2026-11-12）：新药、饰品匣、饰品精炼匣不再免费发，全部进驿站随机上架；药瓶等级仍按层自动升
    p.medTier = 0; p.medChoices = 0; p.refineBox = false; p.accessoryBox = false;
    if (p.kind == Floor::Trio) p.jadeMul = 2;
    return p;
}

// ---------------------------------------------------------------- 塔档
enum class Err { Ok, NoPoints, AtCap, Missing, Locked, NoToken, Already, Bad, NoCycle, RowNotFull };
inline std::string_view ErrText(Err e) {
    switch (e) {
        case Err::Ok: return "";
        case Err::NoPoints: return "点数不足";
        case Err::AtCap: return "已到上限";
        case Err::Missing: return "塔里没有这件";
        case Err::Locked: return "前置节点还没点亮";
        case Err::NoToken: return "没有铸魂令";
        case Err::Already: return "已经铸魂";
        case Err::Bad: return "无效操作";
        case Err::NoCycle: return "需要轮回点：每通过 15 个 Boss 得 1 个，解锁最后一列的一个能力";
        case Err::RowNotFull: return "要先把这一行前面的能力全部点满";
    }
    return "?";
}

// 3.4.0（用户：淬炼大师 + 回收能不能刷点）：spent = 这把实际花进去的淬炼点；回收 / 熔铸 / 回退按它退。-1 = 旧档没记（按等级倒推）
struct OwnedWeapon { int index; int level; bool snapped; int spent = 0; };
// refine = 原生 PBZAccessoryInfo.CurrentLevel + 1（刚获得 = 1）；原生上限 GetAccessoryMaxRefineLevel（基础打磨上限）
struct OwnedAccessory { int index; int refine; };
// 用户 2026-11-11：饰品升级花淬炼点（不再用原生材料 / 尘）。每次打磨的点数，待用户定稿
inline constexpr int kAccessoryRefineTemper = 3;
inline constexpr int kAccessoryRefineSane = 10;  // 读档防坏档的宽松上限（原生打磨上限远低于此）

struct Profile {
    int temper = 0, cultivate = 0;            // 未花的点数
    int temperEarned = 0, cultivateEarned = 0;  // 累计获得（花掉 + 未花 = 累计，测试用作守恒检查）
    int soulTokens = 0;
    int medLevel = 1;
    std::vector<OwnedWeapon> weapons;
    std::array<int, kNodeCount> nodes{};       // 每个节点的等级
    std::vector<int> medicines;               // kMedicines 下标
    std::vector<OwnedAccessory> accessories;
    // 3.1.0 驿站【交易】（用户 2026-11-13）：熔铸记录。banned = 被熔掉的兵器，从本局武器池永久移除（不再上架 / 掉落 / 进三选一）；
    // melt = 被熔铸强化的兵器 → 次数（每次攻击 / 破防 +15%，最多 10 次）；meltTotal = 本局累计熔掉几把（每 15 把送 1 个铸魂令）
    std::vector<int> banned;
    std::vector<std::pair<int, int>> melt;
    int meltTotal = 0;
    // 3.1.0 淬炼大师（不存档，引擎 / Runner 按是否持有海克斯每次设置）：1 个淬炼点升 2 级，回退按一半返还（防止升了再退刷点）
    bool temperMaster = false;
    int LevelCost(int levels) const { return temperMaster ? (levels + 1) / 2 : levels; }
    int LevelRefund(int levels) const { return temperMaster ? levels / 2 : levels; }
    bool Banned(int weapon) const { return std::find(banned.begin(), banned.end(), weapon) != banned.end(); }
    int MeltCount(int weapon) const { for (auto& [w, n] : melt) if (w == weapon) return n; return 0; }

    // 开局：黑伤 + 选定的影之武装，都是 1 级；药葫芦 1 级；能力全关
    static Profile Start(int shaqiChoice) {
        Profile p;
        p.AddWeapon(WeaponIndex(kStarterMain, 0));
        p.AddWeapon(WeaponIndex(kStarterShaqi[std::clamp(shaqiChoice, 0, 2)], 1));
        p.medicines.push_back(MedicineIndex(kStarterMedicine));
        return p;
    }
    // 新局起手：3 主（默认装备前 2 把）+ 羽弓和巨凶斧 + 2 药，都 1 级、能力全关。
    // 其余 Boss 兵器靠击杀，非 Boss 装备 / 道具 / 药靠奖励或驿站；生命 / 杀气上限回到原游戏初值（tower_save.h 清永久加成道具）
    static Profile Kit() {
        Profile p;
        for (auto id : kStarterKitMain) p.AddWeapon(WeaponIndex(id, 0));
        for (auto id : kStarterKitShaqi) p.AddWeapon(WeaponIndex(id, 1));
        for (auto id : kStarterKitMeds) p.AddMedicine(MedicineIndex(id));
        return p;
    }
    // 试玩（3 层）：送一些点和药，几层里就能看到兵器特效、能力和药
    void TrialKit() {
        Earn(12, 10);
        medLevel = 2;
        AddMedicine(MedicineIndex(kMedTier1[0]));  // 已有就不重复
    }

    void Earn(int t, int c) { temper += t; cultivate += c; temperEarned += t; cultivateEarned += c; }

    // ---- 兵器
    int Find(int weapon) const {
        for (size_t i = 0; i < weapons.size(); ++i) if (weapons[i].index == weapon) return static_cast<int>(i);
        return -1;
    }
    bool AddWeapon(int weapon) {  // 新兵器 1 级；已有就不重复
        if (weapon < 0 || weapon >= kWeaponCount || Find(weapon) >= 0 || Banned(weapon)) return false;  // 熔掉的不再获得
        weapons.push_back({weapon, 1, false, 0});
        return true;
    }
    Err Upgrade(int weapon, int limit = kWeaponCap) {  // 1 淬炼点 = +1 级
        const int s = Find(weapon);
        if (s < 0) return Err::Missing;
        if (weapons[s].level >= std::min(std::clamp(limit, kWeaponCap, 60), kWeapons[weapon].maxLevel)) return Err::AtCap;
        if (temper < 1) return Err::NoPoints;
        const int cap = std::min(std::clamp(limit, kWeaponCap, 60), kWeapons[weapon].maxLevel);
        --temper; ++weapons[s].spent; weapons[s].level = std::min(cap, weapons[s].level + (temperMaster ? 2 : 1));
        return Err::Ok;
    }
    Err Downgrade(int weapon, int toLevel) {  // 回退：点数全额返还
        const int s = Find(weapon);
        if (s < 0) return Err::Missing;
        if (toLevel < 1 || toLevel >= weapons[s].level) return Err::Bad;
        auto& w = weapons[s];
        if (w.spent >= 0) {  // 3.4.0：按实际花的点数等比例退（向下取整），剩下的留在这把上
            const int back = w.level > 1 ? w.spent * (w.level - toLevel) / (w.level - 1) : 0;
            temper += back; w.spent -= back;
        } else temper += LevelRefund(w.level - toLevel);
        w.level = toLevel;
        return Err::Ok;
    }
    Err Snap(int weapon) {
        const int s = Find(weapon);
        if (s < 0) return Err::Missing;
        if (!kWeapons[weapon].snapSoul) return Err::Bad;
        if (weapons[s].snapped) return Err::Already;
        if (soulTokens < 1) return Err::NoToken;
        --soulTokens; weapons[s].snapped = true;
        return Err::Ok;
    }
    int TemperSpent() const { int s = 0; for (auto& w : weapons) s += w.level - 1; return s; }

    // ---- 能力修行
    // 前置：三条线按列从左到右；榕树之心要两个前置都亮（表里没写前置的榕树节点不设限）
    bool Unlocked(int n) const {
        const auto& d = kNodes[n];
        if (d.line >= 3) {
            for (int p : d.parents) if (p >= 0 && nodes[p] < 1) return false;
            return true;
        }
        for (int i = 0; i < kNodeCount; ++i)
            if (kNodes[i].line == d.line && kNodes[i].column < d.column && nodes[i] < 1) return false;
        return true;
    }
    int FinalsUnlocked() const { int c = 0; for (int i = 0; i < kNodeCount; ++i) c += IsFinalNode(i) && nodes[i] > 0; return c; }
    // 末列越过塔内上限的级数合计（无尽轮回点已用掉的）
    int FinalExtra() const { int c = 0; for (int i = 0; i < kNodeCount; ++i) if (IsFinalNode(i)) c += std::max(0, nodes[i] - NodeCap(i)); return c; }
    // 当前可升到的上限：末列在还有无尽轮回点时可再 +1（到原生上限为止）
    int CapFor(int n, int bossStage) const {
        if (n < 0 || n >= kNodeCount) return 0;
        if (!IsFinalNode(n)) return NodeCap(n);
        const bool spare = EndlessCycles(bossStage) > FinalExtra();
        return std::min(kNodes[n].maxLevel, std::max(NodeCap(n), nodes[n] + (spare ? 1 : 0)));
    }
    bool RowFull(int n) const {
        for (int i = 0; i < kNodeCount; ++i)
            if (i != n && kNodes[i].line == kNodes[n].line && !IsFinalNode(i) && nodes[i] < NodeCap(i)) return false;
        return true;
    }
    // 还能不能新解锁一个末列能力（已解锁的末列继续升级不受轮回点限制）
    bool CycleLeft(int clearedFloors) const { return FinalsUnlocked() < CyclePoints(clearedFloors); }
    Err Activate(int n, int clearedFloors = 50) {  // 默认：常规 50 层全部轮回点、没有无尽轮回点
        if (n < 0 || n >= kNodeCount) return Err::Bad;
        if (nodes[n] >= CapFor(n, clearedFloors)) return Err::AtCap;
        if (!Unlocked(n)) return Err::Locked;
        if (IsFinalNode(n) && nodes[n] == 0) {
            if (!RowFull(n)) return Err::RowNotFull;
            if (!CycleLeft(clearedFloors)) return Err::NoCycle;
        }
        if (cultivate < NodeCost(n)) return Err::NoPoints;
        cultivate -= NodeCost(n); ++nodes[n];
        return Err::Ok;
    }
    void ResetNodes() {  // 洗点：全部返还
        for (int i = 0; i < kNodeCount; ++i) { cultivate += NodeCost(i) * nodes[i]; nodes[i] = 0; }
    }
    int CultivateSpent() const { int s = 0; for (int i = 0; i < kNodeCount; ++i) s += NodeCost(i) * nodes[i]; return s; }

    // ---- 药与饰品
    bool HasMedicine(int m) const { return std::find(medicines.begin(), medicines.end(), m) != medicines.end(); }
    bool AddMedicine(int m) {
        if (m < 0 || m >= kMedicineCount || HasMedicine(m)) return false;
        medicines.push_back(m);
        return true;
    }
    int MedUses(int m) const { return kMedicines[m].uses[std::clamp(medLevel, 1, kMedLevelMax) - 1]; }
    // 饰品：没有就加入；已有且可精炼就精炼 +1；都不行返回 false
    bool AddAccessory(int a) {
        if (a < 0 || a >= kAccessoryCount) return false;
        for (auto& x : accessories)
            if (x.index == a) {
                if (x.refine >= kAccessories[a].refine) return false;
                ++x.refine;
                return true;
            }
        accessories.push_back({a, 1});
        return true;
    }

    // ---- 成长进度（界面与节奏测试用）
    int WeaponsMaxed() const { int n = 0; for (auto& w : weapons) n += w.level >= kWeaponCap; return n; }
    bool NodesMaxed() const { for (int i = 0; i < kNodeCount; ++i) if (nodes[i] < NodeCap(i)) return false; return true; }

    // ---- 存档（追加在 run.txt 里，每行 g.键=值）
    std::string Save() const {
        std::ostringstream o;
        o << "g.points=" << temper << ',' << cultivate << ',' << temperEarned << ',' << cultivateEarned << ',' << soulTokens << ',' << medLevel << '\n';
        o << "g.weapons=";
        for (size_t i = 0; i < weapons.size(); ++i) o << (i ? ";" : "") << kWeapons[weapons[i].index].id << ':' << kWeapons[weapons[i].index].kind << ':' << weapons[i].level << ':' << weapons[i].snapped << ':' << weapons[i].spent;
        o << "\ng.nodes=";
        bool first = true;
        for (int i = 0; i < kNodeCount; ++i) if (nodes[i]) { o << (first ? "" : ";") << kNodes[i].id << ':' << nodes[i]; first = false; }
        o << "\ng.medicines=";
        for (size_t i = 0; i < medicines.size(); ++i) o << (i ? ";" : "") << kMedicines[medicines[i]].id;
        o << "\ng.accessories=";
        for (size_t i = 0; i < accessories.size(); ++i) o << (i ? ";" : "") << kAccessories[accessories[i].index].id << ':' << accessories[i].refine;
        o << '\n';
        if (meltTotal || !banned.empty() || !melt.empty()) {  // 3.1.0 交易：旧档没有这两行 = 没熔铸过
            o << "g.melt=" << meltTotal;
            for (auto& [w, n] : melt) o << ';' << kWeapons[w].id << ':' << kWeapons[w].kind << ':' << n;
            o << "\ng.banned=";
            for (size_t i = 0; i < banned.size(); ++i) o << (i ? ";" : "") << kWeapons[banned[i]].id << ':' << kWeapons[banned[i]].kind;
            o << '\n';
        }
        return o.str();
    }
    // 按 ID 存，表更新后找不到的条目丢弃（不让整局读不出来）
    static bool Load(std::string_view text, Profile* out, int weaponLimit = kWeaponCap) {
        Profile p;
        bool any = false;
        auto split = [](std::string_view s, char c) {
            std::vector<std::string_view> v;
            size_t a = 0;
            while (a <= s.size()) { const size_t b = s.find(c, a); v.push_back(s.substr(a, b == std::string_view::npos ? std::string_view::npos : b - a)); if (b == std::string_view::npos) break; a = b + 1; }
            return v;
        };
        auto num = [](std::string_view s) { int v = 0; bool neg = !s.empty() && s[0] == '-'; for (char c : s.substr(neg)) { if (c < '0' || c > '9') return 0; v = v * 10 + (c - '0'); } return neg ? -v : v; };
        for (auto line : split(text, '\n')) {
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            const size_t eq = line.find('=');
            if (line.rfind("g.", 0) != 0 || eq == std::string_view::npos) continue;
            const auto k = line.substr(2, eq - 2), v = line.substr(eq + 1);
            any = true;
            if (k == "points") {
                auto f = split(v, ',');
                if (f.size() < 6) return false;
                p.temper = num(f[0]); p.cultivate = num(f[1]); p.temperEarned = num(f[2]); p.cultivateEarned = num(f[3]); p.soulTokens = num(f[4]); p.medLevel = std::clamp(num(f[5]), 1, kMedLevelMax);
            } else if (k == "weapons" && !v.empty()) {
                for (auto e : split(v, ';')) {
                    auto f = split(e, ':');
                    if (f.size() < 4) continue;
                    const int w = WeaponIndex(f[0], num(f[1]));
                    if (w >= 0 && p.Find(w) < 0) p.weapons.push_back({w, std::clamp(num(f[2]), 1, std::min(std::clamp(weaponLimit, kWeaponCap, 60), kWeapons[w].maxLevel)), f[3] == "1", f.size() >= 5 ? std::max(-1, num(f[4])) : -1});
                }
            } else if (k == "nodes" && !v.empty()) {
                for (auto e : split(v, ';')) {
                    auto f = split(e, ':');
                    if (f.size() < 2) continue;
                    if (const int n = NodeIndex(f[0]); n >= 0) p.nodes[n] = std::clamp(num(f[1]), 0, IsFinalNode(n) ? kNodes[n].maxLevel : NodeCap(n));  // 3.0.0：末列可超过 20（无尽轮回点）
                }
            } else if (k == "melt" && !v.empty()) {
                auto e = split(v, ';');
                p.meltTotal = std::max(0, num(e[0]));
                for (size_t i = 1; i < e.size(); ++i) {
                    auto f = split(e[i], ':');
                    if (f.size() < 3) continue;
                    const int w = WeaponIndex(f[0], num(f[1]));
                    if (w >= 0 && p.MeltCount(w) == 0) p.melt.push_back({w, std::clamp(num(f[2]), 1, 10)});
                }
            } else if (k == "banned" && !v.empty()) {
                for (auto e : split(v, ';')) {
                    auto f = split(e, ':');
                    if (f.size() < 2) continue;
                    const int w = WeaponIndex(f[0], num(f[1]));
                    if (w >= 0 && !p.Banned(w)) p.banned.push_back(w);
                }
            } else if (k == "medicines" && !v.empty()) {
                for (auto e : split(v, ';')) p.AddMedicine(MedicineIndex(e));
            } else if (k == "accessories" && !v.empty()) {
                for (auto e : split(v, ';')) {
                    auto f = split(e, ':');
                    if (const int a = f.empty() ? -1 : AccessoryIndex(f[0]); a >= 0) p.accessories.push_back({a, std::clamp(f.size() > 1 ? num(f[1]) : 1, 1, kAccessoryRefineSane)});  // 3.4.0（玩家：打磨后存档重进等级退回、点没退）：不再按静态表截断，上限由原生接口在打磨时检查
                }
            }
        }
        if (!any) return false;
        *out = p;
        return true;
    }
};

// ---------------------------------------------------------------- 祝福（原强化卡精简，方案 5.6）
enum class Bless { Temper, Iron, Life, Break, Gather, Swift, Spring, LastStand, Riposte, Feast };
struct BlessDef { Bless kind; std::string_view id, name, text; double perStack; int maxStack; bool ready; };
inline constexpr BlessDef kBlessings[] = {
    {Bless::Temper, "temper", "淬锋", "攻击 / 杀气攻击 +8%", 0.08, 3, true},
    {Bless::Iron, "iron", "铁骨", "受到伤害 -8%", 0.08, 3, true},
    {Bless::Life, "life", "厚生", "生命上限 +10%", 0.10, 3, true},
    {Bless::Break, "break", "破军", "削杀气 +15%", 0.15, 3, true},
    {Bless::Gather, "gather", "聚煞", "杀气恢复速度 +15%", 0.15, 3, true},  // 实际绑定 ShachiRegen（不是上限，也不是击中获取）
    {Bless::Swift, "swift", "疾风", "攻速 +5%", 0.05, 2, true},
    {Bless::Spring, "spring", "回春", "每层开打回 15% 生命（已退役，不再抽取）", 0.15, 2, false},
    {Bless::LastStand, "last_stand", "余勇", "生命低于 30% 时攻击 +20%", 0.20, 1, false},  // 第二批（需按血量切换）
    {Bless::Riposte, "riposte", "回锋", "完美格挡后 3 秒攻击 +15%", 0.15, 1, false},      // 第二批（需完美格挡事件）
    {Bless::Feast, "feast", "嗜战", "处决回 10% 生命", 0.10, 1, false},                   // 第二批（需处决事件）
};
inline constexpr int kBlessCount = static_cast<int>(std::size(kBlessings));
inline constexpr int kBlessTotalCap = 8;  // 一局最多 8 层

struct Blessings {
    std::array<int, kBlessCount> stacks{};
    int Total() const { int s = 0; for (int x : stacks) s += x; return s; }
    int Room(int b) const { return std::min(kBlessings[b].maxStack - stacks[b], kBlessTotalCap - Total()); }
    // 加 n 层（稀有度：凡 1 / 灵 2 / 玄 3），超出上限的部分丢弃；返回实际加了几层
    int Add(int b, int n) {
        if (b < 0 || b >= kBlessCount) return 0;
        const int add = std::clamp(n, 0, std::max(0, Room(b)));
        stacks[b] += add;
        return add;
    }
    double V(Bless k) const { return stacks[static_cast<int>(k)] * kBlessings[static_cast<int>(k)].perStack; }
    // 乘在原生值上的倍率（各自封顶）
    double AttackMul() const { return std::min(1.5, 1 + V(Bless::Temper)); }
    double TakenMul() const { return std::max(0.7, 1 - V(Bless::Iron)); }
    double MaxHpMul() const { return std::min(1.5, 1 + V(Bless::Life)); }
    double PostureMul() const { return std::min(1.6, 1 + V(Bless::Break)); }
    double ShachiMul() const { return std::min(1.6, 1 + V(Bless::Gather)); }
    double SpeedMul() const { return std::min(1.1, 1 + V(Bless::Swift)); }
    double HealOnFloor() const { return std::min(0.3, V(Bless::Spring)); }
    bool Any() const { return Total() > 0; }
};

// ---------------------------------------------------------------- 三选一（方案 5.2）
enum class Pick { TemperPack, CultivatePack, Accessory, MedicineCharge, Weapon, Item, Jade, Blessing };
struct PickOffer { Pick kind = Pick::Jade; int index = -1; int amount = 0; };  // index：饰品 / 兵器 / 祝福 / 道具下标；amount：点数 / 魂玉 / 祝福层数

struct Xs {  // xorshift64（与 tower::Rng 同算法，避免这里依赖 tower_logic.h）
    uint64_t s;
    uint64_t Next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    int Below(int n) { return n > 0 ? static_cast<int>(Next() % static_cast<uint64_t>(n)) : 0; }
};

// 稀有度（凡 / 灵 / 玄）按区抬升
inline int BlessRarity(Xs& r, int zone) {
    static constexpr int w[5][3] = {{75, 22, 3}, {62, 30, 8}, {50, 35, 15}, {40, 38, 22}, {30, 40, 30}};
    const int x = r.Below(100), z = std::clamp(zone, 0, 4);
    return x < w[z][0] ? 1 : x < w[z][0] + w[z][1] ? 2 : 3;
}
inline int DrawBlessing(Xs& r, const Blessings& have) {
    std::vector<int> c;
    for (int i = 0; i < kBlessCount; ++i) if (kBlessings[i].ready && have.Room(i) > 0) c.push_back(i);
    return c.empty() ? -1 : c[r.Below(static_cast<int>(c.size()))];
}
// 抽一件饰品：优先没有的；全有了就抽能精炼的；都不行返回 -1
// ok：额外准入（引擎传装备进度表合同；313987b 实测固定饰品匣绕过了合同）
template <class Ok>
inline int DrawAccessory(Xs& r, const Profile& p, Ok ok) {
    std::vector<int> fresh, refine;
    for (int i = 0; i < kAccessoryCount; ++i) {
        if (!AccessoryInPool(i) || !ok(i)) continue;
        auto it = std::find_if(p.accessories.begin(), p.accessories.end(), [i](const OwnedAccessory& a) { return a.index == i; });
        if (it == p.accessories.end()) fresh.push_back(i);
        else if (it->refine < kAccessories[i].refine) refine.push_back(i);
    }
    auto& from = fresh.empty() ? refine : fresh;
    return from.empty() ? -1 : from[r.Below(static_cast<int>(from.size()))];
}
inline int DrawAccessory(Xs& r, const Profile& p) { return DrawAccessory(r, p, [](int) { return true; }); }
// 非 Boss 兵器（低概率）：塔里还没有的主武器 / 影之武装
inline int DrawWeapon(Xs& r, const Profile& p) {
    std::vector<int> c;
    for (int i = 0; i < kWeaponCount; ++i) if (p.Find(i) < 0) c.push_back(i);
    return c.empty() ? -1 : c[r.Below(static_cast<int>(c.size()))];
}

inline std::array<PickOffer, 3> DrawPicks(Xs& r, int floor, int floors, const Profile& p, const Blessings& b, int itemCount, int jade) {
    std::array<PickOffer, 3> o{};
    const bool late = floor > kGrowthEndsAfter && floors >= 10;
    const int zone = floors < 10 ? 0 : std::clamp((floor - 1) / 10, 0, 4);
    auto blessing = [&](PickOffer& slot) {
        const int i = DrawBlessing(r, b);
        if (i < 0) return false;
        slot = {Pick::Blessing, i, BlessRarity(r, zone)};
        return true;
    };
    // 成长格
    if (!late) o[0] = {r.Below(2) ? Pick::CultivatePack : Pick::TemperPack, -1, 2};
    else if (!blessing(o[0])) o[0] = {Pick::Jade, -1, std::max(1, jade * 3)};
    // 装备格
    const int roll = r.Below(100);
    if (!late && roll < 10) { if (const int w = DrawWeapon(r, p); w >= 0) o[1] = {Pick::Weapon, w, 1}; }
    if (o[1].index < 0) {
        if (const int a = DrawAccessory(r, p); a >= 0 && (late || roll < 70)) o[1] = {Pick::Accessory, a, 1};
        else o[1] = {Pick::MedicineCharge, -1, 1};
    }
    // 道具格
    const int roll2 = r.Below(100);
    if (roll2 < 25 && blessing(o[2])) {}
    else if (roll2 < 70 && itemCount > 0) o[2] = {Pick::Item, r.Below(itemCount), 1};
    else o[2] = {Pick::Jade, -1, std::max(1, late ? jade * 2 : jade * 3 / 2)};
    return o;
}

// 新药选择：从该档里抽 n 个塔里还没有的（第 4 档 = 长生饮 + 三档）
template <class Ok>
inline std::vector<int> DrawMedicines(Xs& r, int tier, int n, const Profile& p, Ok ok) {
    std::vector<int> pool;
    auto add = [&](std::string_view id) { if (const int m = MedicineIndex(id); m >= 0 && !p.HasMedicine(m) && ok(m)) pool.push_back(m); };
    if (tier == 1) for (auto id : kMedTier1) add(id);
    if (tier == 2) for (auto id : kMedTier2) add(id);
    if (tier >= 3) for (auto id : kMedTier3) add(id);
    std::vector<int> out;
    if (tier == 4) if (const int m = MedicineIndex(kMedRare); m >= 0 && !p.HasMedicine(m) && ok(m)) { out.push_back(m); --n; }
    while (n-- > 0 && !pool.empty()) {
        const int k = r.Below(static_cast<int>(pool.size()));
        out.push_back(pool[k]);
        pool.erase(pool.begin() + k);
    }
    return out;
}
inline std::vector<int> DrawMedicines(Xs& r, int tier, int n, const Profile& p) { return DrawMedicines(r, tier, n, p, [](int) { return true; }); }
// 饰品匣：3 件互不相同
template <class Ok>
inline std::vector<int> DrawAccessoryBox(Xs& r, const Profile& p, Ok ok) {
    std::vector<int> out;
    Profile tmp = p;
    for (int k = 0; k < 3; ++k) {
        const int a = DrawAccessory(r, tmp, ok);
        if (a < 0) break;
        out.push_back(a);
        tmp.accessories.push_back({a, kAccessories[a].refine});  // 同一个匣里不重复
    }
    return out;
}
inline std::vector<int> DrawAccessoryBox(Xs& r, const Profile& p) { return DrawAccessoryBox(r, p, [](int) { return true; }); }

// ---------------------------------------------------------------- 平衡：玩家实际强度 → Boss 补偿
// 开层时读玩家实际攻击 / 生命（原生按兵器等级、能力、饰品算出来，再乘祝福），相对塔的基准算涨幅，补偿约八成 / 七成。
// 基准 = 塔档起点（黑伤 1 级、能力全关）下的实测值，由引擎在进塔后第一层记下。
inline double PowerHpComp(double attackNow, double attackBase) { return attackBase > 1 ? std::pow(std::max(1.0, attackNow / attackBase), 0.8) : 1.0; }
inline double PowerDmgComp(double hpNow, double hpBase, double takenMul) {
    return hpBase > 1 ? std::pow(std::max(1.0, hpNow / hpBase / std::max(0.2, takenMul)), 0.7) : 1.0;
}

}  // namespace tower::growth
