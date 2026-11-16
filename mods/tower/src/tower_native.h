// 影塔 v2 原生成长的游戏端（tower_session.h 的 IGame 的真实实现）+ 读回当前装配。全部在游戏线程调用。
// 只用实机验证过的原生接口（notes/tower-growth-plan.md、notes/codex-tower-growth-data.md）：
//   兵器 HasWeapon / GetWeaponLevel / UpgradeWeapon(免费加级) / ResetWeaponToLevel(降级)；
//   能力 玩家自己的 PBZAbilityNodesComponent.ActivateNode / DeactivateNode（ASC 取玩家的）；
//   发放 PushMedicineToInventory / PushAccessoryToInventory / 兵器走核心物品编辑（inv::Apply）。
// 未实测的（铸魂 SnapSoulWeapon、药瓶 UpgradeMedicine 的绝对等级语义、打开角色页）每次都读回核对，失败如实返回。
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "tower_session.h"
#include "tower_trip.h"

namespace towernative {

struct Loadout {
    bool ok = false;
    std::vector<std::string> main, shaqi;      // 当前装备的 2 主兵器 / 2 影之武装（空槽为空串）
    std::string medBottom, medTop;             // 药瓶槽：底 0 / 顶 1
    std::vector<std::string> accessories;      // 已装饰品（不含空槽）
    int accessorySlots = 0;                    // 当前可装饰品数（随能力节点变化）
};

// backup：真档备份（引擎提供，绕开沙盒读真文件）
tower::session::IGame& Game(std::function<bool(std::string*)> backup);
Loadout ReadLoadout();
tower::trip::IIo& Trip(std::function<bool(std::string*)> backup);  // 进出独立塔档的游戏端
int Level(std::string_view weaponId, int kind);  // 读回兵器等级（没有返回 -1）
int AccessoryLevel(std::string_view id);         // 饰品等级（原生 CurrentLevel + 1；没有 -1）
void ZeroMedicineUses();                         // 回归基本功：药瓶剩余次数清零
int AccessoryMaxLevel(std::string_view id);      // 原生可打磨上限（同一约定；接口缺失 -1）
int NodeLevel(std::string_view nodeId);
bool OpenCharacterPage(std::string* why);  // 原生 CommonUI 角色页，资产与布局待本批实测
int Snapped(std::string_view id, int kind); // -1 = 接口不可用，0/1 = 原生铸魂状态
int StoryLocked();
// 3.6.0 反震：玩家锁定的敌人 Actor（没锁定返回 nullptr）；游戏线程调用
void* LockTarget();
std::string PlayerStates();  // 诊断：玩家当前全部 ActorState 标签
void PreloadClass(const std::string& path);  // 异步加载蓝图类（不占游戏线程），接口不可用时什么都不做
int EnterDomain(void* boss);  // Boss 自己的 AI 组件收到"玩家进入领域"：1 已调 / 0 没找到组件 / -1 接口不可用  // 玩家在原生剧情镜头锁（开场台词只能慢走）：1 是 / 0 否 / -1 读不到
int EnsureAccessorySlots(int unlocks);  // 饰品槽：基础 2 + unlocks（0–4）格，只加不减；返回读回的可装数，-1 = 不可用

// 和 PBZTower.dll 同目录的 world-template.json（塔档世界模板）；没有返回 false
bool WorldTemplateText(std::string* json);
}  // namespace towernative
