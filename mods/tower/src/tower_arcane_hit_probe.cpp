#include "tower_arcane_hit_probe.h"
#ifdef PBZ_HEX_TEST
#include <windows.h>
#include <array>
#include <atomic>
#include <cstring>
#include <string>
#include "../../../src/core/sdk/pbz_module.h"
#include "../../../src/core/host/log.h"
#include "tower_api.h"
#include "tower_perf.h"
#include "tower_arcane_hit_sig.h"
namespace towerarcanehit {
namespace {
namespace ue=pbz::ue; namespace pe=pbz::pe; using ue::UObject;
using Native=void(*)(UObject*,UObject*,void*,void*,void*,void*);
pbz::abshook::Hook hook; Native original=nullptr;
std::atomic<bool> enabled{false}; DWORD gameThread=0; UObject* player=nullptr;
UObject *moduleClass=nullptr,*bulletClass=nullptr,*lightClass=nullptr;
int ownerOff=-1,bulletOff=-1,bulletOwnerOff=-1,sourceOff=-1,playerIndex=-1;
std::array<Hit,32> queue{}; size_t head=0,tail=0; bool retired=false; int logs=0, sourceLogs=0;
bool MainImage(const void* address,size_t size){
    auto* base=reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
    if(!base || !ue::Read(base,&dos,sizeof dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<=0 || dos.e_lfanew>4096 ||
       !ue::Read(base+dos.e_lfanew,&nt,sizeof nt) || nt.Signature!=IMAGE_NT_SIGNATURE)return false;
    const auto a=reinterpret_cast<uintptr_t>(address),b=reinterpret_cast<uintptr_t>(base);
    return a>=b && a-b<nt.OptionalHeader.SizeOfImage && size<=nt.OptionalHeader.SizeOfImage-(a-b);
}
bool Owned(){
    if(!hook.target || hook.len<14 || hook.len>32)return false;
    std::array<uint8_t,32> have{},want{};if(!ue::Read(hook.target,have.data(),hook.len))return false;
    if(!hook.on)return std::memcmp(have.data(),hook.orig,hook.len)==0;
    want.fill(0x90);want[0]=0xff;want[1]=0x25;want[2]=want[3]=want[4]=want[5]=0;std::memcpy(want.data()+6,&hook.detour,8);
    return std::memcmp(have.data(),want.data(),hook.len)==0;
}
bool Field(UObject* cls,const char* name,const char* type,int size,int* off){
    for(int depth=0;cls && ue::Live(cls) && depth<64;++depth,cls=ue::Get<UObject*>(cls+ue::Layout::StructSuper)){
        int count=0;
        for(auto* f=ue::Get<UObject*>(cls+ue::Layout::StructChildProps);f && count++<2048;f=ue::Get<UObject*>(f+ue::Layout::FieldNext)){
            if(ue::Name(ue::Get<uint32_t>(f+ue::Layout::FieldName))!=name)continue;
            auto* fc=ue::Get<UObject*>(f+ue::Layout::FieldClass);
            const int at=ue::Get<int>(f+ue::Layout::PropOffset,-1);
            if(!fc || ue::Name(ue::Get<uint32_t>(fc+ue::Layout::FieldClassName))!=type || ue::Get<int>(f+0x34)!=size || at<0)return false;
            *off=at;return true;
        }
    }
    return false;
}
// G 批：只算真的掉血的命中；同一攻击对象打同一目标 50 ms 内只算一次（多颗判定子弹 / 重复回调）
struct LastHit { UObject* bullet=nullptr; UObject* target=nullptr; unsigned long long at=0; };
std::array<LastHit,8> recent{}; size_t recentAt=0;
void Intercept(UObject* module,UObject* target,void* a,void* b,void* c,void* d){
    UObject* ability=nullptr; UObject* hitBullet=nullptr; int index=-1,targetIndex=-1;bool hit=false; float* hp=nullptr; float before=0;
    if(enabled.load(std::memory_order_acquire) && GetCurrentThreadId()==gameThread && player && ue::Live(player) &&
       ue::Get<int>(player+ue::Layout::ObjIndex,-1)==playerIndex && towerapi::InsideTower() && module && target &&
       ue::Live(module) && ue::Live(target) && ue::IsA(module,moduleClass) && ue::Get<UObject*>(module+ownerOff)==player){
        auto* bullet=ue::Get<UObject*>(module+bulletOff);
        if(bullet && ue::Live(bullet) && ue::IsA(bullet,bulletClass) && ue::Get<UObject*>(bullet+bulletOwnerOff)==player){
            hitBullet=bullet;
            ability=ue::Get<UObject*>(bullet+sourceOff);
            if (ability && ue::Live(ability) && sourceLogs < 30 &&
                (ue::Get<UObject*>(ability+ue::Layout::ObjOuter)!=player || !ue::IsA(ability,lightClass))) {
                for (const auto& t:pbz::attr::Targets()) if (t.obj==target && t.who==pbz::attr::Who::Enemies && pbz::attr::Alive(t)) {
                    ++sourceLogs;
                    auto* outer=ue::Get<UObject*>(ability+ue::Layout::ObjOuter);
                    pbz::log::Info("秘术冲拳共同入口取证：玩家攻击对象 {}，来源能力 {}，所属对象 {}，敌人 {}（只读，不充能）",
                                   ue::ObjName(bullet),ue::ClassName(ability),outer && ue::Live(outer)?ue::ClassName(outer):"-",ue::ClassName(target));
                    break;
                }
            }
            if(ability && ue::Live(ability) && ue::Get<UObject*>(ability+ue::Layout::ObjOuter)==player && ue::IsA(ability,lightClass)){
                index=ue::Get<int>(ability+ue::Layout::ObjIndex,-1);
                for(const auto& t:pbz::attr::Targets())if(t.obj==target && t.who==pbz::attr::Who::Enemies && pbz::attr::Alive(t)){
                    targetIndex=ue::Get<int>(target+ue::Layout::ObjIndex,-1);hit=index>=0 && targetIndex>=0;
                    if(const int ho=pbz::attr::AttrOffset("Health");ho>=0){hp=pbz::attr::AttrCurrent(t,ho);before=ue::Get<float>(hp,-1.0f);}
                    break;
                }
            }
        }
    }
    if(original)original(module,target,a,b,c,d);
    if(hit){
        const float after=hp?ue::Get<float>(hp,-1.0f):-1.0f;
        if(!hp || !(before>0) || !(after<before-0.5f))hit=false;  // 无敌帧 / 格挡 / 0 伤害不算
        const auto nowMs=GetTickCount64();
        for(const auto& r:recent)if(hit && r.bullet==hitBullet && r.target==target && nowMs-r.at<50)hit=false;
        if(hit){recent[recentAt]={hitBullet,target,nowMs};recentAt=(recentAt+1)%recent.size();}
    }
    if(hit && enabled.load(std::memory_order_acquire)){
        const size_t next=(tail+1)%queue.size();
        if(next!=head){queue[tail]={ability,index,target,targetIndex,GetTickCount64()};tail=next;}
        if(logs++<30 && ue::Live(ability) && ue::Live(target) && hitBullet && ue::Live(hitBullet))pbz::log::Info("秘术冲拳命中观察：玩家 {} 命中敌人 {} / {}（对象索引 {}，攻击对象 {} / {}；原生入口；未在钩子中改值）",ue::ClassName(ability),ue::ClassName(target),ue::ObjName(target),targetIndex,ue::ObjName(hitBullet),ue::Get<int>(hitBullet+ue::Layout::ObjIndex,-1));
    }
}
uint8_t* FindTarget(){
    auto* cls=towerperf::ClassNamed("BulletAttackModule"); auto* fn=cls?pe::FindFunction(cls,"DealDamageToAvatar"):nullptr;
    auto* exec=fn?ue::Get<uint8_t*>(fn+pbz::nx::kFuncOffset):nullptr;
    std::array<uint8_t,768>w{};std::array<uint8_t,112>leaf{};std::array<uint8_t,64>inner{};
    if(!MainImage(exec,w.size()) || !ue::Read(exec,w.data(),w.size()))return nullptr;
    const int a=tower::arcanehit::WrapperCall(w.data(),w.size());if(a<0)return nullptr;
    int32_t rel=0;std::memcpy(&rel,w.data()+a+1,4);
    auto* l=reinterpret_cast<uint8_t*>(reinterpret_cast<intptr_t>(exec)+a+5+rel);
    if(!MainImage(l,leaf.size()) || !ue::Read(l,leaf.data(),leaf.size()))return nullptr;
    const int b=tower::arcanehit::LeafCall(leaf.data(),leaf.size());if(b<0)return nullptr;
    std::memcpy(&rel,leaf.data()+b+1,4);auto* result=reinterpret_cast<uint8_t*>(reinterpret_cast<intptr_t>(l)+b+5+rel);
    return MainImage(result,inner.size()) && ue::Read(result,inner.data(),inner.size()) && tower::arcanehit::InnerSignature(inner.data(),inner.size()) ? result : nullptr;
}
}
bool Start(void* raw){
    if(retired || !raw || !ue::Live(static_cast<UObject*>(raw)) || !towerapi::InsideTower())return false;
    if(hook.on){Stop();if(hook.on || retired)return false;}
    moduleClass=towerperf::ClassNamed("BulletAttackModule");bulletClass=towerperf::ClassNamed("BulletObjectBase");lightClass=towerperf::ClassNamed("BP_ActionAbilityBase_C");
    if(!moduleClass || !bulletClass || !lightClass || !Field(moduleClass,"LastOwnerAvatar","ObjectProperty",8,&ownerOff) ||
       !Field(moduleClass,"Bullet","InterfaceProperty",16,&bulletOff) || !Field(bulletClass,"BulletOwnerAvatar","ObjectProperty",8,&bulletOwnerOff) ||
       !Field(bulletClass,"BulletSourceAbility","ObjectProperty",8,&sourceOff))return false;
    auto* target=FindTarget();if(!target)return false;
    if(hook.target){if(hook.target!=target || !Owned())return false;}
    else {std::string why;if(!pbz::abshook::Prepare(hook,target,reinterpret_cast<void*>(&Intercept),&why)){pbz::log::Info("秘术冲拳：原生命中入口不可用 {}",why);return false;}}
    original=reinterpret_cast<Native>(hook.tramp);player=static_cast<UObject*>(raw);playerIndex=ue::Get<int>(player+ue::Layout::ObjIndex,-1);gameThread=GetCurrentThreadId();head=tail=0;logs=0;sourceLogs=0;
    if(playerIndex<0 || !Owned() || !pbz::abshook::Apply(hook,true)){player=nullptr;return false;}
    enabled.store(true,std::memory_order_release);pbz::log::Info("秘术冲拳：已开启原生命中观察，等待实机校验");return true;
}
bool Pop(Hit* out){if(!out || head==tail)return false;*out=queue[head];head=(head+1)%queue.size();return true;}
void Stop(){
    enabled.store(false,std::memory_order_release);head=tail=0;player=nullptr;
    if(!hook.on)return;
    if(!Owned() || !pbz::abshook::Apply(hook,false)){retired=true;pbz::log::Info("秘术冲拳：命中观察还原未确认，保留原入口现场");return;}
    pbz::log::Info("秘术冲拳：原生命中观察已还原");
}
}
#else
namespace towerarcanehit { bool Start(void*){return false;} bool Pop(Hit*){return false;} void Stop(){} }
#endif
