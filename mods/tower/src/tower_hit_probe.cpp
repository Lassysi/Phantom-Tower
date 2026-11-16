#include "tower_hit_probe.h"
#include "tower_weapon_power_probe.h"
#ifdef PBZ_HEX_TEST
#include <windows.h>
#include <algorithm>
#include <vector>
#include "../../../src/core/sdk/pbz_module.h"
#include "../../../src/core/host/log.h"
#include "tower_perf.h"
namespace towerhitprobe {
namespace {
namespace ue = pbz::ue; namespace pe = pbz::pe; namespace attr = pbz::attr;
using ue::UObject;
int token = 0, weaponPeToken = 0, attackerOff = -1, targetOff = -1, ascOff = -1, events = 0, hits = 0, weaponCalls = 0;
DWORD ownerThread = 0;
UObject* player = nullptr; UObject* playerAsc = nullptr;
}
void Stop() {
    towerweaponprobe::Stop();
    if (token) { pe::Unobserve(token); token = 0; pbz::log::Info("灼烧取证：观察结束，计算调用 {}，玩家对敌人 {}", events, hits); }
    if (weaponPeToken) { pe::Unobserve(weaponPeToken); weaponPeToken = 0; }
    if (weaponCalls) pbz::log::Info("重量级取证：原生武器强度入口调用 {} 次（只读，尚未实现增伤）", weaponCalls);
    weaponCalls = 0;
    player = playerAsc = nullptr; ownerThread = 0;
}
bool Start(void* rawPlayer) {
    Stop();
    player = static_cast<UObject*>(rawPlayer);
    auto* cls = towerperf::ClassNamed("DamageCalculatorBase");
    auto* fn = cls ? pe::FindFunction(cls, "CalculateDamage") : nullptr;
    auto* pc = towerperf::ClassNamed("PBZCharacter_Player");
    attackerOff = fn ? ue::PropOffset(fn, "AttackerAbilitySystem") : -1;
    targetOff = fn ? ue::PropOffset(fn, "TargetAbilitySystem") : -1;
    ascOff = pc ? ue::PropOffset(pc, "AbilitySystemComponent") : -1;
    playerAsc = player && ue::Live(player) && ascOff >= 0 ? ue::Get<UObject*>(player + ascOff) : nullptr;
    if (!fn || attackerOff < 0 || targetOff < 0 || !playerAsc || !ue::Live(playerAsc)) { Stop(); return false; }
    ownerThread = GetCurrentThreadId(); events = hits = 0;
    token = pe::Observe(fn, [](UObject*, void* raw) {
        if (!raw || GetCurrentThreadId() != ownerThread || !player || !ue::Live(player) || !playerAsc || !ue::Live(playerAsc)) return;
        ++events;
        auto* p = static_cast<uint8_t*>(raw);
        auto* attacker = ue::Get<UObject*>(p + attackerOff);
        auto* target = ue::Get<UObject*>(p + targetOff);
        if (attacker != playerAsc || !target || !ue::Live(target)) return;
        for (const auto& t : attr::Targets()) {
            if (t.who != attr::Who::Enemies || !attr::Alive(t) || ue::Get<UObject*>(t.obj + ascOff) != target) continue;
            if (++hits <= 30) pbz::log::Info("灼烧取证：玩家攻击目标 {}（ASC 匹配，{}）", ue::ClassName(t.obj), ue::ObjName(t.obj));
            break;
        }
    });
    if (!token) { Stop(); return false; }
    towerweaponprobe::Start(player);
    // Read-only route reconnaissance: do not change attack, results or weapon data.
    auto* wc = towerperf::ClassNamed("PBZ_WeaponConfigBase");
    auto* wf = wc ? pe::FindFunction(wc, "GetWeaponPower") : nullptr;
    if (wf) {
        const int cfg = ue::PropOffset(wf, "WeaponConfig"), avatar = ue::PropOffset(wf, "Avatar");
        weaponPeToken = pe::Observe(wf, [cfg,avatar](UObject*,void* raw) {
            if (!raw || GetCurrentThreadId() != ownerThread || cfg < 0 || avatar < 0) return;
            auto* p=static_cast<uint8_t*>(raw); auto* actor=ue::Get<UObject*>(p+avatar); auto* config=ue::Get<UObject*>(p+cfg);
            if (actor != player || !config || !ue::Live(config)) return;
            if (++weaponCalls <= 12) pbz::log::Info("重量级取证：玩家武器强度计算 config={}（只读）",ue::ObjName(config));
        });
        pbz::log::Info("重量级取证：武器强度 ProcessEvent 只读观察 {}；无回调不代表没有计算", weaponPeToken != 0);
    }
    pbz::log::Info("灼烧取证：只读观察已开启；仅识别玩家攻击者与目标，不修改战斗结果");
    return true;
}
}
#else
namespace towerhitprobe { bool Start(void*) { return false; } void Stop() {} }
#endif
