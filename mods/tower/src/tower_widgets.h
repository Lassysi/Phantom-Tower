#pragma once
#include <string>
#include "../../../src/core/sdk/pbz_module.h"

namespace towerwidgets {
using pbz::ue::UObject;
enum class Panel { Character, Weapon, Ability, Soul, Accessory };
// All calls run on the game thread. Uses original widget assets and CommonUI layers.
bool Open(Panel panel, std::string* why);
bool PanelActive();                // 塔压的原生面板、或非塔打开的原生模态（会抢焦点 / 模态的 CommonUI 窗口）正激活
UObject* NativeModal();            // 当前激活的原生模态（不是塔自己的）；没有返回 nullptr
#ifdef PBZ_HEX_TEST
std::string QaCloseBlocking();     // 4.0.0 自动爬塔：关掉挡住开层 / 读秒的原生弹窗与整备面板（DeactivateWidget），返回关掉的类名
#endif
std::string NativeModalName();     // 类名，没有为 "-"
void Own(UObject* widget);         // 塔自己弹的对话框：不算原生模态
void OverlayBusy(bool busy);
bool MenuBusy();
std::string BarDebug();         // QA：原生血条 / 杀气条当前长度、填充、对应上限与原生长度上限（自动爬塔日志用）
void TrackPanel(UObject* widget);
bool IsActivated(UObject* widget);  // CommonActivatableWidget::IsActivated
void Tick();
void Reset();
UObject* Pawn();
UObject* Controller();
UObject* Default(UObject* cls);
UObject* LoadClass(const char* path);
UObject* LoadAsset(const char* path);  // 3.1.0：按路径同步加载资源（贴图等）
UObject* CreateWidget(UObject* cls);  // WidgetBlueprintLibrary.Create（拥有者 = 玩家控制器）
uint64_t Name(const char* value);
bool SetText(UObject* widget, const std::string& value, const char* method = "SetText", const char* param = "InText");
// 3.6.1：读控件文字（EditableText.GetText → KismetTextLibrary.Conv_TextToString），只取 ASCII 部分（数量输入框用）
bool GetText(UObject* widget, std::string* out);
bool Countdown(int number); // Native non-interactive HUD. 0 removes it.
// 倒下表现（用户 2026-11-06：不真死，沿用原生"殒命"书法 + 倒地动作，再弹塔的结算）。只显示：不推原生界面层、关掉它的复活 Tick，
// 隐藏天数 / 返回按钮 / 死亡文字。on=false 撤下并恢复移动。任何一步缺接口返回 false，调用方直接结算
bool DeathShow(bool on);
// 血条重画（用户 2026-11-06："进入下一层后血量显示依然不满"）：塔直接写内存补满生命不会触发属性变化通知，
// 玩家血条 WB_PlayerHPBar_V1 一直停在旧值。补满后调它的 InitMaxBarValue / InitHPBar 按当前属性重画；返回重画的个数
int RefreshHpBar();
int RefreshHpBarFast(float before, float after);  // 缓存血条控件，优先走原生生命变化显示回调
// 同步塔内改写的生命 / 杀气上限到原生 HUD；变更时调用游戏自身回调。
void SyncMaxBars(float health, float healthMax, float shachi, float shachiMax, bool force = false);
void ResetMaxBarSync();
// 原生血条入口被接管后用的真实值：当前生命 / 杀气每次现读这两个属性字段，上限用塔本帧确认过的值。每帧在 SyncMaxBars 前调用
void SetBarSource(void* owner, const float* health, float healthMax, const float* shachi, float shachiMax);
// 用户 2026-11-11：有屏幕 Boss 血条（WB_BossHPBar）的是 Boss，血条挂在模型上（WB_EnemyHPBar）的是杂兵。
// 返回 1 = Boss 血条，2 = 头顶血条，0 = 还没看到原生血条。按类缓存，2 秒重扫一次
int NativeBarKind(void* actor);
// 让游戏原生敌人 / Boss 血条读取塔内生命变化；不创建外置界面。
int RefreshEnemyHpBar(void* actor, float health, float maxHealth);
// 3.6.0 无尽回复：杀气写进属性后原生条不会自己刷新，按比例调 ForSeq_UpdateShachi
int RefreshEnemyShachiBar(void* actor, float shachi, float maxShachi);
} // namespace towerwidgets
