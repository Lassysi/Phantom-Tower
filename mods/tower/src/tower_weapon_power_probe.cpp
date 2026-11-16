#include "tower_weapon_power_probe.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include "../../../src/core/sdk/pbz_module.h"
#include "../../../src/core/host/log.h"
#include "tower_api.h"
#include "tower_perf.h"
#include "tower_weapon_power_sig.h"

namespace towerweaponprobe {
namespace {
namespace ue=pbz::ue; namespace pe=pbz::pe;
using ue::UObject;
using Native=void(*)(UObject*,UObject*,float*,float*);
pbz::abshook::Hook hook; Native original=nullptr;
std::atomic<bool> active{false}; DWORD gameThread=0; UObject* player=nullptr;
bool retired=false, testBonus=false; int calls=0;
bool MainImage(const void* a,size_t n) {
    auto* b=reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)); IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    if(!b || !ue::Read(b,&dos,sizeof dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<=0 || dos.e_lfanew>4096 ||
       !ue::Read(b+dos.e_lfanew,&nt,sizeof nt) || nt.Signature!=IMAGE_NT_SIGNATURE)return false;
    const auto x=reinterpret_cast<uintptr_t>(a), base=reinterpret_cast<uintptr_t>(b);
    return x>=base && x-base<nt.OptionalHeader.SizeOfImage && n<=nt.OptionalHeader.SizeOfImage-(x-base);
}
bool Owned() {
    if(!hook.target || hook.len<14 || hook.len>32)return false;
    std::array<uint8_t,32> have{},want{}; if(!ue::Read(hook.target,have.data(),hook.len))return false;
    if(!hook.on)return std::memcmp(have.data(),hook.orig,hook.len)==0;
    want.fill(0x90); want[0]=0xff; want[1]=0x25;
    want[2]=want[3]=want[4]=want[5]=0; std::memcpy(want.data()+6,&hook.detour,8);
    return std::memcmp(have.data(),want.data(),hook.len)==0;
}
void Intercept(UObject* cfg,UObject* actor,float* power,float* shachiPower) {
    if(original)original(cfg,actor,power,shachiPower);
    if(!active.load(std::memory_order_acquire) || GetCurrentThreadId()!=gameThread || actor!=player ||
       !towerapi::InsideTower() || !ue::Live(actor) || !ue::Live(cfg))return;
    float a=0,b=0;
    if(!ue::Read(power,&a,4) || !ue::Read(shachiPower,&b,4) || !std::isfinite(a) || !std::isfinite(b))return;
    // N9m 顺序（用户）：武器强度（含升级）先 × 熔铸，伤害再 × 角色攻击（含攻击%），重量级打手的生命攻击最后平加、不吃熔铸和攻击%
    if(const float m=towerapi::MeltPowerMul(cfg); m!=1.0f && m>0.0f) {
        const float p1=a*m, s1=b*m; ue::Write(power,&p1,4); if(shachiPower!=power)ue::Write(shachiPower,&s1,4);
        static int meltLogs=0; if(++meltLogs<=12)pbz::log::Info("影塔熔铸：{} 原生强度 {:.2f}/{:.2f} → {:.2f}/{:.2f}",ue::ObjName(cfg),a,b,p1,s1);
        a=p1; b=s1;
    }
    // 3.3.4 影刃专精：只乘影之武装（PBZ_SubWeaponConfig_C，排除道具 PBZ_UseableItemConfig_C）的攻击强度，主武器不动；
    // 倍率 = (1+X+1)/(1+X)，X 为影塔攻击加成，效果等于「攻击加成合计 +100%」，面板不显示
    if(const float se=towerapi::ShadowEdgeMul(); se>1.0f) {
        static UObject* subCls=nullptr; static UObject* itemCls=nullptr;
        if(!subCls) subCls=towerperf::ClassNamed("PBZ_SubWeaponConfig_C");
        if(!itemCls) itemCls=towerperf::ClassNamed("PBZ_UseableItemConfig_C");
        if(subCls && ue::IsA(cfg,subCls) && !(itemCls && ue::IsA(cfg,itemCls))) {
            const float p1=a*se; ue::Write(power,&p1,4);
            static int seLogs=0; if(++seLogs<=8)pbz::log::Info("影刃专精：{} 影之武装强度 {:.2f} → {:.2f}（×{:.2f}）",ue::ObjName(cfg),a,p1,se);
            a=p1;
        }
    }
    float adjusted=a, hp=0, attack=0; bool changed=false;
    auto* mainClass=towerperf::ClassNamed("PBZ_MainWeaponConfig_C");
    const bool selected=towerapi::Growth().on && towerapi::Growth().hexes.Has(tower::hex::Heavy);
    if((selected || testBonus) && mainClass && ue::IsA(cfg,mainClass) && power!=shachiPower) {
        const int healthOff=pbz::attr::AttrOffset("Health_Max"), attackOff=pbz::attr::AttrOffset("Attack");
        for(const auto& t:pbz::attr::Targets()) if(t.obj==actor && pbz::attr::Alive(t) && healthOff>=0 && attackOff>=0) {
            hp=ue::Get<float>(pbz::attr::AttrCurrent(t,healthOff)); attack=ue::Get<float>(pbz::attr::AttrCurrent(t,attackOff));
            if(tower::weaponpower::AddHealthPower(a,attack,hp,&adjusted,1.0,towerapi::Growth().hexes.HeavyShare()))changed=ue::Write(power,&adjusted,4);  // 用户定（N9l 撤回）：先 × 攻击% 再 + 15% 生命，生命部分不吃攻击%
            break;
        }
    }
#ifdef PBZ_HEX_TEST
    if(++calls<=50)pbz::log::Info("重量级测试：武器配置 {}，原生强度 {:.3f} → {:.3f}，生命上限 {:.0f}、角色攻击 {:.1f}（假设验证：{}）",ue::ObjName(cfg),a,adjusted,hp,attack,changed?"已加成":"只读");
#endif
}
uint8_t* FindTarget() {
    if(retired)return nullptr;
    if(hook.on)return Owned()?static_cast<uint8_t*>(hook.target):nullptr;
    auto* cls=towerperf::ClassNamed("PBZ_WeaponConfigBase"); auto* fn=cls?pe::FindFunction(cls,"GetWeaponPower"):nullptr;
    auto* exec=fn?ue::Get<uint8_t*>(fn+pbz::nx::kFuncOffset):nullptr;
    std::array<uint8_t,384> wrapper{}; std::array<uint8_t,128> body{};
    if(!MainImage(exec,wrapper.size()) || !ue::Read(exec,wrapper.data(),wrapper.size()))return nullptr;
    const int call=tower::weaponpower::WrapperCall(wrapper.data(),wrapper.size());
    if(call<0)return nullptr;
    int32_t rel=0;std::memcpy(&rel,wrapper.data()+call+1,4);
    auto* target=reinterpret_cast<uint8_t*>(reinterpret_cast<intptr_t>(exec)+call+5+rel);
    if(!MainImage(target,body.size()) || !ue::Read(target,body.data(),body.size()) || !tower::weaponpower::NativeSignature(body.data(),body.size()))return nullptr;
    return target;
}
}
bool Supported() { return FindTarget()!=nullptr; }
bool Ensure(void* raw) {
    if(hook.on && active.load(std::memory_order_acquire)) { player=static_cast<UObject*>(raw); return true; }
    static unsigned long long next=0; const auto now=GetTickCount64(); if(now<next)return false; next=now+2000;
    return Start(raw);
}
bool Start(void* raw) {
    if(retired || !raw || !ue::Live(static_cast<UObject*>(raw)))return false;
    if(hook.on){ player=static_cast<UObject*>(raw); calls=0; active.store(true,std::memory_order_release); return true; }
    auto* target=FindTarget(); if(!target){pbz::log::Info("重量级：武器计算接口特征不匹配，未启用");return false;}
    std::string why; if(!pbz::abshook::Prepare(hook,target,reinterpret_cast<void*>(&Intercept),&why)){
        pbz::log::Info("重量级只读取证：无法准备观察：{}",why);return false;
    }
    original=reinterpret_cast<Native>(hook.tramp); gameThread=GetCurrentThreadId(); player=static_cast<UObject*>(raw); calls=0; testBonus=false;
    if(!Owned() || !pbz::abshook::Apply(hook,true)){pbz::log::Info("重量级只读取证：观察入口启用失败");return false;}
    active.store(true,std::memory_order_release); pbz::log::Info("重量级：原生武器强度接口已启用");return true;
}
bool SetTestBonus(bool enabled) {
    if(!active.load(std::memory_order_acquire) || !hook.on || GetCurrentThreadId()!=gameThread || !towerapi::InsideTower())return false;
    testBonus=enabled; calls=0; pbz::log::Info("重量级测试：生命上限 15% 换算候选 {}（仅测试，需验证原生伤害路径）",enabled?"开启":"关闭"); return true;
}
void Stop() {
    active.store(false,std::memory_order_release); player=nullptr; testBonus=false;
    if(!hook.on)return;
    if(!Owned() || !pbz::abshook::Apply(hook,false)){retired=true;pbz::log::Info("重量级只读取证：还原未确认，保留原路及现场");return;}
    pbz::log::Info("重量级只读取证：已还原入口，玩家武器计算 {} 次",calls);
}
}
