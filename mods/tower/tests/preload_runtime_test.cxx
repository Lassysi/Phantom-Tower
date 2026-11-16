// Actual relocated-bytecode validator and entry router. No game or injection.
#include <windows.h>
#include <array>
#include <cstdio>
#include <map>
#include <memory>
#define PBZ_MODULE
#include "../src/tower_preload_runtime.h"
namespace fixture {
using pbz::ue::UObject;
struct Object {std::array<uint8_t,256> bytes{};std::string name,cls;};
std::vector<std::unique_ptr<Object>> objects;
std::map<UObject*,Object*> identities;
std::map<std::string,UObject*> unique;
std::map<uint32_t,std::string> names;
std::map<std::string,uint32_t> indices;
std::array<UObject*,4> functions{};
std::array<std::vector<uint8_t>,4> scripts;
UObject* base=nullptr;
uint32_t Name(std::string_view s){auto [i,fresh]=indices.try_emplace(std::string(s),static_cast<uint32_t>(indices.size()+1));if(fresh)names[i->second]=s;return i->second;}
UObject* Make(std::string_view name,std::string_view cls){
 const auto key=std::string(cls)+":"+std::string(name);
 if(auto i=unique.find(key);i!=unique.end())return i->second;
 auto object=std::make_unique<Object>();object->name=name;object->cls=cls;auto* ptr=object->bytes.data();identities[ptr]=object.get();unique[key]=ptr;objects.push_back(std::move(object));return ptr;
}
template<class T>void Put(UObject* p,int off,T value){std::memcpy(p+off,&value,sizeof value);}
uint64_t Relocate(const towerpreload::contract::Ref& ref){
 using towerpreload::contract::Kind;
 if(ref.kind==Kind::Name)return Name(ref.name);
 if(ref.kind==Kind::Object){auto* p=Make(ref.name,ref.cls);auto* owner=Make(ref.owner,"Class");Put(p,0x20,owner);return reinterpret_cast<uint64_t>(p);}
 auto* field=Make(ref.name,"fixture-field");auto* fieldClass=Make(ref.cls,"fixture-field-class");Put(fieldClass,0x30,Name(ref.cls));Put(field,8,fieldClass);
 // Captured live oracle: K2Node_CustomEvent_Loaded_1 is comparison name
 // K2Node_CustomEvent_Loaded plus Number=2, not a literal suffix in Name().
 std::string comparison(ref.name);uint32_t number=0;const auto suffix=comparison.find_last_of('_');
 if(suffix!=std::string::npos && suffix+1<comparison.size() && std::all_of(comparison.begin()+suffix+1,comparison.end(),[](char c){return c>='0'&&c<='9';})){number=static_cast<uint32_t>(std::stoul(comparison.substr(suffix+1))+1);comparison.resize(suffix);}
 Put(field,0x20,Name(comparison));Put(field,0x24,number);
 auto* owner=Make(ref.owner,ref.owner=="PlayLevelSequence_C"?"Class":"Function");Put(field,0x18,reinterpret_cast<uintptr_t>(owner)|1);
 return reinterpret_cast<uint64_t>(field);
}
void Build(){
 using namespace towerpreload;
 base=Make("PlayLevelSequence_C","Class");
 for(size_t i=0;i<functions.size();++i){functions[i]=Make(contract::rules[i].function,"Function");Put(functions[i],0x20,base);}
 for(size_t i=0;i<functions.size();++i){
  const auto& rule=contract::rules[i];scripts[i].assign(rule.scriptSize,0x53);
  for(int k=0;k<rule.segmentN;++k){const auto& s=rule.segments[k];std::memcpy(scripts[i].data()+s.begin,s.bytes.data(),s.bytes.size());for(int r=0;r<s.refN;++r){auto value=Relocate(s.refs[r]);std::memcpy(scripts[i].data()+s.begin+s.refs[r].at,&value,8);}}
  struct Arr{uint8_t* data;int32_t num,max;};Put(functions[i],0xb0,Arr{scripts[i].data(),rule.scriptSize,rule.scriptSize});
 }
 // All script functions belong to the exact class, including the graph object
 // referenced by wrappers and local-variable field owners.
 for(auto* f:functions)Put(f,0x20,base);
}
}
namespace pbz::ue {
bool Read(const void* p,void* dst,size_t n){if(!p)return false;std::memcpy(dst,p,n);return true;}
bool Live(UObject* o){return fixture::identities.count(o)>0;}
std::string ObjName(UObject* o){return Live(o)?fixture::identities.at(o)->name:"";}
std::string ClassName(UObject* o){return Live(o)?fixture::identities.at(o)->cls:"";}
std::string Name(uint32_t i){auto it=fixture::names.find(i);return it==fixture::names.end()?"None":it->second;}
int PropOffset(UObject* f,std::string_view name){return f==fixture::functions.back() && name=="EntryPoint"?0:-1;}
}
namespace pbz::pe {ue::UObject* FindFunction(ue::UObject* cls,std::string_view name){if(cls!=fixture::base)return nullptr;for(size_t i=0;i<fixture::functions.size();++i)if(towerpreload::contract::rules[i].function==name)return fixture::functions[i];return nullptr;}}
namespace pbz::log {void Write(const std::string&) {}}
int main(){
 using namespace towerpreload;
 int checks=0,failed=0;auto check=[&](bool value,const char* why){++checks;if(!value){++failed;std::printf("FAIL %s\n",why);}};
 fixture::Build();check(runtime::Ready(fixture::base),"audited relocated map-switch preload CFG validates");
 auto* numberedField=fixture::unique.at("fixture-field:K2Node_CustomEvent_Loaded_1");
 check(pbz::ue::Name(pbz::ue::Get<uint32_t>(numberedField+0x20))=="K2Node_CustomEvent_Loaded" && runtime::FieldName(numberedField+0x20)=="K2Node_CustomEvent_Loaded_1","captured numbered FField keeps ComparisonIndex and Number semantics distinct");
 fixture::Put<uint32_t>(numberedField,0x24,3);Reset();check(!runtime::Ready(fixture::base),"different numbered FField cannot alias the expected preload local");fixture::Put<uint32_t>(numberedField,0x24,2);Reset();check(runtime::Ready(fixture::base),"restoring the full numbered field identity revalidates before entry");
 for(size_t i=0;i+1<fixture::functions.size();++i)check(Allows(fixture::base,fixture::functions[i],nullptr),"verified async/preload completion wrapper survives after travel is no longer busy");
 for(int entry:contract::graphEntries)check(Allows(fixture::base,fixture::functions.back(),&entry),"only audited loader graph entries survive direct and latent re-entry");
 // Live wrapper oracle: ordinary Play input targets 10804, never a preload entry.
 for(int entry:{10804,12585,-1,0,4196,4994,13251})check(!Allows(fixture::base,fixture::functions.back(),&entry),"play, mid-game scene continuation, unknown and adjacent graph entries stay blocked");
 check(!Allows(fixture::base,fixture::functions.back(),nullptr),"graph execution without a validated input buffer is never admitted");
 auto* foreign=fixture::Make("OnLoaded_Other","Function");check(!Allows(fixture::base,foreign,nullptr),"a similarly named async callback grants no execution right");
 // FScriptName comparison indices change with package load order. Preserve
 // semantic identity while relocating every callback/latent literal again.
 for(size_t i=0;i<fixture::functions.size();++i){const auto& rule=contract::rules[i];for(int k=0;k<rule.segmentN;++k){const auto& s=rule.segments[k];for(int r=0;r<s.refN;++r)if(s.refs[r].kind==contract::Kind::Name){const uint64_t renamed=fixture::Name(s.refs[r].name)+10000;fixture::names[static_cast<uint32_t>(renamed)]=s.refs[r].name;std::memcpy(fixture::scripts[i].data()+s.begin+s.refs[r].at,&renamed,8);}}}
 Reset();check(runtime::Ready(fixture::base),"every preload callback/latent FName relocates across package load orders");
 auto original=fixture::scripts.back()[12427];fixture::scripts.back()[12427]^=1;Reset();check(!runtime::Ready(fixture::base),"changing the verified preload jump target refuses the whole profile");fixture::scripts.back()[12427]=original;
 auto* property=fixture::unique.at("fixture-field:MapSwitchPreloadDone");auto oldIndex=pbz::ue::Get<uint32_t>(property+0x20);fixture::Put(property,0x20,fixture::Name("HasPlay"));Reset();check(!runtime::Ready(fixture::base),"a relocated property slot cannot alias a cinematic play flag");fixture::Put(property,0x20,oldIndex);
 auto* callback=fixture::functions[0];auto* owner=pbz::ue::Get<pbz::ue::UObject*>(callback+0x20);fixture::Put(callback,0x20,fixture::Make("DifferentSequenceClass","Class"));Reset();check(!runtime::Ready(fixture::base),"matching bytes from an unrelated class do not grant preload rights");fixture::Put(callback,0x20,owner);
 fixture::Put<int32_t>(callback,0xb8,35);Reset();check(!runtime::Ready(fixture::base),"changed Script layout/length fails closed");fixture::Put<int32_t>(callback,0xb8,36);
 Reset();check(runtime::Ready(fixture::base),"a clean separately verified profile can initialize again");
 std::printf("TOWER_PRELOAD_RUNTIME_%s %d checks (actual bytecode matcher/router, no game)\n",failed?"FAILED":"OK",checks);return failed?1:0;
}
