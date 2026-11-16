#include "hooks.h"

#include <map>
#include <mutex>

#include "../host/log.h"
#include "../third_party/minhook/include/MinHook.h"
#include "abshook.h"

namespace pbz::hooks {
namespace {

// MinHook 优先；游戏 exe 附近申请不到跳板内存时改用远跳钩子
struct Entry { void* target; bool abs; abshook::Hook h; };
std::mutex g_lock;
std::map<std::string, Entry> g_hooks;
bool g_init = false;
bool g_retired = false;

}  // namespace

bool Init(std::string* why) {
    std::lock_guard lk(g_lock);
    if (g_retired) { if (why) *why = "旧核心已经停用"; return false; }
    if (g_init) return true;
    if (MH_Initialize() != MH_OK) { *why = "MinHook 初始化失败"; return false; }
    g_init = true;
    return true;
}

bool Create(const char* name, void* target, void* detour, void** original) {
    std::lock_guard lk(g_lock);
    if (!g_init || g_retired || !target) return false;
    if (g_hooks.count(name)) return true;
    const auto st = MH_CreateHook(target, detour, original);
    if (st == MH_OK) {
        g_hooks.emplace(name, Entry{target, false, {}});
        log::Info("钩子 {} @ {}", name, target);
        return true;
    }
    if (st != MH_ERROR_MEMORY_ALLOC) {
        log::Info("钩子 {} 创建失败：{}", name, MH_StatusToString(st));
        return false;
    }
    Entry e{target, true, {}};
    std::string why;
    if (!abshook::Prepare(e.h, target, detour, &why)) {
        log::Info("钩子 {} 创建失败：附近无跳板内存，远跳也不行（{}）", name, why);
        return false;
    }
    *original = e.h.tramp;
    g_hooks.emplace(name, e);
    log::Info("钩子 {} @ {}（远跳，覆盖 {} 字节）", name, target, e.h.len);
    return true;
}

bool Enable(const char* name, bool on) {
    std::lock_guard lk(g_lock);
    if (on && g_retired) return false;
    auto it = g_hooks.find(name);
    if (it == g_hooks.end()) return false;
    if (it->second.abs) return abshook::Apply(it->second.h, on);
    const auto st = on ? MH_EnableHook(it->second.target) : MH_DisableHook(it->second.target);
    return st == MH_OK || st == MH_ERROR_ENABLED || st == MH_ERROR_DISABLED;
}

void RemoveAll() {
    std::lock_guard lk(g_lock);
    if (!g_init || g_retired) return;
    g_retired = true;
    for (auto& [n, e] : g_hooks)
        if (e.abs) abshook::Apply(e.h, false);  // 跳板内存不释放：可能还有线程正在里面
    MH_DisableHook(MH_ALL_HOOKS);
    // The core DLL stays resident. Its original-call trampolines must too:
    // another thread may be about to call one after leaving a callback, even
    // when ProcessEvent's in-flight count has reached zero. MH_Uninitialize
    // frees these executable pages; disabling alone restores game code safely.
    // A retired ledger rejects delayed attempts to re-enable old hooks.
}

}  // namespace pbz::hooks
