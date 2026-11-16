// 3.6.0 统一出界拉回 + 隐形硬墙（用户 2026-11-14，方案第 7 / 7b 条）。纯逻辑，离线测试 plan_test 覆盖；游戏对象读写在 tower.cpp（游戏线程）。
// - 判定线 = 场地真实雾墙（实测圆，没实测时用塔的战斗区外接圆）往里 1.5 米。
// - 3.6.0 打包前（用户 2026-11-15：不要再「一靠近雾门就闪到场地中间」）：只有明确出了雾门才拉回——
//   在判定线外持续 3 秒，或一帧内位移 > 8 米（闪现 / 瞬移）且落在线外 0.5 秒。不再按「路被墙隔开」拉回，也不再有「放回中心」兜底。
// - 拉回点：出去那一侧、雾门内 2 米（顶在雾门边），地面检测不过再向两边找，最后才退到更靠里；不搬玩家，原生技能不改。
// - 硬墙（无尽层所有敌人；前 100 层的飞天 Boss）：每帧越过判定线就把位置夹回线内、切掉向外的速度（保留沿墙速度），高度也限制。
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace tower::wall {
inline constexpr double kInset = 150, kOutSeconds = 3.0, kJumpCm = 800, kJumpOutSeconds = 0.5, kWalledSeconds = 2.0;
inline constexpr double kEdgeGap = 200, kPlayerGap = 400, kHardSeconds = 10, kHardWindow = 20;
inline constexpr int kHardPulls = 3;
struct P { double x = 0, y = 0, z = 0; };
// 场地边界：圆心 + 半径；有实测时 rays = 各方向真实可站半径（厘米，从 +X 轴逆时针等分），判定线按方向插值
struct Circle {
    double x = 0, y = 0, r = 0; bool ok = false;
    std::vector<double> rays;
    double RadiusAt(double px, double py) const {
        if (rays.size() < 4) return r;
        const double n = static_cast<double>(rays.size());
        double a = std::atan2(py - y, px - x);
        if (a < 0) a += 2 * 3.14159265358979323846;
        const double t = a / (2 * 3.14159265358979323846) * n;
        const size_t i = static_cast<size_t>(t) % rays.size(), j = (i + 1) % rays.size();
        const double f = t - std::floor(t);
        return rays[i] * (1 - f) + rays[j] * f;
    }
    double LineAt(double px, double py) const { return std::max(100.0, RadiusAt(px, py) - kInset); }
    double Line() const { return std::max(100.0, (rays.size() >= 4 ? *std::min_element(rays.begin(), rays.end()) : r) - kInset); }  // 最窄处（拉回点用）
};

inline double Dist2D(const P& a, double x, double y) { return std::hypot(a.x - x, a.y - y); }
inline bool Outside(const Circle& c, const P& p) { return c.ok && Dist2D(p, c.x, c.y) > c.LineAt(p.x, p.y); }
// 把 p 夹回判定线内（留 20 厘米）；返回是否动过
inline bool ClampXY(const Circle& c, P& p) {
    if (!Outside(c, p)) return false;
    const double d = Dist2D(p, c.x, c.y), k = (c.LineAt(p.x, p.y) - 20) / std::max(1.0, d);
    p.x = c.x + (p.x - c.x) * k; p.y = c.y + (p.y - c.y) * k;
    return true;
}
// 切掉 (x, y) 处向外的径向速度（向内、沿墙的保留）
inline bool CutOutward(const Circle& c, const P& p, double& vx, double& vy) {
    const double dx = p.x - c.x, dy = p.y - c.y, d = std::hypot(dx, dy);
    if (d < 1) return false;
    const double ux = dx / d, uy = dy / d, out = vx * ux + vy * uy;
    if (out <= 0) return false;
    vx -= out * ux; vy -= out * uy;
    return true;
}

// 每个敌人一份：出线计时、被墙隔开计时、最近 3 次拉回时刻
struct Track {
    double outSince = -1, walledSince = -1, hardUntil = -1, probeAt = -1;
    P last{}; bool hasLast = false, jumped = false;
    std::array<double, kHardPulls> pulls{-1e9, -1e9, -1e9};
    int pulled = 0, blocked = 0;  // 本层拉回 / 硬墙拦截次数（日志）
    bool Hard(double now) const { return now < hardUntil; }
    // 每帧：更新位置，返回这一帧该不该拉回（不含硬墙）
    bool Due(const Circle& c, const P& p, double now) {
        if (hasLast && Dist2D(p, last.x, last.y) > kJumpCm) jumped = true;  // 闪现 / 瞬移：一帧内位移超过 8 米
        last = p; hasLast = true;
        if (!Outside(c, p)) { outSince = -1; jumped = false; return false; }  // 线内一律不拉（不再按「被墙隔开」拉）
        if (outSince < 0) outSince = now;
        return now - outSince >= (jumped ? kJumpOutSeconds : kOutSeconds);
    }
    void Walled(bool on, double now) { walledSince = on ? (walledSince < 0 ? now : walledSince) : -1; }
    void Pulled(double now) {
        std::rotate(pulls.begin(), pulls.begin() + 1, pulls.end());
        pulls.back() = now;
        ++pulled; outSince = -1; walledSince = -1; jumped = false; hasLast = false;
    }
};

// 拉回候选点（3.6.0 打包前）：先放出去那一侧的雾门内 2 米（顶在雾门边）；地面检测不过时向两边每 10° 找，
// 再往里退到 0.8 / 0.6 倍；离玩家 < 4 米的跳过；全都不行才给圆心。hard / seed 参数保留只为调用处不变
inline std::vector<P> PullCandidates(const Circle& c, const P& from, const P& player, bool hard, uint64_t seed) {
    (void)hard; (void)seed;
    std::vector<P> out;
    if (!c.ok) return out;
    const double base = std::atan2(from.y - c.y, from.x - c.x);
    for (const double depth : {1.0, 0.8, 0.6})
        for (int i = 0; i < 7; ++i) {
            const double a = base + (i % 2 ? 1 : -1) * ((i + 1) / 2) * (3.14159265358979323846 / 18.0);
            const double lx = c.x + std::cos(a), ly = c.y + std::sin(a);
            const double d = std::max(50.0, (c.LineAt(lx, ly) - (kEdgeGap - kInset)) * depth);  // 判定线已在墙内 1.5 米，再往里 0.5 米 = 墙内 2 米
            const P q{c.x + std::cos(a) * d, c.y + std::sin(a) * d, from.z};
            if (!Outside(c, q) && Dist2D(q, player.x, player.y) >= kPlayerGap) out.push_back(q);
        }
    out.push_back({c.x, c.y, from.z});
    return out;
}
}  // namespace tower::wall
