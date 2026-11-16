// 原生执行入口拦截离线测试：假 UFunction（+0x128 放原生函数指针）与假 FFrame，按游戏的分派方式"取指针再调用"。
// 覆盖：挂上后经过跳板、参数缓冲调用交给处理函数、处理函数拦下时不调原函数、蓝图字节码调用原样放行、
// 卸下写回原入口、入口已被改过时不乱写、重复挂只换处理函数、槽位用完拒绝。
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../host/log.h"
#include "native_exec.cpp"

namespace pbz::ue {
bool Live(UObject* o) { return o != nullptr; }
bool Read(const void* p, void* out, size_t n) { std::memcpy(out, p, n); return true; }
bool Write(void* p, const void* in, size_t n) { std::memcpy(p, in, n); return true; }
std::string ObjName(UObject*) { return "Fake"; }
}  // namespace pbz::ue
namespace pbz::diag {
bool Guard(const char*, void (*fn)(void*), void* ctx) { fn(ctx); return true; }
}  // namespace pbz::diag

static int g_checks = 0, g_failed = 0;
static void Check(bool ok, const char* what) { ++g_checks; if (!ok) { ++g_failed; std::printf("FAIL %s\n", what); } }

static int g_origCalls = 0;
static void Original(pbz::ue::UObject*, void* stack, void* result) {
    ++g_origCalls;
    if (result) *static_cast<int*>(result) = 7;
    (void)stack;
}
using Native = void (*)(pbz::ue::UObject*, void*, void*);

struct FakeFunc { alignas(16) uint8_t bytes[0x200] = {}; };
struct FakeFrame { void* vtbl = nullptr; uint8_t flags[8] = {}; void* node = nullptr; void* object = nullptr; uint8_t* code = nullptr; uint8_t* locals = nullptr; };

static Native EntryOf(FakeFunc& f) { Native n; std::memcpy(&n, f.bytes + pbz::nx::kFuncOffset, sizeof n); return n; }
// 游戏的分派：从 UFunction+0x128 取指针再调用
static void Dispatch(FakeFunc& f, FakeFrame& fr, void* result) { EntryOf(f)(reinterpret_cast<pbz::ue::UObject*>(0x1000), &fr, result); }

int main() {
    static_assert(offsetof(FakeFrame, node) == 0x10 && offsetof(FakeFrame, code) == 0x20 && offsetof(FakeFrame, locals) == 0x28, "fake FFrame layout");
    FakeFunc fn;
    Native orig = &Original;
    std::memcpy(fn.bytes + pbz::nx::kFuncOffset, &orig, sizeof orig);
    auto* ufn = reinterpret_cast<pbz::ue::UObject*>(fn.bytes);

    int seen = -1;
    bool block = false;
    std::string why;
    Check(pbz::nx::Hook(ufn, [&](pbz::ue::UObject*, uint8_t* params, void* result) {
        seen = params[0];
        if (block && result) *static_cast<int*>(result) = 42;
        return block;
    }, &why), "hook installs");
    Check(EntryOf(fn) != &Original, "entry now points at a trampoline");

    uint8_t params[16] = {5};
    FakeFrame reflect;  // 反射 / 脚本调用：Node = 本函数、Code 空、Locals = 参数缓冲
    reflect.node = ufn; reflect.locals = params;
    int result = 0;
    Dispatch(fn, reflect, &result);
    Check(seen == 5 && g_origCalls == 1 && result == 7, "param-buffer call: handler sees params, original still runs when not handled");
    block = true;
    result = 0;
    Dispatch(fn, reflect, &result);
    Check(g_origCalls == 1 && result == 42, "handled call: original skipped, handler wrote the result");
    block = false;

    uint8_t bytecode[4] = {1, 2, 3, 4};
    FakeFrame bp;  // 蓝图字节码调用：Code 非空 → 原样放行
    bp.node = ufn; bp.code = bytecode; bp.locals = params;
    seen = -1;
    Dispatch(fn, bp, &result);
    Check(seen == -1 && g_origCalls == 2 && pbz::nx::Bytecode(ufn) == 1, "bytecode call: passed through untouched");
    FakeFrame other;  // Node 是别的函数（被别的函数的帧调用）→ 放行
    other.node = reinterpret_cast<void*>(0x2000); other.locals = params;
    Dispatch(fn, other, &result);
    Check(seen == -1 && g_origCalls == 3, "foreign frame: passed through");
    Check(pbz::nx::Calls(ufn) == 4, "call counter");

    // 重复挂：只换处理函数，入口不变
    const Native tramp = EntryOf(fn);
    int second = 0;
    Check(pbz::nx::Hook(ufn, [&](pbz::ue::UObject*, uint8_t*, void*) { ++second; return false; }, &why) && EntryOf(fn) == tramp, "re-hook replaces the handler only");
    Dispatch(fn, reflect, &result);
    Check(second == 1, "new handler used");

    // 卸下：写回原入口
    pbz::nx::Unhook(ufn);
    Check(EntryOf(fn) == &Original && !pbz::nx::Hooked(ufn), "unhook restores the original entry");
    Dispatch(fn, reflect, &result);
    Check(g_origCalls == 5, "after unhook the original is called directly");

    // 入口被别人改过：卸下时不写
    Check(pbz::nx::Hook(ufn, [](pbz::ue::UObject*, uint8_t*, void*) { return false; }, &why), "hook again");
    Native foreign = reinterpret_cast<Native>(&Check);
    std::memcpy(fn.bytes + pbz::nx::kFuncOffset, &foreign, sizeof foreign);
    pbz::nx::Unhook(ufn);
    Check(EntryOf(fn) == foreign, "unhook leaves an entry someone else changed");
    std::memcpy(fn.bytes + pbz::nx::kFuncOffset, &orig, sizeof orig);

    // 已经是跳板的入口不再挂（别的副本挂过）
    Check(pbz::nx::Hook(ufn, [](pbz::ue::UObject*, uint8_t*, void*) { return false; }, &why), "hook for copy test");
    FakeFunc copy;
    std::memcpy(copy.bytes + pbz::nx::kFuncOffset, fn.bytes + pbz::nx::kFuncOffset, 8);
    Check(!pbz::nx::Hook(reinterpret_cast<pbz::ue::UObject*>(copy.bytes), [](pbz::ue::UObject*, uint8_t*, void*) { return false; }, &why), "refuses an entry that is already a trampoline");
    pbz::nx::UnhookAll();
    Check(EntryOf(fn) == &Original, "unhook all restores");

    // 槽位用完
    std::vector<FakeFunc> many(pbz::nx::kSlots + 1);
    int ok = 0;
    for (auto& f : many) {
        std::memcpy(f.bytes + pbz::nx::kFuncOffset, &orig, sizeof orig);
        ok += pbz::nx::Hook(reinterpret_cast<pbz::ue::UObject*>(f.bytes), [](pbz::ue::UObject*, uint8_t*, void*) { return false; }, &why);
    }
    Check(ok == pbz::nx::kSlots, "slot limit enforced");
    pbz::nx::UnhookAll();
    bool restored = true;
    for (auto& f : many) restored &= EntryOf(f) == &Original;
    Check(restored, "all slots restored");
    Check(!pbz::nx::Hook(nullptr, [](pbz::ue::UObject*, uint8_t*, void*) { return false; }, &why), "null function refused");

    int observed=0,otherObserved=0,handlerCalls=0;
    const int observation=pbz::nx::Observe(ufn,[&](pbz::ue::UObject* self){++observed;Check(self==reinterpret_cast<pbz::ue::UObject*>(0x1000),"observer receives original owner");},&why);
    const int secondObservation=pbz::nx::Observe(ufn,[&](pbz::ue::UObject*){++otherObserved;},&why);
    Check(observation>0&&secondObservation>0&&observation!=secondObservation,"independent native observers coexist");
    const int originalBefore=g_origCalls;result=0;Dispatch(fn,reflect,&result);
    Check(observed==1&&otherObserved==1&&g_origCalls==originalBefore+1&&result==7,"observer-only script dispatch retains original execution and result");
    Dispatch(fn,bp,&result);Check(observed==2&&otherObserved==2&&g_origCalls==originalBefore+2,"bytecode dispatch also yields execution priority without parsing arguments");
    Check(pbz::nx::Hook(ufn,[&](pbz::ue::UObject*,uint8_t*,void*){++handlerCalls;return false;},&why),"handler shares an observed function");
    Dispatch(fn,reflect,&result);Check(observed==3&&handlerCalls==1,"observer and existing handler both receive script call");
    pbz::nx::Unobserve(observation);Dispatch(fn,reflect,&result);
    Check(observed==3&&otherObserved==4&&handlerCalls==2,"removing one observer retains other owners and handler");
    pbz::nx::Unhook(ufn);Dispatch(fn,reflect,&result);
    Check(otherObserved==5&&handlerCalls==2&&EntryOf(fn)!=&Original,"removing handler retains execution observer");
    pbz::nx::Unobserve(secondObservation);Check(EntryOf(fn)==&Original&&!pbz::nx::Hooked(ufn),"last observer removal restores only its owned native entry");
    int reentrant=0;reentrant=pbz::nx::Observe(ufn,[&](pbz::ue::UObject*){pbz::nx::Unobserve(reentrant);},&why);
    const int beforeReentrant=g_origCalls;Dispatch(fn,bp,&result);
    Check(g_origCalls==beforeReentrant+1&&EntryOf(fn)==&Original,"observer may remove itself without invalidating the active callback or native continuation");
    Check(pbz::nx::Observe(ufn,{},&why)==0&&EntryOf(fn)==&Original,"empty observer leaves no installed hook");
    pbz::nx::Observe(ufn,[](pbz::ue::UObject*){},&why);pbz::nx::UnhookAll();
    Check(EntryOf(fn)==&Original,"global cleanup also restores observer-only functions");

    if (g_failed) { std::printf("CORE_NX_FAILED %d/%d\n", g_failed, g_checks); return 1; }
    std::printf("CORE_NX_OK %d checks (no game process)\n", g_checks);
    return 0;
}
