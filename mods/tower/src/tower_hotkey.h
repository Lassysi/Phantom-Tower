#pragma once
// 1.3.5 自定义打开键（用户 2026-11-08：有玩家键盘没有 Insert）：PBZTowerHotkey.exe 把选好的键写进
// %LOCALAPPDATA%\PBZTower\hotkey.txt（"vk=117" + "name=F6"）。模块每 2 秒看一次文件时间戳，改了就重读，
// 游戏不用重启。Insert 始终有效；自定义键只是多一个。手改文件写了游戏要用的键（移动 / 攻击 / 修饰键…）一律不认，
// 和 exe 里的检查同一份默认占用表（exe 另外还读玩家在游戏里改过的键位）
#include <windows.h>
#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>

namespace tower::hotkey {

// 游戏默认键鼠绑定（PBZInput / PBZContent 的 IMC_* 资源导出，2026-11-08）+ 系统 / 覆盖层常用键 + 影塔对话框确认门用的键
inline constexpr int kReserved[] = {
    'W', 'A', 'S', 'D', VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT,                  // 移动
    'C', 'E', 'F', 'Q', 'R', 'T', 'V', 'M', 'I', 'O', 'P',                 // 下蹲 / 处决交互 / 重击 / 战技 / 药 / 感知 / 地图 / 菜单 / 拍照 / 调试
    '1', '2', '3', '4', '5', '6', '7', '8', VK_NUMPAD3,                     // 换武器 + 调试
    VK_TAB, VK_ESCAPE, VK_SPACE, VK_RETURN, VK_BACK,                         // 地图 / 系统菜单 / 跳跃 / 确认 / 返回
    VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_MENU, VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN, VK_APPS,
    VK_LBUTTON, VK_RBUTTON, VK_MBUTTON,                                      // 轻击 / 格挡 / 锁定
    VK_F12, VK_SNAPSHOT,                                                     // Steam 截图 / 系统截图
    'J', 'K', 'L',                                                           // 影塔对话框的新输入门
};
inline bool Reserved(int vk) { return std::find(std::begin(kReserved), std::end(kReserved), vk) != std::end(kReserved); }
inline bool Usable(int vk) { return vk > 0 && vk < 255 && vk != VK_INSERT && !Reserved(vk); }

struct Setting { int vk = 0; std::string name; };
// "vk=117\nname=F6"；vk 可写十六进制 0x75。不认识 / 不能用 → vk = 0（只有 Insert）
inline Setting Parse(std::string_view text) {
    Setting s;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at); if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(at, end - at); at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.remove_prefix(3);
        if (line.rfind("vk=", 0) == 0) { const std::string v(line.substr(3)); s.vk = static_cast<int>(std::strtol(v.c_str(), nullptr, 0)); }
        else if (line.rfind("name=", 0) == 0) s.name = std::string(line.substr(5));
    }
    if (!Usable(s.vk)) s.vk = 0;
    return s;
}

inline std::wstring Path() {
    wchar_t base[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    return std::wstring(base) + L"\\PBZTower\\hotkey.txt";
}

// 游戏线程每帧调用 Vk()：每 2 秒比一次文件时间戳，变了才读
class Watch {
    Setting cur_;
    FILETIME stamp_{};
    bool had_ = false;
    ULONGLONG next_ = 0;
public:
    template <class Log> int Vk(ULONGLONG now, Log&& log) {
        if (now < next_) return cur_.vk;
        next_ = now + 2000;
        const auto path = Path();
        WIN32_FILE_ATTRIBUTE_DATA a{};
        const bool has = !path.empty() && GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a);
        if (!has) {
            if (had_) { had_ = false; cur_ = {}; log(std::string("影塔界面：自定义打开键已移除，只用 Insert")); }
            return cur_.vk;
        }
        if (had_ && CompareFileTime(&a.ftLastWriteTime, &stamp_) == 0) return cur_.vk;
        had_ = true; stamp_ = a.ftLastWriteTime;
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return cur_.vk;
        char bytes[256]{}; DWORD got = 0;
        ReadFile(f, bytes, sizeof bytes - 1, &got, nullptr);
        CloseHandle(f);
        cur_ = Parse(std::string_view(bytes, got));
        log(cur_.vk ? "影塔界面：自定义打开键 " + (cur_.name.empty() ? std::to_string(cur_.vk) : cur_.name) + "（Insert 仍可用）"
                    : std::string("影塔界面：hotkey.txt 里的键不能用（游戏要用或不认识），只用 Insert"));
        return cur_.vk;
    }
};

}  // namespace tower::hotkey
