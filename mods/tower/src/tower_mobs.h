// 小怪波次（1.4.0 用户 2026-11-08）：两个 Boss 层之间，在本区经典场地连打几轮小怪 / 精英，每轮结束属性三选一。
// 层号不变（第 N 层仍是 Boss），波次记在开第 N 层之前。纯规则，离线测试。
#pragma once
#include "tower_balance.h"
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

#include "mobs.generated.h"

namespace tower::mobs {

// 用户 2026-11-08：前期 1 波、中期 2 波、后期 3 波（floor = 接下来要打的 Boss 层），一共 99 波
inline int WaveCount(int floor) { return floor <= 15 ? 1 : floor <= 35 ? 2 : 3; }
inline bool HasStage(int floor, int floors) { return floors >= 50 && floor >= 2 && floor <= 50; }

struct WavePlan { int normal = 0, elite = 0, normalKinds = 0, eliteKinds = 0; };
// wave 从 0 数。最后一波带精英，前面的波只有小怪；Kinds = 这一波用几种怪（种数少，同一种怪隔得更远才再出现）
inline WavePlan Plan(int floor, int wave, int total) {
    if (wave + 1 >= total) {
        if (floor <= 15) return {4, 2, 2, 1};
        if (floor <= 35) return {3, 3, 1, 2};
        return {3, 4, 1, 2};
    }
    return {wave == 0 ? 6 : 8, 0, 3, 0};
}
// 波次层是给玩家成长爽的层，难度压在 Boss 上；但后期也不能一刀一只：
// 血量按原生随层上涨，统一下调 20%；精英再 ×1.3；简单 ×0.7
inline double HpTarget(double nativeHp, bool elite, int floor, bool easy) {
    return nativeHp * (1.0 + 0.08 * floor) * 0.8 * (elite ? 1.3 : 1.0) * (easy ? 0.7 : 1.0) * balance::MobHpGrow(balance::ShownOfWave(floor)) * balance::EarlyHpRelief(balance::ShownOfWave(floor));  // 3.0.0 分段 + 3.0.1 前期减负
}
inline double DmgMul(int floor, bool easy) {
    const int tier = floor <= 15 ? 0 : floor <= 35 ? 1 : 2;
    // 小怪与精英共用这个倍率，整段波次的攻击减半；Boss 使用独立的 FloorDmg 曲线。
    return 0.5 * (easy ? (tier == 0 ? 0.6 : tier == 1 ? 0.7 : 0.8) : (tier == 0 ? 1.0 : tier == 1 ? 1.3 : 1.6)) * balance::MobDmgGrow(balance::ShownOfWave(floor)) * balance::EarlyDmgRelief(balance::ShownOfWave(floor));  // 3.0.0 分段 + 3.0.1 前期减负
}
inline constexpr double kFarCm = 6000;          // 离场地中心超过 60 米的小怪：移除，算这只结束

// 怪物只有 91 种，第五区自己的小怪只有 3 种：后面的区把前面相邻区的怪一起放进牌堆
inline bool InNormalPool(int mobZone, int zone) {
    const int from = zone <= 2 ? zone : zone == 3 ? 2 : 3;
    return mobZone >= from && mobZone <= zone;
}
inline bool InElitePool(int mobZone, int zone) {
    const int from = zone == 2 ? 1 : zone == 5 ? 4 : zone;
    return mobZone >= from && mobZone <= zone;
}
// 本区在 (floor, wave) 之前已经发出去的牌数
inline void SlotsBefore(int floor, int wave, int zone, int* normal, int* elite) {
    *normal = *elite = 0;
    for (int f = std::max(2, (zone - 1) * 10 + 1); f <= floor; ++f) {
        const int total = WaveCount(f);
        for (int w = 0; w < total && (f < floor || w < wave); ++w) {
            const auto p = Plan(f, w, total);
            *normal += p.normalKinds; *elite += p.eliteKinds;
        }
    }
}

// 按种子洗一副本区的牌，从头往后发：一副发完才会重复，尽量每层每波的怪都不一样；不可用的（bad(i) 为真）不进牌堆
template <class Bad>
inline std::vector<int> Pick(uint64_t seed, int floor, int wave, int zone, const WavePlan& plan, Bad bad) {
    std::vector<int> normals, elites, out;
    const int n = static_cast<int>(std::size(kMobs));
    for (int i = 0; i < n; ++i) {
        if (bad(i)) continue;
        if (kMobs[i].elite ? InElitePool(kMobs[i].zone, zone) : InNormalPool(kMobs[i].zone, zone)) (kMobs[i].elite ? elites : normals).push_back(i);
    }
    uint64_t x = ((seed ^ (static_cast<uint64_t>(zone) * 0x51ED27ull)) * 0x9E3779B97F4A7C15ull) | 1;
    auto next = [&](size_t m) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return static_cast<size_t>(x % m); };
    auto deal = [&](std::vector<int>& deck, int used, int kinds, int count) {
        if (deck.empty() || count <= 0 || kinds <= 0) return;
        for (size_t i = deck.size(); i > 1; --i) std::swap(deck[i - 1], deck[next(i)]);
        const size_t k = std::min(deck.size(), static_cast<size_t>(kinds));
        for (int i = 0; i < count; ++i) out.push_back(deck[(static_cast<size_t>(used) + static_cast<size_t>(i) % k) % deck.size()]);
    };
    int usedNormal = 0, usedElite = 0;
    SlotsBefore(floor, wave, zone, &usedNormal, &usedElite);
    deal(normals, usedNormal, plan.normalKinds, plan.normal);
    deal(elites, usedElite, plan.eliteKinds, plan.elite);
    return out;
}

// 用户 2026-11-08 兜底：一只怪刷不出来就换一种同档（小怪 / 精英）、同区牌堆里的怪；没得换返回 -1
template <class Bad>
inline int Replacement(int failed, int zone, Bad bad) {
    const int n = static_cast<int>(std::size(kMobs));
    if (failed < 0 || failed >= n) return -1;
    const bool elite = kMobs[failed].elite;
    for (int k = 1; k < n; ++k) {
        const int i = (failed + k) % n;
        if (kMobs[i].elite != elite || bad(i)) continue;
        if (elite ? InElitePool(kMobs[i].zone, zone) : InNormalPool(kMobs[i].zone, zone)) return i;
    }
    return -1;
}

}  // namespace tower::mobs
