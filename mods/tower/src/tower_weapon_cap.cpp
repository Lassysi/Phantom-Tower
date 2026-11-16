#include "tower_weapon_cap.h"
// 3.3.4（用户：锻造大师选不了，全部开发内容都要进正式版）：等级上限适配不再只在 QA 构建
#if 1
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <string>
#include "../../../src/core/sdk/pbz_module.h"
#include "../../../src/core/host/log.h"
#include "tower_api.h"
#include "tower_perf.h"
#include "tower_weapon_cap_sig.h"

namespace towerweaponcap {
namespace {
namespace ue = pbz::ue; namespace pe = pbz::pe;
using ue::UObject;
using Native = int (*)(UObject*, uint64_t, bool);
pbz::abshook::Hook hook;
Native original = nullptr;
std::atomic<bool> active{false};
DWORD gameThread = 0;
bool retired = false;

bool MainImage(const void* address, size_t size) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    if (!base || !ue::Read(base, &dos, sizeof dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 4096 ||
        !ue::Read(base + dos.e_lfanew, &nt, sizeof nt) || nt.Signature != IMAGE_NT_SIGNATURE) return false;
    const auto a = reinterpret_cast<uintptr_t>(address), b = reinterpret_cast<uintptr_t>(base);
    return a >= b && a - b < nt.OptionalHeader.SizeOfImage && size <= nt.OptionalHeader.SizeOfImage - (a - b);
}
bool Owned() {
    if (!hook.target || hook.len < 14 || hook.len > 32) return false;
    std::array<uint8_t, 32> actual{}, expected{};
    if (!ue::Read(hook.target, actual.data(), hook.len)) return false;
    if (!hook.on) return std::memcmp(actual.data(), hook.orig, hook.len) == 0;
    expected.fill(0x90); expected[0] = 0xff; expected[1] = 0x25;
    expected[2] = expected[3] = expected[4] = expected[5] = 0;
    std::memcpy(expected.data() + 6, &hook.detour, 8);
    return std::memcmp(actual.data(), expected.data(), hook.len) == 0;
}
int Intercept(UObject* bag, uint64_t name, bool mainWeapon) {
    const int nativeMax = original ? original(bag, name, mainWeapon) : 0;
    if (!active.load(std::memory_order_acquire) || GetCurrentThreadId() != gameThread || !bag || nativeMax <= 0 || !towerapi::InsideTower()) return nativeMax;
    try {
        const auto& g = towerapi::Growth(); if (!g.on) return nativeMax;
        std::string id = ue::Name(static_cast<uint32_t>(name));
        const auto number = static_cast<uint32_t>(name >> 32);
        if (number) id += "_" + std::to_string(number - 1);
        const int w = tower::growth::WeaponIndex(id, mainWeapon ? 0 : 1);
        if (w < 0 || g.profile.Find(w) < 0) return nativeMax;
        const int cap = g.hexes.Has(tower::hex::Forge) ? 60 : tower::growth::kWeaponCap;
        return std::min(cap, tower::growth::kWeapons[w].maxLevel);
    } catch (...) { return nativeMax; }
}
}
bool Enable() {
    if (retired) return false;
    if (hook.on) { active.store(true, std::memory_order_release); return true; }
    auto* cls = towerperf::ClassNamed("PBZBackPackSubsystem");
    auto* fn = cls ? pe::FindFunction(cls, "GetWeaponMaxLevel") : nullptr;
    auto* exec = fn ? ue::Get<uint8_t*>(fn + pbz::nx::kFuncOffset) : nullptr;
    std::array<uint8_t, 224> wrapper{}, code{};
    if (!MainImage(exec, wrapper.size()) || !ue::Read(exec, wrapper.data(), wrapper.size())) return false;
    const int call = tower::weaponcap::WrapperCall(wrapper.data(), wrapper.size());
    if (call < 0) { pbz::log::Info("影塔锻造大师：上限函数包装特征不匹配"); return false; }
    int32_t rel = 0; std::memcpy(&rel, wrapper.data() + call + 1, 4);
    auto* target = reinterpret_cast<uint8_t*>(reinterpret_cast<intptr_t>(exec) + call + 5 + rel);
    if (!MainImage(target, code.size()) || !ue::Read(target, code.data(), code.size()) || !tower::weaponcap::NativeSignature(code.data(), code.size()) ||
        ue::PropOffset(cls, "MainWeaponInfos") != 0x718 || ue::PropOffset(cls, "ShaqiWeaponInfos") != 0x780) {
        pbz::log::Info("影塔锻造大师：原生等级上限特征或属性布局不匹配"); return false;
    }
    std::string why;
    if (!pbz::abshook::Prepare(hook, target, reinterpret_cast<void*>(&Intercept), &why)) { pbz::log::Info("影塔锻造大师：无法准备上限钩子：{}", why); return false; }
    original = reinterpret_cast<Native>(hook.tramp); gameThread = GetCurrentThreadId();
    if (!Owned() || !pbz::abshook::Apply(hook, true)) { pbz::log::Info("影塔锻造大师：无法启用上限钩子"); return false; }
    active.store(true, std::memory_order_release);
    pbz::log::Info("影塔锻造大师：已启用塔内等级上限适配，原档周目不变");
    return true;
}
void Disable() {
    active.store(false, std::memory_order_release);
    if (!hook.on) return;
    if (!Owned() || !pbz::abshook::Apply(hook, false)) { retired = true; pbz::log::Info("影塔锻造大师：上限钩子还原未确认，已回退原始调用并保留现场"); return; }
    pbz::log::Info("影塔锻造大师：已恢复原生等级上限");
}
bool Ready() { return active.load(std::memory_order_acquire) && hook.on && !retired; }
}
#else
namespace towerweaponcap { bool Enable() { return false; } void Disable() {} bool Ready() { return false; } }
#endif
