#include "process_event.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <format>
#include <algorithm>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <unordered_map>

#include "../host/log.h"
#include "diag.h"
#include "hooks.h"
#include "abshook.h"

namespace pbz::pe {
namespace {

using PEFn = void(__fastcall*)(ue::UObject*, ue::UObject*, void*);
constexpr int kIndex = 76;
constexpr uint8_t kPrologue[] = {0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};

PEFn g_target = nullptr;
PEFn g_orig = nullptr;
std::string g_foreign;  // 先挂了 ProcessEvent 的其他模块（兼容报告用）
std::mutex g_lock;
std::unordered_map<ue::UObject*, Interceptor> g_icpt;
std::unordered_map<ue::UObject*, std::vector<std::pair<int, Observer>>> g_obs;  // 只看不改，可多个并存
int g_obsNext = 1;
// 无锁快速判断用：表本身只在 g_lock 下读写（游戏线程增删观察者时别的线程可能正在 Detour 里）
std::atomic<bool> g_anyObs{false}, g_anyIcpt{false};
std::atomic<bool> g_tracing{false};
std::atomic<int> g_users{0};
std::atomic<int> g_writers{0};  // 后台写标记文件的线程数  // 记录器或拦截器在用时才启用钩子

struct Ev { DWORD ms; ue::UObject* obj; ue::UObject* func; DWORD tid; };
std::deque<Ev> g_ring;                       // 最近的调用（约 5 秒）
std::unordered_map<ue::UObject*, uint64_t> g_counts;
bool g_keyDown = false;
int g_markKey = 0;

// 打标记键：默认鼠标前侧键（VK_XBUTTON2）；可在 %LOCALAPPDATA%\PBZLab\core.ini 的 [trace] mark_key 写虚拟键码（如 5 = 后侧键）
int MarkKey() {
    if (g_markKey) return g_markKey;
    wchar_t d[MAX_PATH];
    GetEnvironmentVariableW(L"LOCALAPPDATA", d, MAX_PATH);
    const auto ini = std::wstring(d) + L"\\PBZLab\\core.ini";
    g_markKey = GetPrivateProfileIntW(L"trace", L"mark_key", VK_XBUTTON2, ini.c_str());
    return g_markKey;
}

std::wstring Dir() {
    wchar_t d[MAX_PATH];
    GetEnvironmentVariableW(L"LOCALAPPDATA", d, MAX_PATH);
    auto p = std::wstring(d) + L"\\PBZLab\\trace";
    CreateDirectoryW(p.c_str(), nullptr);
    return p;
}

std::string Stamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    return std::format("{:02}{:02}{:02}-{:03}", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
}

std::string Narrow(const std::wstring& w) {
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), static_cast<int>(s.size()), nullptr, nullptr);
    if (!s.empty()) s.pop_back();
    return s;
}

std::atomic<int> g_inflight{0};  // 正在执行 Detour 的调用数（含嵌套）；卸载前必须等它归零
struct InFlightGuard {
    InFlightGuard() { ++g_inflight; }
    ~InFlightGuard() { --g_inflight; }
};

void __fastcall Detour(ue::UObject* obj, ue::UObject* func, void* params) {
    InFlightGuard guard;
    if (g_tracing) {
        std::lock_guard lk(g_lock);
        const DWORD now = GetTickCount();
        g_ring.push_back({now, obj, func, GetCurrentThreadId()});
        while (!g_ring.empty() && now - g_ring.front().ms > 5000) g_ring.pop_front();
        ++g_counts[func];
    }
    if (g_anyObs.load(std::memory_order_acquire)) {
        std::vector<std::pair<int, Observer>> list;
        {
            std::lock_guard lk(g_lock);
            if (auto it = g_obs.find(func); it != g_obs.end()) list = it->second;
        }
        // 回调在游戏自己的 ProcessEvent 里执行：没接住的异常会直接带崩游戏。出错只跳过这个回调，并留异常报告
        for (auto& [id, fn] : list) {
            diag::Event("pe.observe", obj, func);
            diag::Guarded("ProcessEvent 观察回调", [&] { fn(obj, params); });
        }
    }
    if (g_anyIcpt.load(std::memory_order_acquire)) {
        Interceptor fn;
        {
            std::lock_guard lk(g_lock);
            if (auto it = g_icpt.find(func); it != g_icpt.end()) fn = it->second;
        }
        if (fn) {
            diag::Event("pe.intercept", obj, func);
            bool handled = false;
            diag::Guarded("ProcessEvent 拦截回调", [&] { handled = fn(obj, params); });
            if (handled) return;  // 拦截回调出错：按未拦截处理，照常执行原函数
        }
    }
    g_orig(obj, func, params);
}

void Use(int delta) {
    const int n = g_users += delta;
    hooks::Enable("ProcessEvent", n > 0);
}

}  // namespace

bool Init(std::string* why) {
    auto* cdo = static_cast<ue::UObject*>(nullptr);
    ue::ForEach([&](int, ue::UObject* o) {
        if (ue::ObjName(o) == "Default__Object") { cdo = o; return false; }
        return true;
    });
    if (!cdo) { *why = "Default__Object 未找到"; return false; }
    auto* fn = ue::Get<uint8_t*>(ue::Get<uint8_t*>(cdo) + 8 * kIndex);
    uint8_t head[sizeof kPrologue];
    if (!ue::Read(fn, head, sizeof head)) { *why = "ProcessEvent 入口不可读"; return false; }
    if (memcmp(head, kPrologue, sizeof head) != 0) {
        // 3.0.0：UE4SS 等 Mod 比我们早挂 ProcessEvent，入口是它的跳转——叠加挂接（MinHook / 远跳槽），不再拒绝启动
        if (!abshook::LooksHooked(fn)) { *why = "ProcessEvent 序言不匹配（版本变化）"; return false; }
        g_foreign = abshook::JumpOwner(fn);
        log::Info("ProcessEvent 入口已被其他 Mod 挂接（{}），叠加挂接", g_foreign);
    }
    g_target = reinterpret_cast<PEFn>(fn);
    if (!hooks::Create("ProcessEvent", fn, reinterpret_cast<void*>(&Detour), reinterpret_cast<void**>(&g_orig))) {
        *why = "ProcessEvent 钩子创建失败";
        return false;
    }
    log::Info("ProcessEvent @ {}（虚表 {}）", static_cast<void*>(fn), kIndex);
    return true;
}

bool Ready() { return g_orig != nullptr; }
std::string ForeignHook() { return g_foreign; }
int InFlight() { return g_inflight; }

ue::UObject* FindFunction(ue::UObject* cls, std::string_view name) {
    // 链表都加上限：类对象失效（随关卡卸载）时链表可能首尾相连，没有上限会让游戏线程死循环
    for (int depth = 0; cls && depth < 64; ++depth, cls = ue::Get<ue::UObject*>(cls + ue::Layout::StructSuper)) {
        int n = 0;
        for (auto* f = ue::Get<ue::UObject*>(cls + ue::Layout::StructChildren); f && n < 8192; f = ue::Get<ue::UObject*>(f + ue::Layout::UFieldNext), ++n)
            if (ue::ObjName(f) == name) return f;
    }
    return nullptr;
}

std::vector<ue::UObject*> FunctionsWithPrefix(ue::UObject* cls, std::string_view prefix) {
    std::vector<ue::UObject*> out;
    for (auto* f = ue::Get<ue::UObject*>(cls + ue::Layout::StructChildren); f && out.size() < 512; f = ue::Get<ue::UObject*>(f + ue::Layout::UFieldNext))
        if (ue::ObjName(f).starts_with(prefix)) out.push_back(f);
    return out;
}

void Call(ue::UObject* obj, ue::UObject* func, void* params) {
    if (auto f = g_orig ? g_orig : g_target) f(obj, func, params);
}

void Intercept(ue::UObject* func, Interceptor fn) {
    {
        std::lock_guard lk(g_lock);
        if (g_icpt.count(func)) { g_icpt[func] = std::move(fn); return; }
        g_icpt.emplace(func, std::move(fn));
        g_anyIcpt = true;
    }
    Use(+1);
}

void RemoveIntercept(ue::UObject* func) {
    bool had;
    {
        std::lock_guard lk(g_lock);
        had = g_icpt.erase(func) > 0;
        g_anyIcpt = !g_icpt.empty();
    }
    if (had) Use(-1);
}

int Observe(ue::UObject* func, Observer fn) {
    int id;
    {
        std::lock_guard lk(g_lock);
        id = g_obsNext++;
        g_obs[func].emplace_back(id, std::move(fn));
        g_anyObs = true;
    }
    Use(+1);
    return id;
}

void Unobserve(int id) {
    bool had = false;
    {
        std::lock_guard lk(g_lock);
        for (auto it = g_obs.begin(); it != g_obs.end(); ++it) {
            auto& v = it->second;
            const auto n = v.size();
            std::erase_if(v, [id](auto& x) { return x.first == id; });
            if (v.size() != n) { had = true; if (v.empty()) g_obs.erase(it); break; }
        }
        g_anyObs = !g_obs.empty();
    }
    if (had) Use(-1);
}

std::string Watch(const std::string& classSub, const std::string& func, int mode) {
    // 收集匹配的类上解析到的 UFunction（蓝图子类没覆盖时会解析到父类的同一个函数，去重）
    std::vector<std::pair<ue::UObject*, std::string>> fns;
    ue::ForEach([&](int, ue::UObject* o) {
        const auto cn = ue::ClassName(o);
        if (!cn.ends_with("Class") || ue::IsDefault(o)) return true;
        const auto name = ue::ObjName(o);
        if (name.find(classSub) == std::string::npos) return true;
        if (auto* f = FindFunction(o, func))
            if (std::none_of(fns.begin(), fns.end(), [&](auto& x) { return x.first == f; })) fns.emplace_back(f, name);
        return true;
    });
    for (auto& [f, owner] : fns) {
        if (mode == 0) { RemoveIntercept(f); continue; }
        // 返回值参数：ChildProperties 里带 CPF_ReturnParm(0x400) 的那个
        int retOff = -1;
        std::string retType;
        for (auto* p = ue::Get<uint8_t*>(f + ue::Layout::StructChildProps); p; p = ue::Get<uint8_t*>(p + ue::Layout::FieldNext))
            if (ue::Get<uint64_t>(p + 0x38) & 0x400) {
                retOff = ue::Get<int32_t>(p + ue::Layout::PropOffset);
                retType = ue::Name(ue::Get<uint32_t>(ue::Get<uint8_t*>(p + ue::Layout::FieldClass) + ue::Layout::FieldClassName));
                break;
            }
        auto label = owner + "." + func;
        auto budget = std::make_shared<int>(300);  // 每个函数最多记 300 次，防刷屏
        // 同一个 UFunction 常被大量子类共用（如 BP_AbilityBase 的 K2_CanActivateAbility 被几乎所有能力继承）：
        // 只对类名含 classSub 的对象生效，其余原样放行 —— 曾因此把敌人技能、复活能力也强制成"可激活"
        Intercept(f, [f, retOff, retType, label, mode, budget, classSub](ue::UObject* obj, void* params) {
            const auto cn = ue::ClassName(obj);
            if (cn.find(classSub) == std::string::npos || cn.find("Passive") != std::string::npos) return false;
            Call(obj, f, params);
            auto* ret = retOff >= 0 && params ? static_cast<uint8_t*>(params) + retOff : nullptr;
            auto show = [&]() -> std::string {  // 按返回值类型显示
                if (!ret) return "-";
                if (retType == "BoolProperty") return std::to_string(*ret);
                if (retType == "DoubleProperty") return std::format("{:.4f}", *reinterpret_cast<double*>(ret));
                if (retType == "FloatProperty") return std::format("{:.4f}", *reinterpret_cast<float*>(ret));
                if (retType == "IntProperty") return std::to_string(*reinterpret_cast<int32_t*>(ret));
                if (retType == "StructProperty" || retType == "NameProperty") return ue::Name(*reinterpret_cast<uint32_t*>(ret));  // GameplayTag/FName
                return retType;
            };
            const auto before = show();
            if (ret && retType == "BoolProperty" && mode == 2) *ret = 1;
            if (ret && retType == "BoolProperty" && mode == 3) *ret = 0;
            if ((*budget)-- > 0)
                log::Info("观察 {} 对象 {} 返回 {}{}", label, ue::ObjName(obj), before, mode >= 2 ? " → " + show() : "");
            return true;  // 原函数已在上面执行过
        });
    }
    auto msg = std::format("观察 {}*.{}：{} 个函数，模式 {}", classSub, func, fns.size(), mode);
    log::Info("{}", msg);
    return msg;
}

void Trace(bool on) {
    if (on == g_tracing) return;
    if (!on) DumpCounts();
    {
        std::lock_guard lk(g_lock);
        if (on) { g_ring.clear(); g_counts.clear(); }
    }
    g_tracing = on;
    Use(on ? +1 : -1);
    log::Info("记录器{}", on ? std::format("开始（按键 0x{:X} 打标记）", MarkKey()) : std::string("停止"));
}

bool Tracing() { return g_tracing; }

void Mark(const std::string& label) {
    // 游戏线程里只拷贝调用记录（约 1ms），写文件和解析名字放到后台线程，按键不卡帧
    std::vector<Ev> ring;
    {
        std::lock_guard lk(g_lock);
        ring.assign(g_ring.begin(), g_ring.end());
    }
    const auto st = Stamp();
    const auto path = Dir() + LR"(\mark-)" + std::wstring(st.begin(), st.end()) + L".tsv";
    ++g_writers;
    std::thread([ring = std::move(ring), label, path] {
        try {
            if (FILE* f = _wfsopen(path.c_str(), L"wb", _SH_DENYNO)) {
                const DWORD end = ring.empty() ? 0 : ring.back().ms;
                auto head = std::format("# {}\n# ms_before\tthread\tclass\tobject\tfunction\n", label);
                fwrite(head.data(), 1, head.size(), f);
                for (auto& e : ring) {
                    auto line = std::format("{}\t{}\t{}\t{}\t{}\n", end - e.ms, e.tid, ue::ClassName(e.obj), ue::ObjName(e.obj), ue::ObjName(e.func));
                    fwrite(line.data(), 1, line.size(), f);
                }
                fclose(f);
            }
            log::Info("标记 {}：{} 条调用 → {}", label, ring.size(), Narrow(path));
            --g_writers;
        } catch (...) { --g_writers; }
    }).detach();
}

void Shutdown() {
    if (g_tracing) Trace(false);
    for (int i = 0; i < 250 && g_writers > 0; ++i) Sleep(20);  // 等后台写文件结束，再卸载 DLL
}

std::string DumpCounts() {
    std::unordered_map<ue::UObject*, uint64_t> counts;
    {
        std::lock_guard lk(g_lock);
        counts = g_counts;
    }
    const auto st = Stamp();
    const auto path = Dir() + LR"(\counts-)" + std::wstring(st.begin(), st.end()) + L".tsv";
    if (FILE* f = _wfsopen(path.c_str(), L"wb", _SH_DENYNO)) {
        for (auto& [fn, n] : counts) {
            auto* owner = ue::Get<ue::UObject*>(fn + ue::Layout::ObjOuter);
            auto line = std::format("{}\t{}\t{}\n", n, owner ? ue::ObjName(owner) : "", ue::ObjName(fn));
            fwrite(line.data(), 1, line.size(), f);
        }
        fclose(f);
    }
    return Narrow(path);
}

void PollHotkey() {
    if (!g_tracing) return;
    const bool down = (GetAsyncKeyState(MarkKey()) & 0x8000) != 0;
    if (down && !g_keyDown) Mark("key");
    g_keyDown = down;
}

}  // namespace pbz::pe
