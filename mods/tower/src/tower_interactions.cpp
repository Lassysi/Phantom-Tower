// Player world-interaction filter; no actor deletion and no quest-state writes.
// Boss-owned interactions, bells and native traversal volumes remain available.
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <vector>
#include <unordered_set>
#include "../../../src/core/sdk/pbz_module.h"
#include "../../../src/core/host/log.h"
#include "tower_interactions.h"
#include "tower_widgets.h"
#include "tower_perf.h"

namespace towerinteractions {
namespace ue = pbz::ue;
namespace pe = pbz::pe;
using ue::UObject;
namespace {
bool g_active = false;
UObject* g_class = nullptr;
std::vector<UObject*> g_functions;
std::unordered_set<UObject*> g_logged;
std::vector<std::string> g_allowNames;
std::unordered_set<UObject*> g_allowActors;
bool Allowed(UObject* actor) {
    if (!actor) return true;
    if (!ue::Live(actor)) return false;
    if (!g_allowNames.empty()) {
        const std::string n = ue::ObjName(actor);
        for (auto& a : g_allowNames) if (a == n) return true;
    }
    for (const char* name : {"MelodiousBell", "SpecialMoveVolumeBase"})
        if (auto* c = towerperf::ClassNamed(name); c && ue::IsA(actor, c)) return true;
    const auto owned = pbz::enemies::OwnedActors();
    auto* current = actor;
    const int owner = ue::PropOffset(towerperf::ClassNamed("Actor"), "Owner");
    const int instigator = ue::PropOffset(towerperf::ClassNamed("Actor"), "Instigator");
    for (int depth = 0; current && depth < 12 && ue::Live(current); ++depth) {
        if (g_allowActors.count(current) || std::find(owned.begin(), owned.end(), current) != owned.end()) return true;
        if (instigator >= 0) {
            auto* source = ue::Get<UObject*>(current + instigator);
            if (source && ue::Live(source) && (g_allowActors.count(source) || std::find(owned.begin(), owned.end(), source) != owned.end())) return true;
        }
        if (owner < 0) break;
        auto* next = ue::Get<UObject*>(current + owner);
        if (next == current) break;
        current = next;
    }
    return false;
}
bool PlayerComponent(UObject* component) {
    return component && ue::Live(component) && ue::Get<UObject*>(component + ue::Layout::ObjOuter) == towerwidgets::Pawn();
}
void Record(UObject* actor) {
    if (!actor || !ue::Live(actor) || !g_logged.insert(actor).second) return;
    pbz::log::Info("影塔交互隔离：屏蔽原场地交互 actor={} class={} wall_ms={}", ue::ObjName(actor), ue::ClassName(actor), GetTickCount64());
}
void Install() {
    auto* cls = towerperf::ClassNamed("PBZInteractComponent");
    if (!cls || cls == g_class) return;
    // 类首次加载 / 换图重挂只换钩子，不能丢掉已设好的遭遇权限。
    for (auto* f : g_functions) pe::RemoveIntercept(f);
    g_functions.clear(); g_logged.clear(); g_active = true; g_class = cls;
    for (const char* name : {"SetCurrentInteractActor", "StartFlowFadeInteract", "ShouldDisplayScreenOutActor"}) {
        auto* f = pe::FindFunction(cls, name); if (!f) continue;
        const int target = ue::PropOffset(f, "Actor"), ret = ue::PropOffset(f, "ReturnValue");
        if (target < 0) continue;
        pe::Intercept(f, [target, ret](UObject* self, void* raw) {
            if (!g_active || !PlayerComponent(self)) return false;
            auto* p = static_cast<uint8_t*>(raw); auto* actor = ue::Get<UObject*>(p + target);
            if (Allowed(actor)) return false;
            Record(actor); if (ret >= 0) p[ret] = 0; return true;
        }); g_functions.push_back(f);
    }
    for (const char* name : {"StartInteract", "StartFadeInteract"}) {
        auto* f = pe::FindFunction(cls, name); const int current = ue::PropOffset(cls, "CurrentInteractActor");
        if (!f || current < 0) continue;
        const int ret = ue::PropOffset(f, "ReturnValue");
        pe::Intercept(f, [current, ret](UObject* self, void* raw) {
            if (!g_active || !PlayerComponent(self)) return false;
            auto* actor = ue::Get<UObject*>(self + current);
            if (Allowed(actor)) return false;
            Record(actor); if (ret >= 0) static_cast<uint8_t*>(raw)[ret] = 0; return true;
        }); g_functions.push_back(f);
    }
    pbz::log::Info("影塔交互隔离：挂接 {} 个反射入口（原生直接调用覆盖仍待实机核验）", g_functions.size());
}
}
void Begin() { End(); g_active = true; Install(); }
void End() { g_active = false; for (auto* f : g_functions) pe::RemoveIntercept(f); g_functions.clear(); g_logged.clear(); g_class = nullptr; g_allowNames.clear(); g_allowActors.clear(); }
void Tick() { if (g_active) Install(); }
void AllowNames(const std::vector<std::string>& actorNames) { g_allowNames = actorNames; }
void AllowActors(const std::vector<UObject*>& actors) { g_allowActors = std::unordered_set<UObject*>(actors.begin(), actors.end()); }
} // namespace towerinteractions
