// 诊断模式"只开沙盒"（Codex 2026-10-31 对照实验需要）：存档沙盒、原生存档锁照常工作，
// 影塔的战斗、界面（钟菜单入口 / 对话框 / 按钮观察）、传送、清场、剧情拦截全部不活动，
// 用来在沙盒保护下单独测试修改器功能。开关落盘（%LOCALAPPDATA%\PBZTower\sandbox-only.flag），
// 模块加载时就生效，适合"先开诊断模式再进档"。只能在没有进行中的局时打开。
#pragma once
#include <windows.h>

#include <string>

namespace towerdiag {

inline std::wstring FlagPath() {
    wchar_t base[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return {};
    return std::wstring(base) + L"\\PBZTower\\sandbox-only.flag";
}

inline int& Cached() { static int v = -1; return v; }

inline bool SandboxOnly() {
    int& v = Cached();
    if (v < 0) { const auto p = FlagPath(); v = !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
    return v == 1;
}

inline bool SetSandboxOnly(bool on) {
    const auto p = FlagPath();
    if (p.empty()) return false;
    bool ok;
    if (on) {
        CreateDirectoryW(p.substr(0, p.find_last_of(L'\\')).c_str(), nullptr);
        HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        ok = f != INVALID_HANDLE_VALUE;
        if (ok) CloseHandle(f);
    } else {
        ok = DeleteFileW(p.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
    }
    if (ok) Cached() = on ? 1 : 0;
    return ok;
}

}  // namespace towerdiag
