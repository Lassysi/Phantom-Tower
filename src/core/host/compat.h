// 3.0.0 兼容报告：列出和游戏同目录（PBZ.exe 所在文件夹）载入的第三方 DLL（UE4SS 的 dwmapi.dll、ReShade 的 dxgi.dll 等），
// 写进 %LOCALAPPDATA%\PBZTower\core-status.txt（安装器"状态"读它），真正挂不上时弹一次说明，不影响游戏本身。
#pragma once
#include <windows.h>
#include <tlhelp32.h>

#include <string>
#include <vector>

namespace pbz::compat {

inline std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), static_cast<int>(s.size()), nullptr, nullptr);
    if (!s.empty()) s.pop_back();
    return s;
}

inline std::wstring Lower(std::wstring s) { for (auto& c : s) c = static_cast<wchar_t>(towlower(c)); return s; }

// 游戏目录里不属于游戏、也不属于影塔的模块（文件名）
inline std::vector<std::string> ForeignModules() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = Lower(exe);
    dir.resize(dir.find_last_of(L"\\/") + 1);
    static const wchar_t* kGame[] = {L"pbz.exe", L"steam_api64.dll", L"tbbmalloc.dll", L"avrt.dll"};
    std::vector<std::string> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return out;
    MODULEENTRY32W me{sizeof me};
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        const std::wstring path = Lower(me.szExePath);
        if (path.rfind(dir, 0) != 0) continue;
        const std::wstring rel = path.substr(dir.size());
        if (rel.rfind(L"d3d12\\", 0) == 0 || rel.rfind(L"pbztower\\", 0) == 0) continue;  // 游戏自带 Agility SDK、影塔自己
        bool game = false;
        for (auto* g : kGame) if (rel == g) game = true;
        if (!game) out.push_back(Utf8(me.szModule));
    }
    CloseHandle(snap);
    return out;
}

inline std::wstring StatusPath() {
    wchar_t base[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return {};
    std::wstring d = std::wstring(base) + L"\\PBZTower";
    CreateDirectoryW(d.c_str(), nullptr);
    return d + L"\\core-status.txt";
}

// status: ok / conflict；detail：一句话原因；写 UTF-8，三行：状态、原因、第三方模块
inline void WriteStatus(const std::string& status, const std::string& detail, const std::vector<std::string>& mods) {
    const auto path = StatusPath();
    if (path.empty()) return;
    std::string list;
    for (auto& m : mods) list += (list.empty() ? "" : ", ") + m;
    const std::string text = status + "\r\n" + detail + "\r\n" + list + "\r\n";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &n, nullptr);
    CloseHandle(f);
}

// 不阻塞任何游戏线程：单独线程弹系统对话框（全屏时可能在游戏后面，安装器状态页同样会显示）
inline void Warn(const std::string& detail, const std::vector<std::string>& mods) {
    std::string list;
    for (auto& m : mods) list += (list.empty() ? "" : "、") + m;
    auto* text = new std::wstring;
    const std::string body = "影塔·百战 没能接入游戏，本次不启用（游戏本身不受影响）。\n\n原因：" + detail +
        (list.empty() ? std::string() : "\n同目录的其他 Mod：" + list) +
        "\n\n可以先移除或停用上面的 Mod 再试；仍有问题请把 %LOCALAPPDATA%\\PBZLab\\core.log 发给作者。";
    text->resize(MultiByteToWideChar(CP_UTF8, 0, body.c_str(), -1, nullptr, 0));
    MultiByteToWideChar(CP_UTF8, 0, body.c_str(), -1, text->data(), static_cast<int>(text->size()));
    CloseHandle(CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        auto* t = static_cast<std::wstring*>(p);
        MessageBoxW(nullptr, t->c_str(), L"影塔·百战 · 兼容提示", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
        delete t;
        return 0;
    }, text, 0, nullptr));
}

}  // namespace pbz::compat
