#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace pbz::enemies {
struct Vec { double x = 0, y = 0, z = 0; };
inline bool Finite(Vec p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
inline double Distance2D(Vec a, Vec b) { return std::hypot(a.x - b.x, a.y - b.y); }
struct Capsule { double radius = 0, halfHeight = 0; };
inline bool Valid(Capsule c) {
    return std::isfinite(c.radius) && std::isfinite(c.halfHeight) &&
           c.radius >= 1 && c.radius <= 2000 && c.halfHeight >= c.radius && c.halfHeight <= 5000;
}
struct Ground { Vec point, normal; bool blocking = false, penetrating = false; };
struct Landing { Vec center; Capsule capsule; bool navProjected = false; };
struct SceneQuery {
    virtual ~SceneQuery() = default;
    // Navigation is optional. A missing navmesh still requires all floor and
    // room checks. Projection output is never trusted without distance limits.
    virtual bool Project(Vec candidate, Vec& out) = 0;
    virtual bool Floor(Vec xyAtPlayerHeight, Ground& out) = 0;
    virtual bool Room(Vec center, Capsule capsule) = 0;
};

// A caller-supplied inner arena region narrows the ordinary ground / capsule
// search; it never bypasses those checks or the reservations for live actors.
struct RegionQuery : SceneQuery {
    SceneQuery& inner;
    Vec center;
    double radius;
    RegionQuery(SceneQuery& scene, Vec c, double r) : inner(scene), center(c), radius(r) {}
    bool Project(Vec p, Vec& out) override { return inner.Project(p, out); }
    bool Floor(Vec p, Ground& out) override { return inner.Floor(p, out); }
    bool Room(Vec p, Capsule c) override {
        return Finite(center) && std::isfinite(radius) && radius > 0 &&
               Distance2D(center, p) + c.radius <= radius && inner.Room(p, c);
    }
};

// 落点拒绝原因计数（诊断：生成失败时随回执报告是哪一项把所有候选点都否掉了）
struct LandingReject { int nav = 0, tooClose = 0, floor = 0, steep = 0, offset = 0, height = 0, support = 0, overlap = 0, room = 0; };
inline LandingReject& LastLandingReject() { static LandingReject r; return r; }

// Fixed bounded search: 3 rings, 12 headings each, forward first. UE units=cm.
// Footprint support rejects narrow ledges, steep ground and lower floors.
inline std::vector<Landing> FindLandings(SceneQuery& scene, Vec player, Vec forward,
                                       Capsule playerCapsule, Capsule enemy, int count,
                                       double distanceCm, const std::vector<Landing>& reserved = {}) {
    std::vector<Landing> out;
    auto& rej = LastLandingReject();
    rej = {};
    if (!Finite(player) || !Finite(forward) || !Valid(playerCapsule) || !Valid(enemy) ||
        count < 1 || count > 8 || !std::isfinite(distanceCm) || distanceCm < 200 || distanceCm > 2000) return out;
    const double heading = std::atan2(forward.y, forward.x);
    const double base = (std::max)(distanceCm, playerCapsule.radius + enemy.radius + 150.0);
    for (int ring = 0; ring < 3 && static_cast<int>(out.size()) < count; ++ring) {
        const double radius = base + ring * (std::max)(200.0, enemy.radius * 1.5);
        if (radius + enemy.radius > 4000) continue;
        for (int n = 0; n < 12 && static_cast<int>(out.size()) < count; ++n) {
            const int step = n == 0 ? 0 : (n % 2 ? (n + 1) / 2 : -n / 2);
            const double angle = heading + step * 3.14159265358979323846 / 6.0;
            Vec candidate{player.x + radius * std::cos(angle), player.y + radius * std::sin(angle), player.z};
            Vec projected{};
            const bool nav = scene.Project(candidate, projected);
            if (nav) {
                if (!Finite(projected) || Distance2D(candidate, projected) > 150 || std::abs(projected.z - player.z) > 250) { ++rej.nav; continue; }
                candidate.x = projected.x; candidate.y = projected.y;
            }
            if (Distance2D(candidate, player) < playerCapsule.radius + enemy.radius + 100 ||
                Distance2D(candidate, player) > 4000) { ++rej.tooClose; continue; }
            Ground ground{};
            if (!scene.Floor(candidate, ground) || !ground.blocking || ground.penetrating || !Finite(ground.point) || !Finite(ground.normal)) { ++rej.floor; continue; }
            if (ground.normal.z < 0.70710678) { ++rej.steep; continue; }
            if (Distance2D(candidate, ground.point) > 5) { ++rej.offset; continue; }
            if (std::abs(ground.point.z - (player.z - playerCapsule.halfHeight)) > 180) { ++rej.height; continue; }
            bool supported = true;
            const double edge = enemy.radius * .75;
            for (Vec offset : {Vec{edge, 0, 0}, Vec{-edge, 0, 0}, Vec{0, edge, 0}, Vec{0, -edge, 0}}) {
                Ground support{};
                if (!scene.Floor({candidate.x + offset.x, candidate.y + offset.y, player.z}, support) ||
                    !support.blocking || support.penetrating || !Finite(support.point) || !Finite(support.normal) ||
                    support.normal.z < 0.70710678 || std::abs(support.point.z - ground.point.z) > 45) { supported = false; break; }
            }
            if (!supported) { ++rej.support; continue; }
            Landing landing{{candidate.x, candidate.y, ground.point.z + enemy.halfHeight + 5}, enemy, nav};
            bool overlaps = false;
            for (const std::vector<Landing>* group : {&reserved, static_cast<const std::vector<Landing>*>(&out)})
                for (const auto& old : *group)
                    if (std::abs(old.center.z - landing.center.z) < old.capsule.halfHeight + enemy.halfHeight &&
                        Distance2D(old.center, landing.center) < old.capsule.radius + enemy.radius + 40) overlaps = true;
            if (overlaps) { ++rej.overlap; continue; }
            if (scene.Room(landing.center, {enemy.radius + 5, enemy.halfHeight + 2})) out.push_back(landing); else ++rej.room;
        }
    }
    return out;
}
}  // namespace pbz::enemies
