// 影塔传送（纯逻辑，可离线测试）：擂台选择、进塔位置记录、传送状态机。
// 游戏端经 IMove（tower_travel.cpp 里接运行库的传送接口）；跨关卡先走游戏原生的钟传送（带加载界面），
// 到了同一关卡再走点传送（运行库地图传送的悬停等加载）。
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <format>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "native_spots.generated.h"
#include "travel_data.generated.h"
#include "venues.generated.h"
#include "tower_fixed_venue.h"

namespace tower::travel {

struct Pose { double x = 0, y = 0, z = 0, yaw = 0; };

// 1.1.1（用户 2026-11-07 第 9 层拙壁，玩家反馈卡脚）：原游戏拙壁没有雾门、没有剧情，碰到就打。导出的玩家落点在雾门圈外的山坡上
// （比场地高约 5 米），原生刷怪点压在圈边；玩家要自己跳进圈，没跳进来就卡在雾门边和墙角之间。
// 只对这些层：人和 Boss 都放到雾门凸包中心两侧各 kCenterGap，面对面（方向沿原落点 → 中心，人仍在原来那一侧）
inline constexpr std::string_view kCenteredVenues[] = {"BP_Monster_Zhuobi."};
inline constexpr double kCenterGap = 400;
struct CenterPair { Pose player, boss; };
inline bool CenteredPair(std::string_view path, CenterPair* out) {
    if (std::none_of(std::begin(kCenteredVenues), std::end(kCenteredVenues), [&](std::string_view k) { return path.find(k) != std::string_view::npos; })) return false;
    const VenueDef* v = nullptr;
    for (auto& d : kVenues) if (d.path == path && d.hullN >= 3) { v = &d; break; }
    const NativeSpot* s = nullptr;
    for (auto& n : kNativeSpots) if (n.path == path) { s = &n; break; }
    if (!v || !s || v->hullAt + v->hullN > static_cast<int>(std::size(kVenueHull)) / 2) return false;
    double cx = 0, cy = 0;
    for (int i = 0; i < v->hullN; ++i) { cx += kVenueHull[2 * (v->hullAt + i)]; cy += kVenueHull[2 * (v->hullAt + i) + 1]; }
    cx /= v->hullN; cy /= v->hullN;
    const double dx = s->x - cx, dy = s->y - cy, len = std::hypot(dx, dy);
    if (len < 1) return false;
    const double ux = dx / len, uy = dy / len, z = (v->zmin + v->zmax) / 2;
    const double toBoss = std::atan2(-uy, -ux) * 180.0 / 3.14159265358979323846;
    out->player = {cx + ux * kCenterGap, cy + uy * kCenterGap, z, toBoss};
    out->boss = {cx - ux * kCenterGap, cy - uy * kCenterGap, z, toBoss + 180};
    return true;
}
// 1.1.1（用户 2026-11-07 第 22 层游龙：每次都生成在水下）：导出的玩家落点 Z 524，比 Boss 出生点（743）和雾门（746）低 2 米多，
// 落在河道深处。只对这些层：玩家落到 Boss 出生点往雾门凸包中心方向 kSpawnerGap 处、Boss 出生点的高度（落地仍按地面射线解析），面朝 Boss
inline constexpr std::string_view kNearSpawnerVenues[] = {"BP_Monster_YouLong_Harpoon."};
inline constexpr double kSpawnerGap = 600;
inline bool NearSpawner(std::string_view path, Pose* out) {
    if (std::none_of(std::begin(kNearSpawnerVenues), std::end(kNearSpawnerVenues), [&](std::string_view k) { return path.find(k) != std::string_view::npos; })) return false;
    const VenueDef* v = nullptr;
    for (auto& d : kVenues) if (d.path == path && d.hullN >= 3) { v = &d; break; }
    if (!v || v->hullAt + v->hullN > static_cast<int>(std::size(kVenueHull)) / 2) return false;
    double cx = 0, cy = 0;
    for (int i = 0; i < v->hullN; ++i) { cx += kVenueHull[2 * (v->hullAt + i)]; cy += kVenueHull[2 * (v->hullAt + i) + 1]; }
    cx /= v->hullN; cy /= v->hullN;
    double dx = cx - v->sx, dy = cy - v->sy, len = std::hypot(dx, dy);
    if (len < 1) { dx = 1; dy = 0; len = 1; }
    const double px = v->sx + dx / len * kSpawnerGap, py = v->sy + dy / len * kSpawnerGap;
    *out = {px, py, v->sz, std::atan2(v->sy - py, v->sx - px) * 180.0 / 3.14159265358979323846};
    return true;
}
inline bool Finite(const Pose& p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) && std::isfinite(p.yaw); }

inline double DistXY(const Pose& a, const Pose& b) { return std::hypot(a.x - b.x, a.y - b.y); }
// 在这个点上：水平 r 内、高度差 3 米内（同一 XY 的上下层 / 雾门外高台不算）
inline bool AtSpot(const Pose& a, const Pose& b, double r) { return Finite(a) && Finite(b) && DistXY(a, b) <= r && std::abs(a.z - b.z) <= 300; }
inline bool SameLevel(std::string a, std::string b) {
    for (auto* s : {&a, &b}) std::transform(s->begin(), s->end(), s->begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return !a.empty() && a == b;
}

// 目的地：bell 非空时它是跨关卡的入口（bellPose = 钟的传送点）；target 是最终落点（同关卡点传送）
// dataLayer：落点所在世界分区单元属于一个默认不加载的剧情数据层（实例名），点传送前要先激活，离开后还原
struct Dest { std::string name, bell, level, dataLayer; Pose bellPose, target; bool directFirst = false; bool resolveSurfaceHeight = false; double arrive = 0; };  // arrive：到达容差（厘米），0 = Traveler::kArrive  // Native authored centers need not equal a standing capsule's height.

// 原生场地所在单元只在剧情数据层里（初始 Unloaded），不激活就没有地面。
// 39 层寒岁：PBZ_Pangzhen_WP 单元 CVIY43F7XO1UQVBR8G2AJISPY 只挂 DL_3721Unload（实例 InitialRuntimeState=0），
// 实机 PID69764 庞镇 RepActiveDataLayerNames 5 个里没有它 → 传到 Z≈2001791 悬空 20 秒被拒 / 坠空。
// 其余 Boss 场地单元都在 Main_Logic / Main_LA 等常驻层（venues.json cell_data_layers 全表核过）
struct NativeLayer { std::string_view path, instance, asset; };
inline constexpr NativeLayer kNativeLayers[] = {
    {"/PBZCombatAsset/ActorData/Mobs/QiShu/BP_Monster_QiShu_Bing.BP_Monster_QiShu_Bing_C",
     "DataLayer_946171F64F6F9FF290B62AA52648BE79", "DL_3721Unload"},
};
inline std::string NativeLayerFor(std::string_view path) {
    for (auto& l : kNativeLayers) if (l.path == path) return std::string(l.instance);
    return {};
}

// 玩家标定的地点："<id> x y z yaw [关卡]"，一行一个；后写的覆盖先写的。
// id = 通用擂台（arena_z1_1…）或 Boss 原生场地（boss:<身份>，如 boss:MoTian）
struct Spot { Pose at; std::string level; };
using Centers = std::map<std::string, Spot>;
inline std::string BossKey(std::string_view identity) { return "boss:" + std::string(identity); }
inline Centers ParseCenters(const std::string& text) {
    Centers out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string id;
        Spot s;
        if (!(ls >> id >> s.at.x >> s.at.y >> s.at.z >> s.at.yaw) || id[0] == '#') continue;
        if (!std::isfinite(s.at.x) || !std::isfinite(s.at.y) || !std::isfinite(s.at.z)) continue;
        ls >> s.level;
        if (s.level == "-") s.level.clear();
        out[id] = s;
    }
    return out;
}
inline std::string FormatCenters(const Centers& c) {
    std::string s = "# 影塔擂台 / Boss 原生场地（游戏内标定）：id x y z yaw 关卡\n";
    for (auto& [id, p] : c) s += std::format("{} {:.1f} {:.1f} {:.1f} {:.1f} {}\n", id, p.at.x, p.at.y, p.at.z, p.at.yaw, p.level.empty() ? "-" : p.level);
    return s;
}

// 每区一个通用擂台（本区不吃场景的层都在这里打）：已标定的优先，否则按数据顺序取第一个
// （每区首选排在前面：和本区原生场地同关卡的那张图，换场地不用加载）
inline int ChooseArena(int zone, uint64_t /*seed*/, const Centers& centers) {
    int first = -1;
    for (int i = 0; i < static_cast<int>(std::size(kArenas)); ++i) {
        if (kArenas[i].zone != zone) continue;
        if (centers.count(std::string(kArenas[i].id))) return i;
        if (first < 0) first = i;
    }
    return first;
}
inline Dest ArenaDest(int index, const Centers& centers) {
    const auto& a = kArenas[index];
    Dest d;
    d.name = std::string(a.id);
    d.bell = std::string(a.bell);
    d.bellPose = {a.x, a.y, a.z, a.yaw};
    auto it = centers.find(d.name);
    if (it != centers.end()) { d.target = it->second.at; d.level = it->second.level; }
    else d.target = d.bellPose;  // 没标定：落在钟旁接近点
    return d;
}
// Boss 原生场地：标定过才有（没标定返回 false，引擎退回通用擂台）。钟由 FillBell 按关卡补
inline bool BossDest(std::string_view identity, std::string_view name, const Centers& centers, Dest* d) {
    auto it = centers.find(BossKey(identity));
    if (it == centers.end()) return false;
    *d = {};
    d->name = std::string(name) + "的场地";
    d->target = it->second.at;
    d->level = it->second.level;
    return true;
}
// 固定场地（tower_fixed_venue.h，榕树内场等）：落到实测落点，经同图最近的钟
inline bool FixedDest(std::string_view path, Dest* d) {
    const auto* v = FindFixedVenue(path);
    if (!v) return false;
    *d = {};
    d->name = std::string(v->name);
    d->target = {v->px, v->py, v->pz, v->pyaw};
    d->level = std::string(v->world);
    d->resolveSurfaceHeight = true;
    // 实机 B 批第 1 层：同图最近的钟（榕树断钟）离落点只有 8 米，旧的 15 米到达容差把钟旁（缺口外、圈外）当成已到，没有点传送
    d->arrive = 300;
    return true;
}
// Boss 原生刷怪点（从游戏地图单元提取，native_spots.generated.h）：站到刷怪点上，Boss 在身边生成
inline bool NativeDest(std::string_view path, std::string_view name, Dest* d) {
    for (auto& s : kNativeSpots) {
        if (s.path != path) continue;
        *d = {};
        d->name = std::string(name) + "的原生场地";
        d->target = {s.x, s.y, s.z, s.yaw};
        // 1.0.1（用户 2026-11-07 第 26 层万钧实机）：导出的玩家落点是原生遭遇开始前的站位（锁链门外的钟旁），原生剧情才把人带进场地；
        // 改为塔生成后没有剧情，人和 Boss 都留在门外。只对实测有问题的层：落点改到 Boss 出生点前 6 米（朝原落点一侧），面向 Boss
        //   26 层万钧（落点离出生点 36 米，锁链门外）；15 层斩骑（40 米，雾门外：玩家反馈 Boss 一靠近门就往外冲、卡在外面）
        if (path.find("BP_Monster_WanJun_JinGua.") != std::string_view::npos || path.find("BP_Monster_GuiChai_Yellow_MoDao.") != std::string_view::npos) {
            const double dx = s.x - s.bx, dy = s.y - s.by, len = std::max(1.0, std::hypot(dx, dy));
            const double yaw = std::atan2(-dy, -dx) * 180.0 / 3.14159265358979323846;
            d->target = {s.bx + dx / len * 600, s.by + dy / len * 600, s.bz, yaw};
        }
        if (CenterPair c; CenteredPair(path, &c)) d->target = c.player;  // 9 层拙壁：落到雾门圈中间（见 kCenteredVenues）
        if (Pose p; NearSpawner(path, &p)) d->target = p;                 // 22 层游龙：落到 Boss 出生点旁、场地高度（见 kNearSpawnerVenues）
        d->level = std::string(s.level);
        d->resolveSurfaceHeight = true;
        d->dataLayer = NativeLayerFor(path);
        return true;
    }
    return false;
}
inline bool BellPose(const std::string& id, Pose* p) {
    for (auto& b : kBells) if (b.id == id) { *p = {b.x, b.y, b.z, b.yaw}; return true; }
    return false;
}

// 游戏端接口（tower_travel.cpp 实现；测试里用替身）
struct IMove {
    virtual ~IMove() = default;
    virtual bool Where(Pose* p, std::string* level) = 0;            // 玩家位置 + 所在关卡；没有玩家（加载中）返回 false
    virtual std::string BellLevel(const std::string& bell) = 0;    // 钟所在关卡
    virtual bool Bell(const std::string& bell, const Pose& at) = 0; // 跨关卡钟传送（异步：之后关卡切换）
    virtual bool Point(const Pose& at, std::string* msg) = 0;       // 同关卡点传送（可能进入悬停等加载）
    virtual bool Busy() = 0;                                        // 点传送还在等加载
    virtual bool Ready() { return true; }                           // 原生目的地加载遮罩已结束（跨图后才做点传送）
    virtual bool StandingAt(const Pose&) { return false; }          // Fresh native walking floor at this exact current position, never a remote target.
    // 直接把玩家放到落点（不经运行库的地面查询；之后由塔自己的落地 / 地面审计把关）。不支持返回 false
    virtual bool Direct(const Pose&, std::string*) { return false; }
    // 在目标关卡里把数据层实例激活并读回（已激活也返回 true）。找不到 / 读回不对返回 false
    virtual bool EnsureLayer(const std::string&, std::string* msg) { *msg = "数据层接口不可用"; return false; }
};

// 运行库点传送失败消息的前缀（src/core/features/move Ground::kFaultTag）：落点查询在游戏函数里异常。
// 这种失败不换点、不重抽再跨图，整层停下等玩家决定（影塔 7c2668e 第三层黑屏，Codex 2026-11-01）
inline constexpr std::string_view kNativeFault = "原生异常：";

// 同关卡里离 at 最近的钟（钟的关卡只能在游戏里查，levelOf 由调用方给）：按距离从近到远核对，最多查 40 个
// （不同关卡的坐标会重叠，必须核对关卡）
template <class LevelOf>
inline std::string NearestBell(const Pose& at, const std::string& level, LevelOf levelOf) {
    std::vector<std::pair<double, int>> order;
    for (int i = 0; i < static_cast<int>(std::size(kBells)); ++i) {
        const auto& b = kBells[i];
        order.push_back({std::hypot(b.x - at.x, b.y - at.y) + std::abs(b.z - at.z) * 0.5, i});
    }
    std::sort(order.begin(), order.end());
    for (size_t k = 0; k < order.size() && k < 40; ++k)
        if (SameLevel(levelOf(std::string(kBells[order[k].second].id)), level)) return std::string(kBells[order[k].second].id);
    return {};
}
// 目的地有关卡、但钟不在那个关卡（或没有钟）：换成那个关卡里离目标最近的钟
template <class LevelOf>
inline void FillBell(Dest* d, LevelOf levelOf) {
    if (d->level.empty()) return;
    if (!d->bell.empty() && SameLevel(levelOf(d->bell), d->level)) return;
    const auto bell = NearestBell(d->target, d->level, levelOf);
    Pose p;
    if (!bell.empty() && BellPose(bell, &p)) { d->bell = bell; d->bellPose = p; }
}

enum class State { Idle, Level, Point, Done, Failed };

// 传送状态机：Go 之后每帧 Step；Done / Failed 后保持，直到下一次 Go 或 Reset
class Traveler {
public:
    static constexpr double kArrive = 1500;     // 离落点 15 米内算到达
    static constexpr double kInPlace = 600;     // 出发前已在落点 6 米内才不传送（与开层 GoFloor 一致）
    static constexpr double kLevelTimeout = 120, kPointTimeout = 40, kSettle = 1.5, kDirectGrace = 8;

    void Go(const Dest& d) { dest_ = d; state_ = State::Idle; started_ = false; going_ = true; t_ = settle_ = 0; retried_ = direct_ = directUsed_ = stop_ = waitingCover_ = direct2_ = false; directFailAt_ = -1; directWhy_.clear(); why_.clear(); }
    void Reset() { state_ = State::Idle; started_ = false; going_ = false; dest_ = {}; }
    // Go 之后到 Done / Failed 之前都算进行中（包括还没跑第一步：外层靠它决定要不要调 Step，2026-10-31 Codex 查出首帧永不推进）
    bool Active() const { return going_ && (state_ == State::Idle || state_ == State::Level || state_ == State::Point); }
    State state() const { return state_; }
    const Dest& dest() const { return dest_; }
    const std::string& why() const { return why_; }
    // 失败且不该自动重试（原生异常 / 加载遮罩一直不结束）：外层取消本层，不换 Boss 再跨图
    bool stop() const { return state_ == State::Failed && stop_; }
    bool directUsed() const { return directUsed_; }  // 本次到达靠直接放置（运行库地面查询拒绝过）

    double ArriveTol() const { return dest_.arrive > 0 ? dest_.arrive : kArrive; }
    State Step(IMove& g, double dt) {
        if (!started_) { started_ = true; Begin(g); return state_; }
        t_ += dt;
        if (state_ == State::Level) {
            Pose p; std::string lv;
            // 换关卡期间玩家不存在 / 还在旧关卡：等新关卡里出现玩家并稳定一会儿
            if (g.Where(&p, &lv) && SameLevel(lv, level_)) {
                if ((settle_ += dt) >= kSettle) {
                    if (g.Ready() && AtSpot(p, dest_.target, ArriveTol())) return Finish(State::Done, "");
                    if (g.Ready()) PointStep(g);  // 玩家已在新关卡，但原生加载遮罩还在：等它结束再查落点
                    else waitingCover_ = true;
                }
            } else settle_ = 0;
            if (state_ == State::Level && t_ > kLevelTimeout) {
                if (waitingCover_) stop_ = true;  // 加载一直不结束：不再换目的地跨图
                return Finish(State::Failed, waitingCover_ ? "跨关卡后原生加载一直未结束" : "跨关卡传送超时");
            }
        } else if (state_ == State::Point) {
            if (t_ > kPointTimeout) return Finish(State::Failed, "点传送超时");
            if (g.Busy()) return state_;
            Pose p; std::string lv;
            if (!g.Where(&p, &lv)) return state_;
            if (SameLevel(lv, level_) && g.Ready() && (AtSpot(p, dest_.target, ArriveTol()) || AcceptSurfaceHeight(g,p))) return Finish(State::Done, "");
            if ((settle_ += dt) < kSettle) return state_;
            // 34 层力者（断魂冰面）：运行库地面查询认不出这块冰面，悬停 20 秒后传回原处；再试一次又是 20 秒悬空（用户：场景不停跳）。
            // 第一次被拒就直接放到落点（落地交给塔的地面审计）；直接放置不可用时才按旧办法再试一次点传送
            // 1.1.1（自测扫描第 42 层蛊鹤高空平台：点传送落在平台上（比数据落点低 6 米），刚落地 0.3 秒"站稳"还不成立，
            // 直接放置又判目标点没地面 → 立即停机跳层）：直接放置失败后再宽限 8 秒，每帧仍按"站稳在场地地面上"判到达，
            // 4 秒时再试一次直接放置；宽限用完才停
            if (!direct_ && g.Ready()) {
                direct_ = true;
                std::string msg;
                if (SameLevel(lv, level_) && g.Direct(dest_.target, &msg) && AcceptDirect(g)) { directUsed_ = true; t_ = settle_ = 0; return state_; }
                directFailAt_ = t_; directWhy_ = msg;
                return state_;
            }
            if (directFailAt_ >= 0) {
                if (t_ - directFailAt_ < kDirectGrace) {
                    if (!direct2_ && t_ - directFailAt_ >= kDirectGrace / 2 && g.Ready()) {
                        direct2_ = true;
                        std::string msg;
                        if (SameLevel(lv, level_) && g.Direct(dest_.target, &msg) && AcceptDirect(g)) { directUsed_ = true; directFailAt_ = -1; t_ = settle_ = 0; return state_; }
                        if (!msg.empty()) directWhy_ = msg;
                    }
                    return state_;
                }
                stop_ = true;
                return Finish(State::Failed, directWhy_.empty() ? "落点支撑未确认，停止入场" : directWhy_);
            }
            if (!retried_ && g.Ready()) { retried_ = true; PointStep(g); return state_; }
            return Finish(State::Failed, "落点无法到达");
        }
        return state_;
    }

private:
    void Begin(IMove& g) {
        if (!Finite(dest_.target)) { stop_=true; Finish(State::Failed,"目的地坐标无效"); return; }
        Pose p; std::string lv;
        if (!g.Where(&p, &lv)) { Finish(State::Failed, "找不到玩家"); return; }
        level_ = !dest_.level.empty() ? dest_.level : !dest_.bell.empty() ? g.BellLevel(dest_.bell) : lv;
        if (level_.empty()) { Finish(State::Failed, "钟所在关卡未知：" + dest_.bell); return; }
        if (SameLevel(lv, level_)) {
            // 用户 2026-11-11 实机：上一场 Boss 打完人在雾门边（离同场地起点 9 米），影塔开层判「不在 6 米内」要求传送，
            // 这里却按到达半径 15 米直接判已到、没有传送 → 人留在雾门凸包外、入场准入失败、自动换到别的地图。
            // 出发前「已就位」用和开层一致的 6 米；15 米只用于传送之后的到达判定
            if (g.Ready() && AtSpot(p, dest_.target, kInPlace)) { Finish(State::Done, ""); return; }
            if (!g.Ready()) { state_ = State::Level; t_ = settle_ = 0; waitingCover_ = true; return; }  // 同关卡但遮罩未结束：按跨关卡等
            PointStep(g);
            return;
        }
        if (dest_.bell.empty() || !SameLevel(g.BellLevel(dest_.bell), level_)) { Finish(State::Failed, "不在目标关卡，也没有通往那里的钟"); return; }
        if (!g.Bell(dest_.bell, dest_.bellPose)) { Finish(State::Failed, "钟传送不可用"); return; }
        state_ = State::Level; t_ = settle_ = 0;
    }
    void PointStep(IMove& g) {
        std::string msg;
        std::string originLevel;
        havePointOrigin_=g.Where(&pointOrigin_,&originLevel) && SameLevel(originLevel,level_) && Finite(pointOrigin_);
        pointGroundConfirmed_=false;
        // 落点在默认不加载的剧情数据层里：先激活（已在目标关卡、遮罩已结束），激活不了就停，不悬空等 20 秒、不直接放
        if (!dest_.dataLayer.empty() && !g.EnsureLayer(dest_.dataLayer, &msg)) {
            stop_ = true;
            Finish(State::Failed, "落点所在数据层无法激活：" + (msg.empty() ? dest_.dataLayer : msg));
            return;
        }
        msg.clear();
        // 34 层力者冰面：运行库曾拒绝过这个落点 → 不再悬空 20 秒再传回原处，直接放；直接放不可用才走点传送
        if (dest_.directFirst && !direct_) {
            direct_ = true;
            if (g.Direct(dest_.target, &msg) && AcceptDirect(g)) { directUsed_ = true; state_ = State::Point; t_ = settle_ = 0; return; }
        }
        if (!g.Point(dest_.target, &msg)) {
            stop_ = msg.rfind(kNativeFault, 0) == 0;  // 原生异常：不重试（运行库已停用本地图落点查询）
            Finish(State::Failed, msg.empty() ? "点传送失败" : msg);
            return;
        }
        pointGroundConfirmed_=msg=="已传送"; // Core's synchronous surface-validated success; excludes the disabled-feature fallback.
        state_ = State::Point; t_ = settle_ = 0;
    }
    State Finish(State s, const std::string& why) { state_ = s; why_ = why; return s; }
    bool AcceptSurfaceHeight(IMove& g, const Pose& p) {
        // Only AFTER a native center's Core point teleport has finished. The
        // 37th-floor center is Z5860.2; the standing capsule is near Z5410.
        // Do not widen AtSpot for marked/home points or accept a remote floor.
        if (!dest_.resolveSurfaceHeight || directUsed_ || !Finite(p) ||
            DistXY(p,dest_.target)>300 || std::abs(p.z-dest_.target.z)>2000) return false;
        if (!pointGroundConfirmed_ && (!havePointOrigin_ || std::hypot(p.x-pointOrigin_.x,p.y-pointOrigin_.y,p.z-pointOrigin_.z)<30)) return false;
        if (!g.StandingAt(p)) return false;
        dest_.target.z=p.z;
        return true;
    }
    bool AcceptDirect(IMove& g) {
        Pose p; std::string lv;
        if (!g.Where(&p,&lv) || !SameLevel(lv,level_) || !Finite(p) || DistXY(p,dest_.target)>300 || std::abs(p.z-dest_.target.z)>2000) return false;
        dest_.target.z=p.z; // Direct has proved this capsule's actual landing surface.
        return true;
    }

    Dest dest_;
    Pose pointOrigin_; bool havePointOrigin_=false,pointGroundConfirmed_=false;
    State state_ = State::Idle;
    std::string level_, why_;
    bool started_ = false, retried_ = false, direct_ = false, directUsed_ = false, going_ = false, stop_ = false, waitingCover_ = false, direct2_ = false;
    double directFailAt_ = -1;
    std::string directWhy_;
    double t_ = 0, settle_ = 0;
};

// 进塔位置（出塔传回）：一行 "level bell x y z yaw"
struct Home { std::string level, bell; Pose at; bool valid = false; };
inline std::string FormatHome(const Home& h) { return std::format("{} {} {:.1f} {:.1f} {:.1f} {:.1f}\n", h.level, h.bell.empty() ? "-" : h.bell, h.at.x, h.at.y, h.at.z, h.at.yaw); }
inline Home ParseHome(const std::string& s) {
    Home h;
    std::istringstream in(s);
    if (in >> h.level >> h.bell >> h.at.x >> h.at.y >> h.at.z >> h.at.yaw) { if (h.bell == "-") h.bell.clear(); h.valid = true; }
    return h;
}
inline Dest HomeDest(const Home& h) {
    Dest d;
    d.name = "进塔位置";
    d.target = h.at;
    d.level = h.level;
    if (!h.bell.empty() && BellPose(h.bell, &d.bellPose)) d.bell = h.bell;
    return d;
}

}  // namespace tower::travel
