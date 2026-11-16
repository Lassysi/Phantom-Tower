// 无尽混编层（用户 2026-11-12）：一层最多 3 波，每波最多 3 只 Boss、少量小怪（多了卡），组合全随机。
// 纯逻辑：按种子生成波次表；引擎（tower.cpp）按表逐波生成，本波全部确认死亡后 2.5 秒刷下一波，最后一波打完才结算。
// Boss 只从可直接生成的条目里抽（排除吃场景 / 只能原生场地 / 舞狮 / 左殇 / 大师兄 / 魂 / 暂译，多阶段 Boss 按条目单刷某一阶段）。
#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <random>
#include <string_view>
#include <vector>

namespace tower::endless {

inline constexpr int kMaxWaves = 3, kMaxBosses = 3, kMaxMobs = 4;

struct Wave { std::vector<int> bosses, mobs; };

struct Pools {
    std::vector<int> bosses;                           // 可抽的 Boss 池序号
    std::function<std::string_view(int)> identity;     // 同一波内身份不重复
    int mobCount = 0;                                  // 小怪表大小（序号 0..mobCount-1）
    std::function<bool(int)> mobOk;                    // 小怪可用（未判坏）
    std::function<bool(int)> solo;                     // 只能单独出场的 Boss（圣母娘娘：冻结控制，同波别的 Boss 会一直打被控住的目标）
    int allies = 0;                                    // 本局仆从大师友军数（最多 3）：有友军时每波小怪减少，给友军留位置
    int maxBosses = 2;                                 // 每波 Boss 上限：显示第 200 层前 2（1v2），200 层起 3（用户 2026-11-12）
};

// 每波至少 1 只 Boss 或 2 只小怪；整层至少 1 只 Boss；第一波一定有 Boss（入场读秒、准入都按 Boss 走）
inline std::vector<Wave> Generate(uint64_t seed, int floor, const Pools& p) {
    std::vector<Wave> out;
    if (p.bosses.empty()) return out;
    std::mt19937_64 rng(seed * 0x9E3779B97F4A7C15ull + static_cast<uint64_t>(floor) * 0xBF58476D1CE4E5B9ull + 0xE1D0);
    auto roll = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    const int waves = roll(1, kMaxWaves);
    for (int w = 0; w < waves; ++w) {
        Wave wave;
        // 第一波 1–3 只 Boss；后面的波 0–3 只（0 只时至少 2 只小怪）
        const int cap = std::clamp(p.maxBosses, 1, kMaxBosses);
        const int nb = std::min<int>(w == 0 ? roll(1, cap) : roll(0, cap), static_cast<int>(p.bosses.size()));
        // 3.6.0 打包前（用户：雷当 / 长鸿出现明显偏多）：池里多版本 / 多阶段的 Boss 占好几条，按条目洗牌会让它们多出 2–5 倍。
        // 改成先按身份分组、组间等概率，再在组内随机挑一个版本（同仆从大师）
        std::vector<std::string_view> ids; std::vector<std::vector<int>> groups;
        for (int b : p.bosses) {
            const auto id = p.identity ? p.identity(b) : std::string_view();
            const auto at = std::find(ids.begin(), ids.end(), id);
            if (at == ids.end() || id.empty()) { ids.push_back(id); groups.push_back({b}); }
            else groups[static_cast<size_t>(at - ids.begin())].push_back(b);
        }
        std::shuffle(groups.begin(), groups.end(), rng);
        std::vector<int> pool;
        for (const auto& g : groups) pool.push_back(g[static_cast<size_t>(roll(0, static_cast<int>(g.size()) - 1))]);
        bool alone = false;
        for (int b : pool) {
            if (static_cast<int>(wave.bosses.size()) >= nb) break;
            const bool s = p.solo && p.solo(b);
            if (s && !wave.bosses.empty()) continue;  // 只能单独出场的不进已有 Boss 的波
            if (std::any_of(wave.bosses.begin(), wave.bosses.end(), [&](int x) { return p.identity(x) == p.identity(b); })) continue;
            wave.bosses.push_back(b);
            if (s) { alone = true; break; }
        }
        int nm = roll(0, kMaxMobs);
        if (wave.bosses.empty()) nm = std::max(nm, 2);
        if (wave.bosses.size() >= 3) nm = std::min(nm, 2);  // 3 Boss 波小怪更少，控制同屏数量
        if (p.allies > 0) nm = std::min(nm, wave.bosses.empty() ? 2 : std::max(0, 3 - static_cast<int>(wave.bosses.size())));  // 给友军留位置
        if (alone) nm = 0;
        for (int tries = 0; static_cast<int>(wave.mobs.size()) < nm && tries < 64 && p.mobCount > 0; ++tries) {
            const int m = roll(0, p.mobCount - 1);
            if (!p.mobOk || p.mobOk(m)) wave.mobs.push_back(m);
        }
        if (wave.bosses.empty() && wave.mobs.empty()) continue;
        out.push_back(std::move(wave));
    }
    if (out.empty() || out[0].bosses.empty()) return {};
    return out;
}

}  // namespace tower::endless
