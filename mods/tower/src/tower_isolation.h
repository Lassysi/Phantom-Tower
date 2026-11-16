// 塔内原生副作用隔离。两层（1–32 层实测后重做，见 notes/tower-repair-1-32.md 第 A / B 节）：
//
// 会话层 Session（进塔 Inside → 出塔事务开始；单层取消 / 失败 / 奖励 / 商店都不撤）：
//   - 原生战斗组的副作用全部收住：组脚本的 OnGroupCleared / OnGroupMemberDead / OnGroupMemberDespawn / OnGroupDeactivated
//     （原生兵器 / 道具在 MobGroupScript_AddItemAfterCombat_C.OnGroupCleared、PBZMobGroupScript_KillLoot 里发）一律吞掉；
//     组上同名多播委托（关卡蓝图 / Flow 节点在等的"清组 / 成员死亡"）暂时清空，出塔前放回；
//   - 静默原生刷怪点：先给组加清组阻断、收委托，再停刷 / 取消待刷（旧顺序先取消再阻断，组判定清空 → 开场"斩"+ 发兵器）；
//     会话期间凡是刷 Boss 池类的刷怪点（迟发的沐小葵 / 白赤练 / 万钧）都静默；
//   - 原生背包入账（PBZBackPackSubsystem.AddWeaponItem / AddPropsToBackPack / AddPropsByMap / AddPropsSilently / AddSkin /
//     AddCompleteAddItemMobDomainName）不是塔自己发的就拒绝；读档恢复（bIsAddByLoadData）和传送途中放行；
//   - 剧情过场：Flow 节点的播放完成回调对被冻结的过场一律吞掉（Flow 停在这个节点，不往下发奖 / 换图 / 推任务）；
//   - 场地交互过滤（宝箱 / 拾取 / E）。
// 层 Floor（每层开始 / 结束）：场地 key、塔 Boss 归入原生组、过场巡检。
//
// 塔自己的发放走 pe::Call（不经过拦截）；GrantScope 标出塔发放的范围（嵌套调用也放行）。
#pragma once
#include <string>
#include <string_view>

#include "tower_replay.h"

namespace towerisolate {

// 剧情过场判定（纯规则，离线测试）：塔生成角色自己的演出、去场地途中的传送演出不算；Flow 图放的一律算；
// 其余要"本层开始后才开始播（ambient=false）且不循环"
inline bool IsStory(bool flowDriven, bool ambient, bool looping, bool towerOwned, bool travelling) {
    if (towerOwned || travelling) return false;
    return flowDriven || (!ambient && !looping);
}

// 原生入账判定（纯规则，离线测试）：会话外不管；塔自己发的、读档恢复、传送途中放行；其余拒绝
enum class Grant { Allow, Block };
inline Grant JudgeGrant(bool session, bool towerGranting, bool loadData, bool travelling) {
    if (!session || towerGranting || loadData || travelling) return Grant::Allow;
    return Grant::Block;
}

// 组事件判定：会话中，原生组的"清组 / 成员死亡 / 成员退场 / 停用"脚本与委托全部不执行（塔的胜负靠自己的血量判定，不依赖它们）；
// "进战 / 出战 / 激活 / 成员生成"放行（场地机制、MetaAI 协作可能依赖）
inline bool SideEffectEvent(std::string_view fn) {
    return fn == "OnGroupCleared" || fn == "OnGroupMemberDead" || fn == "OnGroupMemberDespawn" || fn == "OnGroupDeactivated";
}

// 会话层
void Session(bool inside, int floor, std::string_view run);  // 每帧（无论有没有进行中的层）：inside = 塔档会话 Inside
bool SessionOn();
void Quiet(void* spawner);                                    // 静默一个原生刷怪点（场地扫描、会话巡检共用；顺序见上）
void AllowTowerSpawn(void* spawner);                          // 塔生成兜底：放行这个已静默刷怪点的生成入口（组隔离保留），本层结束 EndTowerSpawn
void EndTowerSpawn();
struct GrantScope { GrantScope(); ~GrantScope(); GrantScope(const GrantScope&) = delete; GrantScope& operator=(const GrantScope&) = delete; };

// 原生遭遇重放（本层）：契约里的刷怪点不静默、过场生成不拦，放行表里的过场照播（不跳尾、Flow 照常往下走），
// 契约的交互物件可以交互；其余照会话 / 层隔离（战后过场照样冻结）。nullptr = 本层不是重放层
void SetReplay(const tower::replay::Contract* contract);
bool ReplayEntryReady(std::string* why); // 原生蓝图播放入口必须已挂接，才允许玩家开启重放遭遇
bool ReplayPhaseReady(int phase,std::string* why); // newly streamed phases must have their native group milestone listener before adoption
// 冻结的过场若已把玩家切进电影模式（播放节点在 Play 之前就做了准备），结算前把控制权还给玩家
void RestoreControl(const char* why);

// 层
void Begin(std::string_view venueKey);   // 本层开始（出发去场地之前）：venueKey = 场地对应的 Boss 身份
void End();                              // 本层结束 / 作废 / 停止：撤本层的塔 Boss 归组、停巡检（会话层不撤）
void Tick(float dt);                     // 游戏线程每帧（有进行中的层时）
void BlockGroupOf(void* actor);          // 移除原生怪之前调用：给它的战斗组加清组阻断
bool NativeMember(void* actor);          // 重放：Actor 属于契约的原生阶段组（原生遭遇组员，清场保留）
void Adopt(void* pawn);                  // 塔 Boss 进场：归入场地原生战斗组（同类刷怪点的组；多 Boss 并入主 Boss 的组）
bool Unsafe(std::string_view venueKey);  // 已知无法隔离原生剧情的场地
bool InPlayerWorld(void* actor);          // 放在关卡里（Outer 是 Level）且与玩家同一世界；过场占位模板 / 预览对象为 false
bool TookStory();                        // 本层拦到 / 发现过过场
std::string Status();                    // JSON 片段

}  // namespace towerisolate
