#pragma once
#include <string>
namespace towerpoints {
bool PrepareUi(int panel, std::string* why); // 0 info, 1 weapon, 2 ability, 3 soul
void ClearUi();
int UiBalance(); // -1 outside a native tower growth panel; never a giant dust budget
int UiMode();    // 当前塔内原生面板：0 无 / 1 兵器 / 2 能力 / 3 铸魂（面板没开时 0）
void UiTick();
void RefreshWeaponDust(const char* why);  // 塔内武器表被重新载入后，把每级尘价重新改为 1（"最大升级"按淬炼点数）
void EnterInternal();
void LeaveInternal();
struct Internal {
    Internal() { EnterInternal(); }
    ~Internal() { LeaveInternal(); }
    Internal(const Internal&) = delete;
    Internal& operator=(const Internal&) = delete;
};
} // namespace towerpoints
