#pragma once
#include "../../../src/core/sdk/pbz_module.h"
#include "tower_entry_logic.h"
namespace towerentry {
void Begin(bool quick = false);  // quick：波次层，就绪后直接开打，不读秒
void BeginReplay(); // 不锁玩家、不读秒；场地就绪后 PrepareReplay 只做一次开层补满与读回
tower::entry::State PrepareReplay();
void End();
int Unstick(const char* why);
bool LoadingClear(); // Unknown is false; never start behind an unknown loading screen.
bool Hold(pbz::ue::UObject* actor, bool faceBoss = true);
void ForgetEndedWaveActor(pbz::ue::UObject* actor);
bool Holding();
bool GamePaused();  // 游戏暂停（GameplayStatics.IsGamePaused；读不到玩家也算暂停）
tower::entry::State Tick(bool actorsReady);
const char* Why();
const char* State();
void SetFullState(bool on);  // 开层满状态（默认开；start 的 "full_state":"off" 关）
bool FullStateEnabled();
// 点"下一层"、开层装备套用之后立刻补满一次（药次数 / 影武弹药 / 清上层药效 / 生命与杀气写满），不钉住；读秒时照常再补满并钉住。
// 用户 2026-11-06：原来只在读秒那一刻补，进场后等 Boss 初始化的十来秒一直显示上一层打完的血和药
bool TopUpNow();
int AddSpiritShadow(int n);  // 3.4.0 暗源：施加原生 GE_Talent_RestoreSpiritShadow（瞬时 +1 暗源）n 次；返回成功次数，接口不可用 -1
// 生命回满并让血条刷新：先走原生 PBZRestoreQueryLibrary.ResetHealthToMax（会发属性变化通知），再写满兜底，最后重画血条
void FullHealVisible(const char* why);
const char* TopUpState();    // none / done / failed（本次入场）
} // namespace towerentry
