#include "gamethread.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <string>
#include <string_view>
#include <mutex>
#include <optional>
#include <vector>

#include "../host/log.h"
#include "diag.h"
#include "scan.h"
#include "ue.h"

namespace pbz::gamethread {
namespace {

using TickFn = void(__fastcall*)(void*, float, bool);

// UGameEngine::Tick 特征码（按版本指纹登记；找不到就禁用游戏线程，功能不可用而不是崩溃）
struct Sig { const char* build; const char* aob; };
constexpr Sig kTickSigs[] = {
    {"26367565", "4C 8B DC 55 53 57 41 54 41 55 41 56 49 8D 6B A1 48 81 EC F8 00 00 00 41 0F 29 7B A8 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 F7 41 0F 29 73 B8"}, // Updated build: read-only GameEngine CDO vtable94 verified.
    {"26266538", "4C 8B DC 55 53 57 41 54 41 55 41 56 49 8D 6B A1 48 81 EC F8 00 00 00 41 0F 29 7B A8 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 F7 41 0F 29 73 B8"},  // 虚表索引 94，实机唯一
    // 游戏更新后的兜底：同一函数开头，只把编译器常改的栈大小 / 局部变量偏移换成通配；命中后仍须在 GameEngine 默认对象的虚表里
    // 找到它才采用（不是 Tick 就找不到，等于没命中），所以宽松特征不会挂错函数
    {"generic", "4C 8B DC 55 53 57 41 54 41 55 41 56 49 8D 6B ?? 48 81 EC ?? ?? ?? ?? 41 0F 29 7B ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4"},
};

void* g_engine = nullptr;
void** g_origVtbl = nullptr;
std::vector<void*>* g_copy = nullptr;  // 每次挂接新建、从不释放：重挂时旧副本可能仍被引用
int g_badIndex = -1;                   // 试用失败的虚表位置，重试时不再用
TickFn g_origTick = nullptr;
std::atomic<bool> g_ready{false};
std::atomic<int> g_inFlight{0};
std::atomic<unsigned long long> g_frames{0};
std::atomic<uint32_t> g_costUs{0}, g_peakUs{0};  // 核心每帧耗时（平滑值 / 峰值），微秒  // 正在执行 HookTick 的次数；卸载前必须归零
std::mutex g_qLock;
struct Item { std::function<void()> fn; const char* tag; };
std::vector<Item> g_queue;
// 特征码都没命中、按引擎版本的固定虚表位置挂上时先"试用"：前 30 次调用的帧时间都正常才放功能运行；
// 连续不正常就自己摘掉钩子（游戏不受影响），日志里说明。特征码命中的直接可用。
constexpr int kUe57TickIndex = 94;  // UE5.7 的 UGameEngine::Tick 虚表位置：只随引擎大版本变，游戏打补丁不变
std::atomic<bool> g_probation{false};
std::atomic<int> g_probeOk{0}, g_probeBad{0};
std::function<void(float)> g_frame;

void Fault() {
    static int n = 0;
    if (n++ < 5) log::Info("游戏线程回调异常，已跳过（详情见异常记录）");
    if (n > 50) g_frame = nullptr;  // 持续出错就停掉功能 Tick
}

void __fastcall HookTick(void* self, float dt, bool idle) {
    ++g_inFlight;
    g_origTick(self, dt, idle);
    if (g_probation) {
        // 只把明显不是帧时间的值算失败：慢电脑加载 / 编译着色器时一帧卡几秒到几十秒是正常的（旧判据 dt<2 会在这类机器上误撤）
        const bool sane = std::isfinite(dt) && dt >= 0 && dt < 3600;
        if (sane && ++g_probeOk >= 30) { g_probation = false; log::Info("游戏线程：按引擎版本位置挂接，已确认可用"); }
        else if (!sane && ++g_probeBad >= 3) {
            g_probation = false;
            g_ready = false;
            g_badIndex = kUe57TickIndex;
            InterlockedExchangePointer(static_cast<PVOID*>(g_engine), g_origVtbl);  // 不像帧循环：原样摘掉
            log::Info("游戏线程：引擎版本位置不对，已撤回（游戏不受影响）");
        }
        --g_inFlight;
        return;  // 试用期间不跑任何功能
    }
    ++g_frames;
    std::vector<Item> q;
    {
        std::lock_guard lk(g_qLock);
        q.swap(g_queue);
    }
    // 核心出错只停这一项、记日志与异常报告（码 / 地址 / 栈 / 当时在做什么 / minidump），不能把游戏带崩
    diag::MarkGameThread();
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    for (auto& f : q) {
        if (!diag::Guarded(f.tag, [&] { f.fn(); })) Fault();
    }
    if (g_frame && g_ready && !diag::Guarded("功能帧", [&] { g_frame(dt); })) Fault();
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    const auto us = static_cast<uint32_t>((t1.QuadPart - t0.QuadPart) * 1000000 / fq.QuadPart);
    g_costUs = (g_costUs * 31 + us) / 32;
    if (us > g_peakUs) g_peakUs = us;
    --g_inFlight;
}

void* FindEngine() {
    auto* cls = ue::FindClass("GameEngine");
    if (!cls) return nullptr;
    void* hit = nullptr;
    ue::ForEach([&](int, ue::UObject* o) {
        if (ue::IsA(o, cls) && !ue::IsDefault(o)) { hit = o; return false; }
        return true;
    });
    return hit;
}

void* FindEngineCdo() {
    auto* cls = g_engine ? ue::ClassOf(static_cast<ue::UObject*>(g_engine)) : nullptr;
    void* hit = nullptr;
    if (cls)
        ue::ForEach([&](int, ue::UObject* o) {
            if (ue::ClassOf(o) == cls && ue::IsDefault(o)) { hit = o; return false; }
            return true;
        });
    return hit;
}

std::vector<int> Pattern(std::string_view aob) {  // "48 8B ?? ..." → 字节（-1 = 通配）
    std::vector<int> pat;
    for (size_t i = 0; i < aob.size();) {
        if (aob[i] == ' ') { ++i; continue; }
        if (aob[i] == '?') { pat.push_back(-1); i += (i + 1 < aob.size() && aob[i + 1] == '?') ? 2 : 1; continue; }
        pat.push_back(std::stoi(std::string(aob.substr(i, 2)), nullptr, 16));
        i += 2;
    }
    return pat;
}

std::optional<int> IndexFromIni() {
    wchar_t dir[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return std::nullopt;
    const auto ini = std::wstring(dir) + L"\\PBZLab\\core.ini";
    const int v = GetPrivateProfileIntW(L"gamethread", L"tick_index", -1, ini.c_str());
    return v >= 0 ? std::optional<int>(v) : std::nullopt;
}

}  // namespace

bool Install(std::string* why) {
    g_engine = FindEngine();
    if (!g_engine) { *why = "GameEngine 实例未找到"; return false; }
    auto** vtbl = *static_cast<void***>(g_engine);

    int index = -1;
    if (auto i = IndexFromIni()) index = *i;
    // 直接拿 GameEngine 默认对象虚表里的每个函数去比特征（实例虚表可能已被别的核心副本换过，默认对象的不会）：
    // 命中的必然是虚表里的函数；先精确特征，再宽松特征（游戏更新后编译器改了栈大小等）
    auto** classVtbl = vtbl;
    if (auto* cdo = FindEngineCdo()) classVtbl = *static_cast<void***>(cdo);
    // 测试兜底用：core.ini [gamethread] skip_signatures=1 跳过特征码，直接走引擎版本位置 + 试用
    wchar_t dir[MAX_PATH];
    const bool skipSigs = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH) &&
        GetPrivateProfileIntW(L"gamethread", L"skip_signatures", 0, (std::wstring(dir) + L"\\PBZLab\\core.ini").c_str()) == 1;
    for (const auto& s : kTickSigs) {
        if (index >= 0 || !s.aob || skipSigs) continue;
        const auto pat = Pattern(s.aob);
        for (int i = 0; i < 512 && index < 0; ++i) {
            auto* fn = static_cast<uint8_t*>(ue::Get<void*>(classVtbl + i));
            if (!scan::InCode(fn)) continue;
            std::vector<uint8_t> code(pat.size());
            if (!ue::Read(fn, code.data(), code.size())) continue;
            if (std::equal(pat.begin(), pat.end(), code.begin(), [](int want, uint8_t got) { return want < 0 || want == got; })) {
                index = i;
                if (std::string_view(s.build) == "generic") log::Info("游戏线程：精确特征未命中（游戏可能已更新），用宽松特征找到 Tick，虚表索引 {}", i);
            }
        }
    }
    // 特征码都没命中（游戏更新 / 其他平台版本）：用引擎版本的固定位置，但要求 GameEngine 自己重写了这个位置（Tick 必然重写），
    // 并进入试用期验证；游戏换了引擎大版本时这里会失败并撤回
    bool probe = false;
    if (index < 0) {
        auto* engineCls = ue::FindClass("Engine");
        ue::UObject* engineCdo = nullptr;
        if (engineCls) ue::ForEach([&](int, ue::UObject* o) {
            if (ue::ClassOf(o) == engineCls && ue::IsDefault(o)) { engineCdo = o; return false; }
            return true;
        });
        void* mine = ue::Get<void*>(classVtbl + kUe57TickIndex);
        void* base = engineCdo ? ue::Get<void*>(*reinterpret_cast<void***>(engineCdo) + kUe57TickIndex) : nullptr;
        if (g_badIndex != kUe57TickIndex && scan::InCode(mine) && mine != base) { index = kUe57TickIndex; probe = true; log::Info("游戏线程：特征码未命中（游戏版本不同），改用引擎版本位置 {} 试用", index); }
    }
    if (index < 0) { *why = "游戏版本差异过大，暂时无法挂接"; return false; }

    // 复制足够长的虚表（到第一个非代码指针为止）
    if (g_copy && vtbl == g_copy->data()) return g_ready = true;  // 仍挂着
    auto* copy = new std::vector<void*>;
    for (int i = 0; i < 1024; ++i) {
        void* p = ue::Get<void*>(vtbl + i);
        if (!scan::InCode(p)) break;
        copy->push_back(p);
    }
    if (index >= static_cast<int>(copy->size())) { delete copy; *why = "虚表长度异常"; return false; }
    g_copy = copy;
    g_origVtbl = vtbl;
    g_origTick = reinterpret_cast<TickFn>((*g_copy)[index]);
    (*g_copy)[index] = reinterpret_cast<void*>(&HookTick);
    g_probeOk = 0; g_probeBad = 0;
    g_probation = probe;
    InterlockedExchangePointer(static_cast<PVOID*>(g_engine), g_copy->data());
    g_ready = true;
    log::Info("游戏线程就绪：GameEngine {} Tick 索引 {}", g_engine, index);
    return true;
}

void Uninstall() {
    if (!g_ready) return;
    InterlockedExchangePointer(static_cast<PVOID*>(g_engine), g_origVtbl);
    g_ready = false;
    // 等正在执行的 HookTick 返回（加载场景时原 Tick 可能很久），再留一点时间给 --g_inFlight 之后的 ret
    for (int i = 0; i < 300 && g_inFlight > 0; ++i) Sleep(20);
    Sleep(50);
    log::Info("游戏线程已还原");
}

bool Ready() { return g_ready; }

bool Healthy() {
    if (!g_ready || !g_engine || !g_copy) return false;
    void* cur = nullptr;
    // 别的程序（其他 Mod / 注入工具）把 GameEngine 虚表指针换掉了，我们的 Tick 不会再被调用
    if (!ue::Read(g_engine, &cur, sizeof cur) || cur != g_copy->data()) { g_ready = false; return false; }
    return true;
}
unsigned long long Frames() { return g_frames; }
uint32_t CostUs() { return g_costUs; }
uint32_t PeakUs() { return g_peakUs.exchange(0); }

void Post(std::function<void()> fn) { Post(std::move(fn), "核心队列"); }

void Post(std::function<void()> fn, const char* tag) {
    std::lock_guard lk(g_qLock);
    g_queue.push_back({std::move(fn), tag});
}

void SetFrameCallback(std::function<void(float)> fn) { g_frame = std::move(fn); }

}  // namespace pbz::gamethread
