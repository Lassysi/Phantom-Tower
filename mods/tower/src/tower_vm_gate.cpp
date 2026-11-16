#include "tower_vm_gate.h"
#include "tower_vm_bridge.h"
#include <atomic>
#include <unordered_map>
#include "../../../src/core/sdk/pbz_module.h"
#include "../../../src/core/host/log.h"
#include "../../../src/core/services/diag.h"
#include "tower_perf.h"

namespace towervm {
namespace {
using pbz::ue::UObject;
using Native=void(*)(UObject*,void*,void*);
pbz::abshook::Hook bridgeHook;
Native original=nullptr;
uint8_t* sharedWrapper=nullptr;
std::atomic<bool> enabled{false};
std::atomic<bool> faulted{false};
std::atomic<DWORD> thread{0};
std::unordered_map<UObject*,Handler> handlers;
std::unordered_map<std::string,Handler> classHandlers;
struct CachedClass {uint64_t name=0;UObject* parent=nullptr;Handler* handler=nullptr;};
std::unordered_map<UObject*,CachedClass> classCache;
thread_local UObject* cleanup=nullptr;
bool retired=false;
uint64_t calls=0,blocked=0;

void Dispatch(UObject* self,void* stack,void* result) {
    const Native call=original; // trampoline retained even after disable
    if(!enabled.load(std::memory_order_acquire) || GetCurrentThreadId()!=thread.load() || !stack) {if(call)call(self,stack,result);return;}
    auto* frame=static_cast<uint8_t*>(stack);
    auto* fn=pbz::ue::Get<UObject*>(frame+0x10);
    auto it=handlers.find(fn);
    Handler* handler=it==handlers.end()?nullptr:&it->second;
    if(self && !classHandlers.empty()) {
        auto* cls=pbz::ue::ClassOf(self);
        const auto name=cls?pbz::ue::Get<uint64_t>(cls+pbz::ue::Layout::ObjName):0;
        auto* parent=cls?pbz::ue::Get<UObject*>(cls+pbz::ue::Layout::StructSuper):nullptr;
        auto [c,fresh]=classCache.try_emplace(cls,CachedClass{});
        if(fresh || c->second.name!=name || c->second.parent!=parent){
            c->second={name,parent,nullptr};
            // QTE/FlowLogic node subclasses inherit the same scene contract.
            for(int depth=0;cls && depth<64;++depth,cls=pbz::ue::Get<UObject*>(cls+pbz::ue::Layout::StructSuper)){
                auto found=classHandlers.find(pbz::ue::ObjName(cls));
                if(found!=classHandlers.end()){c->second.handler=&found->second;break;}
            }
        }
        if(c->second.handler)handler=c->second.handler;
    }
    if(!handler || cleanup==self){if(call)call(self,stack,result);return;}
    ++calls;
    Action decision=Action::Block;
    try {towerperf::Section _("蓝图执行门");decision=(*handler)(self,fn,pbz::ue::Get<void*>(frame+0x28));}
    catch(...) {
        faulted=true;++blocked;
        pbz::diag::Event("tower_vm_handler_fault",self,fn);
        pbz::log::Info("影塔蓝图执行门：处理异常，匹配节点已冻结；保留存档保护和现场，停止本层");
        return;
    }
    if(decision==Action::Block){++blocked;return;} // caller owns/destructs frame; void script function has no result
    auto* before=cleanup;
    if(decision==Action::Cleanup)cleanup=self;
    try {if(call)call(self,stack,result);} catch(...) {cleanup=enabled.load()?before:nullptr;throw;}
    cleanup=enabled.load()?before:nullptr;
}
}
bool Add(UObject* fn,Handler handler,std::string* why) {
    if(retired || faulted.load() || !fn || !pbz::ue::Live(fn)){if(why)*why="蓝图执行入口对象无效、处理异常或旧钩子未还原";return false;}
    // Never swallow a non-void function: its result/out-param contract is not ours.
    if(pbz::ue::PropOffset(fn,"ReturnValue")>=0){if(why)*why="只接管无返回值剧情执行函数";return false;}
    if(!bridgeHook.target) {
        auto* wrapper=pbz::ue::Get<uint8_t*>(fn+pbz::nx::kFuncOffset);
        std::array<uint8_t,128> bytes{};
        if(!wrapper || !pbz::ue::Read(wrapper,bytes.data(),bytes.size())){if(why)*why="读不到蓝图解释包装入口";return false;}
        auto* target=static_cast<uint8_t*>(bridge::Interpreter(bytes.data()));
        // Interpreter() was matched against a local copy: rebase the relative target.
        if(target)target=reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(wrapper)+reinterpret_cast<uintptr_t>(target)-reinterpret_cast<uintptr_t>(bytes.data()));
        std::array<uint8_t,32> signature{};
        if(!target || !pbz::ue::Read(target,signature.data(),signature.size()) || !(bridge::Signature(signature.data()) || bridge::ForeignJump(signature.data()))){if(why)*why="蓝图解释包装/执行入口特征不匹配";return false;}
        if(!bridge::Prepare(bridgeHook,target,reinterpret_cast<void*>(&Dispatch),why))return false;
        original=reinterpret_cast<Native>(bridgeHook.tramp);
        sharedWrapper=wrapper;
        pbz::log::Info("影塔蓝图执行门：已准备真实解释入口 {}（支持字节码直接调用，游戏线程限定）",static_cast<void*>(target));
    }
    if(pbz::ue::Get<uint8_t*>(fn+pbz::nx::kFuncOffset)!=sharedWrapper){if(why)*why="不是已验证的蓝图解释函数";return false;}
    if(!bridge::OwnedBytes(bridgeHook)){retired=true;if(why)*why="蓝图执行入口字节已被其他持有者修改，保留现场";return false;}
    thread=GetCurrentThreadId();
    handlers[fn]=std::move(handler);
    if(!bridgeHook.on && !pbz::abshook::Apply(bridgeHook,true)){handlers.erase(fn);if(why)*why="蓝图执行门启用失败";return false;}
    enabled.store(true,std::memory_order_release);
    return true;
}
void Remove(UObject* fn){handlers.erase(fn);}
void Class(std::string name,Handler handler){classHandlers[std::move(name)]=std::move(handler);classCache.clear();}
bool Ready(){return enabled.load() && bridgeHook.on && !retired && !faulted.load();}
bool Faulted(){return faulted.load() || retired;}
void End(){
    enabled.store(false,std::memory_order_release);
    handlers.clear();classHandlers.clear();classCache.clear();cleanup=nullptr;
    if(bridgeHook.on){
        if(!bridge::OwnedBytes(bridgeHook) || !pbz::abshook::Apply(bridgeHook,false)){
            retired=true;pbz::log::Info("影塔蓝图执行门：还原失败或原字节持有者改变，已转原路放行，保留跳板和现场");return;
        }
        pbz::log::Info("影塔蓝图执行门：已还原，剧情执行 {} 次、播放前冻结 {} 次",calls,blocked);
    }
    calls=blocked=0;
    faulted=false;
}
}
