// Native group milestones are part of a replay encounter, not Tower rewards.
// Rebind only the audited ClearGroup script's own native listener, after the
// session guard has muted the group. Never invoke its cleared event ourselves.
#pragma once
#include "tower_replay.h"
#include "tower_preload_runtime.h"
#include "group_contract.generated.h"
#include "tower_widgets.h"
#include <map>

namespace towergroup {
namespace ue=pbz::ue;
namespace pe=pbz::pe;
using ue::UObject;
struct List {void* data=nullptr;int32_t num=0,max=0;bool operator==(const List&)const=default;};
struct Binding {UObject* group=nullptr;UObject* script=nullptr;UObject* cls=nullptr;std::string groupName,scriptName;int offset=-1;List native;const tower::replay::Contract* contract=nullptr;int phase=-1;UObject* spawner=nullptr;int scriptsOffset=-1;int deadOffset=-1;List originalDead;};
inline std::map<UObject*,Binding> bindings;
inline std::map<std::string,uint64_t> tagNames;
inline UObject* checkedClass=nullptr;
inline std::array<UObject*,3> functions{};
// BP_AISpawner broadcasts this delegate on real death. The group's own native
// listener dispatches the muted scripts, then evaluates alive/pending/blockers.
// It is an input to group completion, not an external reward listener. Restore
// only a verified original self-listener; never invent weak pointers or fire it.
inline bool NativeDeathListener(UObject* group,const List& list) {
    if(!ue::Live(group) || ue::ClassName(group)!="MobGroup" || !list.data || list.num!=1 || list.max<1 || list.max>128)return false;
    struct Delegate {int32_t index,serial;uint64_t function;};
    Delegate entry{};
    if(!ue::Read(list.data,&entry,sizeof entry) || entry.index<0 || entry.serial<=0 ||
       ue::Get<int32_t>(group+ue::Layout::ObjIndex)!=entry.index || ue::Obj(entry.index)!=group ||
       uint32_t(entry.function>>32)!=0 || ue::Name(uint32_t(entry.function))!="OnMobGroupMemberDeadEvent")return false;
    auto* cls=ue::ClassOf(group);
    auto* fn=cls?pe::FindFunction(cls,"OnMobGroupMemberDeadEvent"):nullptr;
    auto* owner=fn?ue::Get<UObject*>(fn+ue::Layout::ObjOuter):nullptr;
    return fn && ue::Live(fn) && owner==cls && ue::ObjName(owner)=="MobGroup" &&
        ue::PropOffset(fn,"AISpawner")>=0 && ue::PropOffset(fn,"ReturnValue")<0;
}
inline bool KeepNativeDeath(UObject* group,int offset,const List& original,std::string* why) {
    if(offset<0 || !NativeDeathListener(group,original)){*why="原生组成员死亡监听不是唯一已核验的自身回调";return false;}
    const auto current=ue::Get<List>(group+offset);
    if(NativeDeathListener(group,current))return true;
    if(current.num || current.data || current.max){*why="原生组成员死亡监听出现未知绑定，拒绝阶段准入";return false;}
    if(!ue::Write(group+offset,&original,sizeof original) || !NativeDeathListener(group,ue::Get<List>(group+offset))){*why="原生组成员死亡监听恢复读回失败";return false;}
    pbz::log::Info("影塔重放组事件：保留原生组自身死亡监听 group={} fn=OnMobGroupMemberDeadEvent（真实死亡驱动原生清组检查，未伪造死亡或清组）",ue::ObjName(group));
    return true;
}
inline std::string OwnerName(uintptr_t value) {
    std::vector<std::string> fields;
    for(int depth=0;value && depth<8;++depth){
        auto* ptr=reinterpret_cast<UObject*>(value&~uintptr_t{1});
        if(ue::Live(ptr)){
            auto name=ue::ObjName(ptr);
            for(auto i=fields.rbegin();i!=fields.rend();++i)name+="::"+*i;
            return name;
        }
        auto* cls=ue::Get<UObject*>(ptr+ue::Layout::FieldClass);
        const auto kind=cls?towerpreload::runtime::FieldName(cls+ue::Layout::FieldClassName):std::string();
        if(!kind.ends_with("Property"))return {};
        fields.push_back(towerpreload::runtime::FieldName(ptr+ue::Layout::FieldName));
        value=ue::Get<uintptr_t>(ptr+0x18);
    }
    return {};
}
inline bool Reference(const towerpreload::contract::Ref& ref,uint64_t value) {
    if(ref.kind!=towerpreload::contract::Kind::Field)return towerpreload::runtime::Reference(ref,value);
    auto* field=reinterpret_cast<UObject*>(value);if(!field)return false;
    auto* cls=ue::Get<UObject*>(field+ue::Layout::FieldClass);
    return cls && towerpreload::runtime::FieldName(cls+ue::Layout::FieldClassName)==ref.cls &&
        towerpreload::runtime::FieldName(field+ue::Layout::FieldName)==ref.name && OwnerName(ue::Get<uintptr_t>(field+0x18))==ref.owner;
}
inline bool Same(const Binding& b) {
    return ue::Live(b.group) && ue::ClassName(b.group)=="MobGroup" && ue::ObjName(b.group)==b.groupName &&
        ue::Live(b.script) && ue::ClassOf(b.script)==b.cls && ue::ObjName(b.script)==b.scriptName;
}
inline bool Profile(UObject* cls,std::string* why) {
    bool cached=cls && cls==checkedClass && ue::Live(cls);
    for(int i=0;i<3 && cached;++i)cached=functions[i] && ue::Live(functions[i]) && ue::Get<UObject*>(functions[i]+ue::Layout::ObjOuter)==cls && ue::ObjName(functions[i])==towerpreload::contract::groupRules[i].function;
    if(cached)return true;
    checkedClass=nullptr;functions={};
    for(int i=0;i<3;++i){
        const auto& rule=towerpreload::contract::groupRules[i];
        auto* fn=cls?pe::FindFunction(cls,rule.function):nullptr;
        if(!fn || !ue::Live(fn) || ue::Get<UObject*>(fn+ue::Layout::ObjOuter)!=cls || ue::PropOffset(fn,"ReturnValue")>=0){*why="阶段组脚本函数身份不匹配";return false;}
        const auto script=ue::Get<List>(fn+0xb0);
        if(!script.data || script.num!=rule.scriptSize || script.max<script.num || script.max>65536){*why="阶段组脚本长度不匹配";return false;}
        for(int k=0;k<rule.segmentN;++k){
            const auto& segment=rule.segments[k];std::vector<uint8_t> bytes(segment.bytes.size());
            if(segment.begin<0 || size_t(segment.begin)+bytes.size()>size_t(script.num) ||
               !ue::Read(static_cast<uint8_t*>(script.data)+segment.begin,bytes.data(),bytes.size()) ||
               !towerpreload::Match(segment,bytes,Reference)){*why="阶段组脚本字节/引用不匹配："+std::string(rule.function);return false;}
        }
        functions[i]=fn;
    }
    checkedClass=cls;
    pbz::log::Info("影塔重放组事件：原生阶段通知合同已核验（3函数，绑定与GameplayEvent，无发奖/传送/存档）");
    return true;
}
inline void Disarm() {
    const List empty{};
    for(auto& [_,b]:bindings)if(Same(b)){
        ue::Write(b.group+b.offset,&empty,sizeof empty);
        if(b.deadOffset>=0)ue::Write(b.group+b.deadOffset,&empty,sizeof empty);
    }
    bindings.clear();
}
inline bool Reject(UObject* group,std::string* why,std::string message) {
    if(auto it=bindings.find(group);it!=bindings.end() && Same(it->second)){
        const List empty{};ue::Write(group+it->second.offset,&empty,sizeof empty);
        if(it->second.deadOffset>=0)ue::Write(group+it->second.deadOffset,&empty,sizeof empty);
    }
    *why=std::move(message);return false;
}
inline bool Bind(const tower::replay::Contract& c,int phase,UObject* sp,UObject* group,int scriptsOffset,int clearedOffset,int deadOffset,const List& originalDead,std::string* why) {
    if(phase<0 || phase>=c.phaseN || c.clearTags[phase].empty())return true;
    if(!ue::Live(sp) || ue::ObjName(sp)!=c.spawners[phase] || !ue::Live(group) || ue::ClassName(group)!="MobGroup" || scriptsOffset<0 || clearedOffset<0){*why="原生阶段组身份/字段未就绪";return false;}
    const auto scripts=ue::Get<List>(group+scriptsOffset);
    if(!scripts.data || scripts.num<=0 || scripts.num>32 || scripts.max<scripts.num || scripts.max>128)return Reject(group,why,"原生阶段组脚本表异常");
    const std::string text(c.clearTags[phase]);auto& tag=tagNames[text];if(!tag)tag=towerwidgets::Name(text.c_str());
    if(!tag)return Reject(group,why,"原生阶段标签FName未就绪");
    UObject* selected=nullptr;
    for(int i=0;i<scripts.num;++i){
        auto* sc=ue::Get<UObject*>(static_cast<uint8_t*>(scripts.data)+i*8);
        if(!sc || !ue::Live(sc) || ue::ClassName(sc)!="MobGroupScript_ClearGroup_C")continue;
        auto* cls=ue::ClassOf(sc);const int tags=ue::PropOffset(cls,"EventTag"),owner=ue::PropOffset(cls,"MobGroup");
        if(tags<0 || owner<0 || ue::Get<UObject*>(sc+owner)!=group)continue;
        const auto values=ue::Get<List>(sc+tags);
        // A mixed tag container would also authorize unrelated progression.
        if(!values.data || values.num!=1 || values.max<1 || values.max>128 || ue::Get<uint64_t>(values.data)!=tag)continue;
        if(selected)return Reject(group,why,"原生阶段通知脚本不唯一");
        selected=sc;
    }
    if(!selected)return Reject(group,why,"未找到唯一匹配阶段标签的原生ClearGroup脚本："+text);
    auto* cls=ue::ClassOf(selected);if(!Profile(cls,why))return Reject(group,why,*why);
    // 收组时存下的原始监听，或组当前实际的监听：任一份是唯一、已核验的组自身回调即可作为恢复来源。
    // 18039d2 第 8 层白赤练：场地就在出发地图，会话开始收组时组还没激活，存下的是空表；组激活后原生自己绑回了监听，
    // 只认存档那份会在开层前误拒。两份都不合格（含未知外部绑定）仍拒绝，标准不放宽
    const List currentDead=deadOffset>=0?ue::Get<List>(group+deadOffset):List{};
    const List* deadSource=NativeDeathListener(group,originalDead)?&originalDead:NativeDeathListener(group,currentDead)?&currentDead:nullptr;
    if(!deadSource)return Reject(group,why,"原生组成员死亡监听不是唯一已核验的自身回调（收组时与当前都不是）");
    const List verifiedDead=*deadSource;
    if(auto it=bindings.find(group);it!=bindings.end() && Same(it->second) && it->second.script==selected && ue::Get<List>(group+clearedOffset)==it->second.native){
        if(!KeepNativeDeath(group,deadOffset,verifiedDead,why))return Reject(group,why,*why);
        return true;
    }
    const List empty{};
    if(!ue::Write(group+clearedOffset,&empty,sizeof empty)){*why="阶段组委托隔离写入失败";return false;}
    // Audited native OnGroupActivated only adds its own native cleared callback.
    // Let UE allocate/bind the delegate; do not synthesize FScriptDelegate layout.
    uint8_t args[16]{};pe::Call(selected,functions[0],args);
    const auto native=ue::Get<List>(group+clearedOffset);
    if(!native.data || native.num!=1 || native.max<native.num || native.max>128){ue::Write(group+clearedOffset,&empty,sizeof empty);*why="原生阶段通知重绑读回不为1";return false;}
    if(!KeepNativeDeath(group,deadOffset,verifiedDead,why)){ue::Write(group+clearedOffset,&empty,sizeof empty);return Reject(group,why,*why);}
    bindings[group]={group,selected,cls,ue::ObjName(group),ue::ObjName(selected),clearedOffset,native,&c,phase,sp,scriptsOffset,deadOffset,verifiedDead};
    pbz::log::Info("影塔重放组事件：保留原生阶段通知 contract={} phase={} spawner={} group={} script={} tag={}（仅原生重绑，未执行清组或发奖）",c.id,phase+1,ue::ObjName(sp),ue::ObjName(group),ue::ObjName(selected),c.clearTags[phase]);
    return true;
}
inline bool Refresh(std::string* why) {
    // A late native group activation can add listeners after entry. Recheck
    // the few registered groups each frame, without scanning the object table.
    std::vector<Binding> active;for(auto& [_,b]:bindings)active.push_back(b);
    for(auto& b:active){
        if(!Same(b)){bindings.erase(b.group);continue;}
        if(!Bind(*b.contract,b.phase,b.spawner,b.group,b.scriptsOffset,b.offset,b.deadOffset,b.originalDead,why))return false;
    }
    return true;
}
} // namespace towergroup
