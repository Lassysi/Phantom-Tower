// 场地运行时（见 tower_venue.h）。流程：到场 → 按名字找原生出生点 / 雾门（每 0.5 秒扫一次，最多 10 秒）→
// 量出生点"运行时位置 − 静态坐标"的偏移（凸包按它平移；多数条目只有 level 坐标）→ 静默出生点 80 米内的全部原生刷怪点
// （ShouldSpawn=false、CancelPendingSpawnCompletely、所属 MobGroup 加清组阻断并 Deactivate）→ 读秒时关门、结束时开门。
// 雾门函数来自 SDK：BP_FogDoor_C.Close By Mob Script / OpenByMobScript（原生 MobGroupScript_LinkFogDoor 也这样开关门）。
#include "tower_venue.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../../src/core/host/log.h"
#include "../../../src/core/ipc/pipe.h"
#include "../../../src/core/sdk/pbz_module.h"
#include "../../../src/core/services/process_event.h"
#include "../../../src/core/services/ue.h"
#include "tower_isolation.h"
#include "tower_perf.h"
#include "tower_widgets.h"
#include "venues.generated.h"

namespace towervenue {
namespace {
namespace ue = pbz::ue;
namespace pe = pbz::pe;
using ue::UObject;
using tower::arena::P3;

const tower::VenueDef* g_def = nullptr;
bool g_active = false, g_ready = false, g_found = false, g_closed = false;
uint64_t g_since = 0, g_nextScan = 0;
UObject* g_spawner = nullptr;
std::vector<UObject*> g_mates;  // 同组刷怪点
std::vector<UObject*> g_doors, g_groups;
P3 g_at{}, g_offset{};
int g_silenced = 0;
std::string g_last;

// F36-WALL-001: the generated Ximeng cell contains a small AI-only cage.
// Live 26367565: seven spline bodies, object channel26, block AIPawn19 but
// ignore Pawn2. Solo Tower spawning did not account for this native cage.
// Keep the outer fog doors, and borrow only this evidenced actor's collision.
constexpr std::string_view kCageBoss = "/PBZCombatAsset/ActorData/Mobs/YuDing_00/BP_Monster_FemaleVessel_YuDing.BP_Monster_FemaleVessel_YuDing_C";
constexpr std::string_view kCageActor = "BP_AIWall_C_UAID_BCFCE704F7254EEC02_1711908456";
struct WallLease { UObject* object; UObject* cls; UObject* outer; int index; uint64_t name; bool original; };
std::optional<WallLease> g_wall;

UObject* Fn(UObject* o, const char* name) { return o && ue::Live(o) ? pe::FindFunction(ue::ClassOf(o), name) : nullptr; }
bool Loc(UObject* a, P3* out) {
    auto* f = Fn(a, "K2_GetActorLocation");
    const int ret = f ? ue::PropOffset(f, "ReturnValue") : -1;
    if (ret < 0) return false;
    uint8_t buf[64] = {};
    pe::Call(a, f, buf);
    double v[3]; std::memcpy(v, buf + ret, 24);
    *out = {v[0], v[1], v[2]};
    return true;
}
void Call0(UObject* o, const char* name) { if (auto* f = Fn(o, name)) { uint8_t buf[64] = {}; pe::Call(o, f, buf); } }

bool SameWall(const WallLease& w) {
    return ue::Live(w.object) && ue::ClassOf(w.object) == w.cls &&
        ue::Get<UObject*>(w.object + ue::Layout::ObjOuter) == w.outer &&
        ue::Get<int>(w.object + ue::Layout::ObjIndex, -1) == w.index &&
        ue::Get<uint64_t>(w.object + ue::Layout::ObjName) == w.name && ue::ObjName(w.object) == kCageActor;
}
bool CollisionAPI(UObject* o, UObject** get, int* ret, UObject** set, int* input) {
    *get = Fn(o, "GetActorEnableCollision"); *set = Fn(o, "SetActorEnableCollision");
    *ret = *get ? ue::PropOffset(*get, "ReturnValue") : -1;
    *input = *set ? ue::PropOffset(*set, "bNewActorEnableCollision") : -1;
    const int gs = *get ? ue::Get<int>(*get + ue::Layout::StructSize) : 0;
    const int ss = *set ? ue::Get<int>(*set + ue::Layout::StructSize) : 0;
    return *ret >= 0 && *input >= 0 && gs > *ret && ss > *input && gs <= 64 && ss <= 64;
}
bool Collision(UObject* o, bool* value, std::optional<bool> want = {}) {
    UObject *get, *set; int ret, input;
    if (!CollisionAPI(o, &get, &ret, &set, &input)) return false;
    uint8_t args[64] = {};
    if (want.has_value()) { args[input] = *want ? 1 : 0; pe::Call(o, set, args); std::memset(args, 0, sizeof args); }
    pe::Call(o, get, args); *value = args[ret] != 0;
    return !want.has_value() || *value == *want;
}
bool RestoreWall() {
    if (!g_wall) return true;
    if (!SameWall(*g_wall)) {
        pbz::log::Info("影塔场地内墙：原对象已失效/身份变化，跳过恢复，不触碰复用地址");
        g_wall.reset(); return true;
    }
    bool value = false;
    if (!Collision(g_wall->object, &value, g_wall->original)) {
        pbz::log::Info("影塔场地内墙：恢复校验失败，保留恢复记录"); return false;
    }
    pbz::log::Info("影塔场地内墙：已恢复 {} collision={}", kCageActor, value);
    g_wall.reset(); return true;
}
bool PrepareWall() {
    if (g_wall) {
        bool current = true;
        if (g_def && g_def->path == kCageBoss && SameWall(*g_wall) && Collision(g_wall->object, &current) && !current) return true;
        if (!RestoreWall()) return false;
    }
    if (!g_def || g_def->path != kCageBoss) return true;
    std::vector<UObject*> found;
    ue::ForEach([&](int, UObject* o) {
        if (ue::IsDefault(o) || !ue::Live(o) || ue::ObjName(o) != kCageActor || ue::ClassName(o) != "BP_AIWall_C") return true;
        auto* at = o;
        for (int depth = 0; at && depth < 16; ++depth) {
            if (ue::ObjName(at) == "PBZ_Ximeng_WP") { found.push_back(o); break; }
            at = ue::Get<UObject*>(at + ue::Layout::ObjOuter);
        }
        return true;
    });
    bool original = true;
    if (found.size() != 1 || !Collision(found[0], &original)) {
        g_last = "玉鼎场地内墙对象/碰撞接口未就绪，拒绝生成和读秒";
        pbz::log::Info("影塔场地内墙：{}（匹配 {} 个）", g_last, found.size()); return false;
    }
    if (!original) { pbz::log::Info("影塔场地内墙：{} 原本已关闭，不占用恢复记录", kCageActor); return true; }
    auto* o = found[0];
    g_wall = WallLease{o, ue::ClassOf(o), ue::Get<UObject*>(o + ue::Layout::ObjOuter), ue::Get<int>(o + ue::Layout::ObjIndex, -1),
                       ue::Get<uint64_t>(o + ue::Layout::ObjName), original};
    bool after = true;
    if (!Collision(o, &after, false)) {
        g_last = "玉鼎场地内墙解除未通过读回校验，拒绝生成和读秒";
        pbz::log::Info("影塔场地内墙：{}", g_last); RestoreWall(); return false;
    }
    pbz::log::Info("影塔场地内墙：临时解除 {} collision=true->false（仅本场地，外围雾门保留）", kCageActor);
    return true;
}

// 静默一个原生刷怪点：交给会话隔离（先收组：委托 / 组脚本 / 清组阻断，再停刷 / 取消待刷、停用组；阻断出塔事务开始才撤）。
// 旧顺序是先 CancelPendingSpawnCompletely 再阻断：组判定清空 → 开场"斩"+ 原生兵器 / 道具（1–32 层 14 / 15 / 17 / 29 / 31）
void Silence(UObject* sp) {
    const int go = ue::PropOffset(ue::ClassOf(sp), "MobGroup");
    UObject* g = go >= 0 ? ue::Get<UObject*>(sp + go) : nullptr;
    if (g && ue::Live(g) && std::find(g_groups.begin(), g_groups.end(), g) == g_groups.end()) g_groups.push_back(g);
    towerisolate::Quiet(sp);
    ++g_silenced;
}

// 1.3.0（性能取证构建实测：到场整表扫描每次 50–156 ms，找不到出生点时每 500 ms 再卡一次）：改为分帧扫描，
// 每帧 8192 个对象（约 2–3 ms），一整轮扫完再统一判定。每个类只判一次是不是刷怪点子类
struct ScanPass { bool on = false; int cursor = 0; std::vector<UObject*> spawners, doors; std::unordered_map<UObject*, bool> isSpawner; };
ScanPass g_pass;
constexpr int kScanStep = 8192;
bool ScanStep() {  // true = 一整轮扫完（结果在 g_pass.spawners / doors）
    towerperf::Section _("场地对象扫描");
    auto* spCls = towerperf::ClassNamed("AISpawner");
    auto* doorCls = towerperf::ClassNamed("BP_FogDoor_C");
    if (!g_pass.on) { g_pass = {}; g_pass.on = true; }
    static UObject* batch[kScanStep];
    const int got = ue::ObjBatch(g_pass.cursor, kScanStep, batch);
    for (int i = 0; i < got; ++i) {
        UObject* o = batch[i];
        UObject* c = o ? ue::ClassOf(o) : nullptr;
        if (!c) continue;
        if (c == doorCls) { if (!ue::IsDefault(o) && ue::Live(o)) g_pass.doors.push_back(o); continue; }
        if (!spCls) continue;
        auto [it, fresh] = g_pass.isSpawner.try_emplace(c, false);
        if (fresh) it->second = ue::IsA(o, spCls);
        if (it->second && !ue::IsDefault(o) && ue::Live(o)) g_pass.spawners.push_back(o);
    }
    g_pass.cursor += got ? got : kScanStep;
    if (g_pass.cursor < ue::Num()) return false;
    g_pass.on = false;
    return true;
}

void Scan() {
    std::vector<UObject*> spawners, doors;
    for (auto* o : g_pass.spawners) if (ue::Live(o)) spawners.push_back(o);  // 分帧期间可能有对象被销毁
    for (auto* o : g_pass.doors) if (ue::Live(o)) doors.push_back(o);
    for (auto* s : spawners) if (ue::ObjName(s) == g_def->spawner) { g_spawner = s; break; }
    g_mates.clear();
    if (g_spawner) {
        const int go = ue::PropOffset(ue::ClassOf(g_spawner), "MobGroup");
        UObject* grp = go >= 0 ? ue::Get<UObject*>(g_spawner + go) : nullptr;
        if (grp) for (auto* s : spawners) {
            if (s == g_spawner) continue;
            const int o = ue::PropOffset(ue::ClassOf(s), "MobGroup");
            if (o >= 0 && ue::Get<UObject*>(s + o) == grp) g_mates.push_back(s);
        }
    }
    if (!g_spawner) return;
    g_found = true;
    if (!Loc(g_spawner, &g_at)) g_at = {g_def->sx, g_def->sy, g_def->sz};
    g_offset = {g_at.x - g_def->sx, g_at.y - g_def->sy, g_at.z - g_def->sz};
    for (int i = 0; i < g_def->doorN; ++i) {
        const auto name = tower::kVenueDoors[g_def->doorAt + i];
        for (auto* d : doors) if (ue::ObjName(d) == name) { g_doors.push_back(d); break; }
    }
    // 出生点 80 米内（高差 30 米内）的原生刷怪点全部静默：本场地的 Boss 组和周边串场的遭遇（32ffb7d：白赤练 / 雷当 / 人偶）
    for (auto* s : spawners) {
        P3 p;
        if (!Loc(s, &p)) continue;
        if (std::hypot(p.x - g_at.x, p.y - g_at.y) <= 8000 && std::abs(p.z - g_at.z) <= 3000) Silence(s);
    }
    g_last = std::format("出生点已找到（偏移 {:.0f} / {:.0f} / {:.0f}），雾门 {}/{}，静默刷怪点 {} 个、战斗组 {} 个", g_offset.x, g_offset.y, g_offset.z,
                         g_doors.size(), g_def->doorN, g_silenced, g_groups.size());
    pbz::log::Info("影塔场地：{} {}", ue::ObjName(g_spawner), g_last);
}

}  // namespace

void Arrive(std::string_view bossPath) {
    End();
    g_def = nullptr;
    for (auto& v : tower::kVenues) if (v.path == bossPath) { g_def = &v; break; }
    g_active = true;
    g_since = GetTickCount64();
    g_nextScan = 0;
    g_ready = !g_def;  // 没有场地数据：不用等
    if (!g_def) { g_last = "没有场地数据（venues.json 未收录这只 Boss）"; pbz::log::Info("影塔场地：{}", g_last); }
    else pbz::log::Info("影塔场地：到场，开始找原生出生点 {}（{}{}）", g_def->spawner, g_def->world, g_def->sky ? "，高空独立空间" : "");
}

void Tick() {
    if (!g_active || g_ready || !g_def) return;
    const uint64_t now = GetTickCount64();
    if (!g_pass.on && now < g_nextScan) return;
    if (!ScanStep()) return;  // 一整轮还没扫完：下一帧接着扫
    g_nextScan = now + 500;
    Scan();
    if (g_found) { g_ready = true; return; }
    if (now - g_since > 10000) {
        g_ready = true;
        g_last = g_def->sky ? "10 秒内没找到高空场地原生出生点：场地未确认，本层不开战" : "10 秒内没找到原生出生点：场地边界数据不可用";
        pbz::log::Info("影塔场地：{}", g_last);
    }
}

bool Ready() { return !g_active || g_ready; }
bool Found() { return g_found; }
bool MissingRequired() { return g_active && g_ready && g_def && g_def->sky && !g_found; }
void* Spawner() { return g_found && g_spawner && ue::Live(g_spawner) ? g_spawner : nullptr; }
std::vector<void*> GroupMates() {
    std::vector<void*> v;
    if (g_found) for (auto* s : g_mates) if (ue::Live(s)) v.push_back(s);
    return v;
}

// 3.0.0（用户反馈"雾门锁不住"）：关门后约 1 秒记下每扇门的 bDesiredState（关好的状态），之后每秒核对一次；
// 原生脚本（换波 / 组清空 / 剧情）把门改回去就再关一次并记日志。只在本层关过门时巡检
std::vector<std::pair<UObject*, int>> g_doorWant;  // 门, 关好后的 bDesiredState（-1 = 还没记）
uint64_t g_doorClosedAt = 0, g_doorNextCheck = 0;
int g_doorReclosed = 0;
int DoorState(UObject* d) {
    const int o = ue::PropOffset(ue::ClassOf(d), "bDesiredState");
    return o >= 0 ? (ue::Get<uint8_t>(d + o, 0) ? 1 : 0) : -1;
}
bool CloseDoors() {
    if (g_closed) return true;
    if (!PrepareWall()) return false;
    int n = 0;
    g_doorWant.clear();
    for (auto* d : g_doors) if (ue::Live(d)) { Call0(d, "Close By Mob Script"); ++n; g_doorWant.push_back({d, -1}); }
    g_closed = n > 0;
    g_doorClosedAt = GetTickCount64(); g_doorNextCheck = g_doorClosedAt + 1000; g_doorReclosed = 0;
    if (n) pbz::log::Info("影塔场地：关雾门 {} 扇", n);
    return true;
}
void KeepDoorsClosed() {
    if (!g_closed || g_doorWant.empty()) return;
    const uint64_t now = GetTickCount64();
    if (now < g_doorNextCheck) return;
    g_doorNextCheck = now + 1000;
    for (auto& [d, want] : g_doorWant) {
        if (!ue::Live(d)) continue;
        const int s = DoorState(d);
        if (s < 0) continue;
        if (want < 0) { want = s; continue; }  // 关门 1 秒后的状态 = 关好
        if (s == want) continue;
        Call0(d, "Close By Mob Script");
        if (++g_doorReclosed <= 20) pbz::log::Info("影塔场地：雾门 {} 被原生改回打开，已重新关上（第 {} 次）", ue::ObjName(d), g_doorReclosed);
    }
}
bool DoorsClosed() {
    if (!g_closed) return false;
    for (auto* d : g_doors) if (ue::Live(d)) return true;
    return false;
}
void OpenDoors() {
    RestoreWall();  // Also restore when no fog door was closed or entry failed.
    g_doorWant.clear();
    if (!g_closed) return;
    int n = 0;
    for (auto* d : g_doors) if (ue::Live(d)) { Call0(d, "OpenByMobScript"); ++n; }
    g_closed = false;
    pbz::log::Info("影塔场地：开雾门 {} 扇", n);
}

bool Boundary(std::vector<P3>* hull, P3* center, double* radius, double* zmin, double* zmax) {
    hull->clear();
    if (!g_def || !g_found) return false;
    *center = g_at;
    *radius = g_def->reach > 0 ? std::clamp(g_def->reach * 1.2, 2500.0, 4500.0) : 3500.0;
    *zmin = std::min(g_def->zmin, g_def->sz) + g_offset.z - 800;
    *zmax = std::max(g_def->zmax, g_def->sz) + g_offset.z + 1500;
    // 凸包是真正围起来的场地（面积够、出生点在里面）才用；单扇门的弧线围不出场地，用圆
    if (g_def->hullN >= 3 && g_def->area >= 600) {
        for (int i = 0; i < g_def->hullN; ++i)
            hull->push_back({tower::kVenueHull[(g_def->hullAt + i) * 2] + g_offset.x, tower::kVenueHull[(g_def->hullAt + i) * 2 + 1] + g_offset.y, 0});
        if (!tower::arena::InHull(*hull, g_at)) hull->clear();
    }
    return true;
}

void End() {
    OpenDoors();  // 清组阻断留在会话层（取消 / 换层后原生组也不能补发清组结果）
    g_active = g_ready = g_found = g_closed = false;
    g_spawner = nullptr; g_mates.clear(); g_doors.clear(); g_groups.clear(); g_silenced = 0;
    g_pass = {};
}

std::string Status() {
    return std::format(R"("venue":{{"active":{},"found":{},"doors":{},"closed":{},"silenced":{},"last":"{}"}})", g_active, g_found, g_doors.size(), g_closed, g_silenced,
                       pbz::ipc::Esc(g_last));
}

}  // namespace towervenue
