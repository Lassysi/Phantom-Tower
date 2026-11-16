// 3.7.0 创意层（用户 2026-11-15 定稿）：显示第 50 层起，按局种子随机挑 8 个小兵段换成特殊层；无尽模式也出，概率低。
// 纯逻辑（离线测试）：哪一层是什么类型、阵容怎么配。引擎沿用无尽混编层（大场地、雾门边界、错峰入场、拉回），见 notes/tower-3.7.0-special-floors.md。
//   单挑 Duel        ×1：玩家 vs 1 Boss + 几只小怪
//   以少打多 Outnumbered ×1：玩家 + 友方小怪 vs 2 Boss + 几只小怪
//   混战 Melee       ×2：玩家 + 1 Boss 助战 vs 甲队 2 Boss（enemy）vs 乙队 2 Boss（enemy2），三方互打；两队都清完才过
//   团战 Team        ×2：玩家 + 2 Boss 助战 + 友方小怪 vs 3 Boss + 几只小怪
//   生存 Survival    ×2：玩家 + 1 Boss 助战 vs 5 只打不死的 Boss（攻击按本层），撑满时长立即清场过关；先出的 2:00，后出的 2:30
// 助战 Boss（随机 1–2 个）= 玩家 30% 属性，生存战 5%（仆从大师的仆从在生存战也 5%）；不占仆从大师名额（持有仆从大师时另外叠加）。只用不依赖场地的 Boss（无尽池，含左殇·真龙之躯）。
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace tower::special {

enum class Kind : uint8_t { None, Duel, Outnumbered, Melee, Team, Survival };

inline std::string_view Name(Kind k) {
    switch (k) {
        case Kind::Duel: return "单挑";
        case Kind::Outnumbered: return "以少打多";
        case Kind::Melee: return "混战";
        case Kind::Team: return "团战";
        case Kind::Survival: return "生存";
        default: return "";
    }
}

// ---- 可调数值（用户可改）
inline constexpr int kFirstShown = 50;               // 显示第 50 层起
inline constexpr int kMainCount = 8;                 // 主线 50–98 层的小兵段里挑 8 个
inline constexpr int kSurvivalFirstSec = 90, kSurvivalSecondSec = 120;  // 用户 2026-11-15：1:30 / 2:00
inline constexpr int kEndlessChancePct = 8;          // 无尽层：每层 8% 换成特殊层（不连续两层）
inline constexpr float kAllyShare = 0.30f;  // 助战 30%（生存战另算）
// 生存战友军（Boss 助战 + 仆从大师的仆从）：玩家 10%–15%，随层数增加（显示 50 层 10% → 98 层 15%，无尽 15%；用户 2026-11-15）
inline constexpr float kSurvivalAllyLow = 0.10f, kSurvivalAllyHigh = 0.15f;
inline float SurvivalAllyShare(int shown) {
    const float t = std::clamp((shown - 50) / 48.0f, 0.0f, 1.0f);
    return kSurvivalAllyLow + (kSurvivalAllyHigh - kSurvivalAllyLow) * t;
}
inline constexpr int kAllyMin = 1, kAllyMax = 2;    // 有助战的类型都随机 1–2 个
// 生存战（用户 2026-11-15 实机后）：敌方攻击为本层数值 ×2，几十秒打死助战 / 仆从，然后来追玩家
inline constexpr double kSurvivalEnemyAtk = 2.0;
// 4.0.0（用户）：创意层 2 只及以上 Boss 时，每只血量 = 本层单 Boss 血量 × 0.6（生存战另算）
// 4.1.0（用户 2026-11-16：创意层太弱）：创意层数值按它所在的 Boss 层算（并入成长曲线），2 只及以上时每只生命 / 攻击 ×0.85
inline constexpr double kMultiBossHp = 0.85, kMultiBossDmg = 0.85;
// 4.1.1（用户：混战里玩家可以偷伤害、不会被多个 Boss 针对，掉血太少）：混战每只 Boss 攻击 = 该层 Boss ×1.2（不打 0.85 折扣）
inline constexpr double kMeleeDmg = 1.2;
// 生存战敌方近乎无限生命（用户 2026-11-15 实机：本层血量只有 1043，按 60% 补满时一套连招直接打死）：血量目标 ×10000，掉 10% 立即补满
inline constexpr double kSurvivalEnemyHp = 10000.0;
inline constexpr float kSurvivalRefillAt = 0.90f;
// 实机（21:51）：敌方被打破防后遭处决直接死（与血量无关）→ 生存战敌方杀气掉 10% 即补满，不会破防；
// 友军 5% 属性仍几分钟只掉半管（敌人很少打它们、对友军伤害很低）→ 生存战友军受到伤害 ×20，靠挨打自然死，不定时
inline constexpr float kSurvivalAllyTaken = 6.0f;  // 实机：敌方每下打友军约 6–10 点（70 血）→ ×6 约 2–3 下
// 生存战的友军（Boss 助战、仆从大师的仆从）不能抽这些——它们原生打不死，会一直替玩家挡着
inline bool SurvivalAllyBanned(std::string_view identity) {
    return identity == "gold_mask_guard" || identity == "GuWanYue" || identity == "YanQianSui";  // 金面具黑衣人、女妖骨婉约、琴女烟千岁
}

struct Lineup {
    int enemyBosses = 0, enemy2Bosses = 0;           // 甲队（enemy）/ 乙队（enemy2，混战用）
    int enemyMobs = 0, allyBosses = 0, allyMobs = 0;  // allyBosses > 0 = 这一类有 Boss 助战（实际个数 AllyCount 随机 1–2）
    bool unkillable = false;                         // 生存：敌方 Boss 打不死
};
inline Lineup LineupOf(Kind k) {
    switch (k) {
        case Kind::Duel: return {1, 0, 3, 0, 0, false};
        case Kind::Outnumbered: return {2, 0, 3, 0, 3, false};
        case Kind::Melee: return {2, 2, 0, 1, 0, false};
        case Kind::Team: return {3, 0, 3, 2, 2, false};
        case Kind::Survival: return {4, 0, 0, 1, 0, true};  // 用户 2026-11-15：5 只 → 4 只
        default: return {};
    }
}

struct Floor { Kind kind = Kind::None; int survivalSec = 0; bool early = false; };  // early：4.1.0 前 50 层的创意层（属性减半、生存 1:00 低攻击）

namespace detail {
inline uint64_t Mix(uint64_t x) { x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 27; x *= 0x94D049BB133111EBull; x ^= x >> 31; return x; }
}
inline float AllyShare(Kind k, int shown) { return k == Kind::Survival ? SurvivalAllyShare(shown) : kAllyShare; }
inline int AllyCount(uint64_t seed, int floor, Kind k) {
    if (LineupOf(k).allyBosses <= 0) return 0;
    if (k == Kind::Melee) return 1;  // 用户 2026-11-15：混战 = 4 Boss + 1 个助战（持有仆从大师时另外叠加）
    return kAllyMin + static_cast<int>(detail::Mix(seed ^ static_cast<uint64_t>(floor) * 0x2545F4914F6CDD1Dull ^ 0xA11Eull) % (kAllyMax - kAllyMin + 1));
}

// 主线：F = 小兵段所在的 Boss 层号（小兵段显示 2F-2）。显示 ≥ 50 的段（F 26–50）里按种子挑 8 个，类型配额固定。
// 生存战按层号先后：靠前的 2:00、靠后的 2:30。
// 4.0.0（用户）：4.0 以前开的局读档时已过显示 50 层（如在 70、80 层）→ from = 还没打的第一个小兵段，
// 只在 from 之后剩下的小兵段里放，个数按剩余比例（8 × 剩余段数 / 25，四舍五入）；类型先各不相同、再补重复（混战 / 团战 / 生存）。
// from ≤ kFirstMainF（新局、或老局还没到 50 层）与 3.7.0 完全相同。
inline constexpr int kFirstMainF = kFirstShown / 2 + 1;  // 显示 2F-2 ≥ 50 → F ≥ 26
inline constexpr int kLastMainF = 50;
inline int ScaledCount(int from) {
    const int total = kLastMainF - kFirstMainF + 1;
    const int left = std::clamp(kLastMainF - std::max(from, kFirstMainF) + 1, 0, total);
    return (kMainCount * left * 2 + total) / (2 * total);
}
inline std::vector<std::pair<int, Kind>> MainPlan(uint64_t seed, int from = 1) {
    std::vector<int> cand;
    for (int F = 2; F <= kLastMainF; ++F) if (2 * F - 2 >= kFirstShown && F >= from) cand.push_back(F);
    uint64_t r = detail::Mix(seed ^ 0x5EC1A1F100D5ull);
    for (size_t i = cand.size(); i > 1; --i) { r = detail::Mix(r + i); std::swap(cand[i - 1], cand[r % i]); }
    static constexpr std::array<Kind, kMainCount> kQuota{Kind::Duel, Kind::Outnumbered, Kind::Melee, Kind::Melee, Kind::Team, Kind::Team, Kind::Survival, Kind::Survival};
    std::vector<Kind> kinds(kQuota.begin(), kQuota.end());
    if (from > kFirstMainF) {
        cand.resize(std::min<size_t>(cand.size(), static_cast<size_t>(ScaledCount(from))));
        std::array<Kind, 5> first{Kind::Duel, Kind::Outnumbered, Kind::Melee, Kind::Team, Kind::Survival};
        std::array<Kind, 3> second{Kind::Melee, Kind::Team, Kind::Survival};
        uint64_t q = detail::Mix(seed ^ 0x40C0DE5ull);
        for (size_t i = first.size(); i > 1; --i) { q = detail::Mix(q + i); std::swap(first[i - 1], first[q % i]); }
        for (size_t i = second.size(); i > 1; --i) { q = detail::Mix(q + i); std::swap(second[i - 1], second[q % i]); }
        kinds.assign(first.begin(), first.end());
        kinds.insert(kinds.end(), second.begin(), second.end());
    }
    cand.resize(std::min<size_t>(cand.size(), kMainCount));
    std::vector<std::pair<int, Kind>> out;
    for (size_t i = 0; i < cand.size(); ++i) out.push_back({cand[i], kinds[i]});
    std::sort(out.begin(), out.end());
    return out;
}
// 4.1.0（用户 2026-11-16）：前 50 层也有 5 个创意层——小兵段 F 3–25（显示 4–48 层）分成 5 段，每段按种子随机挑一层（散开），
// 5 种类型按种子打乱各出一次。生存战撑 1:00，敌方攻击大幅降低（不再 ×2，改 ×0.3）。
// 原"Boss 属性为该层 50%"已取消（用户 2026-11-16：太弱、没威胁）：与 50 层后一样按所在 Boss 层的成长曲线取值
inline constexpr int kEarlyFirstF = 3, kEarlyLastF = kFirstMainF - 1, kEarlyCount = 5;
inline constexpr int kEarlySurvivalSec = 60;
inline constexpr double kEarlySurvivalAtk = 0.3;
inline std::vector<std::pair<int, Kind>> EarlyPlan(uint64_t seed, int from = 1) {
    std::array<Kind, 5> kinds{Kind::Duel, Kind::Outnumbered, Kind::Melee, Kind::Team, Kind::Survival};
    uint64_t q = detail::Mix(seed ^ 0xEA51E5ull);
    for (size_t i = kinds.size(); i > 1; --i) { q = detail::Mix(q + i); std::swap(kinds[i - 1], kinds[q % i]); }
    std::vector<std::pair<int, Kind>> out;
    const int span = kEarlyLastF - kEarlyFirstF + 1;
    for (int b = 0; b < kEarlyCount; ++b) {
        const int lo = kEarlyFirstF + span * b / kEarlyCount, hi = kEarlyFirstF + span * (b + 1) / kEarlyCount - 1;
        q = detail::Mix(q + 0x5EEDull + static_cast<uint64_t>(b));
        const int F = lo + static_cast<int>(q % static_cast<uint64_t>(hi - lo + 1));
        if (F >= from) out.push_back({F, kinds[static_cast<size_t>(b)]});
    }
    return out;
}
inline Floor MainFloor(uint64_t seed, int F, int from = 1) {
    if (F < kFirstMainF) {  // 4.1.0 前 50 层
        for (const auto& [f, k] : EarlyPlan(seed, from))
            if (f == F) return {k, k == Kind::Survival ? kEarlySurvivalSec : 0, true};
        return {};
    }
    const auto plan = MainPlan(seed, from);
    int survivalSeen = 0;
    for (const auto& [f, k] : plan) {
        if (k == Kind::Survival) ++survivalSeen;
        if (f == F) return {k, k == Kind::Survival ? (survivalSeen == 1 ? kSurvivalFirstSec : kSurvivalSecondSec) : 0};
    }
    return {};
}
// 老局（没有 spfrom 字段）读档：按进度定 from。floor = 已过的 Boss 层；小兵段 F = floor + 1 打了一部分就从下一段算
inline int FromProgress(int floor, int mobFloor, int mobWave) {
    const int from = floor + 1 + (mobFloor == floor + 1 && mobWave > 0 ? 1 : 0);
    return from <= kFirstMainF ? 1 : from;
}
// 无尽：floor = 内部层号（> 50）。每层独立掷 8%，上一层已是特殊层则这一层不出；生存战时长按出现次数交替 2:00 / 2:30
inline Floor EndlessFloor(uint64_t seed, int floor) {
    auto roll = [&](int f) { return f > 50 && static_cast<int>(detail::Mix(seed ^ (static_cast<uint64_t>(f) * 0x9E3779B97F4A7C15ull) ^ 0xE5D1E55ull) % 100) < kEndlessChancePct; };
    if (!roll(floor) || roll(floor - 1)) return {};
    static constexpr std::array<Kind, 5> kKinds{Kind::Duel, Kind::Outnumbered, Kind::Melee, Kind::Team, Kind::Survival};
    const Kind k = kKinds[detail::Mix(seed ^ static_cast<uint64_t>(floor) * 0xD1B54A32D192ED03ull) % kKinds.size()];
    return {k, k == Kind::Survival ? ((floor % 2) ? kSurvivalSecondSec : kSurvivalFirstSec) : 0};
}

// 抽 Boss：pool 已是"不依赖场地"的候选（无尽池）；同一层身份不重复；solo(b) 的（只能单独出场）只在单 Boss 阵容里用
template<class Identity, class Solo>
std::vector<int> PickBosses(uint64_t seed, int floor, int n, const std::vector<int>& pool, Identity identity, Solo solo) {
    std::vector<int> order(pool);
    uint64_t r = detail::Mix(seed ^ static_cast<uint64_t>(floor) * 0xA0761D6478BD642Full ^ 0xB055ull);
    for (size_t i = order.size(); i > 1; --i) { r = detail::Mix(r + i); std::swap(order[i - 1], order[r % i]); }
    std::vector<int> out;
    for (int b : order) {
        if (static_cast<int>(out.size()) >= n) break;
        if (n > 1 && solo(b)) continue;
        if (std::any_of(out.begin(), out.end(), [&](int x) { return identity(x) == identity(b); })) continue;
        out.push_back(b);
    }
    return out;
}

// 计时器下方的小字（用户：混战、生存要写明玩法）
inline std::string_view HudNote(Kind k) {
    switch (k) {
        case Kind::Melee: return "混战";
        case Kind::Survival: return "生存时长";
        default: return "";
    }
}

}  // namespace tower::special
