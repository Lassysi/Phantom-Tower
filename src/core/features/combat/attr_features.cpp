// 生存 / 战斗 / 时间功能表。属性语义按实机只读结果定（2026-10-28，Build 26266538）：
//   TakenVulnerabilityMultiplier_* / Dealt*Multiplier_* 默认 1（直接乘数，用 Scale）
//   TakenAdditiveMultiplier_* 默认 0（加成项，用 Additive）
// 证据等级：静态 + 只读读值；未实机 on/off。
#include <memory>

#include "../../services/effects.h"

namespace pbz {
namespace {

using namespace fx;
// 生命伤害类型
const std::vector<std::string> kDamage = {"Physical", "PhysicalSharp", "PhysicalBlunt", "Fire", "Ice", "Lightning", "Aqua",
                                          "Elemental", "Chaos", "Blood", "Toxin"};
// 杀气伤害类型：命中削杀气 / 完美格挡反削 / 格挡时被削。杀气耗尽 = 破防（游戏里没有单独的"架势条"）
const std::vector<std::string> kShachi = {"ImpactShachi", "ReflectShachi", "DefenceShachi"};
const std::vector<std::string> kStatus = {"BleedAccumulation", "BloodBurstAccumulation", "ElectrifiedAccumulation", "ElementalAccumulation",
                                          "FrostAccumulation", "IgniteAccumulation", "PoisonAccumulation", "WaterAccumulation"};

template <class... L> std::vector<Effect> Join(L&&... lists) {
    std::vector<Effect> v;
    (v.insert(v.end(), lists.begin(), lists.end()), ...);
    return v;
}

void Add(const char* id, bool toggle, std::vector<Effect> fx) { Features().push_back(std::make_unique<EffectFeature>(id, toggle, std::move(fx))); }

}  // namespace

void RegisterAttributeFeatures() {
    constexpr auto P = Who::Player, E = Who::Enemies;
    const auto S = Kind::Scale, Ad = Kind::Additive, Set = Kind::Set;
    auto& F = Features();

    // ---- 生存 player.* ----
    Add("player.god", true, Join(Each(P, "TakenVulnerabilityMultiplier_", kDamage, S, 0), Each(P, "TakenVulnerabilityMultiplier_", kShachi, S, 0),
                                 Each(P, "TakenVulnerabilityMultiplier_", kStatus, S, 0)));
    using LI = LockFeature::Item;
    F.push_back(std::make_unique<LockFeature>("player.inf_hp", std::vector<LI>{{"Health", "Health_Max", "Health_Min"}}));
    Add("player.demigod", true, {AB(P, "Health_Min", Set, 1)});
    Add("player.dmg_taken_mul", false, Each(P, "TakenVulnerabilityMultiplier_", kDamage, S));
    Add("player.maxhp_mul", false, {A(P, "Health_Max", S)});
    F.push_back(std::make_unique<LockFeature>("player.inf_shachi", std::vector<LI>{{"Shachi", "Shachi_Max", "Shachi_Min"}}));
    Add("player.shachi_gain_mul", false, {A(P, "ShachiRegen", S)});
    Add("player.shachi_max_mul", false, {A(P, "Shachi_Max", S)});
    F.push_back(std::make_unique<LockFeature>("player.inf_ult", std::vector<LI>{{"UltimateCharge", "UltimateCharge_Max", ""}, {"UltimateChargeCount", "UltimateChargeCount_Max", ""}}));
    Add("player.ult_charge_mul", false, {A(P, "UltimateChargeAdditiveRate", Ad)});
    Add("player.no_stagger", true, {A(P, "TakenVulnerabilityMultiplier_Poise", S, 0)});
    Add("player.posture_lock", true, Each(P, "TakenVulnerabilityMultiplier_", kShachi, S, 0));      // 杀气不被削减
    Add("player.posture_taken_mul", false, Each(P, "TakenVulnerabilityMultiplier_", kShachi, S));    // 自身杀气损耗倍率
    Add("player.status_immune", true, Each(P, "TakenVulnerabilityMultiplier_", kStatus, S, 0));

    // ---- 战斗 combat.* ----
    Add("combat.dmg_mul", false, Each(E, "TakenVulnerabilityMultiplier_", kDamage, S));
    Add("combat.one_hit", true, Each(E, "TakenAdditiveMultiplier_", kDamage, Ad, 10000));
    Add("combat.posture_dmg_mul", false, Each(E, "TakenVulnerabilityMultiplier_", kShachi, S));     // 削杀气倍率
    Add("combat.instant_break", true, Each(E, "TakenAdditiveMultiplier_", kShachi, Ad, 10000));      // 一击破防
    // 即时处决：敌人默认破防且血量 ≤30% 才可处决（ExecutionTargetBreakHealthThreshold=0.3，Normal=0）；两个阈值都设为 1
    // 实测：仍须先打空杀气（破防）才出处决提示 —— 一并带上一击破防
    Add("combat.instant_exec", true, Join(std::vector<Effect>{A(E, "ExecutionTargetNormalHealthThreshold", Set, 1), A(E, "ExecutionTargetBreakHealthThreshold", Set, 1)},
                                          Each(E, "TakenAdditiveMultiplier_", kShachi, Ad, 10000)));
    Add("combat.status_mul", false, Each(E, "TakenVulnerabilityMultiplier_", kStatus, S));
    Add("combat.enemy_hp_mul", false, {A(E, "Health_Max", S)});
    Add("combat.enemy_dmg_mul", false, Each(E, "DealtVulnerabilityMultiplier_", kDamage, S));
    Add("combat.enemy_posture_mul", false, Each(E, "TakenVulnerabilityMultiplier_", kShachi, S, NAN, true));  // 敌人杀气抗性
    Add("combat.enemy_atk_speed", false, {ActorF(E, "CustomTimeDilation", S)});
    Add("combat.enemy_freeze", true, {ActorF(E, "CustomTimeDilation", S, 0.01f)});
    Add("combat.atk_speed", false, {ActorF(P, "CustomTimeDilation", S)});

    // ---- 时间 time.* ----
    Add("time.game_speed", false, {WorldF("TimeDilation", S)});
    F.push_back(std::make_unique<FloorFeature>("time.day_lock", "RemainingDays"));
}

}  // namespace pbz
