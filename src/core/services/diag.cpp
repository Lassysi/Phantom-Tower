#include "diag.h"

#include <windows.h>
#include <bcrypt.h>
#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../host/log.h"
#include "ue.h"

namespace pbz::diag {
namespace {

constexpr uint32_t kMaxReports = 12;   // 每进程最多写这么多份报告（JSON）
constexpr int kMaxGuardDumps = 3;      // 被吞异常的 minidump 上限
constexpr int kMaxFatalDumps = 2;      // 可能致命（首次机会）异常的 minidump 上限
constexpr int kEv = 64;
constexpr int kFrames = 64;
constexpr size_t kState = 8192;

thread_local const char* t_where = nullptr;
thread_local int t_guard = 0;
thread_local int t_inFilter = 0;

struct Ev { DWORD ms; DWORD tid; const char* tag; const void* obj; const void* func; };
Ev g_ev[kEv];
std::atomic<uint32_t> g_evNext{0};
std::atomic<DWORD> g_gameTid{0};

SRWLOCK g_stateLock = SRWLOCK_INIT;
std::map<std::string, std::string> g_sections;  // 受 g_stateLock 保护
char g_stateText[kState];
size_t g_stateLen = 0;

enum class Src { Guard, FirstChance };

// 过滤器只往这块静态内存里抄；写入线程负责格式化与写盘
struct Slot {
    CONTEXT ctx;
    EXCEPTION_RECORD rec;
    EXCEPTION_POINTERS ptrs;
    Src src;
    DWORD tid, nowMs;
    SYSTEMTIME at;
    uint32_t seq;
    bool dump;
    char where[128];
    char what[128];
    uint64_t pc[kFrames];
    uint64_t base[kFrames];
    int frames;
    Ev ev[kEv];
    char evObj[kEv][128];
    char evFunc[kEv][96];
    int events;
    char state[kState];
    size_t stateLen;
};
Slot g_slot;
volatile LONG g_busy = 0;        // 1：槽位正被过滤器或写入线程使用
std::atomic<uint32_t> g_seq{0};
std::atomic<int> g_guardDumps{0}, g_fatalDumps{0}, g_dropped{0};
HANDLE g_req = nullptr, g_done = nullptr, g_writer = nullptr;
PVOID g_veh = nullptr;
std::wstring g_dir;
std::mutex g_listLock;
std::vector<std::string> g_list;  // 已写报告摘要（Summary 用）

using DumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
DumpFn g_dump = nullptr;

void CopyStr(char* dst, size_t n, const char* src) {
    size_t i = 0;
    if (src) for (; i + 1 < n && src[i]; ++i) dst[i] = src[i];
    dst[i] = 0;
}

// 受保护地读出 "类名/对象名"（不分配、不加锁）
void Label(const void* o, char* out, size_t n) {
    out[0] = 0;
    if (!o) return;
    auto* obj = static_cast<const uint8_t*>(o);
    uint32_t nameIdx = 0, cls = 0;
    const uint8_t* clsObj = nullptr;
    char a[64], b[64];
    a[0] = b[0] = 0;
    if (ue::Read(obj + ue::Layout::ObjClass, &clsObj, sizeof clsObj) && clsObj && ue::Read(clsObj + ue::Layout::ObjName, &cls, 4)) ue::NameRaw(cls, a, sizeof a);
    if (ue::Read(obj + ue::Layout::ObjName, &nameIdx, 4)) ue::NameRaw(nameIdx, b, sizeof b);
    _snprintf_s(out, n, _TRUNCATE, "%s/%s", a, b);
}

int WalkUnsafe(CONTEXT c, uint64_t* pc, uint64_t* base) {
    int n = 0;
    while (n < kFrames && c.Rip) {
        pc[n] = c.Rip;
        PVOID img = nullptr;
        base[n] = reinterpret_cast<uint64_t>(RtlPcToFileHeader(reinterpret_cast<PVOID>(c.Rip), &img) ? img : nullptr);
        ++n;
        DWORD64 imageBase = 0;
        auto* fe = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
        if (!fe) {  // 叶函数：返回地址就在栈顶
            if (!ue::Read(reinterpret_cast<void*>(c.Rsp), &c.Rip, 8)) break;
            c.Rsp += 8;
        } else {
            PVOID handlerData = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fe, &c, &handlerData, &frame, nullptr);
        }
    }
    return n;
}

int Walk(const CONTEXT& c, uint64_t* pc, uint64_t* base) {
    __try { return WalkUnsafe(c, pc, base); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool FatalClass(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_IN_PAGE_ERROR: case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION: case EXCEPTION_STACK_OVERFLOW: case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: case EXCEPTION_DATATYPE_MISALIGNMENT: case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        case 0xC0000374: /* 堆损坏 */ case 0xC0000409: /* 栈缓冲区溢出 */
            return true;
        default: return false;
    }
}

LONG Capture(EXCEPTION_POINTERS* ep, Src src, const char* what, LONG ret) {
    if (t_inFilter || !ep || !ep->ExceptionRecord || !ep->ContextRecord) return ret;
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) { ++g_dropped; return ret; }  // 另一份报告还在写
    ++t_inFilter;
    const uint32_t seq = ++g_seq;
    bool handedOff = false;
    if (seq <= kMaxReports) {
        Slot& s = g_slot;
        s.ctx = *ep->ContextRecord;
        s.rec = *ep->ExceptionRecord;
        s.rec.ExceptionRecord = nullptr;
        s.ptrs = {&s.rec, &s.ctx};
        s.src = src;
        s.tid = GetCurrentThreadId();
        s.nowMs = GetTickCount();
        GetLocalTime(&s.at);
        s.seq = seq;
        s.dump = src == Src::Guard ? g_guardDumps++ < kMaxGuardDumps : g_fatalDumps++ < kMaxFatalDumps;
        CopyStr(s.where, sizeof s.where, t_where);
        CopyStr(s.what, sizeof s.what, what);
        s.frames = Walk(s.ctx, s.pc, s.base);
        const uint32_t next = g_evNext.load();
        s.events = 0;
        for (int i = 0; i < kEv; ++i) {
            const Ev e = g_ev[(next + i) % kEv];
            if (!e.tag) continue;
            Ev& d = s.ev[s.events];
            d = e;
            Label(e.obj, s.evObj[s.events], sizeof s.evObj[0]);
            Label(e.func, s.evFunc[s.events], sizeof s.evFunc[0]);
            ++s.events;
        }
        s.stateLen = 0;
        if (TryAcquireSRWLockShared(&g_stateLock)) {
            memcpy(s.state, g_stateText, g_stateLen);
            s.stateLen = g_stateLen;
            ReleaseSRWLockShared(&g_stateLock);
        }
        if (g_writer) {
            handedOff = true;
            ResetEvent(g_done);
            SetEvent(g_req);
            WaitForSingleObject(g_done, 8000);  // 超时也继续：写入线程写完会自己放开槽位
        }
    }
    if (!handedOff) InterlockedExchange(&g_busy, 0);
    --t_inFilter;
    return ret;
}

LONG CALLBACK Veh(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (!FatalClass(code) || t_probe > 0 || t_guard > 0 || t_inFilter) return EXCEPTION_CONTINUE_SEARCH;
    return Capture(ep, Src::FirstChance, "首次机会（未被核心接住，游戏原处理会继续）", EXCEPTION_CONTINUE_SEARCH);
}

// ---- 写入线程（可以分配内存、读文件）----

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), static_cast<int>(s.size()), nullptr, nullptr);
    if (!s.empty()) s.pop_back();
    return s;
}

std::string Esc(std::string_view v) {
    std::string o;
    for (unsigned char c : v) {
        if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) o += std::format("\\u{:04x}", c);
        else o += static_cast<char>(c);
    }
    return o;
}

std::wstring ModulePath(uint64_t base) {
    wchar_t p[MAX_PATH];
    const DWORD n = base ? GetModuleFileNameW(reinterpret_cast<HMODULE>(base), p, MAX_PATH) : 0;
    return n ? std::wstring(p, n) : std::wstring();
}

std::string Leaf(const std::wstring& path) {
    const auto k = path.find_last_of(L"\\/");
    return Narrow(k == std::wstring::npos ? path : path.substr(k + 1));
}

std::string Sha256File(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 && BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<uint8_t> buf(1 << 16);
        DWORD got = 0;
        while (ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got) BCryptHashData(h, buf.data(), got, 0);
        uint8_t dig[32];
        if (BCryptFinishHash(h, dig, sizeof dig, 0) == 0)
            for (auto b : dig) out += std::format("{:02x}", b);
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return out;
}

// 已加载映像的 CodeView 记录（RSDS GUID + age + PDB 名）：和 PDB 对得上才能符号化
std::string PdbId(uint64_t base) {
    auto* b = reinterpret_cast<uint8_t*>(base);
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    if (!ue::Read(b, &dos, sizeof dos) || !ue::Read(b + dos.e_lfanew, &nt, sizeof nt)) return {};
    const auto& dd = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    for (DWORD i = 0; i < dd.Size / sizeof(IMAGE_DEBUG_DIRECTORY); ++i) {
        IMAGE_DEBUG_DIRECTORY d;
        if (!ue::Read(b + dd.VirtualAddress + i * sizeof d, &d, sizeof d) || d.Type != IMAGE_DEBUG_TYPE_CODEVIEW) continue;
        struct { DWORD sig; GUID guid; DWORD age; char name[260]; } cv{};
        if (!ue::Read(b + d.AddressOfRawData, &cv, std::min<size_t>(sizeof cv, d.SizeOfData)) || cv.sig != 'SDSR') continue;
        cv.name[259] = 0;
        const auto& g = cv.guid;
        return std::format("\"pdb_guid\":\"{:08X}{:04X}{:04X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}\",\"pdb_age\":{},\"pdb\":\"{}\"",
                           g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7], cv.age, Esc(cv.name));
    }
    return {};
}

const char* CodeName(DWORD c) {
    switch (c) {
        case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
        case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
        case 0xC0000374: return "HEAP_CORRUPTION";
        case 0xC0000409: return "STACK_BUFFER_OVERRUN";
        case 0xE06D7363: return "CPP_EXCEPTION";
        default: return "";
    }
}

void WriteFileUtf8(const std::wstring& path, const std::string& s) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, s.data(), static_cast<DWORD>(s.size()), &w, nullptr);
    CloseHandle(f);
}

void WriteReport() {
    const Slot& s = g_slot;
    const auto stem = std::format(L"{}\\{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}-{}", g_dir, s.at.wYear, s.at.wMonth, s.at.wDay,
                                  s.at.wHour, s.at.wMinute, s.at.wSecond, s.at.wMilliseconds, s.seq);
    std::string dumpNote = "\"skipped\"";
    if (s.dump && g_dump) {
        HANDLE f = CreateFileW((stem + L".dmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei{s.tid, const_cast<EXCEPTION_POINTERS*>(&s.ptrs), FALSE};
            const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                                                         MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData | MiniDumpScanMemory);
            const BOOL ok = g_dump(GetCurrentProcess(), GetCurrentProcessId(), f, type, &mei, nullptr, nullptr);
            CloseHandle(f);
            dumpNote = ok ? "\"" + Esc(Narrow(stem + L".dmp")) + "\"" : std::format("\"failed {}\"", GetLastError());
        }
    }
    const auto& r = s.rec;
    const uint64_t addr = reinterpret_cast<uint64_t>(r.ExceptionAddress);
    std::string j = "{\"schema\":1";
    j += std::format(",\"seq\":{},\"time\":\"{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}\",\"pid\":{},\"tid\":{},\"game_thread\":{}",
                     s.seq, s.at.wYear, s.at.wMonth, s.at.wDay, s.at.wHour, s.at.wMinute, s.at.wSecond, s.at.wMilliseconds,
                     GetCurrentProcessId(), s.tid, s.tid == g_gameTid.load() ? "true" : "false");
    j += std::format(",\"source\":\"{}\",\"handled\":{},\"what\":\"{}\",\"where\":\"{}\"", s.src == Src::Guard ? "guard" : "first_chance",
                     s.src == Src::Guard ? "true" : "false", Esc(s.what), Esc(s.where));
    j += std::format(",\"code\":\"0x{:08X}\",\"code_name\":\"{}\",\"flags\":{},\"address\":\"0x{:X}\"", r.ExceptionCode, CodeName(r.ExceptionCode), r.ExceptionFlags, addr);
    if (r.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r.NumberParameters >= 2)
        j += std::format(",\"access\":\"{}\",\"access_address\":\"0x{:X}\"", r.ExceptionInformation[0] == 0 ? "read" : r.ExceptionInformation[0] == 1 ? "write" : "execute", r.ExceptionInformation[1]);
    const auto& c = s.ctx;
    j += std::format(",\"context\":{{\"rip\":\"0x{:X}\",\"rsp\":\"0x{:X}\",\"rbp\":\"0x{:X}\",\"rax\":\"0x{:X}\",\"rbx\":\"0x{:X}\",\"rcx\":\"0x{:X}\",\"rdx\":\"0x{:X}\",\"rsi\":\"0x{:X}\",\"rdi\":\"0x{:X}\",\"r8\":\"0x{:X}\",\"r9\":\"0x{:X}\",\"r10\":\"0x{:X}\",\"r11\":\"0x{:X}\",\"r12\":\"0x{:X}\",\"r13\":\"0x{:X}\",\"r14\":\"0x{:X}\",\"r15\":\"0x{:X}\"}}",
                     c.Rip, c.Rsp, c.Rbp, c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.R8, c.R9, c.R10, c.R11, c.R12, c.R13, c.R14, c.R15);
    j += ",\"stack\":[";
    std::map<uint64_t, std::wstring> mods;
    for (int i = 0; i < s.frames; ++i) {
        const uint64_t b = s.base[i];
        auto it = mods.find(b);
        if (it == mods.end()) it = mods.emplace(b, ModulePath(b)).first;
        j += std::format("{}{{\"pc\":\"0x{:X}\",\"module\":\"{}\",\"offset\":\"0x{:X}\"}}", i ? "," : "", s.pc[i], Esc(Leaf(it->second)), b ? s.pc[i] - b : 0);
    }
    j += "],\"modules\":[";
    bool first = true;
    for (auto& [b, path] : mods) {
        if (!b) continue;
        const auto leaf = Leaf(path);
        const bool ours = leaf.find("PBZ") != std::string::npos;  // 本项目的 DLL 额外给哈希与 PDB 身份
        j += std::format("{}{{\"base\":\"0x{:X}\",\"path\":\"{}\"", first ? "" : ",", b, Esc(Narrow(path)));
        if (ours) {
            j += std::format(",\"sha256\":\"{}\"", Sha256File(path));
            if (auto id = PdbId(b); !id.empty()) j += "," + id;
        }
        j += "}";
        first = false;
    }
    j += "],\"events\":[";
    for (int i = 0; i < s.events; ++i) {
        const auto& e = s.ev[i];
        j += std::format("{}{{\"ago_ms\":{},\"tid\":{},\"tag\":\"{}\",\"obj\":\"{}\",\"func\":\"{}\"}}", i ? "," : "",
                         static_cast<long>(s.nowMs - e.ms), e.tid, Esc(e.tag ? e.tag : ""), Esc(s.evObj[i]), Esc(s.evFunc[i]));
    }
    j += std::format("],\"state\":\"{}\",\"minidump\":{}", Esc(std::string_view(s.state, s.stateLen)), dumpNote);
    j += std::format(",\"dropped_while_busy\":{}}}", g_dropped.load());
    WriteFileUtf8(stem + L".json", j);

    // 栈顶第一帧落在哪个模块
    std::string top = "?";
    if (s.frames > 0) top = std::format("{}+0x{:X}", Leaf(mods[s.base[0]]), s.base[0] ? s.pc[0] - s.base[0] : 0);
    const auto line = std::format("{{\"seq\":{},\"source\":\"{}\",\"code\":\"0x{:08X}\",\"top\":\"{}\",\"where\":\"{}\",\"what\":\"{}\",\"file\":\"{}\"}}",
                                  s.seq, s.src == Src::Guard ? "guard" : "first_chance", r.ExceptionCode, Esc(top), Esc(s.where), Esc(s.what), Esc(Narrow(stem + L".json")));
    {
        std::lock_guard lk(g_listLock);
        g_list.push_back(line);
    }
    log::Info("异常记录 #{} 0x{:08X} 于 {}（{}；正在做：{}）→ {}", s.seq, r.ExceptionCode, top, s.src == Src::Guard ? "已接住" : "首次机会，交还游戏",
              s.where[0] ? s.where : "-", Narrow(stem + L".json"));
}

DWORD WINAPI Writer(LPVOID) {
    for (;;) {
        if (WaitForSingleObject(g_req, INFINITE) != WAIT_OBJECT_0) return 0;
        try { WriteReport(); } catch (...) {}
        SetEvent(g_done);
        InterlockedExchange(&g_busy, 0);
    }
}

}  // namespace

const char* SwapWhere(const char* what) { const char* p = t_where; t_where = what; return p; }
Scope::Scope(const char* what) : prev(t_where) { t_where = what; }
Scope::~Scope() { t_where = prev; }

void MarkGameThread() { g_gameTid = GetCurrentThreadId(); }

void Event(const char* tag, const void* obj, const void* func) {
    const uint32_t i = g_evNext++ % kEv;
    g_ev[i] = {GetTickCount(), GetCurrentThreadId(), tag, obj, func};
}

void Section(const char* key, const std::string& text) {
    AcquireSRWLockExclusive(&g_stateLock);
    g_sections[key] = text;
    std::string all;
    for (auto& [k, v] : g_sections) all += k + ": " + v + "\n";
    g_stateLen = std::min(all.size(), kState);
    memcpy(g_stateText, all.data(), g_stateLen);
    ReleaseSRWLockExclusive(&g_stateLock);
}

namespace {
bool GuardImpl(const char* what, void (*fn)(void*), void* ctx) {
    const char* prev = t_where;
    t_where = what;
    ++t_guard;
    bool ok = true;
    __try {
        fn(ctx);
    } __except (Capture(GetExceptionInformation(), Src::Guard, what, EXCEPTION_EXECUTE_HANDLER)) {
        ok = false;
    }
    --t_guard;
    t_where = prev;
    return ok;
}
}  // namespace

bool Guard(const char* what, void (*fn)(void*), void* ctx) { return GuardImpl(what, fn, ctx); }

void Install() {
    if (g_veh) return;
    wchar_t dir[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) {
        g_dir = std::wstring(dir) + L"\\PBZLab\\crash";
        CreateDirectoryW((std::wstring(dir) + L"\\PBZLab").c_str(), nullptr);
        CreateDirectoryW(g_dir.c_str(), nullptr);
    }
    wchar_t sys[MAX_PATH];
    if (GetSystemDirectoryW(sys, MAX_PATH))  // 用系统自带的 dbghelp，不用游戏目录里可能存在的旧版本
        if (HMODULE h = LoadLibraryW((std::wstring(sys) + L"\\dbghelp.dll").c_str()))
            g_dump = reinterpret_cast<DumpFn>(GetProcAddress(h, "MiniDumpWriteDump"));
    if (!g_req) g_req = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_done) g_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_writer && !g_dir.empty()) g_writer = CreateThread(nullptr, 0, Writer, nullptr, 0, nullptr);
    g_veh = AddVectoredExceptionHandler(0, Veh);  // 排在其他向量处理之后；只记录，不改变处理结果
    log::Info("异常诊断已就绪：{}（minidump {}）", Narrow(g_dir), g_dump ? "可用" : "不可用");
}

void Uninstall() {
    if (g_veh) RemoveVectoredExceptionHandler(g_veh);
    g_veh = nullptr;  // 写入线程保留：DLL 不释放，下一次 Install 复用
}

std::string Summary() {
    std::lock_guard lk(g_listLock);
    std::string j = std::format("{{\"ok\":true,\"op\":\"diag\",\"dir\":\"{}\",\"total\":{},\"dropped\":{},\"reports\":[", Esc(Narrow(g_dir)), g_seq.load(), g_dropped.load());
    for (size_t i = 0; i < g_list.size(); ++i) j += (i ? "," : "") + g_list[i];
    return j + "]}";
}

}  // namespace pbz::diag
