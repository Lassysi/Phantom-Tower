// 战斗取证（只观察，不改结果）：1–32 层 G1 / G2 / D 类的待追踪项。
//  - 致死来源（20 / 21）：玩家最近几次受伤的数值、伤害标签（投技 / 处决 / 脚本伤害各有标签）、伤害原因（DamageCause）；判负 / 辅助接住时打出来；
//  - 23 骨婉约"普通 Hit 不命中、反制能伤"：每只塔 Boss 本层收到的伤害事件按标签计数，结束时打一行；
//  - 6 清道夫不出招：Boss 开打后 30 秒没让玩家受过伤就记一次（附 AI 控制器类名），只记不改。
// 观察点：PBZCharacter.OnDamage（伤害结算后的回调，含标签）和 AttributeComponent.TakeDamage（伤害原因），都经 ProcessEvent。
// 如果实机日志里两者都没有出现，说明该路径不经 ProcessEvent，需要换观察点（写进证据）。
#pragma once
#include <string>

namespace towercombat {

void Begin(int floor, void* player);  // 本层放行时
void Watch(void* boss, const std::string& name);  // 塔 Boss 回执后
void Tick(double fightSeconds);       // 战斗中每帧（内部限流）
// 血量采样（塔自己每帧读的生命值）：观察点一次都没收到时（33 / 35 层两个观察点都是 0 次），按血量下降记受伤，标明来源是"血量下降"
void Sample(void* who, float hp);
void Adjusted(void* who,float hp,const char* reason); // Tower normalization/refill is not enemy damage
std::string LastHits(int n);           // 玩家最近 n 次受伤（致死来源）
// 无伤挑战（v14）：本层玩家身上是否收到过原生伤害事件（OnDamage）；收到过时，只有伴随伤害事件的扣血才算受伤
// （武器技能主动耗血不经伤害结算，不算）。没收到过（观察点不可用）时调用方退回"扣血即受伤"
bool PlayerDamageRoute();
double SincePlayerDamage();            // 距最近一次玩家伤害事件（战斗秒）；没有返回很大的数
void End(const char* why);             // 本层结束 / 取消：打 Boss 受伤计数汇总并摘掉观察

}  // namespace towercombat
