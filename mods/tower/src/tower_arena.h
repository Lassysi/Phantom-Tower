// 场地选择与战斗边界（纯逻辑，离线测试）。32ffb7d 用户：原生场地隔离不了 / 去不了 / 没地面时，失败关闭，
// 或回退到"已验证"的经典 Boss 竞技场并加有效雾门；无雾门的场地（彭猛）也不能一直跑远。
//   1. ChooseVenue：本局这只 Boss 的原生场地被判坏（剧情冻结失败 / 无钟可达 / 无地面）时，不吃场景的 Boss 改去
//      本区"已验证"的经典竞技场（%LOCALAPPDATA%\PBZTower\arena-verified.txt 里列出的擂台 id，Codex 实测后写入）；
//      吃场景的、或本区没有已验证竞技场的，明确失败、给原因，不在错的地方开打。
//   2. Boundary：战斗区边界（雾门的等效）：以放行时的位置为中心、半径 r。玩家越过边界 → 立刻放回边界内（像撞上雾墙，
//      不是 8 秒后拉回）；Boss 越界超过 15 秒 → 本层作废（脱战 / 卸载不算胜利）。
// 真正的原生雾门实体（BP 雾墙）需要运行库能生成场景物件，本批没有；边界只在影塔战斗中生效，结束 / 取消 / 暂离即解除。
#pragma once
#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace tower::arena {

enum class Kind { Native, Arena, None };
struct Choice { Kind kind = Kind::None; int arena = -1; std::string why; };

// nativeBad：本局原生场地已判坏；scene：这只 Boss 依赖原生场地（吃场景）；arenaVerified(i)：擂台 i 已实测可用
template <class Verified>
inline Choice ChooseVenue(bool hasNative, bool nativeBad, bool scene, int zoneArena, Verified arenaVerified) {
    if (hasNative && !nativeBad) return {Kind::Native, -1, ""};
    const char* lost = hasNative ? "原生场地本局不可用" : "没有原生场地";
    if (scene) return {Kind::None, -1, std::string(lost) + "，这只 Boss 依赖原生场地，不在别处开打"};
    if (zoneArena < 0) return {Kind::None, -1, std::string(lost) + "，本区没有经典竞技场"};
    if (!arenaVerified(zoneArena)) return {Kind::None, -1, std::string(lost) + "，本区经典竞技场还没实测验证（arena-verified.txt）"};
    return {Kind::Arena, zoneArena, std::string(lost) + "，改在已验证的经典竞技场打"};
}

inline std::set<std::string> ParseVerified(const std::string& text) {
    std::set<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto cut = line.find_first_of(" \t#");
        const std::string id = line.substr(0, cut);
        if (!id.empty()) out.insert(id);
    }
    return out;
}

struct P3 { double x = 0, y = 0, z = 0; };
enum class Act { None, PushBack, PullBack, Void };  // PullBack：把 Boss 拉回界内（调用方执行后调 Pulled）

// 凸多边形内（XY；顶点顺序不限，边上算在内）
inline bool InHull(const std::vector<P3>& h, const P3& p) {
    if (h.size() < 3) return false;
    int sign = 0;
    for (size_t i = 0; i < h.size(); ++i) {
        const P3& a = h[i]; const P3& b = h[(i + 1) % h.size()];
        const double c = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        if (std::abs(c) < 1e-6) continue;
        const int s = c > 0 ? 1 : -1;
        if (sign && s != sign) return false;
        sign = s;
    }
    return true;
}

// 战斗区边界（雾门的等效 / 补充）：有场地凸包时用凸包（雾门围成的区域），否则用圆；高度另限
struct Boundary {
    static constexpr double kDefaultRadius = 3500, kBossGrace = 15, kHeight = 1500, kBossPull = 3;
    static constexpr int kMaxFailedPulls = 10;  // 1.1.1：每次拉回本身会试 4 个候选落点（18 层疯医瞬移出界），连败 10 次才可能作废
    P3 center;
    double radius = kDefaultRadius;
    std::vector<P3> hull;            // 非空 = 凸包模式
    double zmin = -1e18, zmax = 1e18;  // 凸包模式的高度范围（圆模式用中心 ± kHeight）
    bool on = false;
    double bossOutSince = -1;
    // 软边：越界不超过 slack（厘米，沿离开中心的方向量）不算出界。39 层寒岁（借用新娘阿花场地，凸包 5 点）追 Boss 到边被反复拉回
    double slack = 0;
    int pulls = 0;                   // 本层已把 Boss 拉回的次数（只计数，不封顶）
    int failedPulls = 0;             // 连续拉回失败次数（读回不在目标点）

    void Arm(const P3& c, double r) { center = c; radius = r > 0 ? r : kDefaultRadius; hull.clear(); zmin = c.z - kHeight; zmax = c.z + kHeight; on = true; bossOutSince = -1; pulls = 0; failedPulls = 0; }
    void ArmHull(const std::vector<P3>& h, const P3& c, double lo, double hi) {
        Arm(c, 0);
        if (h.size() >= 3 && InHull(h, c)) { hull = h; zmin = lo; zmax = hi; }
    }
    void Disarm() { on = false; bossOutSince = -1; hull.clear(); pulls = 0; failedPulls = 0; }
    bool Outside(const P3& p) const { return OutsideBy(p, slack); }
    // give：允许越过边界的距离（厘米）。0 = 严格按凸包 / 圆
    bool OutsideBy(const P3& p, double give) const {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return true;
        if (p.z < zmin || p.z > zmax) return true;
        if (hull.empty()) return std::hypot(p.x - center.x, p.y - center.y) > radius + give;
        if (InHull(hull, p)) return false;
        if (give <= 0) return true;
        const double dx = p.x - center.x, dy = p.y - center.y, d = std::hypot(dx, dy);
        if (d <= give) return false;
        return !InHull(hull, {p.x - dx / d * give, p.y - dy / d * give, 0});  // 往中心退 give 仍在凸包里 = 软边内
    }
    // 往界内放得更深（frac：0 = 中心，1 = 贴边）：入场落点在界外时用，免得刚放进去就因边距不够再判出界
    P3 InsideDeep(const P3& p, double frac) const {
        const auto edge = Inside(p);
        return {center.x + (edge.x - center.x) * frac, center.y + (edge.y - center.y) * frac, edge.z};
    }
    // 玩家越界：沿中心方向放回边界内（凸包按二分找边界，再往里 2 米），高度用中心高度
    P3 Inside(const P3& p) const {
        const double dx = p.x - center.x, dy = p.y - center.y, d = std::hypot(dx, dy);
        if (d < 1) return center;
        if (hull.empty()) {
            const double k = std::max(0.0, radius - 200) / d;
            return {center.x + dx * k, center.y + dy * k, center.z + 50};
        }
        double lo = 0, hi = 1;
        for (int i = 0; i < 24; ++i) { const double m = (lo + hi) / 2; (InHull(hull, {center.x + dx * m, center.y + dy * m, 0}) ? lo : hi) = m; }
        const double k = std::max(0.0, lo - 200 / d);
        return {center.x + dx * k, center.y + dy * k, center.z + 50};
    }
    Act Player(const P3& p) const { return on && Outside(p) ? Act::PushBack : Act::None; }
    // 真实边界的说明（旧文案一律写"半径 35 米"，凸包时也这么写）
    std::string Describe() const {
        if (!on) return "没有战斗区";
        return hull.empty() ? std::format("圆，半径 {:.0f} 米", radius / 100) : std::format("雾门凸包 {} 点", hull.size());
    }
    // 留出 margin（厘米）还在边界里：圆按半径收缩；凸包要求前后左右各偏 margin 的点也在里面；高度两头各收 margin / 2
    bool Contains(const P3& p, double margin) const {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
        if (p.z < zmin + margin / 2 || p.z > zmax - margin / 2) return false;
        if (hull.empty()) return std::hypot(p.x - center.x, p.y - center.y) <= radius - margin;
        for (const P3 d : {P3{0, 0, 0}, P3{margin, 0, 0}, P3{-margin, 0, 0}, P3{0, margin, 0}, P3{0, -margin, 0}})
            if (!InHull(hull, {p.x + d.x, p.y + d.y, 0})) return false;
        return true;
    }
    // Radius of a circle wholly inside the exported convex arena, before
    // adding an enemy's capsule. Used only as a conservative spawn search area.
    double InnerRadius(double margin, double maximum) const {
        if (!on || !std::isfinite(margin) || !std::isfinite(maximum) || margin < 0 || maximum <= 0) return 0;
        double available = hull.empty() ? radius : 1e30;
        if (!hull.empty()) {
            if (!InHull(hull, center)) return 0;
            for (size_t i = 0; i < hull.size(); ++i) {
                const auto& a = hull[i]; const auto& b = hull[(i + 1) % hull.size()];
                const double dx = b.x - a.x, dy = b.y - a.y, len = std::hypot(dx, dy);
                if (!std::isfinite(len) || len < 1e-6) return 0;
                available = std::min(available, std::abs(dx * (center.y - a.y) - dy * (center.x - a.x)) / len);
            }
        }
        return std::max(0.0, std::min(maximum, available - margin));
    }
    // Boss 越界（雾门对敌我都限制；41 层雷婷会自己瞬移出雾门、一层拉回 3 次）：出界 3 秒就拉回，拉回成功不封顶；
    // 只有连续 3 次拉回失败（读回不在目标点）后仍在界外满 15 秒才作废
    Act Boss(const P3& p, double now) { return BossAt(on && Outside(p), now, kBossPull); }
    // 用户 2026-11-12：外部先判定「是否真的在雾门外」（凸包外 + 视线被挡 / 远超软边），这里只管计时与拉回 / 作废
    Act BossAt(bool outside, double now, double pullAfter) {
        if (!on) return Act::None;
        if (!outside) { bossOutSince = -1; return Act::None; }
        if (bossOutSince < 0) bossOutSince = now;
        if (failedPulls < kMaxFailedPulls) return now - bossOutSince >= pullAfter ? Act::PullBack : Act::None;
        return now - bossOutSince >= kBossGrace ? Act::Void : Act::None;
    }
    void Pulled(double now, bool ok = true) { ++pulls; failedPulls = ok ? 0 : failedPulls + 1; bossOutSince = now; }
};

// 入场准入（读秒前；1–32 层 13 / 18 / 24 / 25：Boss 在空气墙外 / 门外、虚空里已读秒、放行后 53–287 ms 就被拉回）：
// 玩家和每只 Boss 都要在真实战斗区里、留出 margin 边距才放行读秒；不过就说清是谁、在哪、边界是什么。
// 地面 / 导航 / 同侧可达不在这里（需要实机的碰撞查询），见 notes/tower-repair-1-32.md 第 C 节
struct Admission { bool ok = true; std::string why; };
inline Admission Admit(const Boundary& b, const P3& player, const std::vector<std::pair<std::string, P3>>& bosses, double margin = 200) {
    if (!b.on) return {};
    auto where = [&](const P3& p) { return std::format("({:.0f}, {:.0f}, {:.0f})", p.x, p.y, p.z); };
    if (!b.Contains(player, margin)) return {false, std::format("玩家 {} 不在战斗区内（{}，高度 {:.0f}–{:.0f}）", where(player), b.Describe(), b.zmin, b.zmax)};
    for (auto& [name, p] : bosses)
        if (!b.Contains(p, margin)) return {false, std::format("{} {} 不在战斗区内（{}，高度 {:.0f}–{:.0f}）", name, where(p), b.Describe(), b.zmin, b.zmax)};
    return {};
}

}  // namespace tower::arena
