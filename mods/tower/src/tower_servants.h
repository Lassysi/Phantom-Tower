#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "tower_floorplay.h"
#include "../../../src/core/features/world/spawn_compat.h"

namespace tower::servants {
inline constexpr float kShare = 0.8f;       // 3.0.0（用户 2026-11-12）：仆从属性 = 玩家 80%（原 50%，5–6 层实机三只全阵亡）
inline constexpr float kCloneShare = 0.5f;  // 绝影「化影」分身仍为玩家 50%（用户 2026-11-12 单独定的口径）
// 仆从的生成标签前缀：不以 "tower-" 开头 → 核心按修改器「独立 Boss」路径处理（阶段替换、移除结束剧情、补原生死亡、回收尸体）
inline constexpr std::string_view kPrefix = "towerally-";

// 3.7.0（用户 2026-11-15）：左殇·真龙之躯直接生成也能正常打 → 敌方、友军助战、仆从大师都可以抽（只这一条，左殇前两段仍排除）
inline constexpr std::string_view kZuoShangP2 = "/PBZCombatAsset/ActorData/Mobs/076_ZuoShang_P2/BP_Monster_ZuoShang_P2.BP_Monster_ZuoShang_P2_C";
inline bool DirectSpawnOk(int index) { return kBossPool[index].path == kZuoShangP2; }
// 用户 2026-11-12：排除舞狮、大师兄、冷荼、梦华。另排除需要原生刷怪点 / 场地锚点 / 依赖的条目（离开原场地无法独立成立）。
// 结束剧情能力（如赤炼终阶段）由核心独立适配移除，不再排除。
inline bool Eligible(int index) {
    const auto& boss = kBossPool[index];
    if (DirectSpawnOk(index)) return true;
    if (boss.identity == "LionDancer" || boss.identity == "HuaShan_Blue" ||
        boss.identity == "LengTu" || boss.identity == "MengHua") return false;
    // 没有官方中文名的条目（池名带「暂译」，右侧友军看板会显示类名，用户 2026-11-12）
    if (boss.name.find("暂译") != std::string_view::npos) return false;
    return !floorplay::SpawnerFirst(index) && floorplay::NativeOnly(index).empty() && floorplay::DepsOf(index).empty();
}
// 核心实际会生成的路径（多阶段里不能独立成立的前段会换成终阶段）
inline std::string_view ActualPath(int index) { return pbz::enemies::StandalonePath(kBossPool[index].path, kPrefix); }

// 不需要核心独立适配的条目（不换阶段、没有锁血/剧情适配规则）：可以走影塔路径生成
inline bool PlainPath(int index) {
    const auto path = kBossPool[index].path;
    return ActualPath(index) == path && pbz::enemies::StandaloneRuleFor(path).effect.empty();
}

// Private floor seed: rerolls and rewards do not change the roster.
// 用户 2026-11-12：「翻来覆去都是这几个」——池里 74 条只有 54 个身份（金面具 4 条、玉琮 5 条…），按条目抽会偏向变体多的。
// 改为按身份等概率：每层先把身份洗牌，每个身份再随机挑一个变体；同一实际路径只算一条（雷荡两种前段都会生成终阶段）
template<class Allowed>
std::vector<int> Order(uint64_t seed, int floor, Allowed allowed) {
    std::vector<std::vector<int>> groups;  // 每个身份的可用变体
    std::vector<std::string_view> ids, paths;
    for (int i = 0; i < static_cast<int>(std::size(kBossPool)); ++i) {
        if (!Eligible(i) || !allowed(i)) continue;
        const auto actual = ActualPath(i);
        if (std::find(paths.begin(), paths.end(), actual) != paths.end()) continue;
        paths.push_back(actual);
        const auto id = kBossPool[i].identity;
        const auto at = std::find(ids.begin(), ids.end(), id);
        if (at == ids.end()) { ids.push_back(id); groups.push_back({i}); }
        else groups[static_cast<size_t>(at - ids.begin())].push_back(i);
    }
    uint64_t rng = (seed ^ (uint64_t(floor) * 0x9E3779B97F4A7C15ull) ^ 0x79586C01F73193D1ull) | 1ull;
    auto next = [&] { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; };
    for (size_t n = groups.size(); n > 1; --n) std::swap(groups[n - 1], groups[next() % n]);
    std::vector<int> pool;
    for (const auto& g : groups) pool.push_back(g[next() % g.size()]);
    return pool;
}

// 3.2.1（用户）：100 层后随机层有 Boss 助战，不占仆从大师的名额（一层最多 2 个助战 + 3 个仆从）。
// 显示 100–150 层：约 1/3 的层 1 个，属性为玩家 20%；150 层以上：约 2/5 的层 1 或 2 个，属性 30%。只由局种子和层号决定（同种子同结果）
struct Assist { int count = 0; float share = 0.0f; };
inline constexpr float kAssistLow = 0.20f, kAssistHigh = 0.30f;
inline Assist AssistFor(uint64_t seed, int shown) {
    if (shown < 100) return {};
    uint64_t r = (seed ^ (static_cast<uint64_t>(shown) * 0xD1B54A32D192ED03ull) ^ 0xA5515A11C0FFEEull) | 1ull;
    for (int i = 0; i < 3; ++i) { r ^= r << 13; r ^= r >> 7; r ^= r << 17; }
    const int roll = static_cast<int>(r % 100);
    if (shown <= 150) return roll < 33 ? Assist{1, kAssistLow} : Assist{};
    if (roll >= 40) return {};
    return {((r >> 32) & 1) ? 2 : 1, kAssistHigh};
}

inline float PoolValue(float current, float oldCap, float newCap, bool first, bool health) {
    if (first) return health ? newCap : std::clamp(current, 0.0f, newCap);
    if (oldCap <= 0) return std::clamp(current, 0.0f, newCap);
    const float value = std::clamp(current / oldCap, 0.0f, 1.0f) * newCap;
    return health && current > 0.5f ? std::min(newCap, std::max(1.0f, value)) : value;
}
// 3.6.3：仆从生命只降不升。上一帧比例 last、这一帧 now（都是 0–1；负数 = 未知）。
// 单帧回升不超过 5% 视为自然回复放行；超过就按 last 还原（敌方转阶段 / 车轮换人时被原生回满）。返回应保持的比例
inline constexpr float kServantRegenStep = 0.05f;
inline float KeepFrac(float last, float now) {
    if (now < 0) return last;
    if (last < 0) return now;
    return now > last + kServantRegenStep ? last : now;
}
}  // namespace tower::servants
