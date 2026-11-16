// 3.7.0 玩法类海克斯（用户 2026-11-15 选定第一批，定稿 6 个（绝技连发删除））：全部数值集中在这里，改数值只改本文件。
// 说明与数值表：notes/tower-3.7.0-playstyle-hexes.md
// 现状：只有数值与纯判定逻辑（离线可测）；海克斯编号已满 64（tower_hex.h Count == 64），接线待编号方案定下后再做。
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace tower::playstyle {

// ---- 用户可改的数值（下标 0/1 = Lv1/Lv2；用户 2026-11-15 定稿：全部最高 Lv2，影武无尽、绝技连发删除） ----
inline constexpr int kMaxLv = 2;
// 1 以牙还牙：完美格挡后，下一击伤害倍率 + 附带削杀气（占敌方杀气上限）；窗口内没打中就作废
inline constexpr std::array<float, 2> kRiposteHitMul{1.5f, 2.0f};
inline constexpr std::array<float, 2> kRiposteShachiPct{0.10f, 0.15f};
inline constexpr float kRiposteWindowSec = 3.0f;
// 2 残影步：完美闪避后 N 秒内，攻击额外造成杀气伤害（倍率加到破防上）
inline constexpr std::array<float, 2> kAfterimageSec{3.0f, 5.0f};
inline constexpr std::array<float, 2> kAfterimageBreakAdd{0.50f, 0.50f};
// 6 处决狂热：处决后回血（占生命上限）、补杀气（占杀气上限）、加攻击若干秒
inline constexpr std::array<float, 2> kFrenzyHeal{0.05f, 0.10f};
inline constexpr std::array<float, 2> kFrenzyShachi{0.25f, 0.35f};
inline constexpr std::array<float, 2> kFrenzyAttack{0.30f, 0.30f};
inline constexpr float kFrenzySec = 6.0f;
// 7 破防连锁：敌人破防时，半径内其他敌人被削掉的杀气（占各自杀气上限）；半径单位：厘米（游戏单位）
inline constexpr std::array<float, 2> kChainRadius{800.0f, 1200.0f};
inline constexpr std::array<float, 2> kChainShachiPct{0.20f, 0.35f};
// 8 绝境时缓：每层生命第一次跌破阈值时，敌人放慢若干秒（时间倍率，0.3 = 敌人只剩 30% 速度）
inline constexpr float kLastStandHp = 0.30f;
inline constexpr std::array<float, 2> kLastStandSec{5.0f, 10.0f};  // 用户 2026-11-15 实机后：2/3 秒 → 5/10 秒
inline constexpr float kLastStandTimeScale = 0.60f;      // 用户 2026-11-15：0.3 只为测试看得清，正式 0.6
inline constexpr float kLastStandWithSlowItem = 0.30f;   // 与缓时符（×0.6）同时生效时，敌人合计速度 0.3
// 12 化影随行：完美闪避时留一个绝影分身；存在秒数、强度（占玩家属性）、冷却。多个分身可同时存在，各自计时
inline constexpr std::array<float, 2> kShadowCloneSec{5.0f, 10.0f};
inline constexpr std::array<float, 2> kShadowCloneShare{0.30f, 0.40f};
inline constexpr float kShadowCloneCooldownSec = 8.0f;

// ---- 纯判定逻辑（与游戏无关，离线测试覆盖） ----
inline int Lv(int rank) { return std::clamp(rank, 1, kMaxLv) - 1; }

// 7 破防连锁：判定"破防"= 杀气从 >0 掉到 ≤0（上一帧读数有效）
inline bool BrokeNow(float lastShachi, float shachi) { return lastShachi > 0.5f && shachi >= 0 && shachi <= 0.5f; }
// 邻居被削后的杀气：至少留 1 点（写 0 会绕过原生破防结算，同飞身踢 3.6.2 的教训），已经 ≤1 的不动
inline float ChainShachiAfter(int rank, float shachi, float shachiMax) {
    if (rank <= 0 || !(shachiMax > 0) || !(shachi > 1.0f)) return shachi;
    return std::max(1.0f, shachi - kChainShachiPct[Lv(rank)] * shachiMax);
}
inline bool InChainRadius(int rank, double dx, double dy, double dz) {
    if (rank <= 0) return false;
    const double r = kChainRadius[Lv(rank)];
    return dx * dx + dy * dy + dz * dz <= r * r;
}

// 8 绝境时缓：每层一次。state 由调用方按层清零
struct LastStand {
    int floor = -1; bool used = false; double until = -1;
    // 返回本帧敌人的时间倍率（1 = 正常）
    float Tick(int rank, int curFloor, double now, float hpFrac) {
        if (curFloor != floor) { floor = curFloor; used = false; until = -1; }
        if (rank <= 0) return 1.0f;
        if (!used && hpFrac >= 0 && hpFrac < kLastStandHp) { used = true; until = now + kLastStandSec[Lv(rank)]; }
        return now < until ? kLastStandTimeScale : 1.0f;
    }
};

// 1 以牙还牙：完美格挡后挂起一次；窗口内第一次命中消耗
struct Riposte {
    double armedUntil = -1;
    void OnPerfectDeflect(double now) { armedUntil = now + kRiposteWindowSec; }
    bool Armed(double now) const { return now < armedUntil; }
    // 命中时调用：返回伤害倍率（未挂起 = 1）并消耗
    float ConsumeOnHit(int rank, double now) {
        if (rank <= 0 || !Armed(now)) return 1.0f;
        armedUntil = -1;
        return kRiposteHitMul[Lv(rank)];
    }
};

// 2 残影步 / 12 化影随行：完美闪避后的计时与冷却
struct Afterimage {
    double until = -1;
    void OnPerfectDodge(int rank, double now) { if (rank > 0) until = now + kAfterimageSec[Lv(rank)]; }
    float BreakAdd(int rank, double now) const { return rank > 0 && now < until ? kAfterimageBreakAdd[Lv(rank)] : 0.0f; }
};
struct CloneGate {
    double readyAt = 0;
    bool TrySpawn(int rank, double now) {
        if (rank <= 0 || now < readyAt) return false;
        readyAt = now + kShadowCloneCooldownSec;
        return true;
    }
};

}  // namespace tower::playstyle
