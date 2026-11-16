// Actual relocated-bytecode validator and entry router. No game or injection.
#include <windows.h>
#include <array>
#include <cstdio>
#include <map>
#include <memory>
#define PBZ_MODULE
#include "../src/tower_replay_group.h"
namespace fixture {
using pbz::ue::UObject;
struct Object {std::array<uint8_t,1024> bytes{};std::string name,cls;};
std::vector<std::unique_ptr<Object>> objects;
std::map<UObject*,Object*> identities;
std::map<std::string,UObject*> unique;
std::map<uint32_t,std::string> names;
std::map<std::string,uint32_t> indices;
std::array<UObject*,3> functions{};
std::array<std::vector<uint8_t>,3> scripts;
UObject* base=nullptr;UObject* groupClass=nullptr;UObject* nativeDeathFn=nullptr;
int bindCalls=0;bool bindCommits=true;std::array<uint8_t,32> nativeDelegate{};
constexpr int scriptsOff=0x1b0, clearedOff=0x98, deadOff=0x68, tagsOff=0x80, ownerOff=0x60;
uint32_t Name(std::string_view s){auto [i,fresh]=indices.try_emplace(std::string(s),static_cast<uint32_t>(indices.size()+1));if(fresh)names[i->second]=s;return i->second;}
UObject* Make(std::string_view name,std::string_view cls){
 const auto key=std::string(cls)+":"+std::string(name);
 if(auto i=unique.find(key);i!=unique.end())return i->second;
 auto object=std::make_unique<Object>();object->name=name;object->cls=cls;auto* ptr=object->bytes.data();identities[ptr]=object.get();unique[key]=ptr;const int32_t index=static_cast<int32_t>(objects.size());std::memcpy(ptr+12,&index,4);objects.push_back(std::move(object));return ptr;
}
template<class T>void Put(UObject* p,int off,T value){std::memcpy(p+off,&value,sizeof value);}
uint64_t Relocate(const towerpreload::contract::Ref& ref){
 using towerpreload::contract::Kind;
 if(ref.kind==Kind::Name)return Name(ref.name);
 if(ref.kind==Kind::Object){auto* p=Make(ref.name,ref.cls);auto* owner=Make(ref.owner,"Class");Put(p,0x20,owner);return reinterpret_cast<uint64_t>(p);}
 auto* field=Make(std::string(ref.owner)+"::"+std::string(ref.name),"fixture-field");auto* fieldClass=Make(ref.cls,"fixture-field-class");Put(fieldClass,0x30,Name(ref.cls));Put(field,8,fieldClass);
 // Captured live oracle: K2Node_CustomEvent_Loaded_1 is comparison name
 // K2Node_CustomEvent_Loaded plus Number=2, not a literal suffix in Name().
 std::string comparison(ref.name);uint32_t number=0;const auto suffix=comparison.find_last_of('_');
 if(suffix!=std::string::npos && suffix+1<comparison.size() && std::all_of(comparison.begin()+suffix+1,comparison.end(),[](char c){return c>='0'&&c<='9';})){number=static_cast<uint32_t>(std::stoul(comparison.substr(suffix+1))+1);comparison.resize(suffix);}
 Put(field,0x20,Name(comparison));Put(field,0x24,number);
 const std::string ownerPath(ref.owner);const auto split=ownerPath.find("::");
 auto* owner=Make(ownerPath.substr(0,split),"fixture-owner");
 if(split!=std::string::npos){
  auto* nested=Make(ownerPath,"fixture-field-owner");auto* kind=Make("ArrayProperty","fixture-field-class");Put(kind,0x30,Name("ArrayProperty"));Put(nested,8,kind);Put(nested,0x20,Name(ownerPath.substr(split+2)));Put(nested,0x18,reinterpret_cast<uintptr_t>(owner)|1);owner=nested;
 }
 Put(field,0x18,reinterpret_cast<uintptr_t>(owner)|(split==std::string::npos?1:0));
 return reinterpret_cast<uint64_t>(field);
}
void Build(){
 using namespace towerpreload;
 base=Make("MobGroupScript_ClearGroup_C","Class");
 groupClass=Make("MobGroup","Class");nativeDeathFn=Make("OnMobGroupMemberDeadEvent","Function");Put(nativeDeathFn,0x20,groupClass);
 for(size_t i=0;i<functions.size();++i){functions[i]=Make(contract::groupRules[i].function,"Function");Put(functions[i],0x20,base);}
 for(size_t i=0;i<functions.size();++i){
  const auto& rule=contract::groupRules[i];scripts[i].assign(rule.scriptSize,0x53);
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
bool Live(UObject* o){return fixture::identities.count(o)>0 && !fixture::identities.at(o)->cls.starts_with("fixture-field");}
std::string ObjName(UObject* o){return Live(o)?fixture::identities.at(o)->name:"";}
std::string ClassName(UObject* o){return Live(o)?fixture::identities.at(o)->cls:"";}
std::string Name(uint32_t i){auto it=fixture::names.find(i);return it==fixture::names.end()?"None":it->second;}
UObject* ClassOf(UObject* obj){return !Live(obj)?nullptr:ClassName(obj)=="MobGroupScript_ClearGroup_C"?fixture::base:ClassName(obj)=="MobGroup"?fixture::groupClass:nullptr;}
UObject* Obj(int index){return index>=0 && size_t(index)<fixture::objects.size()?fixture::objects[index]->bytes.data():nullptr;}
bool Write(void* p,const void* src,size_t n){if(!p)return false;std::memcpy(p,src,n);return true;}
int PropOffset(UObject* obj,std::string_view name){if(obj==fixture::nativeDeathFn)return name=="AISpawner"?0:-1;if(obj!=fixture::base)return -1;return name=="MobGroup"?fixture::ownerOff:name=="EventTag"?fixture::tagsOff:-1;}
}
namespace pbz::pe {ue::UObject* FindFunction(ue::UObject* cls,std::string_view name){if(cls==fixture::groupClass && name=="OnMobGroupMemberDeadEvent")return fixture::nativeDeathFn;if(cls!=fixture::base)return nullptr;for(size_t i=0;i<fixture::functions.size();++i)if(towerpreload::contract::groupRules[i].function==name)return fixture::functions[i];return nullptr;}
void Call(ue::UObject* self,ue::UObject* fn,void*){
 ++fixture::bindCalls;
 if(fn!=fixture::functions[0])throw std::runtime_error("Forbidden clear/event call from binding helper");
 if(!fixture::bindCommits)return;
 auto* group=ue::Get<ue::UObject*>(self+fixture::ownerOff);
 fixture::Put(group,fixture::clearedOff,towergroup::List{fixture::nativeDelegate.data(),1,1});
}}
namespace towerwidgets {uint64_t Name(const char* value){return fixture::Name(value);}}
namespace pbz::log {void Write(const std::string&) {}}

int main(){
 using namespace towergroup;int checks=0,failed=0;
 auto check=[&](bool ok,const char* why){++checks;if(!ok){++failed;std::printf("FAIL %s\n",why);}};
 fixture::Build();std::string why;
 bool profile=Profile(fixture::base,&why);if(!profile)std::printf("PROFILE_WHY %s / %s\n",why.c_str(),towerpreload::runtime::referenceWhy.c_str());check(profile,"current cooked native group binding/event graph validates with semantic references");
 auto* c=tower::replay::Find("changhong");
 auto* sp=fixture::Make(c->spawners[0],"BP_AISpawner_C");auto* group=fixture::Make("native_group","MobGroup");auto* sc=fixture::Make("native_clear_script","MobGroupScript_ClearGroup_C");
 struct Delegate{int32_t index,serial;uint64_t function;};
 Delegate deadEntries[2]={{pbz::ue::Get<int32_t>(group+12),7,fixture::Name("OnMobGroupMemberDeadEvent")},{pbz::ue::Get<int32_t>(sp+12),8,fixture::Name("UnrelatedReward")}};
 List originalDead{deadEntries,1,2};
 uint64_t tags[2]={fixture::Name(c->clearTags[0]),fixture::Name("unrelated.quest")};pbz::ue::UObject* list[2]={sc,sc};
 fixture::Put(sc,fixture::ownerOff,group);fixture::Put(sc,fixture::tagsOff,List{tags,1,1});fixture::Put(group,fixture::scriptsOff,List{list,1,1});
 check(NativeDeathListener(group,originalDead),"captured native self-death delegate identity is accepted");
 check(!NativeDeathListener(group,List{deadEntries,2,2}),"mixed native/reward death listeners are rejected rather than broadly restored");
 auto badDead=deadEntries[0];badDead.index=pbz::ue::Get<int32_t>(sp+12);
 check(!NativeDeathListener(group,List{&badDead,1,1}),"another object's native-looking listener cannot receive rights");
 badDead=deadEntries[0];badDead.function|=uint64_t{1}<<32;
 check(!NativeDeathListener(group,List{&badDead,1,1}),"numbered lookalike native event is rejected");
 badDead=deadEntries[0];badDead.serial=0;
 check(!NativeDeathListener(group,List{&badDead,1,1}),"invalid original weak object identity is rejected");
 check(Bind(*c,0,sp,group,fixture::scriptsOff,fixture::clearedOff,fixture::deadOff,originalDead,&why),"muted native group regains exactly its approved native cleared listener");
 check(pbz::ue::Get<List>(group+fixture::deadOff)==originalDead,"original native death input is preserved as well as completion output, without fabricating a delegate");
 check(fixture::bindCalls==1 && pbz::ue::Get<List>(group+fixture::clearedOff).num==1,"UE native binder executes once and is read back; helper never executes cleared callback");
 check(Bind(*c,0,sp,group,fixture::scriptsOff,fixture::clearedOff,fixture::deadOff,originalDead,&why) && fixture::bindCalls==1,"stable native binding is not recreated every tick");
 fixture::Put(group,fixture::deadOff,List{});
 check(Refresh(&why) && pbz::ue::Get<List>(group+fixture::deadOff)==originalDead && fixture::bindCalls==1,"a muted internal death input is repaired without replaying any death or cleared event");
 fixture::Put(group,fixture::clearedOff,List{fixture::nativeDelegate.data(),2,2});
 check(Refresh(&why) && fixture::bindCalls==2,"later foreign bindings are muted before rebinding the one approved listener");
 fixture::Put(sc,fixture::tagsOff,List{tags+1,1,1});check(!Refresh(&why) && pbz::ue::Get<List>(group+fixture::clearedOff).num==0,"a cached binding does not hide a changed event-tag payload and rights are revoked");fixture::Put(sc,fixture::tagsOff,List{tags,1,1});
 Disarm();check(pbz::ue::Get<List>(group+fixture::clearedOff).num==0,"changing contract revokes the group exception; session restoration owns originals");
 check(pbz::ue::Get<List>(group+fixture::deadOff).num==0,"changing contract also revokes its internal death input before actor cleanup");
 fixture::Put(sc,fixture::tagsOff,List{tags,2,2});
 check(!Bind(*c,0,sp,group,fixture::scriptsOff,fixture::clearedOff,fixture::deadOff,originalDead,&why) && fixture::bindCalls==2,"mixed tag payload cannot advance an unrelated quest");
 fixture::Put(sc,fixture::tagsOff,List{tags+1,1,1});
 check(!Bind(*c,0,sp,group,fixture::scriptsOff,fixture::clearedOff,fixture::deadOff,originalDead,&why),"foreign phase tag is rejected");
 fixture::Put(sc,fixture::tagsOff,List{tags,1,1});fixture::Put(group,fixture::scriptsOff,List{list,2,2});
 check(!Bind(*c,0,sp,group,fixture::scriptsOff,fixture::clearedOff,fixture::deadOff,originalDead,&why),"two candidate scripts fail the unique-native-notification gate");
 fixture::Put(group,fixture::scriptsOff,List{list,1,1});fixture::Put(sc,fixture::ownerOff,sp);
 check(!Bind(*c,0,sp,group,fixture::scriptsOff,fixture::clearedOff,fixture::deadOff,originalDead,&why),"script from another group cannot receive phase rights");fixture::Put(sc,fixture::ownerOff,group);
 check(!Bind(*c,0,group,group,fixture::scriptsOff,fixture::clearedOff,fixture::deadOff,originalDead,&why),"an unrelated spawner cannot borrow contract rights");
 fixture::bindCommits=false;
 check(!Bind(*c,0,sp,group,fixture::scriptsOff,fixture::clearedOff,fixture::deadOff,originalDead,&why) && pbz::ue::Get<List>(group+fixture::clearedOff).num==0,"a native bind without expected readback fails closed");fixture::bindCommits=true;
 check(pbz::ue::Get<List>(group+fixture::deadOff).num==0,"failed binder does not leave partial death-input rights behind");
 fixture::Put(group,fixture::deadOff,List{deadEntries+1,1,1});
 check(!Bind(*c,0,sp,group,fixture::scriptsOff,fixture::clearedOff,fixture::deadOff,originalDead,&why) && pbz::ue::Get<List>(group+fixture::clearedOff).num==0,"unknown late death bindings reject admission instead of allowing reward callbacks");
 fixture::Put(group,fixture::deadOff,List{});
 // 18039d2 第 8 层白赤练：收组时组尚未激活（存下空表），之后原生自己绑回了自身死亡监听
 check(!Bind(*c,0,sp,group,fixture::scriptsOff,fixture::clearedOff,fixture::deadOff,List{},&why),"no verified death listener in the snapshot nor live: admission refused");
 fixture::Put(group,fixture::deadOff,List{deadEntries,2,2});
 check(!Bind(*c,0,sp,group,fixture::scriptsOff,fixture::clearedOff,fixture::deadOff,List{},&why),"empty snapshot plus a mixed live list is still refused");
 fixture::Put(group,fixture::deadOff,originalDead);
 check(Bind(*c,0,sp,group,fixture::scriptsOff,fixture::clearedOff,fixture::deadOff,List{},&why) && pbz::ue::Get<List>(group+fixture::deadOff)==originalDead,
       "empty snapshot but a verified live self-listener (group activated after muting) is accepted and kept");
 fixture::Put(group,fixture::deadOff,List{});
 check(Refresh(&why) && pbz::ue::Get<List>(group+fixture::deadOff)==originalDead,"the live-verified listener becomes the restoration source for later refreshes");
 Disarm();fixture::Put(group,fixture::deadOff,List{});
 auto old=fixture::scripts[0][10];fixture::scripts[0][10]^=1;checkedClass=nullptr;
 check(!Profile(fixture::base,&why),"changing the native binder entrypoint rejects the whole profile");fixture::scripts[0][10]=old;checkedClass=nullptr;
 check(Profile(fixture::base,&why),"clean profile can be verified again");
 for(auto id:{"hook","chilian","changhong"}){
  auto* ct=tower::replay::Find(id);check(ct && !ct->clearTags[0].empty() && !ct->clearTags[1].empty(),"all statically identified group-milestone contracts are configured");
 }
 check(tower::replay::Find("chilian")->clearTags[0]=="fjzl.击杀.弄蛇P2","native Chilian reversed P2 tag for phase one is preserved");
 tower::replay::Progress progress;progress.Begin(2);
 check(progress.CanAdopt(0) && !progress.CanAdopt(1),"phase order begins with phase one");progress.Seen(0,100);progress.Dead(0,110);
 check(!progress.CanAdopt(0) && progress.CanAdopt(1),"repeated old phase is rejected while waiting for next phase");progress.Seen(0,180);
 check(progress.Between() && progress.betweenSince==110 && progress.TimedOut(201,90),"respawn cannot reset bounded missing-next-phase timeout");progress.Seen(1,202);progress.Dead(1,210);
 check(progress.Settle() && !progress.CanAdopt(1),"final phase settles once and cannot be adopted again");
 std::printf("TOWER_REPLAY_GROUP_%s %d checks (actual native binder/profile, no game)\n",failed?"FAILED":"OK",checks);return failed?1:0;
}
