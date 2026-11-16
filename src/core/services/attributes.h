// L1：GAS 属性与 Actor 浮点字段的"倍率仲裁"。
// 角色 PBZCharacter.AttributeSetBase → BasicAttributeSet；每个属性是 GameplayAttributeData{vtbl, BaseValue, CurrentValue}。
// 每帧：功能只声明"想要的倍率"（Want），Commit 时同一字段的多个倍率相乘、只写一次；
//   游戏自己改了值（buff、升级、硬直）就重新记原值；本帧没人要的字段写回原值并忘掉。
// 这样多个功能作用在同一属性上也不会互相覆盖或越乘越大，关闭功能=停止声明，自动还原。
#pragma once
#include <string>
#include <vector>

#include "ue.h"

namespace pbz::attr {

enum class Who { Player, Enemies, World };
enum class Kind {
    Scale,     // 绝对量或乘数（默认 1）：v × m
    Additive,  // 加成项（默认 0，生效值 1+v）：(1+v) × m − 1
    Set,       // 基准值设为 m（多个 Set 取最后一个），同字段的 Scale 仍乘在上面
};

struct Target {
    ue::UObject* obj;   // 角色或 WorldSettings
    uint8_t* set;       // 属性集（World 为空）
    Who who;
    ue::UObject* cls;
};

bool Resolve(std::string* why);
void Refresh(float dt);                           // 有功能开启时游戏线程每帧调用；分帧扫描对象表重建目标列表
const std::vector<Target>& Targets();
bool Alive(const Target& t);
int AttrOffset(const std::string& attribute);     // BasicAttributeSet 内偏移，-1 = 没有
int ActorOffset(const std::string& prop);         // Actor 上的属性偏移（如 CustomTimeDilation）
int WorldOffset(const std::string& prop);         // WorldSettings 上的属性偏移（如 TimeDilation）
int PlayerOffset(const std::string& prop);        // 玩家角色（含蓝图）上的属性偏移（如 RemainingDays）
float* AttrCurrent(const Target& t, int off);     // GameplayAttributeData.CurrentValue
float* AttrBase(const Target& t, int off);        // GameplayAttributeData.BaseValue（伤害、死亡判定改的是它）

void Want(const Target& t, float* field, Kind kind, float m);
void Hold(const Target& t, float* field, float v);  // 本帧直接写成 v；关闭后不回写（锁血、锁杀气用）
void Commit();                                    // 每帧最后调用
void ReleaseAll();                                // 卸载时：全部写回原值

}  // namespace pbz::attr
