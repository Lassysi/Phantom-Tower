// 影塔引擎对模块内其他部分（游戏内界面）开放的接口。全部只在游戏线程调用。
#pragma once
#include <array>
#include <string>
#include <cstdint>

#include "tower_growth_run.h"
#include "tower_trade.h"
#include "tower_logic.h"

namespace towerapi {
// 武库扩建（用户 2026-11-11）：第 kind 类（0 主 / 1 影）轮转里的第 3、4 把（kWeapons 下标，-1 = 没有）
std::array<int, 2> ArsenalExtras(int kind);
bool ArsenalNativeBegin(int kind, int k);  // 原生兵器列表换装事务，负 kind 取消

const tower::Run& Run();                    // 当前局（空闲时 phase = Idle）
bool HasSave();                             // 有可继续的局存档
int ImportedSeed(uint64_t* seed, int* difficulty); // 0 无，1 有效，-1 无效
void Command(const std::string& jsonLine);  // 等同管道 {"op":"tower",…}
std::string Effects();                      // 本层正在生效的神通 / 符（界面显示用），没有返回空
void Housekeep();
const tower::growthrun::Runner& Growth();   // 原生成长：塔档、待选药 / 饰品、出塔进度
std::string Gains();
std::string TripText();                     // 切档进度 / 失败原因（界面显示用）
bool TripFailed();
std::string FloorNote();
bool FailedStageAvailable();
bool FailedStageIsWave();
std::string NextBossPreview();
std::string LastError(int* seq);           // 最近一条要给玩家看的错误与序号（序号变了 = 新错误）
std::string HudFloor();                     // HUD：第 N 层 · 层型（空 = 不显示 HUD）
std::string HudClock();                     // HUD：本层计时（战斗中）
std::string HudGoal();                      // HUD：限时挑战层 / 附加挑战说明与达成状态
std::string BonusText();                    // 本层额外奖励说明（奖励页）
std::string TimesText();                    // 本局逐层用时 + 附加挑战完成数（结算页）                    // 上一次开层没成功的原因（空 = 没有）
bool RebirthOffered();                      // 1.0.1：殒命后正在问用不用重生符
bool ClimbAvailable();                      // 3.5.0：有可继续的 100 层爬塔局（当前位置或停放）
int EndlessNext();                          // 3.5.0：无尽存档的下一层（显示层），没有 = 0
bool EndlessImport(std::string* author, int* shown, int* difficulty);  // 3.5.0：安装器导入、尚未使用的无尽档
int CheckpointShown();                      // 无尽模式：本局可回到的百层存档点（显示层号，0 = 没有）
int RebirthCount();                         // 背包里的重生符数量
int RebirthTarget();                        // 用了回到"已通过 N 层"的准备页
int RebirthNext();                          // 用了之后接着打的层（显示层号）
int ShownNow();                             // 当前层（显示层号，1–100 连排）
std::vector<int> TradeEquipped();           // 3.1.0 驿站交易：当前装备的兵器（不能交出）
std::string TradeNote();                    // 3.1.0 驿站交易：最近一次结果
float MeltPowerMul(void* weaponConfig);
float ShadowEdgeMul();                      // 3.3.4 影刃专精：影之武装强度倍率（没有 = 1）
tower::trade::Prices TradePrices();         // N9i 回收基准价：兵器 / 药 / 饰品（本层驿站同类价）     // N9h 熔铸：这件兵器配置的强度倍率（没熔铸 = 1）；只在游戏线程调
int TradeWeaponPrice();                     // 3.1.0 驿站交易：当前层武器基准价（回收魂玉按 10%）
int ShownNext();                            // 下一场要打的层（显示层号）
std::string Settlement();                   // 通关 / 失败那一刻的结算快照（独立塔档时停在结算页，点退出才出塔）                          // 自动切档停在失败（保护保持，可重试）
int TripPhase();                            // 独立塔档会话：tower::trip::Phase（0 外 / 1 进入中 / 2 塔内 / 3 出塔中 / 4 上次没走完）                        // 最近一层的成长收获文字                           // 空闲时由界面定期调用：上次没换回的塔内兵器（崩溃 / 运行库热换）在这里换回
bool InsideTower();
// 1.1.1：最近一次在场上移除 Boss / 机制角色的时刻（GetTickCount64；没有为 0）。自动回标题要等它之后 3 秒（战斗中移除后马上卸场景偶发闪退）
unsigned long long LastEncounterClear();
bool HiddenUnlocked();                      // 第 51 层入口已解锁（本机文件，失败 / 退出 / 重进存档都保留）                         // 仅正式在塔内（进塔/出塔事务和原档都为false）

}  // namespace towerapi
