#include <windows.h>
#include <array>
#include <cstdio>
#include <map>
#include <thread>
#define PBZ_MODULE
#include "../src/tower_vm_gate.cpp"
namespace fixture {
std::array<uint8_t,512> root{},derived{},otherClass{},self{},other{},function{},childFn{},nativeFrame{};
int effects=0,decisions=0;bool nested=false,cleanupRequest=false;
std::map<void*,std::string> names;
void Native(pbz::ue::UObject* node,void*,void*){
 ++effects;
 if(nested){nested=false;towervm::Dispatch(node,nativeFrame.data(),nullptr);towervm::Dispatch(other.data(),nativeFrame.data(),nullptr);}
}
void ptr(uint8_t* p,void* x){std::memcpy(p,&x,8);}
}
namespace pbz::ue {
bool Read(const void* p,void* d,size_t n){if(!p)return false;std::memcpy(d,p,n);return true;}
bool Live(UObject* o){return o!=nullptr;}
UObject* ClassOf(UObject* o){return Get<UObject*>(o+0x10);}
std::string ObjName(UObject* o){return fixture::names[o];}
std::string Name(uint32_t){return "unused";}
int PropOffset(UObject*,std::string_view){return -1;}
}
namespace pbz::abshook {bool Apply(Hook&,bool){return false;} bool Prepare(Hook&,void*,void*,std::string*){return false;}}
namespace pbz::log {void Write(const std::string&) {}}
namespace pbz::diag {void Event(const char*,const void*,const void*) {}}
int main(){
 int checks=0,fail=0;auto check=[&](bool b,const char* why){++checks;if(!b){++fail;std::printf("FAIL %s\n",why);}};
 using namespace fixture;
 names[root.data()]="PlayLevelSequence_C";names[derived.data()]="PlayLevelSequence_QTE_C";names[otherClass.data()]="Other_C";
 ptr(derived.data()+0x40,root.data());ptr(self.data()+0x10,derived.data());ptr(other.data()+0x10,derived.data());
 uint32_t derivedName=17;std::memcpy(derived.data()+0x18,&derivedName,4);
 ptr(nativeFrame.data()+0x10,function.data());
 towervm::original=&Native;towervm::thread=GetCurrentThreadId();towervm::enabled=true;
 towervm::handlers[function.data()]=[](auto*,auto*,void*){return towervm::Action::Pass;};
 towervm::Class("PlayLevelSequence_C",[](auto*,auto*,void*){++decisions;return cleanupRequest?towervm::Action::Cleanup:towervm::Action::Block;});
 towervm::Dispatch(self.data(),nativeFrame.data(),nullptr);
 check(effects==0&&decisions==1,"new QTE subclass is blocked on its first call even if base function has passthrough registration");
 towervm::Dispatch(self.data(),nativeFrame.data(),nullptr);
 check(effects==0&&decisions==2,"cached class keeps its inherited scene guard");
 ptr(other.data()+0x10,otherClass.data());std::memcpy(otherClass.data()+0x18,&derivedName,4);
 towervm::Dispatch(other.data(),nativeFrame.data(),nullptr);
 check(effects==1&&decisions==2,"same FName in an unrelated class cannot inherit another class's cached scene restriction");ptr(other.data()+0x10,derived.data());effects=0;
 cleanupRequest=true;nested=true;
 towervm::Class("PlayLevelSequence_C",[](auto* node,auto*,void*){++decisions;return node==self.data()?towervm::Action::Cleanup:towervm::Action::Block;});
 towervm::Dispatch(self.data(),nativeFrame.data(),nullptr);
 check(effects==2&&decisions==4,"interrupt cleanup is scoped to the same node; nested call to another scene stays blocked");
 check(towervm::cleanup==nullptr,"nested native return releases cleanup permission");
 towervm::Class("PlayLevelSequence_C",[](auto*,auto*,void*){++decisions;return towervm::Action::Block;});
 std::thread worker([]{towervm::Dispatch(self.data(),nativeFrame.data(),nullptr);});worker.join();
 check(effects==3&&decisions==4,"non-game thread is untouched by the game-thread-only router");
 towervm::enabled=false;towervm::Dispatch(self.data(),nativeFrame.data(),nullptr);
 check(effects==4&&decisions==4,"disabled router immediately passes the original call");
 towervm::enabled=true;towervm::classHandlers.clear();towervm::classCache.clear();towervm::handlers.clear();
 towervm::Dispatch(self.data(),nativeFrame.data(),nullptr);
 check(effects==5,"unregistered script calls are untouched");
 std::string why;
 check(!towervm::Add(function.data(),[](auto*,auto*,void*){return towervm::Action::Block;},&why)&&!why.empty(),"unverified wrapper cannot install a native patch");
 std::printf("TOWER_VM_DISPATCH_%s %d checks (actual router, no game)\n",fail?"FAILED":"OK",checks);return fail?1:0;
}
