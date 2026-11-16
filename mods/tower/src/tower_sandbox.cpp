// 影塔存档沙盒（实验，证据等级：未验证）：开启后游戏对存档目录（…\ProjectPBZ\Saved\SaveGames）的写入、删除、改名
// 全部改到 %LOCALAPPDATA%\PBZTower\sandbox\SaveGames，原存档文件在磁盘上一个字节都不变；读取时沙盒里有就读沙盒（游戏写完回读校验时一致），
// 没有就读原文件。关闭后一切照旧，沙盒里的文件可清空。
// 为什么在文件层而不是拦 CanSaveGame：2026-10-30 调用记录显示每次存档（钟"保存记忆"和自动存档）都先问各系统 CanSaveGame，
// 但不知道某个系统回"不能存"时游戏是整次放弃还是只跳过它——跳过就会写出缺背包的残档；文件层改道则不管游戏怎么存都碰不到原档。
// 钩的是 KernelBase 的 CreateFileW / CreateFile2 / DeleteFileW / MoveFileExW / MoveFileWithProgressW / ReplaceFileW /
// CopyFileW / CopyFileExW / CopyFile2 / SetFileInformationByHandle（按句柄改名：沙盒里打开的文件改名到原目录会把测试档搬回真档）；
// 回调在游戏的任意线程，只做路径改写。
// 2026-10-31 加两层（游戏线程，见文件末尾）：原生存档锁 EnterSaveVeto（塔内游戏根本不存档；运行库被卸掉后锁仍在游戏里）
// 和基准快照（进塔记下真进度 SaveJsonObjectToString，出塔 ReplaceSaveString 放回内存；同进程回标题时 LoadGame 读的是
// 内存缓存，光换磁盘文件没用——Codex 实测）。基准没放回前不许关沙盒（除非强制）。
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <filesystem>
#include <format>
#include <functional>
#include <mutex>
#include <string>

#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include "../../../src/core/host/log.h"
#include "../../../src/core/services/hooks.h"

namespace {

std::atomic<bool> g_on{false};
std::atomic<long> g_writes{0}, g_deletes{0}, g_moves{0}, g_reads{0};
std::wstring g_root, g_rootLower, g_box;  // 原存档目录（带结尾 \）、小写版、沙盒目录（带结尾 \）
bool g_hooked = false;
thread_local int t_inside = 0;  // 我们自己调文件 API 时不再改道

using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using DeleteFileW_t = BOOL(WINAPI*)(LPCWSTR);
using MoveFileExW_t = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, DWORD);
using MoveFileWithProgressW_t = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, LPPROGRESS_ROUTINE, LPVOID, DWORD);
using ReplaceFileW_t = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, LPCWSTR, DWORD, LPVOID, LPVOID);
using CreateFile2_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);
using CopyFileW_t = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, BOOL);
using CopyFileExW_t = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, LPPROGRESS_ROUTINE, LPVOID, LPBOOL, DWORD);
using CopyFile2_t = HRESULT(WINAPI*)(PCWSTR, PCWSTR, COPYFILE2_EXTENDED_PARAMETERS*);
using SetInfo_t = BOOL(WINAPI*)(HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID, DWORD);
CreateFileW_t o_create = nullptr;
DeleteFileW_t o_delete = nullptr;
MoveFileExW_t o_move = nullptr;
MoveFileWithProgressW_t o_moveP = nullptr;
ReplaceFileW_t o_replace = nullptr;
CreateFile2_t o_create2 = nullptr;
CopyFileW_t o_copy = nullptr;
CopyFileExW_t o_copyEx = nullptr;
CopyFile2_t o_copy2 = nullptr;
SetInfo_t o_setInfo = nullptr;

// 存档目录下的路径 → 沙盒里对应的路径；不在存档目录下返回空
std::wstring Map(LPCWSTR path) {
    if (!path || !*path || g_root.empty()) return {};
    wchar_t full[1024];
    const DWORD n = GetFullPathNameW(path, 1024, full, nullptr);
    if (!n || n >= 1024) return {};
    std::wstring p(full, n);
    if (p.rfind(L"\\\\?\\", 0) == 0) p = p.substr(4);
    for (auto& c : p) if (c == L'/') c = L'\\';
    std::wstring low = p;
    for (auto& c : low) c = static_cast<wchar_t>(std::towlower(c));
    if (low.rfind(g_rootLower, 0) != 0) return {};
    return g_box + p.substr(g_root.size());
}
bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
void EnsureDir(const std::wstring& file) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(file).parent_path(), ec);
}
bool Writes(DWORD access, DWORD disposition) {
    return (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE)) || disposition == CREATE_ALWAYS ||
           disposition == CREATE_NEW || disposition == TRUNCATE_EXISTING;
}

HANDLE WINAPI Create(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE tmpl) {
    if (g_on && !t_inside) {
        if (auto box = Map(path); !box.empty()) {
            ++t_inside;
            if (Writes(access, disposition)) {
                EnsureDir(box);
                // 打开已有文件来改（OPEN_EXISTING / OPEN_ALWAYS 写）：先把原文件复制进沙盒，改的是副本
                if (!Exists(box) && disposition != CREATE_ALWAYS && disposition != CREATE_NEW) {
                    wchar_t full[1024];
                    if (GetFullPathNameW(path, 1024, full, nullptr)) CopyFileW(full, box.c_str(), TRUE);
                }
                ++g_writes;
                --t_inside;
                return o_create(box.c_str(), access, share, sa, disposition, flags, tmpl);
            }
            const bool inBox = Exists(box);
            --t_inside;
            if (inBox) { ++g_reads; return o_create(box.c_str(), access, share, sa, disposition, flags, tmpl); }
        }
    }
    return o_create(path, access, share, sa, disposition, flags, tmpl);
}
BOOL WINAPI Delete(LPCWSTR path) {
    if (g_on && !t_inside)
        if (auto box = Map(path); !box.empty()) { ++g_deletes; ++t_inside; if (Exists(box)) o_delete(box.c_str()); --t_inside; return TRUE; }  // 原文件不删
    return o_delete(path);
}
// 改名：两端都改到沙盒；源只在原目录里时复制一份到目标（原文件不动）
BOOL MoveCommon(LPCWSTR from, LPCWSTR to, const std::function<BOOL(LPCWSTR, LPCWSTR)>& orig) {
    auto a = Map(from), b = Map(to);
    if (a.empty() && b.empty()) return orig(from, to);
    ++g_moves; ++t_inside;
    BOOL ok;
    if (b.empty()) ok = Exists(a) ? orig(a.c_str(), to) : CopyFileW(from, to, FALSE);  // 从存档目录挪出去：只复制
    else {
        EnsureDir(b);
        if (!a.empty() && Exists(a)) ok = orig(a.c_str(), b.c_str());
        else ok = CopyFileW(from, b.c_str(), FALSE);
    }
    --t_inside;
    return ok;
}
BOOL WINAPI Move(LPCWSTR from, LPCWSTR to, DWORD f) {
    if (!g_on || t_inside) return o_move(from, to, f);
    return MoveCommon(from, to, [f](LPCWSTR x, LPCWSTR y) { return o_move(x, y, f | MOVEFILE_REPLACE_EXISTING); });
}
BOOL WINAPI MoveP(LPCWSTR from, LPCWSTR to, LPPROGRESS_ROUTINE pr, LPVOID d, DWORD f) {
    if (!g_on || t_inside) return o_moveP(from, to, pr, d, f);
    return MoveCommon(from, to, [=](LPCWSTR x, LPCWSTR y) { return o_moveP(x, y, pr, d, f | MOVEFILE_REPLACE_EXISTING); });
}
BOOL WINAPI Replace(LPCWSTR target, LPCWSTR src, LPCWSTR backup, DWORD f, LPVOID e1, LPVOID e2) {
    if (!g_on || t_inside) return o_replace(target, src, backup, f, e1, e2);
    auto t = Map(target), s = Map(src);
    if (t.empty() && s.empty()) return o_replace(target, src, backup, f, e1, e2);
    ++g_moves; ++t_inside;
    if (!t.empty()) EnsureDir(t);
    const std::wstring sp = !s.empty() && Exists(s) ? s : std::wstring(src);
    const BOOL ok = CopyFileW(sp.c_str(), (t.empty() ? std::wstring(target) : t).c_str(), FALSE);  // 不做备份文件，原档不动
    --t_inside;
    return ok;
}

HANDLE WINAPI Create2(LPCWSTR path, DWORD access, DWORD share, DWORD disposition, LPCREATEFILE2_EXTENDED_PARAMETERS ex) {
    if (g_on && !t_inside) {
        if (auto box = Map(path); !box.empty()) {
            ++t_inside;
            const bool w = Writes(access, disposition);
            if (w) {
                EnsureDir(box);
                if (!Exists(box) && disposition != CREATE_ALWAYS && disposition != CREATE_NEW) {
                    wchar_t full[1024];
                    if (GetFullPathNameW(path, 1024, full, nullptr)) CopyFileW(full, box.c_str(), TRUE);
                }
                ++g_writes;
            }
            const bool use = w || Exists(box);
            if (!w && use) ++g_reads;
            --t_inside;
            if (use) return o_create2(box.c_str(), access, share, disposition, ex);
        }
    }
    return o_create2(path, access, share, disposition, ex);
}
// 复制：目标在存档目录 → 写进沙盒；源在存档目录且沙盒里有 → 用沙盒的
bool CopyPaths(LPCWSTR from, LPCWSTR to, std::wstring* f, std::wstring* t) {
    auto a = Map(from), b = Map(to);
    if (a.empty() && b.empty()) return false;
    *f = !a.empty() && Exists(a) ? a : std::wstring(from);
    *t = b.empty() ? std::wstring(to) : b;
    if (!b.empty()) EnsureDir(b);
    ++g_writes;
    return true;
}
BOOL WINAPI Copy(LPCWSTR from, LPCWSTR to, BOOL failIfExists) {
    std::wstring f, t;
    if (!g_on || t_inside || !CopyPaths(from, to, &f, &t)) return o_copy(from, to, failIfExists);
    ++t_inside; const BOOL ok = o_copy(f.c_str(), t.c_str(), failIfExists); --t_inside;
    return ok;
}
BOOL WINAPI CopyEx(LPCWSTR from, LPCWSTR to, LPPROGRESS_ROUTINE pr, LPVOID d, LPBOOL cancel, DWORD flags) {
    std::wstring f, t;
    if (!g_on || t_inside || !CopyPaths(from, to, &f, &t)) return o_copyEx(from, to, pr, d, cancel, flags);
    ++t_inside; const BOOL ok = o_copyEx(f.c_str(), t.c_str(), pr, d, cancel, flags); --t_inside;
    return ok;
}
HRESULT WINAPI Copy2(PCWSTR from, PCWSTR to, COPYFILE2_EXTENDED_PARAMETERS* ex) {
    std::wstring f, t;
    if (!g_on || t_inside || !CopyPaths(from, to, &f, &t)) return o_copy2(from, to, ex);
    ++t_inside; const HRESULT hr = o_copy2(f.c_str(), t.c_str(), ex); --t_inside;
    return hr;
}
// 按句柄改名（FileRenameInfo / FileRenameInfoEx）：新名字在存档目录 → 改成沙盒里的同名
BOOL WINAPI SetInfo(HANDLE h, FILE_INFO_BY_HANDLE_CLASS cls, LPVOID info, DWORD size) {
    if (!g_on || t_inside || (cls != FileRenameInfo && cls != FileRenameInfoEx) || !info || size < sizeof(FILE_RENAME_INFO))
        return o_setInfo(h, cls, info, size);
    auto* ri = static_cast<FILE_RENAME_INFO*>(info);
    const std::wstring name(ri->FileName, ri->FileNameLength / sizeof(wchar_t));
    const auto box = Map(name.c_str());
    if (box.empty()) return o_setInfo(h, cls, info, size);
    ++g_moves; ++t_inside;
    EnsureDir(box);
    std::vector<uint8_t> buf(sizeof(FILE_RENAME_INFO) + box.size() * sizeof(wchar_t), 0);
    auto* nr = reinterpret_cast<FILE_RENAME_INFO*>(buf.data());
    std::memcpy(nr, ri, offsetof(FILE_RENAME_INFO, FileName));
    nr->RootDirectory = nullptr;
    nr->FileNameLength = static_cast<DWORD>(box.size() * sizeof(wchar_t));
    std::memcpy(nr->FileName, box.c_str(), box.size() * sizeof(wchar_t));
    const BOOL ok = o_setInfo(h, cls, nr, static_cast<DWORD>(buf.size()));
    --t_inside;
    return ok;
}

bool FindRoot(std::string* why) {
    if (!g_root.empty()) return true;  // 已找到（测试夹具会预先设好）
    namespace fs = std::filesystem;
    std::error_code ec;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    for (fs::path p = fs::path(exe).parent_path(); !p.empty() && p != p.root_path(); p = p.parent_path())
        if (fs::is_directory(p / L"ProjectPBZ" / L"Saved" / L"SaveGames", ec)) { g_root = (p / L"ProjectPBZ" / L"Saved" / L"SaveGames").wstring() + L"\\"; break; }
    if (g_root.empty()) { *why = "找不到游戏存档目录"; return false; }
    g_rootLower = g_root;
    for (auto& c : g_rootLower) c = static_cast<wchar_t>(std::towlower(c));
    wchar_t base[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) { *why = "读不到 LOCALAPPDATA"; return false; }
    g_box = std::wstring(base) + L"\\PBZTower\\sandbox\\SaveGames\\";
    fs::create_directories(g_box, ec);
    return true;
}

bool Hook(std::string* why) {
    if (g_hooked) return true;
    if (!FindRoot(why)) return false;
    HMODULE kb = GetModuleHandleW(L"KernelBase.dll");
    if (!kb) { *why = "找不到 KernelBase"; return false; }
    struct H { const char* name; const char* fn; void* detour; void** orig; bool required; };
    const H hs[] = {{"tower.sb.create", "CreateFileW", reinterpret_cast<void*>(&Create), reinterpret_cast<void**>(&o_create), true},
                    {"tower.sb.delete", "DeleteFileW", reinterpret_cast<void*>(&Delete), reinterpret_cast<void**>(&o_delete), true},
                    {"tower.sb.move", "MoveFileExW", reinterpret_cast<void*>(&Move), reinterpret_cast<void**>(&o_move), true},
                    {"tower.sb.movep", "MoveFileWithProgressW", reinterpret_cast<void*>(&MoveP), reinterpret_cast<void**>(&o_moveP), true},
                    {"tower.sb.replace", "ReplaceFileW", reinterpret_cast<void*>(&Replace), reinterpret_cast<void**>(&o_replace), true},
                    {"tower.sb.create2", "CreateFile2", reinterpret_cast<void*>(&Create2), reinterpret_cast<void**>(&o_create2), true},
                    {"tower.sb.copy", "CopyFileW", reinterpret_cast<void*>(&Copy), reinterpret_cast<void**>(&o_copy), true},
                    {"tower.sb.copyex", "CopyFileExW", reinterpret_cast<void*>(&CopyEx), reinterpret_cast<void**>(&o_copyEx), true},
                    {"tower.sb.copy2", "CopyFile2", reinterpret_cast<void*>(&Copy2), reinterpret_cast<void**>(&o_copy2), false},
                    {"tower.sb.setinfo", "SetFileInformationByHandle", reinterpret_cast<void*>(&SetInfo), reinterpret_cast<void**>(&o_setInfo), true}};
    for (auto& h : hs) {
        void* target = reinterpret_cast<void*>(GetProcAddress(kb, h.fn));
        if (!target && !h.required) continue;
        // 任何一条必需的钩不上就整体不开：漏一条写入通道就等于没隔离
        if (!target || !pbz::hooks::Create(h.name, target, h.detour, h.orig)) { *why = std::string("钩子失败：") + h.fn; return false; }
    }
    for (auto& h : hs) if (*h.orig) pbz::hooks::Enable(h.name, true);  // 钩子常驻，开关只看 g_on（关着时原样放行）
    g_hooked = true;
    return true;
}

}  // namespace

namespace towersb {

// 内存里的游戏进度不是进塔前的真进度（塔内改过、基准还没放回）：这时关沙盒，下一次存档就把塔内状态写进真档
std::atomic<bool> g_dirty{false};
std::atomic<int> g_session{0};       // 每次从关到开 +1：新一轮沙盒要重新取基准、用空影子目录
std::atomic<bool> g_tainted{false};  // 上次是强制关的、当时内存还是塔内状态：此后取的"基准"不是真进度
void SetDirty(bool dirty) { g_dirty = dirty; }
bool Dirty() { return g_dirty; }
int Session() { return g_session; }
bool Tainted() { return g_tainted; }
void Untaint() { g_tainted = false; }  // 回标题重进 / 重启游戏后，内存重新来自磁盘真档

// 新一轮沙盒：上一轮的影子档整体挪到 sandbox-archive\<时间>（留证据、最多 5 份），沙盒从空目录开始。
// 否则第二轮会读到上一轮塔内写下的影子档（2026-10-31 Codex 静态核对指出的生命周期缺口）
void ArchiveBox() {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (g_box.empty()) return;
    const fs::path box = fs::path(g_box).parent_path();  // ...\sandbox\SaveGames
    bool any = false;
    for (auto& e : fs::recursive_directory_iterator(box, ec)) if (e.is_regular_file()) { any = true; break; }
    if (!any) return;
    const fs::path root = box.parent_path().parent_path() / L"sandbox-archive";
    SYSTEMTIME t; GetLocalTime(&t);
    const fs::path dst = root / std::format(L"{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    fs::create_directories(dst, ec);
    ++t_inside;
    fs::rename(box, dst / L"SaveGames", ec);
    if (ec) { fs::copy(box, dst / L"SaveGames", fs::copy_options::recursive, ec); fs::remove_all(box, ec); }
    fs::create_directories(box, ec);
    --t_inside;
    std::vector<fs::path> all;
    for (auto& e : fs::directory_iterator(root, ec)) if (e.is_directory()) all.push_back(e.path());
    std::sort(all.begin(), all.end());
    for (size_t i = 0; i + 5 < all.size(); ++i) fs::remove_all(all[i], ec);
    pbz::log::Info("影塔存档沙盒：上一轮影子档已归档到 {}", dst.string());
}

// force：用户明确放弃保护（例如确认要保留塔内状态）；否则脏的时候拒绝关
bool Set(bool on, std::string* why, bool force) {
    if (on && !Hook(why)) return false;
    if (!on && g_on && g_dirty && !force) {
        *why = "游戏内存里还是塔内状态（进塔基准没放回）：先出塔还原，再关沙盒";
        return false;
    }
    if (on && !g_on) {
        ArchiveBox();
        ++g_session;
        g_dirty = g_tainted.load();  // 内存若仍是塔内状态，新一轮一开始就算脏
    }
    if (!on && g_on && g_dirty && force) g_tainted = true;
    g_on = on;
    pbz::log::Info("影塔存档沙盒：{}（原存档 {}）{}", on ? "开" : "关", std::filesystem::path(g_root).string(), !on && force && g_dirty ? "，强制关闭，内存仍是塔内状态" : "");
    return true;
}
bool Active() { return g_on; }
// 改道进沙盒的写 / 删 / 改名累计次数（出塔收尾判断原生存档是否已静下来）
uint64_t Activity() { return static_cast<uint64_t>(g_writes.load()) + g_deletes.load() + g_moves.load(); }
// 读写真存档目录本身、不经沙盒改道（备份真档用）；只影响调用线程
void BeginReal() { ++t_inside; }
void EndReal() { --t_inside; }
std::string State() {
    return std::format(R"("sandbox":{},"dirty":{},"session":{},"tainted":{},"writes":{},"deletes":{},"moves":{},"reads":{})", g_on.load(), g_dirty.load(),
                       g_session.load(), g_tainted.load(), g_writes.load(), g_deletes.load(), g_moves.load(), g_reads.load());
}
// 清空沙盒（关着时才清）
void Clear() {
    if (g_on || g_box.empty()) return;
    std::error_code ec;
    std::filesystem::remove_all(g_box, ec);
    std::filesystem::create_directories(g_box, ec);
}
// 测试夹具：指定原存档目录与沙盒目录（都带结尾 \）
void SetRootsForTest(const std::wstring& root, const std::wstring& box) {
    g_root = root; g_box = box; g_rootLower = root;
    for (auto& c : g_rootLower) c = static_cast<wchar_t>(std::towlower(c));
}

}  // namespace towersb
