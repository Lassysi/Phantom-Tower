#include "native_exec.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>
#include <utility>

#include "../host/log.h"
#include "diag.h"

namespace pbz::nx {
namespace {

using Native = void (*)(ue::UObject* self, void* stack, void* result);

// FFrame（继承 FOutputDevice：虚表 8 + 两个 bool，补齐到 0x10）：Node +0x10、Object +0x18、Code +0x20、Locals +0x28
constexpr int kFrameNode = 0x10, kFrameCode = 0x20, kFrameLocals = 0x28;

struct Slot {
    ue::UObject* func = nullptr;
    Native orig = nullptr;  // 卸下后也保留：万一还有在途调用，跳板仍能转给原函数
    Handler handler;
    std::atomic<std::shared_ptr<const std::vector<std::pair<int, Observer>>>> observers;
    std::atomic<uint64_t> calls{0}, bytecode{0};
};
std::array<Slot, kSlots> g_slots;
int g_observerNext = 1;

template <int N> void Thunk(ue::UObject* self, void* stack, void* result) {
    Slot& s = g_slots[N];
    ++s.calls;
    const Native orig = s.orig;
    if (const auto observers = s.observers.load())
        for (const auto& [token, fn] : *observers)
            diag::Guarded("原生执行观察", [&] { fn(self); });
    if (s.func && s.handler && stack) {
        auto* f = static_cast<uint8_t*>(stack);
        void* node = *reinterpret_cast<void**>(f + kFrameNode);
        void* code = *reinterpret_cast<void**>(f + kFrameCode);
        auto* locals = *reinterpret_cast<uint8_t**>(f + kFrameLocals);
        if (node == s.func && !code && locals) {
            bool handled = false;
            diag::Guarded("原生执行拦截", [&] { handled = s.handler(self, locals, result); });
            if (handled) return;
        } else ++s.bytecode;
    }
    if (orig) orig(self, stack, result);
}

template <size_t... I> constexpr std::array<Native, sizeof...(I)> Table(std::index_sequence<I...>) { return {&Thunk<static_cast<int>(I)>...}; }
constexpr auto kThunks = Table(std::make_index_sequence<kSlots>{});

Slot* Find(ue::UObject* func) {
    for (auto& s : g_slots) if (s.func == func) return &s;
    return nullptr;
}
int IndexOf(const Slot* s) { return static_cast<int>(s - g_slots.data()); }

Slot* Install(ue::UObject* func, std::string* why) {
    if (!func || !ue::Live(func)) { *why = "函数对象无效"; return nullptr; }
    if (Slot* s = Find(func)) return s;
    Slot* free = nullptr;
    for (auto& s : g_slots) if (!s.func) { free = &s; break; }
    if (!free) { *why = "拦截槽位用完"; return nullptr; }
    void* cur = nullptr;
    if (!ue::Read(func + kFuncOffset, &cur, sizeof cur) || !cur) { *why = "读不到原生执行入口"; return nullptr; }
    for (auto t : kThunks) if (reinterpret_cast<void*>(t) == cur) { *why = "入口已经是跳板（别的副本挂过）"; return nullptr; }
    free->orig = reinterpret_cast<Native>(cur); free->calls = 0; free->bytecode = 0; free->func = func;
    void* p = reinterpret_cast<void*>(kThunks[IndexOf(free)]);
    if (!ue::Write(func + kFuncOffset, &p, sizeof p)) { free->func = nullptr; *why = "写入原生执行入口失败"; return nullptr; }
    log::Info("原生执行拦截：已挂 {}（原入口 {}）", ue::ObjName(func), cur);
    return free;
}
void RestoreIfUnused(Slot& s) {
    if (s.handler || s.observers.load()) return;
    void* cur = nullptr;
    const auto thunk = reinterpret_cast<void*>(kThunks[IndexOf(&s)]);
    if (ue::Live(s.func) && ue::Read(s.func+kFuncOffset,&cur,sizeof cur) && cur==thunk) {
        void* orig=reinterpret_cast<void*>(s.orig);ue::Write(s.func+kFuncOffset,&orig,sizeof orig);
    }
    s.func=nullptr;
}

}  // namespace

bool Hook(ue::UObject* func, Handler fn, std::string* why) {
    auto* slot=Install(func,why);if(!slot)return false;slot->handler=std::move(fn);return true;
}

void Unhook(ue::UObject* func) {
    Slot* s = Find(func);
    if (!s) return;
    s->handler = nullptr;
    RestoreIfUnused(*s);
}

int Observe(ue::UObject* func, Observer fn, std::string* why) {
    if(!fn){*why="观察回调为空";return 0;}
    auto* slot=Install(func,why);if(!slot)return 0;
    const auto previous=slot->observers.load();
    auto next=std::make_shared<std::vector<std::pair<int,Observer>>>(previous?*previous:std::vector<std::pair<int,Observer>>{});
    const int token=g_observerNext++;next->emplace_back(token,std::move(fn));
    slot->observers.store(next);return token;
}
void Unobserve(int token) {
    for(auto& slot:g_slots){
        const auto previous=slot.observers.load();if(!previous)continue;
        auto next=std::make_shared<std::vector<std::pair<int,Observer>>>(*previous);
        std::erase_if(*next,[&](const auto& item){return item.first==token;});
        if(next->size()==previous->size())continue;
        slot.observers.store(next->empty()?std::shared_ptr<const std::vector<std::pair<int,Observer>>>{}:next);
        RestoreIfUnused(slot);return;
    }
}

void UnhookAll() {
    for (auto& s : g_slots) if (s.func) {s.handler=nullptr;s.observers.store({});RestoreIfUnused(s);}
}
bool Hooked(ue::UObject* func) { return Find(func) != nullptr; }
uint64_t Calls(ue::UObject* func) { const Slot* s = Find(func); return s ? s->calls.load() : 0; }
uint64_t Bytecode(ue::UObject* func) { const Slot* s = Find(func); return s ? s->bytecode.load() : 0; }

}  // namespace pbz::nx
