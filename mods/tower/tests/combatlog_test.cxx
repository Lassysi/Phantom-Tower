#include <windows.h>
#include <array>
#include <cstdio>
#include <map>
static ULONGLONG clockMs=100;
static ULONGLONG Clock(){return clockMs;}
#define GetTickCount64 Clock
#define PBZ_MODULE
#include "../src/tower_combatlog.cpp"
#undef GetTickCount64
namespace fixture {
std::array<uint8_t,256> cls{},damage{},take{},player{},boss{},component{},other{};
std::map<int,pbz::pe::Observer> observers;
std::map<pbz::ue::UObject*,towervm::Handler> vm;
bool ready=true;
int next=0;
}
namespace pbz::ue {
bool Read(const void* p,void* d,size_t n){if(!p)return false;std::memcpy(d,p,n);return true;}
bool Live(UObject* p){return p!=nullptr;}
UObject* ClassOf(UObject*){return fixture::cls.data();}
int Num(){return 0;}
UObject* FindClass(std::string_view){return fixture::cls.data();}
std::string Name(uint32_t id){return std::to_string(id);}
std::string ObjName(UObject*){return "fixture";}
std::string ClassName(UObject*){return "Fixture_C";}
int PropOffset(UObject*,std::string_view key){return key=="DamageAmount"?0:key=="DamageTags"?8:key=="DamageCause"?0:-1;}
}
namespace pbz::pe {
ue::UObject* FindFunction(ue::UObject*,std::string_view n){return n=="OnDamage"?fixture::damage.data():n=="TakeDamage"?fixture::take.data():nullptr;}
int Observe(ue::UObject*,Observer cb){fixture::observers[++fixture::next]=cb;return fixture::next;}
void Unobserve(int id){fixture::observers.erase(id);}
}
namespace pbz::nx {
bool Hook(ue::UObject*,Handler,std::string*){return false;}
void Unhook(ue::UObject*){}
}
namespace towervm {
bool Ready(){return fixture::ready;}
bool Add(pbz::ue::UObject* fn,Handler cb,std::string*){fixture::vm[fn]=cb;return true;}
void Remove(pbz::ue::UObject* fn){fixture::vm.erase(fn);}
}
namespace pbz::log {void Write(const std::string&) {}}
int main(){
    using namespace towercombat;using namespace fixture;
    int checks=0,failed=0;auto check=[&](bool ok,const char* why){++checks;if(!ok){++failed;std::printf("FAIL %s\n",why);}};
    Begin(23,player.data());Watch(boss.data(),"boss");
    auto* owner=player.data();std::memcpy(component.data()+pbz::ue::Layout::ObjOuter,&owner,8);
    std::array<uint8_t,32> args{};float amount=12;std::memcpy(args.data(),&amount,4);
    check(vm.size()==1,"same OnDamage function is observed once for player and boss");
    Sample(boss.data(),100);Adjusted(boss.data(),50,"entry normalization");Sample(boss.data(),50);
    check(g_bosses.at(boss.data()).events==0,"Tower normalization cannot become a fake damage event");
    observers.at(1)(boss.data(),args.data());
    check(g_onDamagePe==1&&g_bosses.at(boss.data()).events==0,"PE invocation of a VM-observed Blueprint event is a route counter only");
    vm.at(damage.data())(boss.data(),damage.data(),args.data());Sample(boss.data(),38);
    check(g_bosses.at(boss.data()).events==1&&g_bosses.at(boss.data()).total==12&&g_onDamageVm==1,"PE plus actual VM plus immediate HP read counts one damage event");
    uint32_t cause=7;RememberCause(component.data(),&cause,0);
    vm.at(damage.data())(boss.data(),damage.data(),args.data());
    check(g_causes.count(player.data())==1,"damage to another actor cannot consume the player's pending source");
    vm.at(damage.data())(player.data(),damage.data(),args.data());
    check(g_hits.back().cause=="7"&&g_causes.empty(),"same owner's recent cause is consumed exactly once");
    RememberCause(component.data(),&cause,0);clockMs+=101;
    vm.at(damage.data())(player.data(),damage.data(),args.data());
    check(g_hits.back().cause.empty(),"stale source is never attached to later damage");
    RememberCause(other.data(),&cause,0);check(g_causes.empty(),"untracked component cannot provide a lethal source");
    g_now=2;Sample(boss.data(),30);
    check(g_hpDrops==1&&g_bosses.at(boss.data()).tags["血量下降"]==1,"uncaptured HP change remains separately labelled");
    const auto events=g_bosses.at(boss.data()).events;Sample(boss.data(),NAN);
    check(g_bosses.at(boss.data()).events==events,"invalid HP cannot create an event");
    End("test");check(observers.empty()&&vm.empty()&&g_bosses.empty()&&g_causes.empty(),"end removes owned observers and all actor attribution state");
    std::printf("TOWER_COMBATLOG_%s %d checks (actual observers and HP sampling, no game)\n",failed?"FAILED":"OK",checks);return failed?1:0;
}
