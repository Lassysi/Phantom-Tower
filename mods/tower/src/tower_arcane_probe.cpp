#include "tower_arcane_probe.h"
#ifdef PBZ_HEX_TEST
#include <array>
#include <cmath>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "../../../src/core/sdk/pbz_module.h"
#include "../../../src/core/host/log.h"
#include "tower_api.h"
#include "tower_perf.h"
#include "tower_widgets.h"
#include "tower_arcane_charge.h"
#include "tower_arcane_hit_probe.h"
namespace towerarcane {
namespace {
namespace ue = pbz::ue; namespace pe = pbz::pe;
using ue::UObject;
UObject* pawn = nullptr; UObject* component = nullptr; UObject* componentClass = nullptr;
int pawnIndex = -1, componentIndex = -1, componentOff = -1;
std::array<uint64_t,1> tags{}; std::array<int,1> handles{-1};
bool active = false;
bool countCandidate = false, taskBaseline = false;
std::unordered_map<UObject*, int> attackTasks;
struct Swing { UObject* task=nullptr; int taskIndex=-1, abilityIndex=-1; unsigned long long first=0,last=0; std::unordered_map<void*,int> targets; };
std::unordered_map<UObject*,Swing> swings;
std::vector<towerarcanehit::Hit> pendingHits;
int attackLogBudget = 0;
unsigned long long nextCooldownSample = 0;
float lastCooldown = -1;
// Validate each reflected parameter's type, size and offset before calling.
bool Param(UObject* fn, std::string_view name, std::string_view type, int size, int offset, std::string_view structName="GameplayTag") {
    if (!fn || !ue::Live(fn)) return false;
    auto* field = ue::Get<UObject*>(fn + ue::Layout::StructChildProps);
    for (int n=0; field && n<128; ++n, field=ue::Get<UObject*>(field+ue::Layout::FieldNext)) {
        auto* fieldClass=ue::Get<UObject*>(field+ue::Layout::FieldClass);
        const auto nameId=ue::Get<uint32_t>(field+ue::Layout::FieldName);
        const auto typeId=fieldClass ? ue::Get<uint32_t>(fieldClass+ue::Layout::FieldClassName) : 0;
        if (ue::Name(nameId)!=name) continue;
        auto* structType = type=="StructProperty" ? ue::Get<UObject*>(field+0x70) : nullptr;
        return ue::Name(typeId)==type && ue::Get<int>(field+0x34)==size && ue::Get<int>(field+ue::Layout::PropOffset)==offset &&
               (ue::Get<uint64_t>(field+0x38) & 0x80) != 0 &&
               (type!="StructProperty" || (structType && ue::Live(structType) && ue::ObjName(structType)==structName));
    }
    return false;
}
UObject* FindProperty(UObject* cls, std::string_view name) {
    for (int depth=0; cls && ue::Live(cls) && depth<64; ++depth,cls=ue::Get<UObject*>(cls+ue::Layout::StructSuper))
        for (auto* field=ue::Get<UObject*>(cls+ue::Layout::StructChildProps); field; field=ue::Get<UObject*>(field+ue::Layout::FieldNext))
            if (ue::Name(ue::Get<uint32_t>(field+ue::Layout::FieldName))==name) return field;
    return nullptr;
}
bool PropertyIs(UObject* field, std::string_view type, int size) {
    auto* cls=field?ue::Get<UObject*>(field+ue::Layout::FieldClass):nullptr;
    return cls && ue::Name(ue::Get<uint32_t>(cls+ue::Layout::FieldClassName))==type && ue::Get<int>(field+0x34)==size;
}
bool Identity();
bool NativeCharge(float gain, float current) {
    if (!Identity() || !std::isfinite(gain) || gain<=0 || !std::isfinite(current)) return false;
    static UObject* effect=nullptr; static int effectIndex=-1;
    if (!effect || !ue::Live(effect) || ue::Get<int>(effect+ue::Layout::ObjIndex,-1)!=effectIndex) {
        effect=nullptr; effectIndex=-1; int found=0;
        ue::ForEach([&](int,UObject* o){
            if (o && ue::Live(o) && !ue::IsDefault(o) && ue::ClassName(o)=="GameplayEffect" && ue::ObjName(o)=="GE_MA_UltimateCharge_1_Add") { effect=o; ++found; }
            return true;
        });
        if (found!=1) { effect=nullptr; return false; }
        effectIndex=ue::Get<int>(effect+ue::Layout::ObjIndex,-1);
    }
    auto* effectCls=ue::ClassOf(effect); auto* modifiers=FindProperty(effectCls,"Modifiers");
    auto* duration=FindProperty(effectCls,"DurationPolicy");
    if (!PropertyIs(modifiers,"ArrayProperty",16) || !PropertyIs(duration,"EnumProperty",1)) return false;
    const int mo=ue::Get<int>(modifiers+ue::Layout::PropOffset,-1), d=ue::Get<int>(duration+ue::Layout::PropOffset,-1);
    if (mo<0 || d<0 || ue::Get<uint8_t>(effect+d,255)!=0) return false; // instant effect
    auto* inner=ue::Get<UObject*>(modifiers+0x78); auto* modStruct=inner?ue::Get<UObject*>(inner+0x70):nullptr;
    if (!PropertyIs(inner,"StructProperty",824) || !modStruct || !ue::Live(modStruct) || ue::ObjName(modStruct)!="GameplayModifierInfo") return false;
    auto* attrProp=FindProperty(modStruct,"Attribute"); auto* opProp=FindProperty(modStruct,"ModifierOp");
    auto* attrStruct=attrProp?ue::Get<UObject*>(attrProp+0x70):nullptr;
    if (!PropertyIs(attrProp,"StructProperty",56) || !attrStruct || !ue::Live(attrStruct) || ue::ObjName(attrStruct)!="GameplayAttribute" ||
        !PropertyIs(opProp,"ByteProperty",1)) return false;
    const int ao=ue::Get<int>(attrProp+ue::Layout::PropOffset,-1), oo=ue::Get<int>(opProp+ue::Layout::PropOffset,-1);
    auto* data=ue::Get<uint8_t*>(effect+mo); const int n=ue::Get<int>(effect+mo+8,-1), cap=ue::Get<int>(effect+mo+12,-1);
    if (!data || n!=1 || cap<1 || cap>32 || ao<0 || ao+56>824 || oo<0 || oo>=824 || ue::Get<uint8_t>(data+oo,255)!=0) return false;
    const auto* nameData=ue::Get<const wchar_t*>(data+ao); const int nameLen=ue::Get<int>(data+ao+8,-1);
    std::array<wchar_t,15> name{};
    if (!nameData || nameLen!=15 || !ue::Read(nameData,name.data(),name.size()*sizeof(wchar_t)) || std::wstring_view(name.data(),14)!=L"UltimateCharge" || name[14]!=0) return false;
    auto* cls=towerperf::ClassNamed("AttributeUtils"); auto* lib=towerwidgets::Default(cls);
    auto* get=cls?pe::FindFunction(cls,"GetCharacterAttributeValue"):nullptr;
    auto* set=cls?pe::FindFunction(cls,"ModifyCharacterAttributeValue"):nullptr;
    if (!lib || !ue::Live(lib) || !Param(get,"Character","ObjectProperty",8,0) || !Param(get,"Attribute","StructProperty",56,8,"GameplayAttribute") ||
        !Param(get,"ReturnValue","FloatProperty",4,64) || !Param(set,"Character","ObjectProperty",8,0) ||
        !Param(set,"Attribute","StructProperty",56,8,"GameplayAttribute") || !Param(set,"ModiferOp","ByteProperty",1,64) ||
        !Param(set,"Value","FloatProperty",4,68)) return false;
    std::array<uint8_t,80> args{}; std::memcpy(args.data(),&pawn,8);
    if (!ue::Read(data+ao,args.data()+8,56)) return false;
    pe::Call(lib,get,args.data()); float observed=0; std::memcpy(&observed,args.data()+64,4);
    if (!std::isfinite(observed) || std::abs(observed-current)>.25f) return false;
    args.fill(0); std::memcpy(args.data(),&pawn,8);
    if (!ue::Read(data+ao,args.data()+8,56)) return false;
    args[64]=0; // EGameplayModOp::Additive, verified against the game's add effect.
    std::memcpy(args.data()+68,&gain,4); pe::Call(lib,set,args.data());
    return true;
}
bool Identity() {
    return pawn && component && componentClass && ue::Live(pawn) && ue::Live(component) && ue::Live(componentClass) &&
           ue::Get<int>(pawn+ue::Layout::ObjIndex,-1)==pawnIndex && ue::Get<int>(component+ue::Layout::ObjIndex,-1)==componentIndex &&
           ue::ClassOf(component)==componentClass && componentOff>=0 && ue::Get<UObject*>(pawn+componentOff)==component;
}
UObject* Unregister() {
    auto* fn=componentClass ? pe::FindFunction(componentClass,"UnregisterSwitchWeaponCooldownReduction") : nullptr;
    return Param(fn,"SlotTag","StructProperty",8,0) && Param(fn,"Handle","IntProperty",4,8) ? fn : nullptr;
}
}
void Stop() {
    towerarcanehit::Stop();
    if (Identity()) if (auto* fn=Unregister()) for (int i=0;i<static_cast<int>(handles.size());++i) if(handles[i]>=0) {
        std::array<uint8_t,32> args{}; std::memcpy(args.data(),&tags[i],8); std::memcpy(args.data()+8,&handles[i],4);
        pe::Call(component,fn,args.data());
        pbz::log::Info("秘术冲拳测试：已撤销第 {} 个冷却缩减句柄 {}",i,handles[i]);
    }
    pawn=component=componentClass=nullptr; pawnIndex=componentIndex=componentOff=-1; handles={-1}; tags={}; active=false;
    nextCooldownSample=0; lastCooldown=-1; countCandidate=false; taskBaseline=false; attackTasks.clear(); swings.clear(); pendingHits.clear(); attackLogBudget=0;
}
bool SetCountCandidate(bool enabled) {
    if (!active || !Identity()) return false;
    if (enabled && !towerarcanehit::Start(pawn)) { pbz::log::Info("秘术冲拳测试：原生命中入口未通过校验，不开启充能候选"); return false; }
    if (!enabled) towerarcanehit::Stop();
    countCandidate=enabled; taskBaseline=false; attackTasks.clear(); swings.clear(); pendingHits.clear(); attackLogBudget=enabled?40:0;
    pbz::log::Info("秘术冲拳测试：按普通攻击命中充能候选{}；只用于影塔验收", enabled ? "开启" : "关闭");
    return true;
}
void ResetWave() {
    if (!active) return;
    towerarcanehit::Hit hit;
    while (towerarcanehit::Pop(&hit)) {}
    taskBaseline=false; attackTasks.clear(); swings.clear(); pendingHits.clear();
}
bool Start(void* raw) {
    Stop(); auto* actor=static_cast<UObject*>(raw);
    if (!towerapi::InsideTower() || !actor || !ue::Live(actor)) return false;
    const int off=ue::PropOffset(ue::ClassOf(actor),"Character Weapons Component");
    auto* comp=off>=0 ? ue::Get<UObject*>(actor+off) : nullptr;
    auto* cls=comp && ue::Live(comp) ? ue::ClassOf(comp) : nullptr;
    if (!cls || ue::ObjName(cls)!="CharacterWeaponsComponent") return false;
    auto* fn=pe::FindFunction(cls,"RegisterSwitchWeaponCooldownReduction");
    if (!Param(fn,"SlotTag","StructProperty",8,0) || !Param(fn,"Reduction","FloatProperty",4,8) || !Param(fn,"ReturnValue","IntProperty",4,12)) return false;
    pawn=actor; component=comp; componentClass=cls; componentOff=off;
    pawnIndex=ue::Get<int>(actor+ue::Layout::ObjIndex,-1); componentIndex=ue::Get<int>(comp+ue::Layout::ObjIndex,-1);
    if (!Identity() || !Unregister()) { Stop(); return false; }
    tags={towerwidgets::Name("Weapon.Slot.Main")};
    if (!tags[0]) { Stop(); return false; }
    for (int i=0;i<static_cast<int>(handles.size());++i) {
        std::array<uint8_t,32> args{}; const float reduction=1000.0f; int invalid=-1;
        std::memcpy(args.data(),&tags[i],8); std::memcpy(args.data()+8,&reduction,4); std::memcpy(args.data()+12,&invalid,4);
        pe::Call(component,fn,args.data()); std::memcpy(&handles[i],args.data()+12,4);
        if(handles[i]<0) { Stop(); return false; }
    }
    active=true; pbz::log::Info("秘术冲拳测试：主武器已注册原生冷却缩减，句柄 {}；实际效果待实机核验",handles[0]); return true;
}
void CountAttacks() {
    towerarcanehit::Hit hit;
    while(towerarcanehit::Pop(&hit))if(pendingHits.size()<32)pendingHits.push_back(hit);
    const auto wall=GetTickCount64();
    for(auto it=pendingHits.begin();it!=pendingHits.end();){
        auto* ability=static_cast<UObject*>(it->ability);
        if(!ability || !ue::Live(ability) || ue::Get<int>(ability+ue::Layout::ObjIndex,-1)!=it->index || wall<it->time || wall-it->time>1200){it=pendingHits.erase(it);continue;}
        auto* target=static_cast<UObject*>(it->target);
        if(!target || !ue::Live(target) || ue::Get<int>(target+ue::Layout::ObjIndex,-1)!=it->targetIndex){it=pendingHits.erase(it);continue;}
        for(const auto& t:pbz::attr::Targets()) {
            if(t.obj!=pawn || !pbz::attr::Alive(t))continue;
            const int vo=pbz::attr::AttrOffset("UltimateCharge"), vm=pbz::attr::AttrOffset("UltimateCharge_Max");
            const int co=pbz::attr::AttrOffset("UltimateChargeCount"), cm=pbz::attr::AttrOffset("UltimateChargeCount_Max");
            if(vo<0 || vm<0 || co<0 || cm<0)break;
            auto* vc=pbz::attr::AttrCurrent(t,vo); auto* cc=pbz::attr::AttrCurrent(t,co);
            const float value=ue::Get<float>(vc,-1), maximum=ue::Get<float>(pbz::attr::AttrCurrent(t,vm),-1);
            const float count=ue::Get<float>(cc,-1), countMax=ue::Get<float>(pbz::attr::AttrCurrent(t,cm),-1);
            const float gain=tower::arcane::NativeAttackGain(value,maximum,count,countMax);
            if(gain<=0)break;
            if(!NativeCharge(gain,value)) {
                countCandidate=false;
                pbz::log::Info("秘术冲拳测试：原生充能接口校验失败，已停用候选，没有直接修改充能值"); break;
            }
            pbz::log::Info("秘术冲拳测试：{} 命中后调用原生充能 +{:.1f}，调用前 {:.1f}/{:.1f}，绝技点 {:.0f}/{:.0f}（候选）",ue::ClassName(ability),gain,value,maximum,count,countMax);
            break;
        }
        it=pendingHits.erase(it);
        if(!countCandidate){pendingHits.clear();towerarcanehit::Stop();return;}
    }
}
bool Tick(void* actor) {
    if (!active || actor!=pawn || !towerapi::InsideTower() || !Identity()) { Stop(); return false; }
    const auto now=GetTickCount64();
    if (now >= nextCooldownSample) {
        nextCooldownSample=now+100;
        auto* fn=pe::FindFunction(componentClass,"GetCurrentChangeWeaponCooldown");
        if (Param(fn,"SlotTag","StructProperty",8,0) && Param(fn,"ReturnValue","FloatProperty",4,8)) {
            std::array<uint8_t,16> args{}; std::memcpy(args.data(),&tags[0],8);
            pe::Call(component,fn,args.data()); float value=0; std::memcpy(&value,args.data()+8,4);
            if (std::isfinite(value) && value>=0 && (lastCooldown<0 || std::abs(value-lastCooldown)>.01f)) {
                pbz::log::Info("秘术冲拳测试：主武器原生切换冷却 {:.3f} 秒",value); lastCooldown=value;
            }
        }
    }
    if (countCandidate) CountAttacks();
    return true;
}
}
#else
namespace towerarcane { bool Start(void*) { return false; } bool Tick(void*) { return false; } bool SetCountCandidate(bool) { return false; } void ResetWave() {} void Stop() {} }
#endif
