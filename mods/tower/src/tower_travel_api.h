// 影塔传送对引擎开放的接口（tower_travel.cpp）。全部只在游戏线程调用。
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace towertravel {

void Register();
// 本层目的地：手动标定的 boss:<身份> → 游戏数据里的原生刷怪点 → 本区通用擂台。
// 返回 0 已在那里（同关卡 40 米内），1 开始传送，-1 没有目的地（就地打）
int GoFloor(int zone, uint64_t seed, std::string_view path, std::string_view identity, std::string_view name, bool scene);
bool HasVenue(std::string_view path, std::string_view identity);
int ZoneArena(int zone, uint64_t seed);  // 本区通用 / 经典竞技场下标（没有 -1）
int GoArena(int index);                  // 去这个竞技场：同 GoFloor 的返回值
bool ArenaVerified(int index);           // %LOCALAPPDATA%\PBZTower\arena-verified.txt 里列出（Codex 实测过）  // 有原生刷怪点或手动标定过场地
bool RecordHome();                   // 记下进塔位置（已有记录则保留）
void GoHome();                       // 传回进塔位置（没有记录不动）
void ForgetHome();                   // 丢掉进塔位置记录（独立塔档出塔不走传送，靠原档基准回到原位）
// 取消进行中的旅行（暂离 / 取消入场 / 出塔）：不再发后续的钟或点传送；已发出的原生加载由游戏自己落地（等待见 pbz::TeleportBusy）
void Abort(const char* why);
bool Busy();                         // 传送进行中
bool Failed();                       // 上一次传送失败（Why 给原因）
bool Stopped();                      // 失败且不该自动重试（原生异常 / 加载一直不结束）：取消本层，不换 Boss 再跨图
std::string Why();
bool AtDestination(); // Recheck level and XYZ before spawn; a native respawn is not arrival.
// 1.1.1：到达点（同一关卡里才给；关卡不同返回 false，不能点传送过去）。开打前复核不在到达点时先放回再判
bool ArrivalHere(double xyzYaw[4]);
std::string Command(const std::string& value, const std::string& id);  // cmd "arena"：list / mark / unmark / go，返回 JSON 片段

}  // namespace towertravel
