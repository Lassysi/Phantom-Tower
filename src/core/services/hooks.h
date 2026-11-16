// L1：函数钩子账本（MinHook）。每个钩子登记名字，可单独启停；卸载时 RemoveAll 全部还原。
// 钩子只在功能开启时 Enable，关闭即 Disable，游戏代码恢复原样。
#pragma once
#include <string>

namespace pbz::hooks {

bool Init(std::string* why);
// 在 target 上建钩子（默认不启用），original 收到跳回原函数的指针
bool Create(const char* name, void* target, void* detour, void** original);
bool Enable(const char* name, bool on);
void RemoveAll(); // Retire this core: restore targets; keep in-flight trampolines resident.

}  // namespace pbz::hooks
