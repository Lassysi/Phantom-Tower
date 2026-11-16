// L0：PBZCore.dll 入口。注入后起工作线程：等引擎就绪 → 反射初始化 → 解析功能 → 装游戏线程 → 开管道。
// 停用（管道 op=unload 或备用事件）：游戏线程里关闭全部功能、拆钩子 → 还原虚表 → 关管道；DLL 永不释放。
#include <windows.h>

#include <atomic>
#include <format>
#include <cstdlib>
#include <string>
#include <unordered_set>
#include <mutex>
#include <memory>

#include "../features/feature.h"
#include "../features/world/enemy_spawn.h"
#include "../ipc/pipe.h"
#include "../services/attributes.h"
#include "../services/diag.h"
#include "../services/native_exec.h"
#include "../services/gamethread.h"
#include "../services/hooks.h"
#include "../services/process_event.h"
#include "../services/ue.h"
#include "compat.h"
#include "log.h"
#include "modules.h"

namespace pbz { void RefreshUnavailableFeatures(); }

namespace {

HMODULE g_self = nullptr;
HANDLE g_instance = nullptr;  // 同一游戏进程只允许一个核心（命名互斥体）
std::atomic<bool> g_unloading{false};
std::mutex g_bootLock;
std::mutex g_statusLock;
std::string g_startupStatus = "核心初始化中";
void StartupStatus(const std::string& value) { std::lock_guard lock(g_statusLock); g_startupStatus = value; }

void Frame(float dt) {
    if (g_unloading) return;
    pbz::RefreshUnavailableFeatures(); // Late-loaded Blueprint classes must recover without a new DLL.
    pbz::pe::PollHotkey();
    // 没有功能开着就什么都不扫，打开修改器本身不增加游戏负担
    bool any = false;
    for (auto& f : pbz::Features())
        if (f->enabled) { any = true; break; }
    if (any) {
        { pbz::diag::Scope s("属性目标扫描"); pbz::attr::Refresh(dt); }
        for (auto& f : pbz::Features())
            if (f->enabled) {
                pbz::diag::Scope s(f->Id());  // 异常报告里写明是哪个功能
                f->Tick(dt);
            }
    }
    { pbz::diag::Scope s("属性写回"); pbz::attr::Commit(); }  // 关掉功能后的那一帧在这里写回原值；无记录时几乎零开销
    // 每秒把实际开关（含持有者、倍率）记进异常现场；只在变化时重写
    static DWORD last = 0;
    if (GetTickCount() - last > 1000) {
        last = GetTickCount();
        std::string on;
        for (auto& f : pbz::Features())
            if (f->enabled) {
                on += f->Id();
                if (f->value != 1.0) on += std::format("={:g}", f->value);
                const auto holders = pbz::FeatureHolders(f.get());
                if (!holders.empty()) {
                    on += "[";
                    for (size_t i = 0; i < holders.size(); ++i) on += (i ? "," : "") + holders[i];
                    on += "]";
                }
                on += " ";
            }
        static std::string prev = "";
        if (on != prev) { prev = on; pbz::diag::Section("features", on.empty() ? "(无)" : on); }
    }
}

// 卸载顺序（曾因此崩溃：游戏线程几乎总在某个 ProcessEvent 里，栈上有本 DLL 的拦截帧，提前释放 DLL 就会返回到已卸载代码）：
// 1. 在游戏线程的 Tick 最外层（此时不在任何 ProcessEvent 内）关功能、写回原值、拆钩子；
// 2. 等其他线程从拦截代码里全部出来；3. 还原游戏线程钩子；4. 才释放 DLL。任何一步超时就不释放（宁可留着也不崩）。
DWORD WINAPI Unload(LPVOID) {
    std::unique_lock bootstrap(g_bootLock);
    if (pbz::gamethread::Ready()) {
        bootstrap.unlock();
        struct Completion {
            HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            bool clean = false;
            ~Completion() { if (event) CloseHandle(event); }
        };
        auto done = std::make_shared<Completion>();
        if (!done->event) {
            pbz::log::Write("核心停用未完成：无法创建收尾事件，保留核心所有权，请重启游戏后再更新");
            return 0;
        }
        pbz::gamethread::Post([done] {
            try {
            // 先让模块收尾（影塔要先清自己的状态），再清理本工具生成的怪物（要用 UE / PE 服务），再关功能、拆钩子
            pbz::modules::StopAll();
            const auto spawned = pbz::enemies::StopOnGameThread();
            pbz::ipc::Send(spawned.json);
            if (!spawned.ok) {
                pbz::log::Info("停用：生成对象未能全部清理，保留旧核心所有权：{}", spawned.json);
                SetEvent(done->event);
                return;
            }
            for (auto& f : pbz::Features())
                if (f->enabled) f->Disable();
            pbz::attr::ReleaseAll();
            pbz::nx::UnhookAll();  // 原生执行入口写回原值
            pbz::pe::Shutdown();
            pbz::hooks::RemoveAll();
                done->clean = true;
            } catch (...) {
                pbz::log::Write("核心停用收尾异常：保留旧核心所有权，防止新旧核心同时运行");
            }
            SetEvent(done->event);
        });
        // Loading/pausing can stop Tick for over 30 seconds. Keep both the
        // singleton and completion handle alive until the queued task finishes;
        // a timeout must never admit a second core or leave a reused HANDLE in
        // the old task. This worker can wait without blocking the game thread.
        bool reported = false;
        for (;;) {
            const auto result = WaitForSingleObject(done->event, 30000);
            if (result == WAIT_OBJECT_0) break;
            if (result != WAIT_TIMEOUT) {
                pbz::log::Write("核心停用等待失败：保留旧核心所有权，请重启游戏后再更新");
                return 0;
            }
            if (!reported) {
                reported = true;
                pbz::log::Write("核心等待游戏线程收尾：加载或暂停结束后继续，期间禁止重复注入");
            }
        }
        if (!done->clean) return 0;
    } else {
        pbz::pe::Shutdown();
        pbz::hooks::RemoveAll();
        bootstrap.unlock();
    }
    // Do not release ownership while an old ProcessEvent callback can still
    // write game state. The DLL staying resident alone does not prevent races.
    for (int i = 0; pbz::pe::InFlight() > 0; ++i) {
        if (i == 250) pbz::log::Write("核心等待在途拦截结束，继续保留所有权");
        Sleep(20);
    }
    pbz::gamethread::Uninstall();
    pbz::diag::Uninstall();
    pbz::ipc::Stop();
    if (g_instance) { CloseHandle(g_instance); g_instance = nullptr; }
    // 永不释放 DLL：两次实机崩溃都是"返回到已卸载的 PBZCore"，等待在途调用也没能完全避免。
    // 停用后代码留在内存里（约 0.7MB，游戏退出时回收），新核心可以照常再加载（单实例互斥体已释放）。
    pbz::log::Write("核心停用（代码保留在内存，不释放）");
    return 0;
}

void NoCrtAbort(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {}  // 本 DLL 自带的 CRT，不影响游戏

DWORD WINAPI BootImpl();

DWORD WINAPI Boot(LPVOID) {
    _set_invalid_parameter_handler(NoCrtAbort);
    try {
        return BootImpl();
    } catch (...) {
        pbz::log::Info("启动异常，核心停用");
        return 0;
    }
}

DWORD WINAPI BootImpl() {
    g_instance = CreateMutexW(nullptr, TRUE, (L"PBZCore.Instance." + std::to_wstring(GetCurrentProcessId())).c_str());
#ifdef PBZ_MOD_ONLY
    // 影塔安装包的运行库标记：PBZv2 修改器看到它就不再注入自己的运行库，改为连接后发 trainer_unlock
    static HANDLE modOnlyMark = CreateMutexW(nullptr, FALSE, (L"PBZCore.ModOnly." + std::to_wstring(GetCurrentProcessId())).c_str());
    (void)modOnlyMark;
#endif
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_instance);
        g_instance = nullptr;
        FreeLibraryAndExitThread(g_self, 0);  // 已有核心在运行：本副本直接退出，不碰任何东西
    }
    pbz::log::Open();
    pbz::log::Info("PBZCore 0.1.0 载入");
    pbz::diag::Install();
    const auto requestUnload = [] {
        if (!g_unloading.exchange(true)) CloseHandle(CreateThread(nullptr, 0, Unload, nullptr, 0, nullptr));
    };
    // 备用卸载通道：管道坏了也能通过命名事件 Local\PBZCore.Unload.<pid> 让核心自行还原并卸载
    CloseHandle(CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        // 自动复位；可能沿用上一个核心副本留下的同名事件，先清掉旧信号
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, (L"Local\\PBZCore.Unload." + std::to_wstring(GetCurrentProcessId())).c_str());
        if (!ev) return 0;
        ResetEvent(ev);
        // 本副本早已经由管道停用时，这个线程还挂在同名事件上：收到信号要转交给正在运行的核心，不能吞掉（2026-10-30 实测吞过一次）
        if (WaitForSingleObject(ev, INFINITE) == WAIT_OBJECT_0) {
            if (!g_unloading.exchange(true)) CloseHandle(CreateThread(nullptr, 0, Unload, nullptr, 0, nullptr));
            else SetEvent(ev);
        }
        CloseHandle(ev);
        return 0;
    }, nullptr, 0, nullptr));
    // 自检模式（开发用）：只起管道，不碰游戏，用来在替身进程里验证通信
    if (GetEnvironmentVariableW(L"PBZCORE_SELFTEST", nullptr, 0)) {
        pbz::ipc::Start(requestUnload);
        pbz::log::Info("自检模式：仅管道");
        return 0;
    }
    // Publish the full immutable registry before engine discovery. Constructors
    // only describe features; all reflection Resolve runs later on the game thread.
    {
    std::lock_guard bootstrap(g_bootLock);
    if (g_unloading) return 0;
    pbz::RegisterAttributeFeatures();
    pbz::RegisterDeflectFeatures();
    pbz::RegisterAbilityFeatures();
    pbz::RegisterHealFeatures();
    pbz::RegisterTrainFeatures();
    pbz::RegisterFactionBarFeatures();
    pbz::RegisterMoveFeatures();
    pbz::RegisterProgressFeatures();
    pbz::RegisterAutoResponseFeatures();
    pbz::RegisterResFeatures();
    pbz::RegisterWorldFeatures();
    pbz::RegisterCamFeatures();
    for (auto& f : pbz::Features()) { f->available = false; f->reason = "进入游戏后可用"; }
    pbz::gamethread::SetFrameCallback(Frame);
    pbz::ipc::Start(requestUnload);
    }
    StartupStatus("正在连接游戏…");
    std::string why;
    while (!g_unloading) {
        if (pbz::ue::Init(&why)) break;
        StartupStatus("等待游戏加载…");
        Sleep(1000);
    }
    if (g_unloading) return 0;
    pbz::log::Info("反射就绪：对象 {}", pbz::ue::Num());
    {
        std::lock_guard bootstrap(g_bootLock);
        if (g_unloading) return 0;
        if (!pbz::hooks::Init(&why)) pbz::log::Info("{}", why);
        else if (!pbz::pe::Init(&why)) pbz::log::Info("ProcessEvent 不可用：{}", why);
    }
#ifdef PBZ_MOD_ONLY
    // 发布版：游戏线程跑起来后载入同目录的 PBZTower.dll（代理 DLL 只负责载入运行库）
    CloseHandle(CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        // 3.0.0 兼容：游戏线程可能因别的 Mod 改了引擎虚表而挂不上，ProcessEvent 可能被 UE4SS 先挂（已支持叠加）。
        // 等到两者都就绪再载入影塔；3 分钟仍不行 = 真冲突：写状态、弹一次说明，不载入影塔（游戏照常）
        const ULONGLONG since = GetTickCount64();
        bool warned = false;
        for (;;) {
            if (g_unloading) return 0;
            const bool thread = pbz::gamethread::Ready() && pbz::gamethread::Frames() > 0;
            if (thread && pbz::pe::Ready()) break;
            // 标题 / 加载期间游戏线程本来就可能很久不跑：只有游戏线程已在跑、ProcessEvent 仍挂不上，才算冲突
            if (!warned && thread && GetTickCount64() - since > 180000) {
                warned = true;
                const auto mods = pbz::compat::ForeignModules();
                pbz::compat::WriteStatus("conflict", "游戏事件入口挂接失败", mods);
                pbz::compat::Warn("游戏事件入口（ProcessEvent）挂接失败，可能被其他 Mod 以不兼容的方式改写", mods);
                pbz::log::Info("兼容：ProcessEvent 3 分钟仍不可用，不载入影塔");
            }
            Sleep(500);
        }
        {
            const auto mods = pbz::compat::ForeignModules();
            const auto foreign = pbz::pe::ForeignHook();
            std::string list;
            for (auto& m : mods) list += (list.empty() ? "" : ", ") + m;
            pbz::compat::WriteStatus("ok", foreign.empty() ? std::string("正常") : "与 " + foreign + " 叠加挂接", mods);
            pbz::log::Info("兼容：同目录第三方模块 [{}]；ProcessEvent {}", list, foreign.empty() ? "独占" : "叠加于 " + foreign);
        }
        wchar_t self[MAX_PATH] = {};
        GetModuleFileNameW(g_self, self, MAX_PATH);
        std::wstring dir(self);
        dir.resize(dir.find_last_of(L"\\/"));
        pbz::modules::AutoLoad(dir);
        return 0;
    }, nullptr, 0, nullptr));
#endif
    std::string previous;
    // 守护整个核心生命周期，不是挂上一次就结束：着色器编译可能远超两分钟；试用期撤回、或别的程序换掉了引擎虚表，
    // 都会让游戏线程失效——发现后重新挂接，玩家不用重启游戏
    while (!g_unloading) {
        pbz::log::Flush();
        if (pbz::gamethread::Healthy()) { Sleep(2000); continue; }
        {
            std::lock_guard bootstrap(g_bootLock);
            if (g_unloading) break;
            if (pbz::gamethread::Install(&why)) { previous.clear(); continue; }
        }
        StartupStatus(why.find("版本差异") != std::string::npos ? "这个游戏版本改动较大，暂不支持，请等修改器更新" : "进入游戏后自动就绪");
        if (why != previous) { previous = why; pbz::log::Info("等待游戏线程：{}", why); }
        Sleep(1000);
    }
    return 0;
}

}  // namespace

namespace pbz {
std::string CoreStartupStatus() {
    if (gamethread::Ready() && gamethread::Frames() > 0) return "就绪";
    std::lock_guard lock(g_statusLock); return g_startupStatus;
}
bool ResolveFeature(IFeature* feature) {
    if (g_unloading) return false;
    if (feature->enabled) return feature->available;
    std::string reason; bool resolved = false;
    try { resolved = feature->Resolve(&reason); }
    catch (...) { reason = "正在加载，稍后自动可用"; }
    if (resolved || reason != feature->reason) log::Info("{} {}{}", feature->Id(), resolved ? "可用" : "等待：", reason);
    feature->available = resolved; feature->reason = resolved ? "" : reason;
    return resolved;
}
// Called only on the game thread (Frame / IPC posted list). Never re-resolve an
// enabled feature or re-apply user toggles while recovering a late class.
void RefreshUnavailableFeatures() {
    if (g_unloading) return;
    static ULONGLONG last = 0;
    static std::unordered_set<IFeature*> defaultApplied;
    const auto now = GetTickCount64();
    if (now - last < 1000) return;
    last = now;
    if (!pe::Ready()) {
        std::string why;
        // Default__Object can also arrive after the first reflection scan.
        // Init is retried only on this game thread, never by the IPC reader.
        if (hooks::Init(&why) && pe::Init(&why)) log::Info("ProcessEvent 延迟初始化完成");
    }
    for (auto& f : Features()) {
        if (f->enabled || f->available) continue;
        const bool resolved = ResolveFeature(f.get());
#ifdef PBZ_MOD_ONLY
        // 发布版：只自动开影塔自己的功能（tower.ui 界面入口、tower.travel）；修改器功能（大地图传送、阵营血条等）一律不开
        if (std::string_view(f->Id()).rfind("tower.", 0) != 0 && !ipc::TrainerUnlocked()) continue;  // 修改器连上解锁后照常默认开启
#endif
        if (resolved && f->DefaultOn() && defaultApplied.insert(f.get()).second) {
            f->Enable(); log::Info("{} 默认开启", f->Id());
        }
    }
}
} // namespace pbz

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = mod;
        DisableThreadLibraryCalls(mod);
        CloseHandle(CreateThread(nullptr, 0, Boot, nullptr, 0, nullptr));
    }
    return TRUE;
}
