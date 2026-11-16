// Generated from Build26367565 cooked CFG + pinned runtime bytecode. No addresses or FName indices.
#pragma once
#include <array>
#include <cstdint>
#include <string_view>
namespace towerpreload::contract {
using namespace std::string_view_literals;
enum class Kind { Object, Field, Name };
struct Ref {int at;Kind kind;std::string_view name,cls,owner;};
struct Segment {int begin;std::string_view bytes;const Ref* refs;int refN;};
struct Rule {std::string_view function;int scriptSize;const Segment* segments;int segmentN;};
inline constexpr std::array<Ref,3> r0s0refs{{
{1,Kind::Field,"K2Node_CustomEvent_Loaded_1","ObjectProperty","ExecuteUbergraph_PlayLevelSequence"},
{10,Kind::Field,"Loaded","ObjectProperty","OnLoaded_F1DA310C4C72315C01F778B6A609A4C8"},
{19,Kind::Object,"ExecuteUbergraph_PlayLevelSequence","Function","PlayLevelSequence_C"},
}};
inline constexpr Segment r0segments[]={
{0,"\x64""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x46""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x1d""\x7a""\x11""\x00""\x00""\x16""\x04""\x0b""\x53"sv,r0s0refs.data(),static_cast<int>(r0s0refs.size())},
};
inline constexpr std::array<Ref,3> r1s0refs{{
{1,Kind::Field,"K2Node_CustomEvent_Result_1","EnumProperty","ExecuteUbergraph_PlayLevelSequence"},
{10,Kind::Field,"Result","EnumProperty","CustomEvent"},
{19,Kind::Object,"ExecuteUbergraph_PlayLevelSequence","Function","PlayLevelSequence_C"},
}};
inline constexpr Segment r1segments[]={
{0,"\x64""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x46""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x1d""\x8f""\x30""\x00""\x00""\x16""\x04""\x0b""\x53"sv,r1s0refs.data(),static_cast<int>(r1s0refs.size())},
};
inline constexpr std::array<Ref,1> r2s0refs{{
{1,Kind::Object,"ExecuteUbergraph_PlayLevelSequence","Function","PlayLevelSequence_C"},
}};
inline constexpr Segment r2segments[]={
{0,"\x46""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x1d""\x8a""\x30""\x00""\x00""\x16""\x04""\x0b""\x53"sv,r2s0refs.data(),static_cast<int>(r2s0refs.size())},
};
inline constexpr std::array<Ref,19> r3s0refs{{
{6,Kind::Field,"MapSwitchPreloadDone","BoolProperty","PlayLevelSequence_C"},
{17,Kind::Field,"MapSwitchPreloadDone","BoolProperty","PlayLevelSequence_C"},
{27,Kind::Field,"CallFunc_GetDisplayName_ReturnValue","StrProperty","ExecuteUbergraph_PlayLevelSequence"},
{36,Kind::Field,"CallFunc_GetDisplayName_ReturnValue","StrProperty","ExecuteUbergraph_PlayLevelSequence"},
{45,Kind::Object,"GetDisplayName","Function","KismetSystemLibrary"},
{54,Kind::Field,"LoadedLevelSequence","ObjectProperty","PlayLevelSequence_C"},
{64,Kind::Field,"CallFunc_Concat_StrStr_ReturnValue","StrProperty","ExecuteUbergraph_PlayLevelSequence"},
{73,Kind::Field,"CallFunc_Concat_StrStr_ReturnValue","StrProperty","ExecuteUbergraph_PlayLevelSequence"},
{82,Kind::Object,"Concat_StrStr","Function","KismetStringLibrary"},
{126,Kind::Field,"CallFunc_GetDisplayName_ReturnValue","StrProperty","ExecuteUbergraph_PlayLevelSequence"},
{136,Kind::Field,"CallFunc_Concat_StrStr_ReturnValue_1","StrProperty","ExecuteUbergraph_PlayLevelSequence"},
{145,Kind::Field,"CallFunc_Concat_StrStr_ReturnValue_1","StrProperty","ExecuteUbergraph_PlayLevelSequence"},
{154,Kind::Object,"Concat_StrStr","Function","KismetStringLibrary"},
{163,Kind::Field,"CallFunc_Concat_StrStr_ReturnValue","StrProperty","ExecuteUbergraph_PlayLevelSequence"},
{184,Kind::Object,"PrintString","Function","KismetSystemLibrary"},
{194,Kind::Field,"CallFunc_Concat_StrStr_ReturnValue_1","StrProperty","ExecuteUbergraph_PlayLevelSequence"},
{205,Kind::Object,"LinearColor","ScriptStruct","/Script/CoreUObject"},
{258,Kind::Object,"PBZFlowNodeMapSwitchCompleted__DelegateSignature","DelegateFunction","/Script/Flow"},
{267,Kind::Field,"OnMapSwitchCompleted","MulticastInlineDelegateProperty","PBZFlowNode"},
}};
inline constexpr std::array<Ref,37> r3s1refs{{
{2,Kind::Field,"Temp_object_Variable","ObjectProperty","ExecuteUbergraph_PlayLevelSequence"},
{11,Kind::Field,"K2Node_CustomEvent_Loaded_1","ObjectProperty","ExecuteUbergraph_PlayLevelSequence"},
{29,Kind::Field,"K2Node_DynamicCast_As____","ObjectProperty","ExecuteUbergraph_PlayLevelSequence"},
{38,Kind::Object,"LevelSequence","Class","/Script/LevelSequence"},
{47,Kind::Field,"Temp_object_Variable","ObjectProperty","ExecuteUbergraph_PlayLevelSequence"},
{65,Kind::Field,"K2Node_DynamicCast_bSuccess","BoolProperty","ExecuteUbergraph_PlayLevelSequence"},
{76,Kind::Field,"K2Node_DynamicCast_As____","ObjectProperty","ExecuteUbergraph_PlayLevelSequence"},
{86,Kind::Field,"K2Node_DynamicCast_bSuccess","BoolProperty","ExecuteUbergraph_PlayLevelSequence"},
{96,Kind::Field,"LoadedLevelSequence","ObjectProperty","PlayLevelSequence_C"},
{105,Kind::Field,"K2Node_DynamicCast_As____","ObjectProperty","ExecuteUbergraph_PlayLevelSequence"},
{115,Kind::Field,"CallFunc_GetWorldSubsystem_ReturnValue_2","ObjectProperty","ExecuteUbergraph_PlayLevelSequence"},
{124,Kind::Object,"GetWorldSubsystem","Function","SubsystemBlueprintLibrary"},
{134,Kind::Object,"PBZCinematicVTSubsystem","Class","/Script/ProjectPBZ"},
{145,Kind::Field,"CallFunc_GetGameInstanceSubsystem_ReturnValue","ObjectProperty","ExecuteUbergraph_PlayLevelSequence"},
{154,Kind::Object,"GetGameInstanceSubsystem","Function","SubsystemBlueprintLibrary"},
{164,Kind::Object,"EventPipelineGameInstance","Class","/Script/ProjectPBZ"},
{174,Kind::Field,"CallFunc_GetPreload_ReturnValue","FloatProperty","ExecuteUbergraph_PlayLevelSequence"},
{183,Kind::Field,"CallFunc_GetPreload_ReturnValue","FloatProperty","ExecuteUbergraph_PlayLevelSequence"},
{193,Kind::Field,"CallFunc_GetGameInstanceSubsystem_ReturnValue","ObjectProperty","ExecuteUbergraph_PlayLevelSequence"},
{205,Kind::Field,"CallFunc_GetPreload_ReturnValue","FloatProperty","ExecuteUbergraph_PlayLevelSequence"},
{214,Kind::Object,"GetPreload","Function","EventPipelineGameInstance"},
{225,Kind::Field,"CallFunc_Preload_ReturnValue_1","ObjectProperty","ExecuteUbergraph_PlayLevelSequence"},
{235,Kind::Field,"CallFunc_GetWorldSubsystem_ReturnValue_2","ObjectProperty","ExecuteUbergraph_PlayLevelSequence"},
{247,Kind::Field,"CallFunc_Preload_ReturnValue_1","ObjectProperty","ExecuteUbergraph_PlayLevelSequence"},
{256,Kind::Object,"Preload","Function","PBZCinematicVTSubsystem"},
{265,Kind::Field,"LoadedLevelSequence","ObjectProperty","PlayLevelSequence_C"},
{274,Kind::Field,"CallFunc_GetPreload_ReturnValue","FloatProperty","ExecuteUbergraph_PlayLevelSequence"},
{283,Kind::Field,"CallFunc_GetPreload_ReturnValue","FloatProperty","ExecuteUbergraph_PlayLevelSequence"},
{311,Kind::Name,"CustomEvent","",""},
{324,Kind::Field,"K2Node_CreateDelegate_OutputDelegate","DelegateProperty","ExecuteUbergraph_PlayLevelSequence"},
{336,Kind::Field,"CallFunc_Preload_ReturnValue_1","ObjectProperty","ExecuteUbergraph_PlayLevelSequence"},
{348,Kind::Field,"OnFinished","MulticastInlineDelegateProperty","PBZCineVTPreloadHandle"},
{357,Kind::Field,"OnFinished","MulticastInlineDelegateProperty","PBZCineVTPreloadHandle"},
{366,Kind::Field,"K2Node_CreateDelegate_OutputDelegate","DelegateProperty","ExecuteUbergraph_PlayLevelSequence"},
{375,Kind::Object,"Delay","Function","KismetSystemLibrary"},
{390,Kind::Object,"LatentActionInfo","ScriptStruct","/Script/Engine"},
{413,Kind::Name,"ExecuteUbergraph_PlayLevelSequence","",""},
}};
inline constexpr std::array<Ref,7> r3s2refs{{
{1,Kind::Name,"OnLoaded_F1DA310C4C72315C01F778B6A609A4C8","",""},
{14,Kind::Field,"K2Node_CreateDelegate_OutputDelegate_12","DelegateProperty","ExecuteUbergraph_PlayLevelSequence"},
{24,Kind::Object,"LoadAsset","Function","KismetSystemLibrary"},
{34,Kind::Field,"LevelSequence","SoftObjectProperty","PlayLevelSequence_C"},
{43,Kind::Field,"K2Node_CreateDelegate_OutputDelegate_12","DelegateProperty","ExecuteUbergraph_PlayLevelSequence"},
{52,Kind::Object,"LatentActionInfo","ScriptStruct","/Script/Engine"},
{75,Kind::Name,"ExecuteUbergraph_PlayLevelSequence","",""},
}};
inline constexpr std::array<Ref,8> r3s3refs{{
{2,Kind::Field,"CallFunc_GetGameInstanceSubsystem_ReturnValue_1","ObjectProperty","ExecuteUbergraph_PlayLevelSequence"},
{11,Kind::Object,"GetGameInstanceSubsystem","Function","SubsystemBlueprintLibrary"},
{21,Kind::Object,"EventPipelineGameInstance","Class","/Script/ProjectPBZ"},
{32,Kind::Field,"CallFunc_GetUseNewBlackScreen_ReturnValue","BoolProperty","ExecuteUbergraph_PlayLevelSequence"},
{42,Kind::Field,"CallFunc_GetGameInstanceSubsystem_ReturnValue_1","ObjectProperty","ExecuteUbergraph_PlayLevelSequence"},
{54,Kind::Field,"CallFunc_GetUseNewBlackScreen_ReturnValue","BoolProperty","ExecuteUbergraph_PlayLevelSequence"},
{63,Kind::Object,"GetUseNewBlackScreen","Function","EventPipelineGameInstance"},
{74,Kind::Field,"CallFunc_GetUseNewBlackScreen_ReturnValue","BoolProperty","ExecuteUbergraph_PlayLevelSequence"},
}};
inline constexpr std::array<Ref,0> r3s4refs{{
}};
inline constexpr Segment r3segments[]={
{4197,"\x07""\x74""\x10""\x00""\x00""\x01""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x4d""\x14""\x01""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x27""\x0f""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x68""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x01""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x16""\x0f""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x68""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x34""\x41""\x00""\x73""\x00""\x79""\x00""\x6e""\x00""\x63""\x00""\x68""\x00""\x72""\x00""\x6f""\x00""\x6e""\x00""\x6f""\x00""\x75""\x00""\x73""\x00""\x20""\x00""\x56""\x00""\x54""\x00""\x1a""\xff""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x16""\x0f""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x68""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x1f""\x20""\x43""\x6f""\x6d""\x70""\x6c""\x65""\x74""\x65""\x00""\x16""\x68""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x17""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x27""\x27""\x2f""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x10""\x00""\x00""\x00""\x1e""\x0d""\xaa""\x11""\x3f""\x1e""\xd7""\xde""\xff""\x3e""\x1e""\x00""\x00""\x00""\x00""\x1e""\x00""\x00""\x80""\x3f""\x30""\x1e""\x00""\x00""\xc6""\x42""\x21""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x16""\x63""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x01""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x16""\x4d"sv,r3s0refs.data(),static_cast<int>(r3s0refs.size())},
{4474,"\x5f""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x0f""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x2e""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x0f""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x38""\x01""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x4f""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x5f""\x01""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x5f""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x68""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x17""\x20""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x16""\x5f""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x68""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x17""\x20""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x16""\x0f""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x19""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x0a""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x1c""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x16""\x5f""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x19""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x37""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x1c""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x01""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x1e""\x00""\x00""\xa0""\x41""\x21""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x16""\x61""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x17""\x5c""\x19""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x09""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x01""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x68""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x17""\x1e""\x00""\x00""\x80""\x3f""\x2f""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x20""\x00""\x00""\x00""\x5b""\x65""\x10""\x00""\x00""\x1d""\x50""\x88""\x65""\xbb""\x21""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x17""\x30""\x16""\x4d"sv,r3s1refs.data(),static_cast<int>(r3s1refs.size())},
{4903,"\x61""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x17""\x68""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x17""\x01""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x2f""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x20""\x00""\x00""\x00""\x1d""\xff""\xff""\xff""\xff""\x1d""\xc4""\x6f""\x83""\x48""\x21""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x17""\x30""\x16""\x4d"sv,r3s2refs.data(),static_cast<int>(r3s2refs.size())},
{8202,"\x5f""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x68""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x17""\x20""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x16""\x14""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x19""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x0a""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x1c""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x16""\x4f""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x00""\x06""\x27""\x13""\x00""\x00"sv,r3s3refs.data(),static_cast<int>(r3s3refs.size())},
{12426,"\x06""\x0a""\x20""\x00""\x00""\x06""\x65""\x10""\x00""\x00"sv,r3s4refs.data(),static_cast<int>(r3s4refs.size())},
};
inline constexpr Rule rules[]={
{"OnLoaded_F1DA310C4C72315C01F778B6A609A4C8",36,r0segments,static_cast<int>(std::size(r0segments))},
{"CustomEvent",36,r1segments,static_cast<int>(std::size(r1segments))},
{"WaitMapSwitch",18,r2segments,static_cast<int>(std::size(r2segments))},
{"ExecuteUbergraph_PlayLevelSequence",13251,r3segments,static_cast<int>(std::size(r3segments))},
};
inline constexpr int graphEntries[]={4197,4474,4903,8202,12426,12431};
}
