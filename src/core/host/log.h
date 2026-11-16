// L0：日志写到 %LOCALAPPDATA%\PBZLab\core.log（不写游戏目录），同时转发给已连接的面板。
#pragma once
#include <windows.h>

#include <share.h>

#include <cstdio>
#include <format>
#include <functional>
#include <mutex>
#include <string>

namespace pbz::log {

#ifdef PBZ_MODULE
void Write(const std::string& s);  // 模块里由 sdk/module_shim.cpp 转发给运行库
#else
inline std::mutex g_lock;
inline FILE* g_file = nullptr;
inline std::function<void(const std::string&)> g_sink;  // 由 ipc 注册

inline void Open() {
    wchar_t dir[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return;
    std::wstring d = std::wstring(dir) + L"\\PBZLab";
    CreateDirectoryW(d.c_str(), nullptr);
    // 二进制、自己写 UTF-8 字节。不能用 ccs=UTF-8 文本模式：窄字符 fprintf 会触发 CRT 非法参数，直接结束游戏进程
#ifdef PBZ_MOD_ONLY
    // 发布版：每次启动新开一份（上一份留作 core.prev.log），写缓冲 64 KB，不在游戏线程逐行刷盘
    const std::wstring path = d + L"\\core.log";
    MoveFileExW(path.c_str(), (d + L"\\core.prev.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    g_file = _wfsopen(path.c_str(), L"wb", _SH_DENYNO);
    if (g_file) setvbuf(g_file, nullptr, _IOFBF, 1 << 16);
#else
    g_file = _wfsopen((d + L"\\core.log").c_str(), L"ab", _SH_DENYNO);  // 允许外部同时读
#endif
}
#ifdef PBZ_MOD_ONLY
inline constexpr long long kMaxLogBytes = 16ll << 20;  // 发布版单次启动最多 16 MB，超出后只留异常诊断
inline long long g_written = 0;
inline ULONGLONG g_lastFlush = 0;
#endif

inline void Write(const std::string& s) {
    std::lock_guard lk(g_lock);
    SYSTEMTIME t;
    GetLocalTime(&t);
    auto line = std::format("[{:02}:{:02}:{:02}.{:03}] {}", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, s);
    if (g_file) {
#ifdef PBZ_MOD_ONLY
        if (g_written < kMaxLogBytes) {
            fwrite(line.data(), 1, line.size(), g_file);
            fwrite("\r\n", 1, 2, g_file);
            g_written += static_cast<long long>(line.size()) + 2;
            if (g_written >= kMaxLogBytes) { static const char cap[] = "[log] 已达本次上限，后续只记异常诊断\r\n"; fwrite(cap, 1, sizeof cap - 1, g_file); fflush(g_file); }
        }
        const ULONGLONG now = GetTickCount64();
        if (now - g_lastFlush > 2000) { g_lastFlush = now; fflush(g_file); }
#else
        fwrite(line.data(), 1, line.size(), g_file);
        fwrite("\r\n", 1, 2, g_file);
        fflush(g_file);
#endif
    }
    if (g_sink) g_sink(line);
}
#endif

#ifndef PBZ_MODULE
// 发布版写缓冲：由运行库守护线程每 2 秒调用一次，安静时缓冲里的内容也会落盘（崩溃前最多丢 2 秒）
inline void Flush() { std::lock_guard lk(g_lock); if (g_file) fflush(g_file); }
#endif

template <class... A> void Info(std::format_string<A...> f, A&&... a) { Write(std::format(f, std::forward<A>(a)...)); }

}  // namespace pbz::log
