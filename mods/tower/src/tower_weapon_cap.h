#pragma once
// Test-only native cap adapter. The production build leaves the game untouched
// until the native transaction, restore and save/resume have been verified.
namespace towerweaponcap {
bool Enable();
void Disable();
bool Ready();
}
