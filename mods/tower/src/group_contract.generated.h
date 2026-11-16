// Generated from Build26367565 cooked CFG + pinned runtime bytecode. No addresses or FName indices.
#pragma once
#include <array>
#include <cstdint>
#include <string_view>
#include "preload_contract.generated.h"
using namespace std::string_view_literals;
namespace towerpreload::contract {
inline constexpr std::array<Ref,1> g0s0refs{{
{1,Kind::Object,"ExecuteUbergraph_MobGroupScript_ClearGroup","Function","MobGroupScript_ClearGroup_C"},
}};
inline constexpr Segment g0segments[]={
{0,"\x46""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x1d""\x0a""\x00""\x00""\x00""\x16""\x04""\x0b""\x53"sv,g0s0refs.data(),static_cast<int>(g0s0refs.size())},
};
inline constexpr std::array<Ref,1> g1s0refs{{
{1,Kind::Object,"ExecuteUbergraph_MobGroupScript_ClearGroup","Function","MobGroupScript_ClearGroup_C"},
}};
inline constexpr Segment g1segments[]={
{0,"\x46""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x1d""\x4f""\x00""\x00""\x00""\x16""\x04""\x0b""\x53"sv,g1s0refs.data(),static_cast<int>(g1s0refs.size())},
};
inline constexpr std::array<Ref,47> g2s0refs{{
{2,Kind::Field,"EntryPoint","IntProperty","ExecuteUbergraph_MobGroupScript_ClearGroup"},
{11,Kind::Name,"OnMobGroupCleared_Event","",""},
{24,Kind::Field,"K2Node_CreateDelegate_OutputDelegate","DelegateProperty","ExecuteUbergraph_MobGroupScript_ClearGroup"},
{36,Kind::Field,"MobGroup","ObjectProperty","MobGroupScript"},
{48,Kind::Field,"OnMobGroupCleared","MulticastInlineDelegateProperty","MobGroup"},
{57,Kind::Field,"OnMobGroupCleared","MulticastInlineDelegateProperty","MobGroup"},
{66,Kind::Field,"K2Node_CreateDelegate_OutputDelegate","DelegateProperty","ExecuteUbergraph_MobGroupScript_ClearGroup"},
{80,Kind::Field,"EventTag","StructProperty","GameplayEventData"},
{89,Kind::Field,"EventTag","StructProperty","GameplayEventData"},
{98,Kind::Field,"K2Node_MakeStruct_GameplayEventData","StructProperty","ExecuteUbergraph_MobGroupScript_ClearGroup"},
{107,Kind::Object,"GameplayTag","ScriptStruct","/Script/GameplayTags"},
{135,Kind::Field,"Instigator","ObjectProperty","GameplayEventData"},
{144,Kind::Field,"K2Node_MakeStruct_GameplayEventData","StructProperty","ExecuteUbergraph_MobGroupScript_ClearGroup"},
{155,Kind::Field,"Target","ObjectProperty","GameplayEventData"},
{164,Kind::Field,"K2Node_MakeStruct_GameplayEventData","StructProperty","ExecuteUbergraph_MobGroupScript_ClearGroup"},
{175,Kind::Field,"OptionalObject","ObjectProperty","GameplayEventData"},
{184,Kind::Field,"K2Node_MakeStruct_GameplayEventData","StructProperty","ExecuteUbergraph_MobGroupScript_ClearGroup"},
{195,Kind::Field,"OptionalObject2","ObjectProperty","GameplayEventData"},
{204,Kind::Field,"K2Node_MakeStruct_GameplayEventData","StructProperty","ExecuteUbergraph_MobGroupScript_ClearGroup"},
{214,Kind::Field,"ContextHandle","StructProperty","GameplayEventData"},
{223,Kind::Field,"ContextHandle","StructProperty","GameplayEventData"},
{232,Kind::Field,"K2Node_MakeStruct_GameplayEventData","StructProperty","ExecuteUbergraph_MobGroupScript_ClearGroup"},
{241,Kind::Object,"GameplayEffectContextHandle","ScriptStruct","/Script/GameplayAbilities"},
{255,Kind::Field,"InstigatorTags","StructProperty","GameplayEventData"},
{264,Kind::Field,"InstigatorTags","StructProperty","GameplayEventData"},
{273,Kind::Field,"K2Node_MakeStruct_GameplayEventData","StructProperty","ExecuteUbergraph_MobGroupScript_ClearGroup"},
{282,Kind::Field,"EventTag","StructProperty","MobGroupScript_ClearGroup_C"},
{291,Kind::Field,"TargetTags","StructProperty","GameplayEventData"},
{300,Kind::Field,"TargetTags","StructProperty","GameplayEventData"},
{309,Kind::Field,"K2Node_MakeStruct_GameplayEventData","StructProperty","ExecuteUbergraph_MobGroupScript_ClearGroup"},
{318,Kind::Object,"GameplayTagContainer","ScriptStruct","/Script/GameplayTags"},
{331,Kind::Field,"GameplayTags","StructProperty","GameplayTagContainer::GameplayTags"},
{346,Kind::Field,"EventMagnitude","FloatProperty","GameplayEventData"},
{355,Kind::Field,"EventMagnitude","FloatProperty","GameplayEventData"},
{364,Kind::Field,"K2Node_MakeStruct_GameplayEventData","StructProperty","ExecuteUbergraph_MobGroupScript_ClearGroup"},
{378,Kind::Field,"TargetData","StructProperty","GameplayEventData"},
{387,Kind::Field,"TargetData","StructProperty","GameplayEventData"},
{396,Kind::Field,"K2Node_MakeStruct_GameplayEventData","StructProperty","ExecuteUbergraph_MobGroupScript_ClearGroup"},
{405,Kind::Object,"GameplayAbilityTargetDataHandle","ScriptStruct","/Script/GameplayAbilities"},
{420,Kind::Field,"CallFunc_GetPlayerCharacter_ReturnValue","ObjectProperty","ExecuteUbergraph_MobGroupScript_ClearGroup"},
{429,Kind::Object,"GetPlayerCharacter","Function","GameplayStatics"},
{438,Kind::Field,"MobGroup","ObjectProperty","MobGroupScript"},
{453,Kind::Object,"SendGameplayEventToActor","Function","AbilitySystemBlueprintLibrary"},
{462,Kind::Field,"CallFunc_GetPlayerCharacter_ReturnValue","ObjectProperty","ExecuteUbergraph_MobGroupScript_ClearGroup"},
{471,Kind::Object,"GameplayTag","ScriptStruct","/Script/GameplayTags"},
{484,Kind::Name,"GameplayEvent.MobGroup.Clear","",""},
{498,Kind::Field,"K2Node_MakeStruct_GameplayEventData","StructProperty","ExecuteUbergraph_MobGroupScript_ClearGroup"},
}};
inline constexpr Segment g2segments[]={
{0,"\x4e""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x61""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x17""\x5c""\x19""\x01""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x09""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x01""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x06""\xfb""\x01""\x00""\x00""\x0f""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x42""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x2f""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x0c""\x00""\x00""\x00""\x21""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x30""\x5f""\x42""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x2a""\x5f""\x42""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x2a""\x5f""\x42""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x2a""\x5f""\x42""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x2a""\x0f""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x42""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x2f""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x18""\x00""\x00""\x00""\x30""\x0f""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x42""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x01""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x0f""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x42""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x2f""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x20""\x00""\x00""\x00""\x65""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x66""\x30""\x0f""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x42""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x1e""\x00""\x00""\x00""\x00""\x0f""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x42""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x2f""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x28""\x00""\x00""\x00""\x30""\x5f""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x68""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x01""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x1d""\x00""\x00""\x00""\x00""\x16""\x68""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x2f""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x0c""\x00""\x00""\x00""\x21""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x30""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x16""\x04""\x0b""\x53"sv,g2s0refs.data(),static_cast<int>(g2s0refs.size())},
};
inline constexpr Rule groupRules[]={
{"OnGroupActivated",18,g0segments,static_cast<int>(std::size(g0segments))},
{"OnMobGroupCleared_Event",18,g1segments,static_cast<int>(std::size(g1segments))},
{"ExecuteUbergraph_MobGroupScript_ClearGroup",510,g2segments,static_cast<int>(std::size(g2segments))},
};
}
