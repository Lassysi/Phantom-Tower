// 发布版入口：游戏目录里的 avrt.dll 代理（游戏 EXE 静态导入 AVRT.dll，且它不在 KnownDLLs 里，所以会先载入同目录这一份）。
// 只做两件事：
//   1. 把 20 个导出原样转给 System32\avrt.dll（avrt_stubs.asm 的跳转桩，不关心参数签名）；
//   2. 进程是《影之刃零》时，另起线程载入同目录 PBZTower\PBZCore.dll（运行库自己等引擎就绪、再自动载入 PBZTower.dll）。
// 不改游戏 EXE；删掉 avrt.dll 和 PBZTower 文件夹即完全卸载。
#include <windows.h>

#include <string>

extern "C" void* g_avrt[20];  // avrt_stubs.asm 里的跳转目标表，顺序与 kNames 一致
void* g_avrt[20] = {};

namespace {

constexpr const char* kNames[20] = {
    "AvCreateTaskIndex", "AvQuerySystemResponsiveness", "AvQueryTaskIndexValue", "AvRevertMmThreadCharacteristics",
    "AvRtCreateThreadOrderingGroup", "AvRtCreateThreadOrderingGroupExA", "AvRtCreateThreadOrderingGroupExW",
    "AvRtDeleteThreadOrderingGroup", "AvRtJoinThreadOrderingGroup", "AvRtLeaveThreadOrderingGroup",
    "AvRtWaitOnThreadOrderingGroup", "AvSetMmMaxThreadCharacteristicsA", "AvSetMmMaxThreadCharacteristicsW",
    "AvSetMmThreadCharacteristicsA", "AvSetMmThreadCharacteristicsW", "AvSetMmThreadPriority", "AvSetMultimediaMode",
    "AvTaskIndexYield", "AvTaskIndexYieldCancel", "AvThreadOpenTaskIndex"};

HMODULE g_self = nullptr;

// 系统 avrt 缺某个导出时的兜底：返回 FALSE / 0，并设 ERROR_CALL_NOT_IMPLEMENTED（游戏自己会走失败分支）
extern "C" ULONG_PTR AvrtMissing() {
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}

bool LoadSystemAvrt() {
    wchar_t sys[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    HMODULE real = LoadLibraryW((std::wstring(sys) + L"\\avrt.dll").c_str());
    for (int i = 0; i < 20; ++i) {
        FARPROC p = real ? GetProcAddress(real, kNames[i]) : nullptr;
        g_avrt[i] = p ? reinterpret_cast<void*>(p) : reinterpret_cast<void*>(&AvrtMissing);
    }
    return real != nullptr;
}

std::wstring Dir(HMODULE m) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(m, path, MAX_PATH);
    std::wstring s(path);
    return s.substr(0, s.find_last_of(L"\\/"));
}

bool IsGame() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring s(exe);
    s = s.substr(s.find_last_of(L"\\/") + 1);
    return CompareStringOrdinal(s.c_str(), -1, L"PBZ.exe", -1, TRUE) == CSTR_EQUAL;
}

DWORD WINAPI StartMod(LPVOID) {
    const std::wstring dir = Dir(g_self) + L"\\PBZTower";
    if (GetFileAttributesW((dir + L"\\disabled").c_str()) != INVALID_FILE_ATTRIBUTES) return 0;  // 安装器的"暂停 Mod"开关
    LoadLibraryW((dir + L"\\PBZCore.dll").c_str());
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = mod;
        DisableThreadLibraryCalls(mod);
        LoadSystemAvrt();
        // 不在加载器锁里载入运行库：新线程要等 DllMain 全部返回后才真正开始跑
        if (IsGame()) if (HANDLE t = CreateThread(nullptr, 0, StartMod, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
