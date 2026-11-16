// Config-backed summon graph and the actual interaction filter, without a game process.
#include <windows.h>
#include <array>
#include <cstdio>
#include <map>
static ULONGLONG testNow=100;
static ULONGLONG TestClock() { return testNow; }
#define GetTickCount64 TestClock
#define PBZ_MODULE
#include "../src/tower_interactions.cpp"
#undef GetTickCount64
#include "../src/encounter_dependencies.generated.h"
#include "../src/tower_replay.h"

namespace fixture {
std::array<uint8_t,256> actorClass{}, interactClass{}, replacedClass{}, traversalClass{}, nativeBoss{}, executionTarget{}, mechanism{}, unrelated{}, component{}, pawn{}, ownerCycle{};
std::array<std::array<uint8_t,64>,5> functions{};
bool ready = false, replace = false;
std::vector<pbz::ue::UObject*> owned;
std::map<pbz::ue::UObject*,pbz::pe::Interceptor> hooks;
void owner(pbz::ue::UObject* a, pbz::ue::UObject* b) { std::memcpy(a+48,&b,8); }
void instigator(pbz::ue::UObject* a, pbz::ue::UObject* b) { std::memcpy(a+56,&b,8); }
}
namespace pbz::ue {
bool Read(const void* a, void* out, size_t n) { if (!a) return false; std::memcpy(out,a,n); return true; }
bool Live(UObject* a) { return a != nullptr; }
int Num(){return 0;}
UObject* FindClass(std::string_view s) {
    return s == "Actor" ? fixture::actorClass.data() : s == "PBZInteractComponent" && fixture::ready ?
        (fixture::replace ? fixture::replacedClass.data() : fixture::interactClass.data()) : s == "SpecialMoveVolumeBase" ? fixture::traversalClass.data() : nullptr;
}
bool IsA(UObject* a,UObject* c) { return a == fixture::mechanism.data() && c == fixture::traversalClass.data(); }
int PropOffset(UObject*, std::string_view n) { return n == "Owner" ? 48 : n == "Instigator" ? 56 : n == "Actor" ? 0 : n == "ReturnValue" ? 8 : n == "CurrentInteractActor" ? 64 : -1; }
std::string ObjName(UObject* a) { return a == fixture::executionTarget.data() ? "native-execution-target" : "other"; }
std::string ClassName(UObject*) { return "fixture"; }
}
namespace pbz::pe {
ue::UObject* FindFunction(ue::UObject*,std::string_view n) {
    const char* names[] = {"SetCurrentInteractActor","StartFlowFadeInteract","ShouldDisplayScreenOutActor","StartInteract","StartFadeInteract"};
    for (int i=0;i<5;++i) if (n == names[i]) return fixture::functions[i].data();
    return nullptr;
}
void Intercept(ue::UObject* f, Interceptor h) { fixture::hooks[f]=h; }
void RemoveIntercept(ue::UObject* f) { fixture::hooks.erase(f); }
}
namespace pbz::enemies { std::vector<ue::UObject*> OwnedActors() { return fixture::owned; } }
namespace pbz::log { void Write(const std::string&) {} }
namespace towerwidgets { pbz::ue::UObject* Pawn() { return fixture::pawn.data(); } }

int main() {
    int checks=0, failed=0;
    auto check=[&](bool b,const char* why) { ++checks; if(!b) { ++failed; std::printf("FAIL %s\n",why); } };
    using tower::encounter::RequiredBy;
    for (const auto& e:tower::encounter::kEdges) {
        check(!RequiredBy(e.owner,e.target).empty(),"all configuration-backed native targets survive their admitted combat root");
        check(RequiredBy(e.owner,e.owner).empty() && RequiredBy("unrelated",e.target).empty(),"no self-phase adoption or global support exemption");
    }
    check(!RequiredBy("BP_Monster_LengTu_C","BP_Monster_YuBi_Stand_C").empty(),"native nested summons preserve their mechanism dependencies too");
    check(RequiredBy("BP_Monster_LionDancer_C","BP_Monster_DaTouWaWa_HouHouHou_C").empty(),"dolls require the admitted second phase, not just a lion-named floor");
    check(RequiredBy("BP_Monster_LionDancer_P2_C","BP_Monster_DaTouWaWa_HouHouHou_C_Extra").empty(),"exact classes only; name prefixes never confer rights");
    auto* lion=tower::replay::Find("lion");
    check(lion && tower::replay::PhaseOf(*lion,"BP_Monster_DaTouWaWa_HouHouHou_C")<0 && !tower::replay::AllowSequence(lion,"Seq064"),"mechanism preservation grants neither phase/victory status nor a post-kill CG");
    using namespace towerinteractions;
    Begin(); AllowNames({"native-execution-target"}); AllowActors({fixture::nativeBoss.data()});
    fixture::ready=true; testNow+=2100; Tick();
    check(fixture::hooks.size()==5 && Allowed(fixture::executionTarget.data()) && Allowed(fixture::nativeBoss.data()),"late class installation preserves explicit interaction and adopted native boss rights");
    AllowNames({});
    check(!Allowed(fixture::executionTarget.data()),"exact interaction name can be revoked");
    fixture::owner(fixture::executionTarget.data(),fixture::nativeBoss.data());
    check(Allowed(fixture::executionTarget.data()),"an adopted native boss owns interactions even outside Core OwnedActors");
    fixture::owner(fixture::executionTarget.data(),nullptr); fixture::instigator(fixture::executionTarget.data(),fixture::nativeBoss.data());
    check(Allowed(fixture::executionTarget.data()),"native instigator links also retain the mechanism");
    fixture::instigator(fixture::executionTarget.data(),nullptr); AllowActors({fixture::executionTarget.data()});
    check(Allowed(fixture::executionTarget.data()) && !Allowed(fixture::nativeBoss.data()),"replacing the active snapshot revokes old roots");
    check(!Allowed(fixture::unrelated.data()) && Allowed(fixture::mechanism.data()),"unrelated pickups remain blocked, native traversal remains allowed");
    fixture::owner(fixture::ownerCycle.data(),fixture::ownerCycle.data());
    check(!Allowed(fixture::ownerCycle.data()),"an untrusted owner cycle cannot hang or confer rights");
    fixture::replace=true; Tick();
    check(Allowed(fixture::executionTarget.data()) && fixture::hooks.size()==5,"class reload reinstalls hooks without erasing the encounter policy");
    auto* pp=fixture::pawn.data(); std::memcpy(fixture::component.data()+pbz::ue::Layout::ObjOuter,&pp,8);
    std::array<uint8_t,32> args{}; auto* a=fixture::unrelated.data(); std::memcpy(args.data(),&a,8); args[8]=1;
    check(fixture::hooks.at(fixture::functions[2].data())(fixture::component.data(),args.data()) && args[8]==0,"actual intercepted display query blocks unrelated loot");
    a=fixture::executionTarget.data(); std::memcpy(args.data(),&a,8);
    check(!fixture::hooks.at(fixture::functions[2].data())(fixture::component.data(),args.data()),"actual intercepted display query permits the admitted mechanism");
    End(); Begin();
    check(!Allowed(fixture::executionTarget.data()),"end / new session revokes every encounter-specific actor permission");
    End();
    check(fixture::hooks.empty(),"all installed interaction intercepts are relinquished at session end");
    std::printf("TOWER_ENCOUNTER_DEPENDENCIES_%s %d checks (actual interaction.cpp, native config graph, no game)\n",failed?"FAILED":"OK",checks);
    return failed?1:0;
}
