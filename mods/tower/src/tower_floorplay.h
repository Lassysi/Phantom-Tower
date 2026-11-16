// 50 层方案的层型逻辑（纯逻辑，离线测试；方案 notes/tower-50-floors-plan.md）：
//   Resolve：把 floors.generated.h 的一层解析成 Boss 池下标（主选不可用时按备选替换；同时在场的层换完仍不许同身份）；
//   Relay：车轮 / 连战 / 塔主——前一只确认死亡 → 2 秒后刷下一只 → 1 秒出场保护；
//   Factions：混战三方阵营（同身份成组的同阵营）；
//   Challenges：附加挑战（用户 2026-11-01）——限时 / 无伤 / 禁药随机出现在某些层，没有惩罚，完成有额外奖励；
//               全部完成记下来，之后用来开启隐藏第 51 层（另做）。限时挑战层（SoloTimed）是另一回事：超时 Boss 狂暴。
#pragma once
#include "tower_balance.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "boss_pool.generated.h"
#include "floors.generated.h"

namespace tower::floorplay {

using floorplan::Mode;

inline bool Relay(Mode m) { return m == Mode::Relay || m == Mode::ZoneRelay || m == Mode::Final; }
// 团战 / 押注以后接入：接入前按 1v1 打 bosses[0]
inline bool Later(Mode m) { return m == Mode::Team || m == Mode::Bet; }
inline bool Together(Mode m) { return m == Mode::Duo || m == Mode::Brawl || m == Mode::Trio; }  // 同时在场
inline std::string_view ModeName(Mode m, int count) {
    switch (m) {
        case Mode::Solo: return "1v1";
        case Mode::SoloTimed: return "1v1 · 限时";
        case Mode::Duo: return "1v2 双雄";
        case Mode::Relay: return count >= 3 ? "车轮 ×3" : "车轮 ×2";
        case Mode::Brawl: return "混战";
        case Mode::Trio: return "1v3 错峰";
        case Mode::Zone: return "区首领";
        case Mode::ZoneRelay: return "区首领 · 连战";
        case Mode::Final: return "塔主 · 连战";
        case Mode::Team: return "团战 2v2（待接入，按 1v1）";
        case Mode::Bet: return "押注观战（待接入，按 1v1）";
        case Mode::Replay: return "原生遭遇重放";
    }
    return "?";
}

inline int PoolIndex(std::string_view path) {
    for (int i = 0; i < static_cast<int>(std::size(kBossPool)); ++i) if (kBossPool[i].path == path) return i;
    return -1;
}
// 缺必要战斗依赖的类（floors_plan.py DEPS）：返回原因，没有返回空。这些类运行时一律不可用（换备选），不退经典场地冒充通过
inline std::string_view DepsOf(int b) {
    if (b < 0) return {};
    for (auto& d : floorplan::kNeedsDeps) if (d.path == kBossPool[b].path) return d.why;
    return {};
}
// 生成失败分类（36 层：最后一次"游戏暂停中"被记成蛊鹤生成失败）：暂停 / 加载 / 忙是上下文问题，不记成 Boss 不可用
inline bool TransientSpawnError(std::string_view json) {
    if (json.find("没有足够可落地") != std::string_view::npos) return false;  // 落点问题另算（场地）
    for (std::string_view k : {"暂停", "加载", "忙", "paused", "loading", "busy"}) if (json.find(k) != std::string_view::npos) return true;
    return false;
}
// 只能由原生刷怪点刷出的 Boss（到了自己的原生场地、单 Boss 层：先 ClaimSeqSpawn，不直接生成）。
// 荆野带马（BP_Monster_Horse_JingYe）：直接生成时本体位置落在世界原点（30324ba 与 2026-11-05 第 7 层：血条在、人看不见）
// 荆野带马：塔直接生成的本体会每 3 秒左右瞬移到地图各处（1.0.1 用户 2026-11-07 实机：拉回 10 次仍飞走），只能原生刷怪点刷出
inline constexpr std::string_view kSpawnerFirst[] = {"/PBZCombatAsset/ActorData/Mobs/045_JingYe/BP_Monster_JingYe.BP_Monster_JingYe_C"};
inline bool SpawnerFirst(int b) {
    if (b < 0) return false;
    for (auto p : kSpawnerFirst) if (p == kBossPool[b].path) return true;
    return false;
}
// 实机落不了地的场地（以场地主人的池下标给出）：返回证据，没有返回空
inline std::string_view LandingBad(int b) {
    if (b < 0) return {};
    for (auto& d : floorplan::kLandingBad) if (d.path == kBossPool[b].path) return d.why;
    return {};
}
// 只能在原生场地打的类：返回原因，没有返回空（调用方在原生场地不可用时把它当不可用，见 tower.cpp Next）
inline std::string_view NativeOnly(int b) {
    if (b < 0) return {};
    for (auto& d : floorplan::kNativeOnly) if (d.path == kBossPool[b].path) return d.why;
    return {};
}
// 个别 Boss 的血量 / 伤害修正（floors_plan.py BOSS_ADJ）；没有修正返回 1 / 1
struct Adj { double hp = 1, dmg = 1; };
inline Adj BossAdj(int b) {
    if (b < 0) return {};
    for (auto& a : floorplan::kBossAdj) if (a.path == kBossPool[b].path) return {a.hp, a.dmg};
    return {};
}
// 本层这只 Boss 的固定配置（开层后不随玩家属性、读档、重试变化）
inline double FloorHp(const struct Resolved& r, int b);
inline double FloorDmg(const struct Resolved& r, int b);
inline const floorplan::Floor* Find(int floor) {
    for (auto& f : floorplan::kFloors) if (f.floor == floor) return &f;
    for (auto& f : floorplan::kHiddenFloors) if (f.floor == floor) return &f;  // 第 51 层（隐藏）
    return nullptr;
}

// 洗牌方案（Run::shuffleBosses 存档值）：0 固定顺序；1 旧方案（2.0.0：每十场一组）；2 新方案（用户 2026-11-12）
inline constexpr int kShuffleTens = 1, kShuffleWide = 2, kShuffleLate = 3, kShuffleLater = 4;
// 3.0.1 方案 3（玩家反馈：阿瑾 / 大师兄 / 火流星前期太夸张）：火流星（17）、阿瑾（20）和第二组的独眼弓客（21）、游龙（22）对调分组，
// 火流星 / 阿瑾 / 大师兄（28）不排在第 1–30 场（显示 61 层以前）。同批：碧海（11）、舞狮（19）也换到第二组、不早于第 31 场，
// 第一组由断肠（23）、祝原（24）补位。只给新局；已开的 2 号方案局顺序不变
inline constexpr int kLateGroupA[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 23, 12, 13, 14, 15, 16, 21, 18, 24, 22};
inline constexpr int kLateGroupB[] = {17, 20, 11, 19, 25, 26, 27, 28, 29, 44, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40};
// 3.4.3 方案 4（用户：祝原 / 游龙往后放）：游龙（22）、祝原（24）换到第二组且不早于第 31 场（显示 61 层后），
// 第二组的万钧（26）、玉璧（27）补进第一组。只给新局；已开的方案 2 / 3 局顺序不变
inline constexpr int kLaterGroupA[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 23, 12, 13, 14, 15, 16, 21, 18, 27, 26};
inline constexpr int kLaterGroupB[] = {17, 20, 11, 19, 25, 22, 24, 28, 29, 44, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40};
// 新方案：遭遇按 1–20 / 21–40 / 41–49 三组洗（第 50 场左殇固定），前期能碰到更多不同 Boss。
// 组内约束（遭遇编号 = 方案 floors 表的原层号）：
//   - 白赤练（8）、拙壁（9）：不排在第 1–10 场
//   - 烟千岁（25）、骨婉约（29）：不排在第 1–30 场
//   - "玉琮"三形态车轮（44，掉刺骨混沌）：调到第二组，并排在烟千岁、骨婉约之前（驿站才有机会刷出混沌武器）；
//     第二组原来的三张脸魔偶（30）换到第三组，三组仍是 20 / 20 / 9
inline constexpr int kWideGroupA[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
inline constexpr int kWideGroupB[] = {21, 22, 23, 24, 25, 26, 27, 28, 29, 44, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40};
inline constexpr int kWideGroupC[] = {41, 42, 43, 30, 45, 46, 47, 48, 49};
struct EarliestRule { int encounter, earliest; };
inline constexpr EarliestRule kEarliest[] = {{8, 11}, {9, 11}, {25, 31}, {29, 31}};
inline constexpr EarliestRule kEarliestLate[] = {{8, 11}, {9, 11}, {25, 31}, {29, 31}, {17, 31}, {20, 31}, {28, 31}, {11, 31}, {19, 31}};
inline constexpr EarliestRule kEarliestLater[] = {{8, 11}, {9, 11}, {25, 31}, {29, 31}, {17, 31}, {20, 31}, {28, 31}, {11, 31}, {19, 31}, {22, 31}, {24, 31}};
inline constexpr int kChaosEncounter = 44, kAfterChaos[] = {25, 29};
// mode：0 方案 2、1 方案 3、2 方案 4
inline int EarliestOf(int encounter, int late = 0) {
    if (late == 2) { for (auto r : kEarliestLater) if (r.encounter == encounter) return r.earliest; return 0; }
    if (late) { for (auto r : kEarliestLate) if (r.encounter == encounter) return r.earliest; return 0; }
    for (auto r : kEarliest) if (r.encounter == encounter) return r.earliest;
    return 0;
}
inline int WideEncounter(uint64_t seed, int floor, int late = 0) {
    const int* group; int count, first;
    if (floor <= 20) { group = late == 2 ? kLaterGroupA : late ? kLateGroupA : kWideGroupA; count = 20; first = 1; }
    else if (floor <= 40) { group = late == 2 ? kLaterGroupB : late ? kLateGroupB : kWideGroupB; count = 20; first = 21; }
    else { group = kWideGroupC; count = 9; first = 41; }
    std::array<int, 20> order{};
    for (int i = 0; i < count; ++i) order[i] = group[i];
    uint64_t state = (seed ^ (static_cast<uint64_t>(first) * 0xD1B54A32D192ED03ull) ^ 0x5EED2026ull) | 1ull;
    auto next = [&] { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; };
    for (int i = count - 1; i > 0; --i) std::swap(order[i], order[next() % static_cast<uint64_t>(i + 1)]);
    auto free = [&](int i) { return EarliestOf(order[i], late) == 0 && order[i] != kChaosEncounter; };
    // 1) 太早的受限遭遇：和一个够晚、不受限的遭遇对调（候选里随机挑，仍由种子决定）
    for (int i = 0; i < count; ++i) {
        const int need = EarliestOf(order[i], late);
        if (!need || first + i >= need) continue;
        int cand[20], n = 0;
        for (int j = 0; j < count; ++j) if (first + j >= need && free(j)) cand[n++] = j;
        if (n) std::swap(order[i], order[cand[next() % static_cast<uint64_t>(n)]]);
    }
    // 2) 混沌车轮要在烟千岁 / 骨婉约之前：排在它们后面就和更早的一个不受限遭遇对调
    int chaos = -1, gate = count;
    for (int i = 0; i < count; ++i) {
        if (order[i] == kChaosEncounter) chaos = i;
        for (int a : kAfterChaos) if (order[i] == a) gate = std::min(gate, i);
    }
    if (chaos >= 0 && chaos > gate) {
        int cand[20], n = 0;
        for (int j = 0; j < gate; ++j) if (free(j)) cand[n++] = j;
        if (n) std::swap(order[chaos], order[cand[next() % static_cast<uint64_t>(n)]]);
    }
    return order[floor - first];
}
// 新局每十场用独立的确定性洗牌。最后一组只洗 41–49；第 50 场和隐藏场固定。
// 返回原方案中的遭遇编号，供场地、入场、阶段和 Boss 一起移动。
inline int EncounterFloor(uint64_t seed, int floor, int shuffle) {
    if (!shuffle || floor < 1 || floor >= 50) return floor;
    if (shuffle == kShuffleWide || shuffle == kShuffleLate || shuffle == kShuffleLater) return WideEncounter(seed, floor, shuffle == kShuffleLater ? 2 : shuffle == kShuffleLate ? 1 : 0);
    const int first = ((floor - 1) / 10) * 10 + 1;
    const int count = first == 41 ? 9 : 10;
    std::array<int, 10> order{};
    for (int i = 0; i < count; ++i) order[i] = first + i;
    uint64_t state = (seed ^ (static_cast<uint64_t>(first) * 0x9E3779B97F4A7C15ull)) | 1ull;
    auto next = [&] { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; };
    for (int i = count - 1; i > 0; --i) std::swap(order[i], order[next() % static_cast<uint64_t>(i + 1)]);
    return order[floor - first];
}

// 遭遇的角色、场地和多阶段配置一起移动；血量和伤害曲线来自目的层，
// 再应用该遭遇自己的多人份额，避免把高层数值带到第一层。
// 3.0.0（用户 2026-11-12 "太难了不好"，notes/stress-3.0.0.md）：逐层表有三处断崖——内部第 16 层单只血量 ×2.07、第 26 层 ×1.65、第 46 层 ×1.41。
// 从第 1 层顺着走一遍：每层单只血量最多比上一层（平滑后）高 15%、伤害最多高 6%，超出的部分往后摊，之后的层自然追回原表。
// 只降不升（平滑值 ≤ 原表），按单只份额（车轮 / 多人层的份额照旧）
inline constexpr double kRampHpStep = 1.15, kRampDmgStep = 1.06;
inline double RampFactor(int floor, bool hp) {
    struct Table { std::array<double, 51> hp{}, dmg{}; };
    static const Table t = [] {
        Table out; double ph = 0, pd = 0;
        for (int f = 1; f <= 50; ++f) {
            const auto* d = Find(f);
            out.hp[f] = out.dmg[f] = 1.0;
            if (!d || d->hpShare <= 0 || d->dmgShare <= 0) continue;
            const double uh = d->hpTarget / d->hpShare, ud = d->dmgMul / d->dmgShare;
            const double sh = ph > 0 ? std::min(uh, ph * kRampHpStep) : uh, sd = pd > 0 ? std::min(ud, pd * kRampDmgStep) : ud;
            out.hp[f] = uh > 0 ? sh / uh : 1.0; out.dmg[f] = ud > 0 ? sd / ud : 1.0;
            ph = sh; pd = sd;
        }
        return out;
    }();
    if (floor < 1 || floor > 50) return 1.0;
    return hp ? t.hp[floor] : t.dmg[floor];
}
inline floorplan::Floor ScheduledFloor(uint64_t seed, int floor, int shuffle) {
    const auto* target = Find(floor);
    const auto* source = Find(EncounterFloor(seed, floor, shuffle));
    if (!target || !source) return {};
    auto out = *source;
    out.floor = target->floor;
    out.zone = target->zone;
    // A sequential opponent receives the whole solo budget; simultaneous
    // encounters retain their original shares and all encounter mechanics.
    out.hpShare = Relay(source->mode) ? 1.0 : source->hpShare;
    out.dmgShare = Relay(source->mode) ? 1.0 : source->dmgShare;
    if (target->hpShare > 0) out.hpTarget = target->hpTarget / target->hpShare * out.hpShare * RampFactor(target->floor, true);
    if (target->dmgShare > 0) out.dmgMul = target->dmgMul / target->dmgShare * out.dmgShare * RampFactor(target->floor, false);
    return out;
}

struct Resolved {
    bool ok = false;
    Mode mode = Mode::Solo;
    std::vector<int> bosses;  // 出场顺序
    int venue = -1;           // 场地用谁的原生场地（池下标）
    int zoneClassic = -1;     // 场地不可用时退回的本区经典场地（池下标）
    double hpShare = 1, dmgShare = 1;
    double hpTarget = 0, dmgMul = 1;  // v14 逐层配置表（单只目标血量 / 伤害倍率，已含层型份额）
    int par = 120;
    std::string why, swaps;   // 失败原因 / 用了哪些备选
};
// usable(i)：池下标 i 现在能用（准入、本局没生成失败）
inline Resolved Resolve(const floorplan::Floor& f, const std::function<bool(int)>& usable, int displayFloor = -1) {
    Resolved r;
    r.mode = f.mode; r.hpShare = f.hpShare; r.dmgShare = f.dmgShare; r.par = f.par; r.hpTarget = f.hpTarget; r.dmgMul = f.dmgMul;
    r.venue = PoolIndex(f.venue);
    if (f.zone >= 1 && f.zone <= 5) r.zoneClassic = PoolIndex(floorplan::kZoneClassic[f.zone - 1]);
    std::vector<int> alts;
    for (int k = 0; k < f.altN; ++k) if (const int a = PoolIndex(f.alts[k]); a >= 0) alts.push_back(a);
    const int count = Later(f.mode) ? 1 : f.count;
    for (int k = 0; k < count; ++k) {
        const int b = PoolIndex(f.bosses[k]);
        const auto deps = DepsOf(b);
        if (b >= 0 && deps.empty() && usable(b)) { r.bosses.push_back(b); continue; }
        int pick = -1;
        for (int a : alts) {
            if (!usable(a) || !DepsOf(a).empty()) continue;
            bool clash = false;
            for (int x : r.bosses) clash = clash || (Together(f.mode) && kBossPool[x].identity == kBossPool[a].identity) || x == a;
            if (!clash) { pick = a; break; }
        }
        const std::string_view name = b >= 0 ? kBossPool[b].name : std::string_view("（不在池里）");
        const std::string because = deps.empty() ? std::string() : std::format("（缺必要战斗依赖：{}；不算它通过）", deps);
        if (pick < 0) { r.why = std::format("第 {} 层的 {} 不可用{}，也没有能用的备选", displayFloor >= 0 ? displayFloor : f.floor, name, because); return r; }
        r.swaps += std::format("{}{}{} → {}", r.swaps.empty() ? "" : "；", name, because, kBossPool[pick].name);
        r.bosses.push_back(pick);
        alts.erase(std::find(alts.begin(), alts.end(), pick));
        if (k == 0 && f.venue == f.bosses[0]) r.venue = pick;  // 用的是主选自己的场地：跟着换成备选的
    }
    r.ok = !r.bosses.empty();
    return r;
}

inline double FloorHp(const Resolved& r, int b) { return std::max(1.0, r.hpTarget) * BossAdj(b).hp; }
inline double FloorDmg(const Resolved& r, int b) { return std::max(0.1, r.dmgMul) * BossAdj(b).dmg; }
// 4.1.1：第 F 层按单 Boss 算的目标（逐层表值已含层型份额，除回去）——主线创意层当作该层 Boss 层取数值
inline double SoloHp(int floor) { const auto* d = Find(floor); return d ? d->hpTarget / std::max(0.1, d->hpShare) : 0.0; }
inline double SoloDmg(int floor) { const auto* d = Find(floor); return d ? d->dmgMul / std::max(0.1, d->dmgShare) : 0.0; }

// 车轮：Begin 时把第一只之外的排队；Dead() 由引擎在"本轮全部确认死亡"时调用；到时 Due() 返回下一只
struct RelayQueue {
    static constexpr double kGap = 2.0, kGuard = 1.0;
    std::vector<int> queue;
    int index = 0, total = 0;
    double dueAt = -1, guardUntil = -1;
    void Begin(const std::vector<int>& bosses) {
        queue.assign(bosses.begin() + (bosses.empty() ? 0 : 1), bosses.end());
        total = static_cast<int>(bosses.size()); index = bosses.empty() ? 0 : 1; dueAt = guardUntil = -1;
    }
    void Clear() { queue.clear(); index = total = 0; dueAt = guardUntil = -1; }
    bool Pending() const { return !queue.empty(); }
    // 本轮全死：排下一只（返回 true = 还有下一只，不结算）
    bool Dead(double now) {
        if (queue.empty()) return false;
        if (dueAt < 0) dueAt = now + kGap;
        return true;
    }
    // 到时间：取出下一只，开 1 秒出场保护
    int Due(double now) {
        if (queue.empty() || dueAt < 0 || now < dueAt) return -1;
        const int b = queue.front();
        queue.erase(queue.begin());
        ++index;
        dueAt = -1;
        guardUntil = now + kGuard;
        return b;
    }
    bool Guarded(double now) const { return now < guardUntil; }
};

// Retrying a post-kill relay spawn must never affect staggered encounters and
// must stop if cleanup of the failed request could not be proven.
inline bool RetryRelaySpawn(bool followup, int attempts, bool cleaned) {
    return followup && cleaned && attempts >= 0 && attempts < 3;
}

// 混战阵营：enemy / enemy2 / chaos 轮流；与上一只同身份（金面双卫、断肠两版）的同阵营
inline std::vector<std::string> Factions(const std::vector<int>& bosses) {
    static constexpr const char* kSides[] = {"enemy", "enemy2", "chaos"};
    std::vector<std::string> out;
    int side = -1;
    for (size_t i = 0; i < bosses.size(); ++i) {
        const bool sameAsPrev = i > 0 && kBossPool[bosses[i]].identity == kBossPool[bosses[i - 1]].identity;
        if (!sameAsPrev) side = (side + 1) % 3;
        out.push_back(kSides[side]);
    }
    return out;
}

// ---- 挑战（v14，用户 2026-11-05）：每轮随机 7 个，各在不同层；开局按种子定下，读档 / 重试不重抽。
// 只放在单只 Boss、塔生成、3/2/1 倒计时开打的层（非原生遭遇重放、非车轮 / 多只同场）。
//   Berserk 限时挑战 ×5（1.0.1 用户 2026-11-06：原 2 个再加 3 个）：3 分钟内没打完，Boss 狂暴（攻击 ×2、杀气上限再 ×1.5、回复生命上限 1/3 一次；
//           只触发一次，不叠加）；无额外奖励
//   Timed   额外挑战·限时 ×2：5 分钟内击败 → 额外道具；超时无惩罚、不狂暴
//   NoHit   额外挑战·无伤 ×1：Boss 造成的实际扣血才算受伤（格挡仍扣血算受伤；武器技能主动耗血不算）
//   NoPotion 额外挑战·禁药 ×2：正式开战到 Boss 死亡不用药（回复药 / 增益药都算；MOD 符类、武器自带回血不算）
enum class Challenge : uint8_t { None, Timed, NoHit, NoPotion, Berserk };
inline constexpr int kChallengeSeconds = 300;  // 额外挑战·限时（奖励）
// ---- 难度（1.0.1 用户 2026-11-06：开局选"50 层（困难）/ 50 层（简单）"，只区分 Boss 数值，其他一样）----
// 用户 2026-11-07 调整（困难太难）：
// The generated table is the pre-rebalance hard baseline. Keep its per-boss
// adjustments and apply the following reversible difficulty factors at runtime.
enum class Difficulty : int { Hard = 0, Easy = 1 };
inline constexpr bool IsEasy(int diff) { return diff == static_cast<int>(Difficulty::Easy); }
inline std::string_view DifficultyName(int diff) { return IsEasy(diff) ? "简单" : "困难"; }
// 显示层号（用户 2026-11-08）：50 层的局按实际打的顺序连排：Boss 第 F 层显示 2F-1，它前面的波次段显示 2F-2，隐藏层显示 100。
// 内部层号、存档、奖励表、战绩日志仍然用 Boss 层（1–50、51）；短局不变
// 无尽模式（用户 2026-11-12）：第 50 层（显示 99，左殇）之后每层一个显示层：第 51 层 = 显示 100，第 52 层 = 101 …
inline int ShownBoss(int floor, int floors) { return floors < 50 || floor <= 0 ? floor : floor > 50 ? floor + 49 : 2 * floor - 1; }
inline int ShownMobs(int floor, int floors) { return floors < 50 ? floor : 2 * floor - 2; }
inline int ShownTotal(int floors) { return floors < 50 ? floors : 100; }
// 重生符（用户 2026-11-08）：按显示层号退 10 层，波次层也算（第 90 层倒下回第 80 层），最少回第 1 层。
// 退到偶数层 = 重打那一段波次；退到奇数层 = 直接打那层 Boss
inline int RebirthDest(int diedShown) { return diedShown > 11 ? diedShown - 10 : 1; }
inline int RebirthBack(int dest) { return dest / 2; }           // 目的层之前已通过的 Boss 层数
inline bool RebirthToMobs(int dest) { return dest % 2 == 0; }
inline double Blend30To40(int floor, double first, double last) {
    return first + (last - first) * std::clamp((floor - 30) / 10.0, 0.0, 1.0);
}
inline double LateGrowth(int shown) { return std::pow(1.1, std::clamp(shown - 70, 0, 30)); }
inline double BossHpScale(double nativeMax, double target) {
    return nativeMax > 1 && std::isfinite(target) ? std::clamp(target / nativeMax, 0.05, 10000.0) : 1.0;
}
using balance::EarlyHpRelief;  // 3.0.1 前期减负（tower_balance.h）
using balance::EarlyDmgRelief;
inline double DiffHp(int diff, int floor, int floors = 50) {
    const double oldEasy = floor <= 15 ? 2.5 / 5.0 : floor <= 25 ? 3.0 / 10.0 : floor <= 45 ? 4.0 / 16.0 : 4.5 / 22.0;
    if (floors < 50) return IsEasy(diff) ? oldEasy * Blend30To40(floor, 0.80, 1.10) : Blend30To40(floor, 0.75, 0.90);
    const int shown = ShownBoss(floor, floors);
    const double progress = std::clamp((shown - 35) / 35.0, 0.0, 1.0);
    // v3（用户 2026-11-12）：后期 1.1^(层−70) 换成 1.03^(层−70)，再乘难度增长 E(s)（前 30 层 ×1.15，30–60 层每层 +2%，60 层起每层 +3%）
    return (IsEasy(diff) ? oldEasy * (1.00 + 0.40 * progress) : 0.88 + 0.12 * progress) * balance::BossHpGrow(shown) * EarlyHpRelief(shown) * balance::BossHpShift(shown) * balance::MidHpLift(shown) * balance::kBossBase;  // 3.0.0 分段；3.4.3 整体平移；4.0.0 20 层起上抬；4.0.1 5 层起平滑、基数 ×0.8
}
inline double DiffDmg(int diff, int floor, int floors = 50) {
    if (floors < 50) {
        if (!IsEasy(diff)) return Blend30To40(floor, 0.80, 0.90);
        const double oldEasy = 0.8 / (floor <= 15 ? 2.5 : floor <= 35 ? 3.0 : 3.5);
        const double early = 1.35 + 0.10 * std::clamp((floor - 10) / 20.0, 0.0, 1.0);
        return oldEasy * (floor <= 30 ? early : Blend30To40(floor, 1.45, 1.50));
    }
    const int shown = ShownBoss(floor, floors);
    const bool easy = IsEasy(diff);
    const double grow = balance::BossDmgGrow(shown);
    if (shown <= 6) return (easy ? 0.22 : 0.32) * grow * EarlyDmgRelief(shown);
    if (shown <= 10) return (easy ? 0.28 : 0.42) * grow * EarlyDmgRelief(shown);
    if (shown <= 35) {
        const double p = (shown - 10) / 25.0;
        return (easy ? 0.28 + 0.16 * p : 0.42 + 0.38 * p) * grow * EarlyDmgRelief(shown);
    }
    const double p = std::clamp((shown - 35) / 35.0, 0.0, 1.0);
    return (easy ? 0.44 + 0.06 * p : 0.80 + 0.18 * p) * grow * EarlyDmgRelief(shown) * balance::BossDmgShift(shown);  // v3：35 层后不再持平，随 E(s) 上涨（3.0.0 分段）；3.5.0 50 层后 ×0.85
}
// 3.4.3（用户：雷当 / 长鸿 / 炎茹后期伤害太高）：按角色类名统一 ×0.8，固定层、各阶段（雷当 / 长鸿二阶段）、无尽层都生效；小怪段不受影响
inline double LateBossDmg(std::string_view actorClass) {
    if (actorClass.find("LeiDang") != std::string_view::npos || actorClass.find("ChangHong") != std::string_view::npos ||
        actorClass.find("QiShu_Huo") != std::string_view::npos) return 0.6;  // 3.5.0（用户：大幅降低）0.8 → 0.6
    return 1.0;
}
// These multiply the actor's damage attributes, not individual hit or skill data.
inline double EncounterDmg(int shownFloor, bool multiPhase, std::string_view actorClass) {
    double m = shownFloor > 0 && shownFloor <= 20 && multiPhase ? 0.75 : 1.0;
    // Actor-level fallback only: individual fireballs / grab must still be
    // verified in game, because some scripted damage can bypass attributes.
    if (actorClass == "BP_Monster_LionDancer_P2_C") m *= 0.225;
    if (actorClass == "BP_Monster_GuiChai_Red_P2_C") m *= 0.75;
    if (actorClass == "BP_Monster_PowerMan_C" || actorClass == "BP_Monster_PowerMan_P2_C") m *= 0.45;
    return m;
}
inline double PlayerCloneDmg(std::string_view actorClass, bool playerRegistered) {
    // Restore native clone damage; don't guess a player-attack scaling rule.
    (void)actorClass; (void)playerRegistered; return 1.0;
}
// Boss 杀气上限（困难：1–15 层 ×1.5，16 层起 ×2；护盾 Shield 不动）
inline double BossShachiMul(int floor, int diff = 0, int floors = 50) {
    const double base = IsEasy(diff) ? (floors < 50 ? 1.0 : 1.2) : floor <= 15 ? 1.5 : 2.0;
    if (floors < 50) return base;
    const int shown = ShownBoss(floor, floors);
    return base * balance::BossShachiGrow(shown) * balance::MidShachiLift(shown) * balance::kBossBase;  // 3.0.0 分段；4.0.1 杀气跟血量上抬（开方）
}
// 限时挑战（超时狂暴）
struct BerserkRule { int count, seconds; double attack, shachi, heal; };
inline constexpr BerserkRule kBerserkHard{5, 180, 2.0, 1.5, 1.0 / 3.0}, kBerserkEasy{2, 300, 1.5, 1.5, 0.25};
inline const BerserkRule& Berserking(int diff) { return IsEasy(diff) ? kBerserkEasy : kBerserkHard; }
inline constexpr int kBerserkSeconds = kBerserkHard.seconds;
inline int ChallengeLimit(Challenge c, int diff = 0) { return c == Challenge::Berserk ? Berserking(diff).seconds : kChallengeSeconds; }
inline std::string_view ChallengeName(Challenge c) {
    switch (c) {
        // 用户 2026-11-05：只写"禁药"会被当成本层不能用药 → 写全"额外挑战：禁药"；狂暴那种是有惩罚的限时挑战
        case Challenge::Berserk: return "限时挑战";
        case Challenge::Timed: return "额外挑战：限时";
        case Challenge::NoHit: return "额外挑战：无伤";
        case Challenge::NoPotion: return "额外挑战：禁药";
        default: return "";
    }
}
inline bool ChallengeFloor(int floor, uint64_t seed = 0, int shuffle = 0) {
    const auto* f = Find(EncounterFloor(seed, floor, shuffle));
    return f && f->count == 1 && floor >= 3 && f->replay.empty() && (f->mode == Mode::Solo || f->mode == Mode::SoloTimed || f->mode == Mode::Zone);
}
// 分配：限时挑战 5 层（优先第 11 层以后，前期 5 分钟没有意义）、额外限时 2、无伤 1、禁药 2，互不同层；同一种子结果固定
inline std::array<Challenge, 51> Challenges(uint64_t seed, int floors, int diff = 0, int shuffle = 0) {
    std::array<Challenge, 51> out{};
    if (floors < 10) return out;
    uint64_t s = (seed * 0x9E3779B97F4A7C15ull) ^ 0xC4A11E46E5ull;
    if (!s) s = 1;
    auto next = [&] { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; };
    std::vector<int> cand;
    for (int f = 1; f <= std::min(50, floors); ++f) if (ChallengeFloor(f, seed, shuffle)) cand.push_back(f);
    auto take = [&](Challenge c, int n, int minFloor) {
        for (int k = 0; k < n; ++k) {
            std::vector<size_t> ok;
            for (size_t i = 0; i < cand.size(); ++i) if (cand[i] >= minFloor) ok.push_back(i);
            if (ok.empty()) for (size_t i = 0; i < cand.size(); ++i) ok.push_back(i);
            if (ok.empty()) return;
            const size_t i = ok[next() % ok.size()];
            out[cand[i]] = c;
            cand.erase(cand.begin() + static_cast<long>(i));
        }
    };
    take(Challenge::Berserk, Berserking(diff).count, 11);
    take(Challenge::Timed, 2, 1);
    take(Challenge::NoHit, 1, 1);
    take(Challenge::NoPotion, 2, 1);
    return out;
}

// 本层战斗中实际发生的（引擎记录）
struct Fight { double seconds = 0; bool hit = false, potion = false; };
inline bool Done(Challenge c, const Fight& f, int diff = 0) {
    switch (c) {
        case Challenge::Berserk: case Challenge::Timed: return f.seconds > 0 && f.seconds <= ChallengeLimit(c, diff);
        case Challenge::NoHit: return !f.hit;
        case Challenge::NoPotion: return !f.potion;
        default: return false;
    }
}
inline bool Failed(Challenge c, const Fight& f, double seconds, int diff = 0) {
    switch (c) {
        case Challenge::Berserk: case Challenge::Timed: return seconds > ChallengeLimit(c, diff);
        case Challenge::NoHit: return f.hit;
        case Challenge::NoPotion: return f.potion;
        default: return false;
    }
}
// 额外挑战成功 → 额外道具（限时挑战本身没有额外奖励）
inline bool BonusItem(Challenge c, const Fight& f, int diff = 0) { return c != Challenge::Berserk && c != Challenge::None && Done(c, f, diff); }
// 限时挑战超时：Boss 狂暴（只看这一种挑战）
inline bool Berserk(Challenge c, double seconds, int diff = 0) { return c == Challenge::Berserk && seconds > Berserking(diff).seconds; }
// HUD 一行：名称 + 剩余时间 / 状态（简短）
inline std::string ChallengeHud(Challenge c, const Fight& f, double seconds, int diff = 0) {
    if (c == Challenge::None) return {};
    const std::string name(ChallengeName(c));
    if (c == Challenge::Berserk) {
        const int limit = Berserking(diff).seconds;
        if (seconds > limit) return name + "　超时，Boss 狂暴";
        const int left = std::max(0, limit - static_cast<int>(std::max(0.0, seconds)));
        return std::format("{} {}:{:02}", name, left / 60, left % 60);
    }
    if (Failed(c, f, seconds, diff)) return name + "　未达成";
    if (c == Challenge::Timed) {
        const int left = std::max(0, kChallengeSeconds - static_cast<int>(std::max(0.0, seconds)));
        return std::format("{} {}:{:02}", name, left / 60, left % 60);
    }
    return name;
}

inline std::string Clock(double seconds) {
    if (seconds < 0) return "--:--";
    const int t = static_cast<int>(seconds * 10);
    return std::format("{:02}:{:02}.{}", t / 600, (t / 10) % 60, t % 10);
}

}  // namespace tower::floorplay
