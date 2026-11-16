#include "tower_attack_speed.h"

#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include "../../../src/core/sdk/pbz_module.h"
#include "../../../src/core/host/log.h"
#include "tower_perf.h"

namespace towerspeed {
namespace {
namespace ue = pbz::ue; namespace pe = pbz::pe;
using ue::UObject;
UObject *taskClass = nullptr, *setFn = nullptr, *lightClass = nullptr, *heavyClass = nullptr, *switchClass = nullptr;
int ascOff = -1, knownOff = -1, abilityOff = -1, timelineOff = -1, rateOff = -1, paramOff = -1;
bool resolved = false, healthy = true;
float multiplier = 1.5f; // Four times was only for the completed stress test.
float activeMultiplier = 1.0f;
unsigned long long nextClassTry = 0;
struct Lease { int index; UObject* ability; int abilityIndex; UObject* timeline; float original, applied; };
std::unordered_map<UObject*, Lease> leases;
int logBudget = 80;

bool MainImage(const void* address, size_t size) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    if (!base || !ue::Read(base, &dos, sizeof dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 4096 ||
        !ue::Read(base + dos.e_lfanew, &nt, sizeof nt) || nt.Signature != IMAGE_NT_SIGNATURE) return false;
    const auto a = reinterpret_cast<uintptr_t>(address), b = reinterpret_cast<uintptr_t>(base);
    return a >= b && a - b < nt.OptionalHeader.SizeOfImage && size <= nt.OptionalHeader.SizeOfImage - (a - b);
}
bool SetRate(UObject* task, float rate) {
    if (!task || !ue::Live(task) || !setFn || !ue::Live(setFn) || paramOff < 0 || paramOff + 4 > 64) return false;
    std::array<uint8_t, 64> args{}; std::memcpy(args.data() + paramOff, &rate, 4);
    pe::Call(task, setFn, args.data());
    const float got = ue::Get<float>(task + rateOff, -1.0f);
    return std::isfinite(got) && std::abs(got - rate) < .001f;
}
bool Same(UObject* task, const Lease& x) {
    return task && ue::Live(task) && ue::Get<int>(task + ue::Layout::ObjIndex, -1) == x.index &&
           ue::Get<UObject*>(task + abilityOff) == x.ability && x.ability && ue::Live(x.ability) &&
           ue::Get<int>(x.ability + ue::Layout::ObjIndex, -1) == x.abilityIndex && ue::Get<UObject*>(task + timelineOff) == x.timeline;
}
void Restore(UObject* task, const Lease& x) {
    if (!Same(task, x)) return;
    const float now = ue::Get<float>(task + rateOff, -1.0f);
    if (std::isfinite(now) && std::abs(now - x.applied) < .001f && SetRate(task, x.original) && logBudget-- > 0)
        pbz::log::Info("攻速测试：恢复 {} 原速 {:.3f}", ue::ClassName(x.ability), x.original);
}
}

bool Resolve() {
    if (!healthy) return false;
    if (resolved) return true;
    taskClass = ue::FindClass("LGATask_PlayTimeline");
    auto* tasksClass = ue::FindClass("GameplayTasksComponent");
    auto* playerClass = ue::FindClass("PBZCharacter_Player");
    if (!taskClass || !tasksClass || !playerClass) return false;
    setFn = pe::FindFunction(taskClass, "SetPlayRate");
    ascOff = ue::PropOffset(playerClass, "AbilitySystemComponent");
    knownOff = ue::PropOffset(tasksClass, "KnownTasks");
    abilityOff = ue::PropOffset(taskClass, "Ability"); timelineOff = ue::PropOffset(taskClass, "Timeline");
    paramOff = setFn ? ue::PropOffset(setFn, "InPlayRate") : -1;
    if (!setFn || ascOff < 0 || knownOff < 0 || abilityOff < 0 || timelineOff < 0 || paramOff < 0 || paramOff + 4 > 64) return false;
    auto* exec = ue::Get<uint8_t*>(setFn + pbz::nx::kFuncOffset);
    std::array<uint8_t, 192> stub{};
    if (!MainImage(exec, stub.size()) || !ue::Read(exec, stub.data(), stub.size())) return false;
    int offset = -1, matches = 0;
    for (size_t i = 0; i + 8 <= stub.size(); ++i) {
        if (stub[i] != 0x48 || stub[i+1] != 0x8b || stub[i+2] != 0xcf || stub[i+3] != 0xe8) continue;
        int32_t rel = 0; std::memcpy(&rel, stub.data() + i + 4, 4);
        const auto next = reinterpret_cast<intptr_t>(exec) + static_cast<intptr_t>(i) + 8;
        auto* native = reinterpret_cast<uint8_t*>(next + rel);
        std::array<uint8_t, 160> code{};
        if (!MainImage(native, code.size()) || !ue::Read(native, code.data(), code.size())) continue;
        const int size = ue::Get<int>(taskClass + ue::Layout::StructSize, 0);
        const int decoded = tower::attackspeed::DecodeRateOffset(code.data(), code.size(), size);
        if (decoded >= 0) { ++matches; offset = decoded; }
    }
    if (matches != 1) { pbz::log::Info("攻速测试：无法唯一核验任务速率字段（{} 个候选）", matches); return false; }
    rateOff = offset; resolved = true;
    pbz::log::Info("攻速测试：已核验 SetPlayRate 原速字段和参数，等待玩家攻击任务");
    return true;
}
bool Ready() { return resolved; }
void Reset() { for (const auto& [task, x] : leases) Restore(task, x); leases.clear(); }
bool SetMultiplier(float value) {
    if (!std::isfinite(value) || value < 1.0f || value > 4.0f) return false;
    Reset(); multiplier = value;
    pbz::log::Info("攻速测试：测试倍率切换为 {:.2f}", multiplier);
    return true;
}
float Multiplier() { return multiplier; }
void Tick(void* rawPawn, bool enabled, float requested) {
    if (!enabled) { if (!leases.empty()) Reset(); activeMultiplier = 1.0f; return; }
    if (!std::isfinite(requested) || requested < 1.0f || requested > 4.0f) { Reset(); return; }
    if (std::abs(requested - activeMultiplier) > .001f) { Reset(); activeMultiplier = requested; }
    if (!healthy) { if (!leases.empty()) Reset(); return; }
    if (!resolved && !Resolve()) return;
    auto* pawn = static_cast<UObject*>(rawPawn);
    if (!pawn || !ue::Live(pawn)) { Reset(); return; }
    const auto now = GetTickCount64();
    if (now >= nextClassTry) {
        nextClassTry = now + 2000;
        if (!lightClass || !ue::Live(lightClass)) lightClass = ue::FindClass("BP_LightAttack_Base_C");
        if (!heavyClass || !ue::Live(heavyClass)) heavyClass = ue::FindClass("BP_HeavyAttack_Base_C");
        if (!switchClass || !ue::Live(switchClass)) switchClass = ue::FindClass("BP_SwitchAttack_Base_C");
    }
    if (lightClass && !ue::Live(lightClass)) lightClass = nullptr;
    if (heavyClass && !ue::Live(heavyClass)) heavyClass = nullptr;
    if (switchClass && !ue::Live(switchClass)) switchClass = nullptr;
    auto* asc = ue::Get<UObject*>(pawn + ascOff);
    if (!asc || !ue::Live(asc)) { Reset(); return; }
    auto* data = ue::Get<uint8_t*>(asc + knownOff);
    const int n = ue::Get<int32_t>(asc + knownOff + 8, -1), cap = ue::Get<int32_t>(asc + knownOff + 12, -1);
    if (!data || n < 0 || n > 256 || cap < n || cap > 4096) { Reset(); return; }
    std::unordered_map<UObject*, bool> active;
    for (int i = 0; i < n; ++i) {
        auto* task = ue::Get<UObject*>(data + 8 * i);
        if (!task || !ue::Live(task) || !ue::IsA(task, taskClass)) continue;
        auto* ability = ue::Get<UObject*>(task + abilityOff);
        auto* timeline = ue::Get<UObject*>(task + timelineOff);
        if (!ability || !ue::Live(ability) || ue::Get<UObject*>(ability + ue::Layout::ObjOuter) != pawn || !timeline || !ue::Live(timeline)) continue;
        if (!(lightClass && ue::IsA(ability, lightClass)) && !(heavyClass && ue::IsA(ability, heavyClass)) && !(switchClass && ue::IsA(ability, switchClass))) continue;
        active[task] = true;
        auto it = leases.find(task);
        if (it != leases.end() && !Same(task, it->second)) { leases.erase(it); it = leases.end(); }
        const float rate = ue::Get<float>(task + rateOff, -1.0f);
        if (!std::isfinite(rate) || rate < .05f || rate > 20.0f) continue;
        if (it != leases.end() && std::abs(rate - it->second.applied) < .001f) continue;
        if (rate > 5.0f) continue;
        const float scaled = rate * requested;
        Lease lease{ue::Get<int>(task + ue::Layout::ObjIndex, -1), ability, ue::Get<int>(ability + ue::Layout::ObjIndex, -1), timeline, rate, scaled};
        if (lease.index < 0 || lease.abilityIndex < 0) continue;
        if (!SetRate(task, scaled)) {
            // A call may have partially taken effect even when readback fails.
            // Attempt to restore the original before disabling this path.
            const bool restored = Same(task, lease) && SetRate(task, rate);
            healthy = false;
            pbz::log::Info("攻速测试：速率校验失败，已停用；原速恢复{}", restored ? "成功" : "未确认");
            Reset();
            return;
        }
        leases[task] = lease;
        if (logBudget-- > 0) pbz::log::Info("攻速测试：{} 时间轴 {} 原速 {:.3f} → {:.3f}", ue::ClassName(ability), ue::ObjName(timeline), rate, scaled);
    }
    for (auto it = leases.begin(); it != leases.end();) {
        if (active.count(it->first)) { ++it; continue; }
        // A removed task has already stopped; do not call into a retired task.
        it = leases.erase(it);
    }
}
}
