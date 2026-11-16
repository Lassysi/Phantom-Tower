// 场地运行时（用户 2026-11-01：只用场地模型 + 雾门，原生遭遇不启动，Boss 塔自己刷）。全部在游戏线程调用。
// 数据：venues.generated.h（Codex 静态提取的出生点 / 雾门 actor 名、内缩凸包）。证据等级：静态，运行时按名字找 actor、读实际位置。
#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "tower_arena.h"

namespace towervenue {

void Arrive(std::string_view bossPath);  // 到场地后开始找对象（最多 10 秒，等流送）
void Tick();                             // 每帧
bool Ready();                            // 找完了（找到或放弃）
bool Found();                            // 找到了出生点（场地数据可用）
bool MissingRequired();                  // Known sky venue timed out without its native spawner.
void* Spawner();                         // 本场地的原生出生点刷怪点对象（没找到 nullptr）
std::vector<void*> GroupMates();         // 和出生点刷怪点同一战斗组的其他刷怪点（20 层阿瑾的红衣兵）
bool CloseDoors();                       // 读秒前关雾门/解除已确认的内层AI墙；失败拒绝生成
bool DoorsClosed();
void KeepDoorsClosed();                // 3.0.0：战斗中每秒核对雾门，被原生改回打开就再关（自身限流）                      // 原生雾门已关且至少一扇还在：玩家活动范围由原生雾墙限制
void OpenDoors();                        // 本层结束 / 作废 / 暂离：开门
// 战斗边界：凸包（世界坐标）或以出生点为圆心的圆；返回 false = 没有场地数据（调用方用默认圆）
bool Boundary(std::vector<tower::arena::P3>* hull, tower::arena::P3* center, double* radius, double* zmin, double* zmax);
void End();
std::string Status();                    // JSON 片段（诊断）
#ifdef PBZ_HEX_TEST
void BeginSample(const std::string& line);
void SampleTick();
#endif

}  // namespace towervenue
