// 落地 / 同侧可达探测（1–32 层 C 类剩余部分：11 / 26 无地面、13 隔墙、7 / 25 高空悬空 / 虚空）。
// 读秒前准入时调用一次：玩家脚下要有地面；玩家和 Boss 之间"射线被挡"并且"导航路径不完整"两样都成立才判隔墙不可达。
// 只有一样（例如导航数据缺失、柱子挡了射线）只记日志不拒绝——单一信号误判太多（飞行 Boss、柱上对抗、高空空间没有导航网格）。
// 射线：KismetSystemLibrary.LineTraceSingleForObjects（WorldStatic + WorldDynamic），导航：NavigationSystemV1.FindPathToLocationSynchronously。
#pragma once
#include <string>
#include <cmath>

namespace towerreach {

struct Probe {
    bool groundKnown = false, ground = false;  // 玩家脚下 20 米内有静态 / 动态地面
    double groundGap = 0;                       // 脚底到地面的距离（厘米）
    bool lineKnown = false, lineBlocked = false;  // 胸口高度的射线，在离 Boss 2 米以外被挡
    bool navKnown = false, navOk = false;          // 有导航数据且路径完整（不是 partial）
    double capsuleHalfHeight=0,traceGap=0,traceTime=-1,nativeGap=0;
    bool nativeKnown=false,nativeSupport=false;
    bool traceOverlapKnown=false,traceOverlap=false;
    std::string groundSource,groundComponent;
};

enum class Verdict { Pass, NoGround, Walled };
// 纯规则（离线测试）
inline Verdict Decide(const Probe& p) {
    if (p.groundKnown && (!p.ground || !std::isfinite(p.groundGap) || std::abs(p.groundGap)>60)) return Verdict::NoGround;
    if (p.lineKnown && p.lineBlocked && p.navKnown && !p.navOk) return Verdict::Walled;
    return Verdict::Pass;
}
inline const char* Why(Verdict v) {
    switch (v) {
        case Verdict::NoGround: return "玩家脚下没有近距离可站立支撑（虚空 / 悬空 / 流送未就绪；远处有地面也不读秒）";
        case Verdict::Walled: return "玩家和 Boss 之间被墙隔开（射线被挡且导航不可达）";
        default: return "";
    }
}

// 游戏线程：player / boss 是 Actor；at 是它们的位置（厘米）
Probe Measure(void* player, const double playerAt[3], void* boss, const double bossAt[3]);
std::string Describe(const Probe& p);
// Remote landing proof. Never substitutes the player's cached CurrentFloor.
// Resolves a nearby walkable surface and a clear capsule before any actor move.
// trustDynamic：落点曾在实机直接放置成功（venue-direct.txt）。断魂冰面这类地方 WorldDynamic 体积处处初始重叠，
// 只有动态通道挡住时照放；静态通道挡住仍拒绝。先试三档高度（+0 / +60 / +120 厘米）
bool Landing(void* actor, const double nearAt[3], double out[3], std::string* why, bool trustDynamic = false);
bool StopMotion(void* actor); // Clear retained velocity through the native movement API.
bool StandingAt(void* actor, const double at[3]); // Current native floor only; no trace or relocation.

// ---- 生成前地面审计（33–38 层：36 / 37 / 38 在毕峰 / 玉面 / 蛊鹤高空场地，Core 落点 36 个候选全记"无地面"，而玩家明明站在出生点高度）。
// 复刻 Core 落点第一圈几何（玩家周围 distance 厘米、12 个方向），每个候选点按 Core 的窗口（玩家中心 +250 到脚下 -250）分别打：
//   只查 WorldStatic（Core 现行规则）/ 只查 WorldDynamic；读取真实 bStartPenetrating 位。
// Time==0 本身不证明起点在实体里；另一通道的重叠不能覆盖有效地面。
// 只读、不改 Core 规则；结论用来给失败分类，不用来放宽安全检查。
struct FloorAudit {
    int n = 0, staticHits = 0, dynamicHits = 0, startSolid = 0, none = 0;
    int staticOverlap = 0, dynamicOverlap = 0, unresolved = 0;
    bool nativePlayerSupport = false;
    double nativePlayerGap = 0;
    std::string playerOn;   // 玩家脚下命中的类型 + 组件类（"WorldStatic:StaticMeshComponent" / "WorldDynamic:…" / "无"）
    std::string sample;     // 前 3 个候选点的明细
};
enum class FloorVerdict { Ok, NotStatic, StartInSolid, NoFloor, Unknown };
// 纯规则（离线测试）
inline FloorVerdict Classify(const FloorAudit& a) {
    if (a.n == 0) return FloorVerdict::Unknown;
    if (a.staticHits * 3 >= a.n) return FloorVerdict::Ok;
    if (a.dynamicHits * 2 >= a.n) return FloorVerdict::NotStatic;
    // A ray starting inside a volume does not prove that the standing player
    // or all candidates are embedded in solid geometry. Preserve uncertainty.
    if (a.unresolved || (a.startSolid && a.nativePlayerSupport)) return FloorVerdict::Unknown;
    if (a.startSolid * 2 >= a.n) return FloorVerdict::StartInSolid;
    return FloorVerdict::NoFloor;
}
// 运行库落点在这里一定会全被拒（候选点两条通道都没有任何有效支撑，且多数起点重叠），而玩家本人站得稳：
// 不用再同位置重试，直接交给 Boss 原生刷怪点兜底。只用于"要不要立刻走原生刷怪点"，不用于判场地坏 / 判虚空。
// 42 层蛊鹤 e703aba：Static/Dynamic 有效命中 0、初始重叠 12/12、玩家原生支撑 2.1 厘米 → 旧规则判 Unknown，白等 6 次约 11 秒
inline bool CoreWillRefuse(const FloorAudit& a) {
    return a.n > 0 && a.staticHits == 0 && a.dynamicHits == 0 && a.startSolid * 2 >= a.n && a.nativePlayerSupport;
}
inline const char* Why(FloorVerdict v) {
    switch (v) {
        case FloorVerdict::NotStatic: return "场地地面不是 WorldStatic（Core 落点只认 WorldStatic）";
        case FloorVerdict::StartInSolid: return "落点射线起点在实体里（低矮顶棚 / 平台夹层）";
        case FloorVerdict::NoFloor: return "玩家周围这一圈确实没有地面（边缘 / 悬空）";
        case FloorVerdict::Unknown: return "候选支撑未证实（射线起点重叠/字段不全）；玩家原生支撑另列，不据此判虚空或改Core落点规则";
        default: return "";
    }
}
FloorAudit Audit(void* player, const double playerAt[3], double halfHeight, double distanceCm);
// 3.6.0 场地实测（测试包 qa_measure）：只读射线。
// Ray：from → to 之间第一个 WorldStatic / WorldDynamic 有效命中（不算起点在实体里），返回是否命中；dist = 从 from 起的厘米数
bool Ray(void* ctx, const double from[3], const double to[3], double* dist, std::string* comp);
// GroundBelow：(x, y) 处从 zTop 往下到 zBottom 的第一个有效地面（不在实体里、法线朝上），out = 命中点
bool GroundBelow(void* ctx, double x, double y, double zTop, double zBottom, double out[3]);
std::string GroundDiag(void* ctx, double x, double y, double zTop, double zBottom);  // 两通道明细（日志用）
std::string Describe(const FloorAudit& a);

}  // namespace towerreach
