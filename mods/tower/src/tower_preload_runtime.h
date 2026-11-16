#pragma once
#include "tower_preload.h"
#include "../../../src/core/sdk/pbz_module.h"
#include "../../../src/core/host/log.h"

namespace towerpreload {
namespace runtime {
using pbz::ue::UObject;
inline UObject* cls=nullptr;
inline std::array<UObject*,std::size(contract::rules)> functions{};
inline bool checked=false,valid=false;
inline int entryOffset=-1;
inline unsigned long long retryAt=0;
inline std::string referenceWhy,lastWhy,lastLoggedWhy;
inline std::string FieldName(const void* address) {
    namespace ue=pbz::ue;
    const auto* p=static_cast<const uint8_t*>(address);
    auto name=ue::Name(ue::Get<uint32_t>(p));
    const auto number=ue::Get<uint32_t>(p+4);
    if(number)name+="_"+std::to_string(number-1);
    return name;
}
inline bool Reject(std::string why) {
    lastWhy=std::move(why);
    if(lastWhy!=lastLoggedWhy){lastLoggedWhy=lastWhy;pbz::log::Info("影塔剧情预加载：合同未通过，保持冻结（{}）",lastWhy);}
    return false;
}
inline bool Reference(const contract::Ref& r,uint64_t value) {
    namespace ue=pbz::ue;
    if(r.kind==contract::Kind::Name)return value && !(value>>32) && ue::Name(static_cast<uint32_t>(value))==r.name;
    auto* object=reinterpret_cast<UObject*>(value);
    if(r.kind==contract::Kind::Object) {
        if(!ue::Live(object) || ue::ObjName(object)!=r.name || ue::ClassName(object)!=r.cls)return false;
        auto* owner=ue::Get<UObject*>(object+ue::Layout::ObjOuter);
        return owner && ue::Live(owner) && ue::ObjName(owner)==r.owner;
    }
    if(!object)return false;
    auto* fieldClass=ue::Get<UObject*>(object+ue::Layout::FieldClass);
    const auto owner=ue::Get<uintptr_t>(object+0x18); // pinned FFieldVariant, separately validated as a live UObject owner
    auto* ownerObject=reinterpret_cast<UObject*>(owner&~uintptr_t{1});
    const auto type=fieldClass?FieldName(fieldClass+ue::Layout::FieldClassName):std::string();
    const auto name=FieldName(object+ue::Layout::FieldName);
    if(type!=r.cls || name!=r.name){referenceWhy="字段身份："+std::string(r.name)+" / 实际 "+name+" / 类型 "+type;return false;}
    return ownerObject && ue::Live(ownerObject) && ue::ObjName(ownerObject)==r.owner;
}
inline bool Live() {
    namespace ue=pbz::ue;
    if(!cls || !ue::Live(cls))return false;
    for(size_t i=0;i<functions.size();++i)
        if(!functions[i] || !ue::Live(functions[i]) || ue::Get<UObject*>(functions[i]+ue::Layout::ObjOuter)!=cls || ue::ObjName(functions[i])!=contract::rules[i].function)return false;
    return true;
}
inline void Reset(){cls=nullptr;functions={};checked=valid=false;entryOffset=-1;retryAt=0;referenceWhy.clear();lastWhy.clear();}
inline bool Ready(UObject* base) {
    namespace ue=pbz::ue;namespace pe=pbz::pe;
    if(!base || !ue::Live(base))return false;
    if(base==cls && checked && ((valid && Live()) || (!valid && GetTickCount64()<retryAt)))return valid;
    Reset();cls=base;checked=true;retryAt=GetTickCount64()+2000;
    for(size_t i=0;i<functions.size();++i) {
        const auto& rule=contract::rules[i];auto* fn=pe::FindFunction(base,rule.function);functions[i]=fn;
        if(!fn || !ue::Live(fn) || ue::Get<UObject*>(fn+ue::Layout::ObjOuter)!=base || ue::PropOffset(fn,"ReturnValue")>=0)return Reject("函数身份/参数："+std::string(rule.function));
        // UStruct.Script +B0 was independently read back on Build26367565.
        // Exact length, wrapper targets, all safe CFG bytes and all relocation
        // identities must match; this offset alone never authorizes execution.
        struct Script{uint8_t* data;int32_t num,max;};
        const auto script=ue::Get<Script>(fn+0xb0);
        if(!script.data || script.num!=rule.scriptSize || script.max<script.num || script.max>65536)return Reject("脚本布局/长度："+std::string(rule.function));
        for(int k=0;k<rule.segmentN;++k) {
            const auto& segment=rule.segments[k];
            if(segment.begin<0 || size_t(segment.begin)+segment.bytes.size()>size_t(script.num))return Reject("脚本片段边界："+std::string(rule.function));
            std::vector<uint8_t> bytes(segment.bytes.size());
            referenceWhy.clear();
            if(!ue::Read(script.data+segment.begin,bytes.data(),bytes.size()) || !Match(segment,bytes,Reference))return Reject(std::string(rule.function)+" 片段 "+std::to_string(segment.begin)+" "+(referenceWhy.empty()?"字节/引用不同":referenceWhy));
        }
    }
    entryOffset=ue::PropOffset(functions.back(),"EntryPoint");
    if(entryOffset<0 || entryOffset+4>16 || !Live())return Reject("图入口参数或对象身份未就绪");
    valid=true;
    pbz::log::Info("影塔剧情预加载：地图切换纯预加载合同已核验（4函数/8片段，仅加载与完成通知，不放行播放或Flow输出）");
    return true;
}
}
inline bool Allows(pbz::ue::UObject* base,pbz::ue::UObject* fn,void* params) {
    if(!runtime::Ready(base))return false;
    for(size_t i=0;i+1<runtime::functions.size();++i)if(fn==runtime::functions[i])return true;
    if(fn!=runtime::functions.back() || !params)return false;
    return GraphEntry(pbz::ue::Get<int32_t>(static_cast<uint8_t*>(params)+runtime::entryOffset));
}
inline void Reset(){runtime::Reset();}
}
