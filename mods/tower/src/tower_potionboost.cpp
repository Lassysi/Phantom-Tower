#include "tower_potionboost.h"
#include "tower_widgets.h"
#include "tower_perf.h"
#include "../../../src/core/host/log.h"
#include <cmath>
#include <cstdint>
#include <string_view>
#include <array>
#include <windows.h>

namespace towerpotion {
namespace {
namespace ue = pbz::ue;
using ue::UObject;
struct Array { void* data; int32_t num, max; };
struct Definition { const char* name; const char* path; };
// Statically audited native effects that heal Health from captured Health_Max.
constexpr Definition kEffects[] = {
    {"GE_Medicine_Medicine_01_C", "/PBZCombatAsset/Prototypes/GameplayEffects/Medicine/GE_Medicine_Medicine_01.GE_Medicine_Medicine_01_C"},
    {"GE_Medicine_Medicine_02_C", "/PBZCombatAsset/Prototypes/GameplayEffects/Medicine/GE_Medicine_Medicine_02.GE_Medicine_Medicine_02_C"},
    {"GE_Medicine_Medicine_03_C", "/PBZCombatAsset/Prototypes/GameplayEffects/Medicine/GE_Medicine_Medicine_03.GE_Medicine_Medicine_03_C"},
    {"GE_Medicine_Medicine_04_C", "/PBZCombatAsset/Prototypes/GameplayEffects/Medicine/GE_Medicine_Medicine_04.GE_Medicine_Medicine_04_C"},
    {"GE_Medicine_Medicine_05_C", "/PBZCombatAsset/Prototypes/GameplayEffects/Medicine/GE_Medicine_Medicine_05.GE_Medicine_Medicine_05_C"},
    {"GE_Medicine_Medicine_06_C", "/PBZCombatAsset/Prototypes/GameplayEffects/Medicine/GE_Medicine_Medicine_06.GE_Medicine_Medicine_06_C"},
    {"GE_Medicine_Medicine_07_C", "/PBZCombatAsset/Prototypes/GameplayEffects/Medicine/GE_Medicine_Medicine_07.GE_Medicine_Medicine_07_C"},
    {"GE_Medicine_Medicine_16_C", "/PBZCombatAsset/Prototypes/GameplayEffects/Medicine/GE_Medicine_Medicine_16.GE_Medicine_Medicine_16_C"},
    {"GE_Medicine_Attack_HealthRecover_C", "/PBZCombatAsset/Prototypes/GameplayEffects/Medicine/Attack/GE_Medicine_Attack_HealthRecover.GE_Medicine_Attack_HealthRecover_C"},
    {"GE_Medicine_IncreaseAmplification_HealthRecover_C", "/PBZCombatAsset/Prototypes/GameplayEffects/Medicine/IncreaseAmplification/GE_Medicine_IncreaseAmplification_HealthRecover.GE_Medicine_IncreaseAmplification_HealthRecover_C"},
    {"GE_Medicine_IncreaseShachiAttack_HealthRecover_C", "/PBZCombatAsset/Prototypes/GameplayEffects/Medicine/IncreaseShachiAttack/GE_Medicine_IncreaseShachiAttack_HealthRecover.GE_Medicine_IncreaseShachiAttack_HealthRecover_C"},
    {"GE_Medicine_KuYeLu_HealthRegen_C", "/PBZCombatAsset/Prototypes/GameplayEffects/Medicine/KuYeLu/GE_Medicine_KuYeLu_HealthRegen.GE_Medicine_KuYeLu_HealthRegen_C"},
    {"GE_Medicine_RemoveDebuff_HealthRegen_C", "/PBZCombatAsset/Prototypes/GameplayEffects/Medicine/Medicine_RemoveDebuff/GE_Medicine_RemoveDebuff_HealthRegen.GE_Medicine_RemoveDebuff_HealthRegen_C"},
    {"GE_Medicine_Buff_NingFengDan_HealthRecover_C", "/PBZCombatAsset/Prototypes/GameplayEffects/Medicine/NingFengDan/GE_Medicine_Buff_NingFengDan_HealthRecover.GE_Medicine_Buff_NingFengDan_HealthRecover_C"},
    {"GE_Medicine_RecoverShachiWeaponBullet_HealthRecover_C", "/PBZCombatAsset/Prototypes/GameplayEffects/Medicine/RecoverShachiWeaponBullet/GE_Medicine_RecoverShachiWeaponBullet_HealthRecover.GE_Medicine_RecoverShachiWeaponBullet_HealthRecover_C"},
    {"GE_Medicine_ReductionDamage_HealthRegen_C", "/PBZCombatAsset/Prototypes/GameplayEffects/Medicine/ReductionDamage/GE_Medicine_ReductionDamage_HealthRegen.GE_Medicine_ReductionDamage_HealthRegen_C"},
    {"GE_Medicine_RegenShachi_HealthRegen_C", "/PBZCombatAsset/Prototypes/GameplayEffects/Medicine/RegenShachi/GE_Medicine_RegenShachi_HealthRegen.GE_Medicine_RegenShachi_HealthRegen_C"},
};
struct State {
    UObject* cdo = nullptr; UObject* cls = nullptr; void* modifiers = nullptr; int modifiersOffset = -1;
    float* value = nullptr; float original = 0, last = 0; bool attempted = false;
    UObject* calcCdo = nullptr; UObject* calcClass = nullptr; float* extra = nullptr; float extraOriginal = 0, extraLast = 0;
    double* threshold = nullptr; double thresholdOriginal = 0, thresholdLast = 0;
};
std::array<State, std::size(kEffects)> g{};
size_t g_cursor = 0;
ULONGLONG g_nextLoad = 0;
// 没装备的药效资源没有引用，垃圾回收约每分钟卸载一次；这里不限频就会每分钟重新阻塞加载一轮（实机日志）
std::array<ULONGLONG, std::size(kEffects)> g_retryAt{};

bool HasName(const uint8_t* attribute, int off, const wchar_t* expected) {
    const auto a = ue::Get<Array>(attribute + off);
    wchar_t name[64]{};
    if (!a.data || a.num < 1 || a.num > 63 || !ue::Read(a.data, name, static_cast<size_t>(a.num) * sizeof(wchar_t))) return false;
    return std::wstring_view(name) == expected;
}
float* FindCoefficient(const Definition& def, State& state) {
    auto* ge = towerperf::ClassNamed("GameplayEffect");
    auto* modifier = towerperf::ClassNamed("GameplayModifierInfo");
    auto* mag = towerperf::ClassNamed("GameplayEffectModifierMagnitude");
    auto* based = towerperf::ClassNamed("AttributeBasedFloat");
    auto* capture = towerperf::ClassNamed("GameplayEffectAttributeCaptureDefinition");
    auto* attribute = towerperf::ClassNamed("GameplayAttribute");
    auto* scalable = towerperf::ClassNamed("ScalableFloat");
    auto* owner = towerperf::ClassNamed("BasicAttributeSet");
    if (!ge || !modifier || !mag || !based || !capture || !attribute || !scalable || !owner) return nullptr;
    // Exact asset path avoids a full object-table scan for every unloaded drug.
    auto* cls = towerwidgets::LoadClass(def.path);
    auto* cdo = towerwidgets::Default(cls);
    if (!cdo || !ue::Live(cdo)) return nullptr;
    const int mo = ue::PropOffset(ge, "Modifiers"), ma = ue::PropOffset(modifier, "Attribute"), mm = ue::PropOffset(modifier, "ModifierMagnitude");
    const int mt = ue::PropOffset(mag, "MagnitudeCalculationType"), mb = ue::PropOffset(mag, "AttributeBasedMagnitude");
    const int bc = ue::PropOffset(based, "Coefficient"), ba = ue::PropOffset(based, "BackingAttribute");
    const int ca = ue::PropOffset(capture, "AttributeToCapture"), an = ue::PropOffset(attribute, "AttributeName"), ao = ue::PropOffset(attribute, "AttributeOwner"), sv = ue::PropOffset(scalable, "Value");
    const int modSize = ue::Get<int>(modifier + ue::Layout::StructSize, -1), magSize = ue::Get<int>(mag + ue::Layout::StructSize, -1);
    const int basedSize = ue::Get<int>(based + ue::Layout::StructSize, -1), attrSize = ue::Get<int>(attribute + ue::Layout::StructSize, -1);
    if (mo < 0 || ma < 0 || mm < 0 || mt < 0 || mb < 0 || bc < 0 || ba < 0 || ca < 0 || an < 0 || ao < 0 || sv < 0 ||
        attrSize != 0x38 || modSize < mm + magSize || magSize < mb + basedSize || basedSize < ba + ca + attrSize ||
        basedSize < bc + sv + 4 || attrSize < an + 16 || attrSize < ao + 8 || modSize < ma + attrSize) return nullptr;
    const auto mods = ue::Get<Array>(cdo + mo);
    if (!mods.data || mods.num < 1 || mods.num > 8 || mods.max < mods.num) return nullptr;
    auto* item = static_cast<uint8_t*>(mods.data);
    auto* dest = item + ma; auto* magnitude = item + mm; auto* basedData = magnitude + mb; auto* captured = basedData + ba + ca;
    if (ue::Get<uint8_t>(magnitude + mt) != 1 || !HasName(dest, an, L"Health") || !HasName(captured, an, L"Health_Max") ||
        ue::Get<UObject*>(dest + ao) != owner || ue::Get<UObject*>(captured + ao) != owner) return nullptr;
    state.cdo = cdo; state.cls = cls; state.modifiers = mods.data; state.modifiersOffset = mo;
    return reinterpret_cast<float*>(basedData + bc + sv);
}
// The gourd and low-health medicine also have an extra conditional heal. Its
// calculation compares current HP with captured native maximum, so both the
// threshold and the resulting native-max-based amount must follow the bonus.
void FindConditional(const Definition& def, State& state) {
    const std::string_view expected = std::string_view(def.name) == "GE_Medicine_Medicine_01_C" ? "GEC_Medicine_HealthConditionRecover_YaoHuLu_C" :
                                      std::string_view(def.name) == "GE_Medicine_Medicine_05_C" ? "GEC_Medicine_HealthConditionRecover_AdditiveRecover_C" : "";
    if (expected.empty()) return;
    auto* ge = towerperf::ClassNamed("GameplayEffect"); auto* modifier = towerperf::ClassNamed("GameplayModifierInfo");
    auto* mag = towerperf::ClassNamed("GameplayEffectModifierMagnitude"); auto* custom = towerperf::ClassNamed("CustomCalculationBasedFloat");
    auto* attribute = towerperf::ClassNamed("GameplayAttribute"); auto* scalable = towerperf::ClassNamed("ScalableFloat"); auto* owner = towerperf::ClassNamed("BasicAttributeSet");
    if (!ge || !modifier || !mag || !custom || !attribute || !scalable || !owner) return;
    const int mo = ue::PropOffset(ge, "Modifiers"), ma = ue::PropOffset(modifier, "Attribute"), mm = ue::PropOffset(modifier, "ModifierMagnitude");
    const int mt = ue::PropOffset(mag, "MagnitudeCalculationType"), mc = ue::PropOffset(mag, "CustomMagnitude");
    const int cc = ue::PropOffset(custom, "CalculationClassMagnitude"), cf = ue::PropOffset(custom, "Coefficient");
    const int an = ue::PropOffset(attribute, "AttributeName"), ao = ue::PropOffset(attribute, "AttributeOwner"), sv = ue::PropOffset(scalable, "Value");
    const int stride = ue::Get<int>(modifier + ue::Layout::StructSize, -1), ms = ue::Get<int>(mag + ue::Layout::StructSize, -1);
    const int cs = ue::Get<int>(custom + ue::Layout::StructSize, -1), as = ue::Get<int>(attribute + ue::Layout::StructSize, -1);
    if (mo < 0 || ma < 0 || mm < 0 || mt < 0 || mc < 0 || cc < 0 || cf < 0 || an < 0 || ao < 0 || sv < 0 ||
        as != 0x38 || stride < mm + ms || stride < ma + as || ms < mc + cs || cs < cc + 8 || cs < cf + sv + 4 || as < an + 16 || as < ao + 8) return;
    const auto mods = ue::Get<Array>(state.cdo + mo);
    if (!mods.data || mods.num != 2 || mods.max < 2) return;
    auto* item = static_cast<uint8_t*>(mods.data) + stride; auto* dest = item + ma; auto* magnitude = item + mm; auto* customData = magnitude + mc;
    auto* calc = ue::Get<UObject*>(customData + cc);
    if (ue::Get<uint8_t>(magnitude + mt) != 2 || !HasName(dest, an, L"Health") || ue::Get<UObject*>(dest + ao) != owner ||
        !calc || !ue::Live(calc) || ue::ObjName(calc) != expected) return;
    auto* calcCdo = towerwidgets::Default(calc); const int off = ue::PropOffset(calc, "Healththreshold_Percentage");
    if (!calcCdo || !ue::Live(calcCdo) || off < 0 || off + 8 > ue::Get<int>(calc + ue::Layout::StructSize, -1)) return;
    auto* extra = reinterpret_cast<float*>(customData + cf + sv); auto* threshold = reinterpret_cast<double*>(calcCdo + off);
    float e = 0; double t = 0;
    if (!ue::Read(extra, &e, 4) || !ue::Read(threshold, &t, 8) || !std::isfinite(e) || e <= 0 || !std::isfinite(t) || t <= 0 || t > 1) return;
    state.calcCdo = calcCdo; state.calcClass = calc; state.extra = extra; state.extraOriginal = state.extraLast = e;
    state.threshold = threshold; state.thresholdOriginal = state.thresholdLast = t;
}
bool SameEffect(const State& s) {
    if (!s.cdo || !s.cls || !ue::Live(s.cdo) || !ue::Live(s.cls) || ue::ClassOf(s.cdo) != s.cls || !ue::IsDefault(s.cdo) || s.modifiersOffset < 0) return false;
    return ue::Get<Array>(s.cdo + s.modifiersOffset).data == s.modifiers;
}
bool SameCalc(const State& s) { return s.calcCdo && s.calcClass && ue::Live(s.calcCdo) && ue::Live(s.calcClass) && ue::ClassOf(s.calcCdo) == s.calcClass && ue::IsDefault(s.calcCdo); }
}

void Clear() {
    for (auto& entry : g) if (SameEffect(entry)) {
        float current = 0;
        if (entry.value && ue::Read(entry.value, &current, 4) && current == entry.last) {
            ue::Write(entry.value, &entry.original, 4);
            pbz::log::Info("影塔药水：治疗系数已恢复 {:.3f}", entry.original);
        }
        if (entry.extra) { float currentExtra = 0; if (ue::Read(entry.extra, &currentExtra, 4) && currentExtra == entry.extraLast) ue::Write(entry.extra, &entry.extraOriginal, 4); }
        if (entry.threshold && SameCalc(entry)) { double currentThreshold = 0; if (ue::Read(entry.threshold, &currentThreshold, 8) && currentThreshold == entry.thresholdLast) ue::Write(entry.threshold, &entry.thresholdOriginal, 8); }
    }
    g = {}; g_cursor = 0; g_nextLoad = 0; g_retryAt = {};
}

bool Sync(float multiplier, bool allowLoad) {
    if (!std::isfinite(multiplier) || multiplier < 1 || multiplier > 1001) return false;
    // Do not load potion assets when the player has no tower life bonus.
    // Existing entries still run below so a removed bonus restores coefficients.
    if (allowLoad && multiplier > 1.0001f && GetTickCount64() >= g_nextLoad) {
        const size_t i = g_cursor; g_cursor = (g_cursor + 1) % g.size(); g_nextLoad = GetTickCount64() + 200;
        auto& entry = g[i]; const auto& def = kEffects[i];
        if (entry.value && !SameEffect(entry)) entry = {};  // 关卡切换卸载后重新装入
        if (!entry.attempted && GetTickCount64() >= g_retryAt[i]) {
        g_retryAt[i] = GetTickCount64() + 90000;
        entry.attempted = true; entry.value = FindCoefficient(def, entry);
        if (!entry.value || !ue::Read(entry.value, &entry.original, 4) || !std::isfinite(entry.original) || entry.original <= 0) {
            entry.value = nullptr; pbz::log::Info("影塔药水：{} 原生药效合同不符，未更改", def.name);
        } else {
            entry.last = entry.original;
            FindConditional(def, entry);
        }
        }
    }
    for (size_t i = 0; i < g.size(); ++i) {
        auto& entry = g[i];
        if (!entry.value || !SameEffect(entry)) continue;
        float current = 0;
        if (!ue::Read(entry.value, &current, 4)) continue;
        if (current != entry.last) { pbz::log::Info("影塔药水：{} 系数被其他组件更改，停止接管", kEffects[i].name); entry.value = nullptr; continue; }
        const float wanted = entry.original * multiplier;
        if (current != wanted) {
            if (!ue::Write(entry.value, &wanted, 4)) continue;
            entry.last = wanted;
            pbz::log::Info("影塔药水：{} 治疗系数 {:.3f} → {:.3f}，生命倍率 {:.3f}", kEffects[i].name, entry.original, wanted, multiplier);
        }
        if (entry.extra && entry.threshold && SameCalc(entry)) {
            float extra = 0; double threshold = 0;
            if (ue::Read(entry.extra, &extra, 4) && ue::Read(entry.threshold, &threshold, 8) && extra == entry.extraLast && threshold == entry.thresholdLast) {
                const float wantExtra = entry.extraOriginal * multiplier; const double wantThreshold = entry.thresholdOriginal * multiplier;
                const bool changing = extra != wantExtra || threshold != wantThreshold;
                if (extra != wantExtra && ue::Write(entry.extra, &wantExtra, 4)) entry.extraLast = wantExtra;
                if (threshold != wantThreshold && ue::Write(entry.threshold, &wantThreshold, 8)) entry.thresholdLast = wantThreshold;
                if (changing) pbz::log::Info("影塔药水：{} 低血量附加系数 {:.3f}，阈值 {:.3f}", kEffects[i].name, entry.extraLast, entry.thresholdLast);
            }
        }
    }
    return g[0].value != nullptr;
}
}
