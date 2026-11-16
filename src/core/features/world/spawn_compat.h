#pragma once
#include <string_view>

namespace pbz::enemies {
// Explicit, reviewed standalone encounter rules. Do not infer phases by suffix
// or remove every effect whose name happens to contain "LockHealth".
inline constexpr std::string_view kLeiDang = "/PBZCombatAsset/ActorData/Mobs/047_LeiDang/BP_Monster_LeiDang.BP_Monster_LeiDang_C";
inline constexpr std::string_view kLeiDangJyx = "/PBZCombatAsset/ActorData/Mobs/047_LeiDang/BP_Monster_LeiDang_JYX.BP_Monster_LeiDang_JYX_C";
inline constexpr std::string_view kLeiDangFinal = "/PBZCombatAsset/ActorData/Mobs/050_LeiDang_P2/BP_Monster_LeiDang_P2.BP_Monster_LeiDang_P2_C";
inline constexpr std::string_view kChiLian = "/PBZCombatAsset/ActorData/Mobs/063_ChiLian_P1/BP_Monster_ChiLian.BP_Monster_ChiLian_C";
inline constexpr std::string_view kChiLianFinal = "/PBZCombatAsset/ActorData/Mobs/065_ChiLian_P2/BP_Monster_ChiLian_P2.BP_Monster_ChiLian_P2_C";
inline constexpr std::string_view kChild = "/PBZCombatAsset/ActorData/Mobs/023_Vessel_Child/BP_Monster_Vessel_Child.BP_Monster_Vessel_Child_C";
inline constexpr std::string_view kYellowSummon = "/PBZCombatAsset/ActorData/Mobs/068_YuZan/BP_Monster_MaleThug_Club_Yellow_Summon.BP_Monster_MaleThug_Club_Yellow_Summon_C";
inline constexpr std::string_view kLengTu = "/PBZCombatAsset/ActorData/Mobs/077_LengTu/BP_Monster_LengTu.BP_Monster_LengTu_C";
inline constexpr std::string_view kLengTuArenaAbilities[] = {
    "/PBZCombatAsset/ActorData/Mobs/077_LengTu/Ability/Summons/GA_LengTu_SwitchPhase_Teleport.GA_LengTu_SwitchPhase_Teleport_C",
    "/PBZCombatAsset/ActorData/Mobs/077_LengTu/Ability/Summons/GA_LengTu_ToBattle_Teleport.GA_LengTu_ToBattle_Teleport_C"};
inline constexpr std::string_view kLengTuBodyEffects[] = {
    "/PBZCombatAsset/ActorData/Mobs/077_LengTu/GameplayEffect/GE_LengTu_SecondPhase.GE_LengTu_SecondPhase_C",
    "/PBZCombatAsset/ActorData/Mobs/077_LengTu/GameplayEffect/GE_LengTu_FinalPhase_Buff.GE_LengTu_FinalPhase_Buff_C"};

// Story summon and transition controllers replaced by the independent body stage.
inline constexpr std::string_view kLengTuStoryAbilities[] = {
    "/PBZCombatAsset/ActorData/Mobs/077_LengTu/Ability/GA_LengTu_FinalPhase.GA_LengTu_FinalPhase_C",
    "/PBZCombatAsset/ActorData/Mobs/077_LengTu/Ability/Passive/GA_LengTu_GeneralSpawn_Passive.GA_LengTu_GeneralSpawn_Passive_C",
    "/PBZCombatAsset/ActorData/Mobs/077_LengTu/Ability/Passive/GA_LengTu_SummonPassive_General.GA_LengTu_SummonPassive_General_C",
    "/PBZCombatAsset/ActorData/Mobs/077_LengTu/Ability/Passive/GA_LengTu_SummonPassive_P1_PengMeng.GA_LengTu_SummonPassive_P1_PengMeng_C",
    "/PBZCombatAsset/ActorData/Mobs/077_LengTu/Ability/Passive/GA_LengTu_SummonPassive_P1_Scavenger.GA_LengTu_SummonPassive_P1_Scavenger_C",
    "/PBZCombatAsset/ActorData/Mobs/077_LengTu/Ability/Passive/GA_LengTu_SummonPassive_P2_GuiChai.GA_LengTu_SummonPassive_P2_GuiChai_C",
    "/PBZCombatAsset/ActorData/Mobs/077_LengTu/Ability/Passive/GA_LengTu_SummonPassive_P2_HuaShan.GA_LengTu_SummonPassive_P2_HuaShan_C",
    "/PBZCombatAsset/ActorData/Mobs/077_LengTu/Ability/Passive/GA_LengTu_SummonPassive_P3_FengXueDaoKe.GA_LengTu_SummonPassive_P3_FengXueDaoKe_C",
    "/PBZCombatAsset/ActorData/Mobs/077_LengTu/Ability/Passive/GA_LengTu_SummonPassive_P3_Vessels.GA_LengTu_SummonPassive_P3_Vessels_C",
    "/PBZCombatAsset/ActorData/Mobs/077_LengTu/Ability/Passive/GA_LengTu_SummonPassive_P4_FemaleDolls.GA_LengTu_SummonPassive_P4_FemaleDolls_C",
    "/PBZCombatAsset/ActorData/Mobs/077_LengTu/Ability/Passive/GA_LengTu_SummonPassive_P4_LionDancer.GA_LengTu_SummonPassive_P4_LionDancer_C",
    "/PBZCombatAsset/ActorData/Mobs/077_LengTu/Ability/Passive/GA_LengTu_SummonPassive_P5_LeiDang.GA_LengTu_SummonPassive_P5_LeiDang_C",
    "/PBZCombatAsset/ActorData/Mobs/077_LengTu/Ability/Summons/GA_LengTu_Summon_Defeat.GA_LengTu_Summon_Defeat_C",
    "/PBZCombatAsset/ActorData/Mobs/077_LengTu/Ability/Summons/GA_LengTu_Summon_Defeat_02.GA_LengTu_Summon_Defeat_02_C",
    "/PBZCombatAsset/ActorData/Mobs/077_LengTu/Ability/Summons/GA_LengTu_Summon_P1.GA_LengTu_Summon_P1_C",
    "/PBZCombatAsset/ActorData/Mobs/077_LengTu/Ability/Summons/GA_LengTu_Summon_P2.GA_LengTu_Summon_P2_C",
    "/PBZCombatAsset/ActorData/Mobs/077_LengTu/Ability/Summons/GA_LengTu_Summon_P3.GA_LengTu_Summon_P3_C",
    "/PBZCombatAsset/ActorData/Mobs/077_LengTu/Ability/Summons/GA_LengTu_Summon_P4.GA_LengTu_Summon_P4_C"};

inline std::string_view StandalonePath(std::string_view path, std::string_view tag) {
    if (tag.starts_with("tower-")) return path;
    if ((path == kLeiDang || path == kLeiDangJyx) && !tag.starts_with("auto-response-native-")) return kLeiDangFinal;
    if (path == kChiLian) return kChiLianFinal;
    return path;
}

struct StandaloneRule { std::string_view config, effect, cinematicAbility; };
inline StandaloneRule StandaloneRuleFor(std::string_view path) {
    if (path == kChiLianFinal) return {
        "/PBZCombatAsset/ActorData/Mobs/065_ChiLian_P2/ActorConfig/ActorC_ChiLian_P2.ActorC_ChiLian_P2",
        "/PBZCombatAsset/Prototypes/GameplayEffects/GE_LockHealth.GE_LockHealth_C",
        "/PBZCombatAsset/ActorData/Mobs/065_ChiLian_P2/Ability/GA_ChiLian_P2_Death_Passive.GA_ChiLian_P2_Death_Passive_C"};
    if (path == kChild || path == kYellowSummon) return {
        path == kChild ? "/PBZCombatAsset/ActorData/Mobs/023_Vessel_Child/ActorData/ActorC_Vessel_Child.ActorC_Vessel_Child" :
                        "/PBZCombatAsset/ActorData/Mobs/068_YuZan/ActorData/ActorC_MaleThug_Club_Summon.ActorC_MaleThug_Club_Summon",
        "/PBZCombatAsset/ActorData/Mobs/023_Vessel_Child/GameplayEffects/GE_Vessel_Child_LockHealth.GE_Vessel_Child_LockHealth_C", {}};
    if (path == kLengTu) return {
        "/PBZCombatAsset/ActorData/Mobs/077_LengTu/ActorConfig/ActorC_Lengtu.ActorC_Lengtu",
        "/PBZCombatAsset/ActorData/Mobs/077_LengTu/GameplayEffect/GE_LengTu_LockHealth.GE_LengTu_LockHealth_C",
        kLengTuStoryAbilities[0]};
    // BladeMan/PengMeng low-health berserk, MoYuan switch
    // phases are combat mechanics; retain their own native locks/abilities.
    return {};
}
}  // namespace pbz::enemies
