// 固定场地运行时（tower_fixed_venue.h 的条目，例如榕树内场）。与 tower_venue.cpp（v1.3.5 Boss 原生契约，基线锁定）分开：
// 到场 → 分帧扫对象找参考刷怪点和雾门（最多 10 秒，高空独立空间找不到就不开战）→ 参考刷怪点 80 米内的原生刷怪点全部交给会话隔离静默
// → 读秒前关雾门、结束开门。原生刷怪点只当位置参考，不调用 ClaimSeqSpawn。全部在游戏线程调用。
#include "tower_venue_fixed.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <unordered_map>
#include <vector>

#include "../../../src/core/host/log.h"
#include "../../../src/core/ipc/pipe.h"
#include "../../../src/core/sdk/pbz_module.h"
#include "../../../src/core/services/process_event.h"
#include "../../../src/core/services/ue.h"
#include "tower_fixed_venue.h"
#include "tower_isolation.h"
#include "tower_perf.h"

namespace towerfixed {
namespace {
namespace ue = pbz::ue;
namespace pe = pbz::pe;
using ue::UObject;
using tower::arena::P3;

const tower::FixedVenue* g_def = nullptr;
bool g_ready = false, g_found = false, g_closed = false, g_timedOut = false;
uint64_t g_since = 0, g_nextScan = 0;
UObject* g_spawner = nullptr;
std::vector<UObject*> g_doors;
P3 g_at{}, g_offset{};
int g_silenced = 0;
std::string g_last;

struct Pass { bool on = false; int cursor = 0; std::vector<UObject*> spawners, doors; std::unordered_map<UObject*, bool> isSpawner; };
Pass g_pass;
constexpr int kStep = 8192;

UObject* Fn(UObject* o, const char* name) { return o && ue::Live(o) ? pe::FindFunction(ue::ClassOf(o), name) : nullptr; }
bool Loc(UObject* a, P3* out) {
    auto* f = Fn(a, "K2_GetActorLocation");
    const int ret = f ? ue::PropOffset(f, "ReturnValue") : -1;
    if (ret < 0) return false;
    uint8_t buf[64] = {};
    pe::Call(a, f, buf);
    double v[3]; std::memcpy(v, buf + ret, 24);
    if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) return false;
    *out = {v[0], v[1], v[2]};
    return true;
}
void Call0(UObject* o, const char* name) { if (auto* f = Fn(o, name)) { uint8_t buf[64] = {}; pe::Call(o, f, buf); } }

bool ScanStep() {
    towerperf::Section _("固定场地对象扫描");
    auto* spCls = towerperf::ClassNamed("AISpawner");
    auto* doorCls = towerperf::ClassNamed("BP_FogDoor_C");
    if (!g_pass.on) { g_pass = {}; g_pass.on = true; }
    static UObject* batch[kStep];
    const int got = ue::ObjBatch(g_pass.cursor, kStep, batch);
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
    g_pass.cursor += got ? got : kStep;
    if (g_pass.cursor < ue::Num()) return false;
    g_pass.on = false;
    return true;
}

void Scan() {
    UObject* sp = nullptr;
    for (auto* s : g_pass.spawners) if (ue::Live(s) && ue::ObjName(s) == g_def->spawner) { sp = s; break; }
    if (!sp) return;
    P3 at{};
    if (!Loc(sp, &at)) return;
    g_spawner = sp; g_at = at;
    g_offset = {at.x - g_def->sx, at.y - g_def->sy, at.z - g_def->sz};
    for (auto* d : g_pass.doors) if (ue::Live(d) && ue::ObjName(d) == g_def->door) { g_doors.push_back(d); break; }
    for (auto* s : g_pass.spawners) {
        P3 p;
        if (!ue::Live(s) || !Loc(s, &p)) continue;
        if (std::hypot(p.x - at.x, p.y - at.y) <= 8000 && std::abs(p.z - at.z) <= 3000) { towerisolate::Quiet(s); ++g_silenced; }
    }
    g_found = true;
    g_last = std::format("{}：参考刷怪点已找到（偏移 {:.0f} / {:.0f} / {:.0f}），雾门 {}/1，静默刷怪点 {} 个", g_def->name, g_offset.x, g_offset.y, g_offset.z,
                         g_doors.size(), g_silenced);
    pbz::log::Info("影塔固定场地：{}", g_last);
}
}  // namespace

bool Is(std::string_view path) { return tower::FindFixedVenue(path) != nullptr; }
bool Active() { return g_def != nullptr; }

void Arrive(std::string_view path) {
    End();
    g_def = tower::FindFixedVenue(path);
    if (!g_def) return;
    g_since = GetTickCount64();
    pbz::log::Info("影塔固定场地：到场 {}（{}），找参考刷怪点 {}", g_def->name, g_def->world, g_def->spawner);
}

void Tick() {
    if (!g_def || g_ready) return;
    const uint64_t now = GetTickCount64();
    if (!g_pass.on && now < g_nextScan) return;
    if (!ScanStep()) return;
    g_nextScan = now + 500;
    Scan();
    if (g_found) { g_ready = true; return; }
    if (now - g_since > 10000) {
        g_ready = g_timedOut = true;
        g_last = std::string(g_def->name) + "：10 秒内没找到参考刷怪点，场地未加载，本层不开战";
        pbz::log::Info("影塔固定场地：{}", g_last);
    }
}

bool Ready() { return !g_def || g_ready; }
bool Found() { return g_found; }
bool MissingRequired() { return g_def && g_timedOut && !g_found; }
void* Spawner() { return g_found && g_spawner && ue::Live(g_spawner) ? g_spawner : nullptr; }

bool CloseDoors() {
    if (g_closed) return true;
    int n = 0;
    for (auto* d : g_doors) if (ue::Live(d)) { Call0(d, "Close By Mob Script"); ++n; }
    g_closed = n > 0;
    if (n) pbz::log::Info("影塔固定场地：关雾门 {} 扇", n);
    return true;  // 没找到雾门也开打：边界由影塔的圆负责
}
bool DoorsClosed() {
    if (!g_closed) return false;
    for (auto* d : g_doors) if (ue::Live(d)) return true;
    return false;
}
void OpenDoors() {
    if (!g_closed) return;
    int n = 0;
    for (auto* d : g_doors) if (ue::Live(d)) { Call0(d, "OpenByMobScript"); ++n; }
    g_closed = false;
    pbz::log::Info("影塔固定场地：开雾门 {} 扇", n);
}

bool Boundary(std::vector<P3>* hull, P3* center, double* radius, double* zmin, double* zmax) {
    hull->clear();
    if (!g_def || !g_found) return false;
    *center = {g_def->cx + g_offset.x, g_def->cy + g_offset.y, g_def->pz + g_offset.z};
    *radius = g_def->radius;
    *zmin = std::min(g_def->zmin, g_def->sz) + g_offset.z - 800;
    *zmax = std::max(g_def->zmax, g_def->pz) + g_offset.z + 1500;
    return true;
}

void End() {
    OpenDoors();
    g_def = nullptr;
    g_ready = g_found = g_closed = g_timedOut = false;
    g_spawner = nullptr; g_doors.clear(); g_silenced = 0; g_pass = {};
}

std::string Status() {
    return std::format(R"("fixed_venue":{{"active":{},"found":{},"doors":{},"closed":{},"silenced":{},"last":"{}"}})", g_def != nullptr, g_found, g_doors.size(),
                       g_closed, g_silenced, pbz::ipc::Esc(g_last));
}

}  // namespace towerfixed
