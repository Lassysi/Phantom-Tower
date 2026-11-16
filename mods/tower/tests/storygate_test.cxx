// Actual Blueprint entry gate and native weapon argument builder, no game.
#include <windows.h>
#include <array>
#include <cstdio>
#include <map>
static ULONGLONG virtualTime=100;
static ULONGLONG Clock() { return virtualTime; }
#define GetTickCount64 Clock
#define PBZ_MODULE
#include "../src/tower_storygate.cpp"
#undef GetTickCount64
#include "../src/tower_weapon_grant.cpp"
namespace fixture {
std::array<uint8_t,256> cls{},input{},graph{},finished{},done{},finish{},node{},sequence{},bag{},weaponFn{},flowAsset{},flowBase{},flowCls{},templateAsset{},activeFn{},pinCls{},secondNode{},stringClass{},stringCdo{},toNameFn{};
bool classReady=false,travel=false,inside=true,sandbox=true,missingPin=false,missingSoft=false,missingGrantField=false,overlap=false,missingGuid=false,flowActive=true,missingNameInput=false;
std::vector<pbz::ue::UObject*> objects;
int entryCalls=0,activeCalls=0,deactivateAt=0;
std::map<pbz::ue::UObject*,pbz::pe::Interceptor> intercepts;
int calls=0,grantDepth=0,depthAtCall=0;
std::array<uint8_t,256> lastArgs{};
void soft(uint32_t n) {std::memcpy(node.data()+64+16,&n,4);}
}
namespace pbz::ue {
bool Read(const void* p,void* dst,size_t n) {if(!p)return false;std::memcpy(dst,p,n);return true;}
bool Live(UObject* p) {return p!=nullptr;}
UObject* ClassOf(UObject* obj) {return obj==fixture::stringCdo.data()?fixture::stringClass.data():fixture::cls.data();}
bool IsA(UObject*,UObject*) {return true;}
bool IsDefault(UObject* obj){return obj==fixture::stringCdo.data();}
void ForEach(const std::function<bool(int,UObject*)>& f){f(0,fixture::stringCdo.data());}
int Num(){return static_cast<int>(fixture::objects.size());}
int ObjBatch(int from,int count,UObject** out){int n=0;for(int i=from;i<static_cast<int>(fixture::objects.size())&&n<count;++i)out[n++]=fixture::objects[i];return n;}
UObject* FindClass(std::string_view name) {if(!fixture::classReady)return nullptr;return name=="KismetStringLibrary"?fixture::stringClass.data():name=="PlayLevelSequence_C"?fixture::cls.data():name=="FlowNode"?fixture::flowBase.data():name=="FlowAsset"?fixture::flowCls.data():name=="FlowPin"?fixture::pinCls.data():nullptr;}
std::string Name(uint32_t id) {
 // Captured ABI6 behavior: UTF-16 播放/中断 are truncated, not returned as UTF-8.
 switch(id) {case 1:return std::string("\xad\x3e",2);case 2:return std::string("\x2d\xad",2);case 62:return "Seq062";case 63:return "Seq063";case 64:return "Seq064";case 47:return "Seq047";case 31:return "Seq031";case 99:return "Double";default:return "None";}
}
std::string ObjName(UObject* p) {return p==fixture::sequence.data()?"Seq064":"node";}
std::string ClassName(UObject*) {return "PlayLevelSequence_C";}
int PropOffset(UObject* obj,std::string_view name) {
 if(obj==fixture::toNameFn.data())return name=="InString"?(fixture::missingNameInput?-1:0):name=="ReturnValue"?16:-1;
 if(name=="NodeGuid")return fixture::missingGuid?-1:0x38;
 if(name=="AssetGuid")return 0x28;
 if(name=="TemplateAsset")return 0xe8;
 if(obj==fixture::activeFn.data())return name=="ReturnValue"?0:-1;
 if(name=="InputPins")return 144;
 if(name=="HasPlay")return 128;
 if(obj==fixture::weaponFn.data()) {
  if (name=="WeaponID")return 0;if(name=="bIsNormalWeapon")return 8;
  if(name=="bPushWidget")return fixture::missingGrantField?-1:fixture::overlap?4:9;
  if(name=="bIsAddByLoadData")return 10;if(name=="bAutoEquipment")return 11;return -1;
 }
 return name=="PinName"?(fixture::missingPin?-1:0):name=="LevelSequence"?(fixture::missingSoft?-1:64):name=="LoadedLevelSequence"?120:-1;
}
}
namespace pbz::pe {
ue::UObject* FindFunction(ue::UObject*,std::string_view n) {
 if(n=="Conv_StringToName")return fixture::toNameFn.data();
 if(n=="K2_ExecuteInput")return fixture::input.data();if(n=="ExecuteUbergraph_PlayLevelSequence")return fixture::graph.data();if(n=="OnLevelSequenceFinished")return fixture::finished.data();
 if(n=="OnFinished")return fixture::done.data();if(n=="FinishLevelSequence")return fixture::finish.data();
 if(n=="AddWeaponItem")return fixture::weaponFn.data();if(n=="IsActive")return fixture::activeFn.data();return nullptr;
}
std::vector<ue::UObject*> FunctionsWithPrefix(ue::UObject*,std::string_view){return {fixture::input.data(),fixture::graph.data(),fixture::finished.data(),fixture::done.data(),fixture::finish.data()};}
void Intercept(ue::UObject* f,Interceptor h) {fixture::intercepts[f]=h;}
void RemoveIntercept(ue::UObject* f) {fixture::intercepts.erase(f);}
void Call(ue::UObject* self,ue::UObject* fn,void* p) {
 if(fn==fixture::toNameFn.data()){
  struct Str{const wchar_t* data;int32_t num,max;};Str in{};std::memcpy(&in,p,sizeof in);
  uint64_t value=0;if(in.data && in.num==3 && in.max==3){const std::wstring_view s(in.data,2);value=s==L"播放"?1:s==L"中断"?2:0;}
  std::memcpy(static_cast<uint8_t*>(p)+16,&value,8);return;
 }
 if(fn==fixture::activeFn.data()){++fixture::activeCalls;static_cast<uint8_t*>(p)[0]=fixture::flowActive && !(fixture::deactivateAt && fixture::activeCalls>=fixture::deactivateAt);return;}
 if(fn==fixture::input.data()){++fixture::entryCalls;self[128]=1;return;}
 ++fixture::calls;fixture::depthAtCall=fixture::grantDepth;std::memcpy(fixture::lastArgs.data(),p,256);
}
}
namespace towervm {
static std::map<pbz::ue::UObject*,Handler> table;
bool Add(pbz::ue::UObject* f,Handler h,std::string*){table[f]=h;return true;}
void Class(std::string,Handler){}
void Remove(pbz::ue::UObject* f){table.erase(f);}
bool Ready(){return !table.empty();}
bool Faulted(){return false;}
void End(){table.clear();}
}
namespace pbz {bool TeleportBusy(){return fixture::travel;}}
namespace towertravel {bool Busy(){return fixture::travel;}}
namespace towerapi {bool InsideTower(){return fixture::inside;}}
namespace towersb {bool Active(){return fixture::sandbox;}}
namespace pbz::log {void Write(const std::string&) {}}
namespace towerisolate {GrantScope::GrantScope(){++fixture::grantDepth;} GrantScope::~GrantScope(){--fixture::grantDepth;}}

int main(){
 int checks=0,failed=0;auto check=[&](bool b,const char* why){++checks;if(!b){++failed;std::printf("FAIL %s\n",why);}};
 using namespace towerstorygate;
 std::string why;Session(true);check(!Ready(&why)&&!why.empty(),"replay cannot arm before the real Blueprint node class is present");
 fixture::classReady=true;virtualTime+=2100;fixture::missingNameInput=true;Session(true);
 check(!Ready(&why),"unverified native string-to-name parameter layout keeps replay entry closed");
 fixture::missingNameInput=false;Session(true);check(Ready(&why)&&fixture::intercepts.size()==4,"real node input and parked completion events installed after class load");
 check(pbz::ue::Name(1)!="播放" && playPinName==1 && interruptPinName==2,"real lossy Core name behavior cannot prevent resolving complete native Chinese FNames");
 auto* lion=tower::replay::Find("lion");SetReplay(lion);std::array<uint8_t,16> args{};uint32_t play=1;std::memcpy(args.data(),&play,4);
 auto invoke=[&](){return fixture::intercepts.at(fixture::input.data())(fixture::node.data(),args.data());};
 fixture::soft(62);check(!invoke(),"initial CG is admitted at Blueprint entry before the player exists");
 fixture::soft(63);check(!invoke(),"phase-transition CG is admitted by the same contract");
 check(towervm::table.at(fixture::graph.data())(fixture::node.data(),fixture::graph.data(),nullptr)==towervm::Action::Pass,"local ubergraph transition bypassing ProcessEvent still reaches a guarded allow boundary");
 check(!fixture::intercepts.at(fixture::finished.data())(fixture::node.data(),nullptr),"admitted transition can run its native finish callback");
 fixture::soft(64);check(invoke()&&fixture::calls==0,"post-kill CG stops before any camera/input/load/player call");
 check(towervm::table.at(fixture::graph.data())(fixture::node.data(),fixture::graph.data(),nullptr)==towervm::Action::Block,"local/async ubergraph bypass cannot enter camera work for post-kill scene");
 for(auto* fn:{fixture::finished.data(),fixture::done.data(),fixture::finish.data()})check(fixture::intercepts.at(fn)(fixture::node.data(),nullptr),"parked completion never pushes onward to loot/quest/save");
 uint32_t stop=2;std::memcpy(args.data(),&stop,4);check(invoke(),"a frozen never-started scene cannot advance through interrupt/completion cleanup");std::memcpy(args.data(),&play,4);
 fixture::node[0x38]=7;fixture::soft(63);
 check(towervm::table.at(fixture::graph.data())(fixture::node.data(),fixture::graph.data(),nullptr)==towervm::Action::Pass,"a reused object address with a new node identity cannot inherit an old parked scene");fixture::soft(64);
 fixture::travel=true;check(!invoke(),"bell/map travel can keep its original sequence");fixture::travel=false;
 SetReplay(nullptr);fixture::soft(62);check(invoke(),"ordinary Tower sessions do not inherit replay CG rights");
 fixture::soft(47);check(invoke(),"other Boss post-kill sequences use the shared Blueprint gate");
 fixture::soft(0);check(invoke(),"undeclared/unresolved scene fails closed without starting camera work");
 auto* loaded=fixture::sequence.data();std::memcpy(fixture::node.data()+120,&loaded,8);check(invoke(),"loaded scene fallback also blocks post-kill CG");
 // Independent cooked GUID oracle: Lion final branch versus P1 transition.
 nodeGuidOffset=0x38;assetGuidOffset=0x28;templateOffset=0xe8;flowAssetClass=fixture::cls.data();
 auto* asset=fixture::flowAsset.data();std::memcpy(fixture::node.data()+0x20,&asset,8);
 const tower::terminal::Guid assetGuid={0x20A8DFD3u,0x4298D9DDu,0x29FB3990u,0x2D342EEDu};
 const tower::terminal::Guid ending={0x2E8813B7u,0x42E68329u,0x14580BBAu,0x67214859u};
 const tower::terminal::Guid transition={0xB9644A37u,0x4945B81Bu,0x555AA5A1u,0x9E8D65DBu};
 std::memcpy(asset+0x28,assetGuid.data(),16);std::memcpy(fixture::node.data()+0x38,ending.data(),16);
 check(Terminal(fixture::node.data()),"exact terminal frontier is parked before its own player move/dialogue/CG work");
 std::memcpy(fixture::node.data()+0x38,transition.data(),16);
 check(!Terminal(fixture::node.data()),"same Death Flow's P1 transition frontier stays unblocked");
 tower::terminal::Guid unrelated=assetGuid;unrelated[0]^=1;std::memcpy(asset+0x28,unrelated.data(),16);std::memcpy(fixture::node.data()+0x38,ending.data(),16);
 check(!Terminal(fixture::node.data()),"matching node GUID in a different Flow asset grants no terminal ownership");
 // Native intro driver: exact active Flow + asset/node GUID, no naked Play call.
 Session(false);Session(true);const auto* red=tower::replay::Find("red");SetReplay(red);
 auto* source=fixture::templateAsset.data();std::memcpy(asset+0xe8,&source,8);
 std::memcpy(source+0x28,red->entryAsset.data(),16);std::memcpy(fixture::node.data()+0x38,red->entryNode.data(),16);
 fixture::node[128]=0;uint64_t playPin=1;
 struct Arr{uint8_t* data;int32_t num,max;};Arr list{reinterpret_cast<uint8_t*>(&playPin),1,1};
 std::memcpy(fixture::node.data()+144,&list,sizeof list);int32_t stride=8;std::memcpy(fixture::pinCls.data()+pbz::ue::Layout::StructSize,&stride,4);
 fixture::objects={fixture::node.data()};fixture::flowActive=false;
 check(Start(*red,&why)==Entry::Waiting&&fixture::entryCalls==0,"inactive native Flow never starts a cinematic");
 fixture::flowActive=true;fixture::activeCalls=0;fixture::deactivateAt=2;
 check(Start(*red,&why)==Entry::Waiting&&fixture::entryCalls==0,"Flow becoming inactive during a scan is revalidated before invocation");fixture::deactivateAt=0;
 std::memcpy(fixture::secondNode.data(),fixture::node.data(),fixture::node.size());fixture::objects.push_back(fixture::secondNode.data());
 check(Start(*red,&why)==Entry::Failed&&fixture::entryCalls==0,"two active exact instances refuse ambiguous replay entry");fixture::objects.resize(1);
 uint64_t numbered=playPin|(uint64_t{1}<<32);Arr numberedList{reinterpret_cast<uint8_t*>(&numbered),1,1};std::memcpy(fixture::node.data()+144,&numberedList,sizeof numberedList);
 check(Start(*red,&why)==Entry::Failed&&fixture::entryCalls==0,"numbered FName cannot alias the unnumbered native Play input");
 uint64_t duplicate[]{playPin,playPin};Arr duplicateList{reinterpret_cast<uint8_t*>(duplicate),2,2};std::memcpy(fixture::node.data()+144,&duplicateList,sizeof duplicateList);
 check(Start(*red,&why)==Entry::Failed&&fixture::entryCalls==0,"duplicate native Play inputs fail closed before invoking any scene");
 std::memcpy(fixture::node.data()+144,&list,sizeof list);
 check(Start(*red,&why)==Entry::Started&&fixture::entryCalls==1,"one exact active instance receives its native Play input once");
 check(Start(*red,&why)==Entry::Started&&fixture::entryCalls==1,"already-playing native intro is not invoked twice");
 fixture::objects.clear();fixture::missingGuid=true;Session(false);Session(true);
 check(!Ready(&why),"missing exact Flow GUID layout refuses readiness even if the scene VM gate is installed");fixture::missingGuid=false;
 Session(false);check(fixture::intercepts.empty(),"exit relinquishes the installed Blueprint hooks");
 fixture::missingPin=true;Session(true);check(!Ready(&why),"missing PinName refuses replay readiness instead of proceeding unguarded");Session(false);fixture::missingPin=false;
 fixture::missingSoft=true;Session(true);check(!Ready(&why),"missing scene property refuses replay readiness");check(Protected(&why),"shared Flow/VM protection remains available while a concrete scene layout is missing");Session(false);fixture::missingSoft=false;
 fixture::soft(63);Session(true);SetReplay(lion);check(!fixture::intercepts.at(fixture::finished.data())(fixture::node.data(),nullptr),"a fresh session has no stale parked node or completion restriction");Session(false);
 using towerweapongrant::Grant;uint64_t id=99;
 check(Grant(fixture::bag.data(),id,true)&&fixture::calls==1&&fixture::lastArgs[8]==1&&fixture::lastArgs[9]==1,"one native normal-weapon grant requests original pickup UI");
 check(!Grant(fixture::bag.data(),id,true)&&fixture::calls==1,"a delayed native ownership read cannot repeat the same grant or popup");
 check(fixture::lastArgs[10]==0&&fixture::lastArgs[11]==0&&fixture::depthAtCall==1&&fixture::grantDepth==0,"grant is scoped and never masquerades as load-data or changes equipment");
 uint64_t got=0;std::memcpy(&got,fixture::lastArgs.data(),8);check(got==id,"the exact native FName reaches AddWeaponItem");
 check(Grant(fixture::bag.data(),id,false)&&fixture::calls==2&&fixture::lastArgs[8]==0&&fixture::lastArgs[9]==1,"shaqi weapons request the same original display without a second grant");
 fixture::inside=false;check(!Grant(fixture::bag.data(),id,true)&&fixture::calls==2,"no native grant outside the Tower sandbox session");fixture::inside=true;
 fixture::sandbox=false;check(!Grant(fixture::bag.data(),id,true)&&fixture::calls==2,"no grant when sandbox protection is absent even if the trip still says inside");fixture::sandbox=true;
 fixture::missingGrantField=true;check(!Grant(fixture::bag.data(),id,true)&&fixture::calls==2,"missing display field does not silently grant without requested UI");fixture::missingGrantField=false;
 fixture::overlap=true;check(!Grant(fixture::bag.data(),id,true)&&fixture::calls==2,"overlapping reflected parameters are rejected before a native call");fixture::overlap=false;
 check(!Grant(fixture::bag.data(),0,true)&&fixture::calls==2,"None weapon cannot produce a grant or popup");
 towerweapongrant::Reset();check(Grant(fixture::bag.data(),id,true)&&fixture::calls==3,"a separately entered sandbox session can acquire its own weapon again");
 std::printf("TOWER_STORYGATE_%s %d checks (actual storygate and weapon grant, no game)\n",failed?"FAILED":"OK",checks);return failed?1:0;
}
