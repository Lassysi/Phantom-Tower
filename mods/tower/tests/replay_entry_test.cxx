// Actual entry.cpp lifecycle with a virtual wall clock and no game process.
#include <windows.h>
#include <cstdio>
static ULONGLONG testNow = 100;
static ULONGLONG TestClock() { return testNow; }
#define GetTickCount64 TestClock
#define PBZ_MODULE
#include "../src/tower_entry.cpp"
#undef GetTickCount64

namespace fixture {
int nonzeroCountdown = 0, writes = 0;
int medicineRestores = 0, bulletRestores = 0;
bool inside = true, ready = false, commitHolds = true;
std::array<uint8_t, 0x80> pawn{}, level{}, world{}, bag{}, asc{}, cls{}, lib{}, fnAsc{}, fnQuick{}, fnBullet{}, fnRemove{};
std::array<float, 4> attributes{10, 900, 3, 440};
std::vector<pbz::attr::Target> targets;
std::array<uint8_t,0x80> boss{},bossAsc{},support{},supportAsc{},pc{},fnGetInhibit{},fnSetInhibit{},fnMove{};
bool playerInhibited=false,bossInhibited=false,supportInhibited=false;int moveLocks=0,playerFrozen=0,bossFrozen=0,supportFrozen=0;
void Init() {
    auto* w = world.data(); auto* l = level.data();
    std::memcpy(pawn.data() + pbz::ue::Layout::ObjOuter, &l, 8);
    std::memcpy(level.data() + pbz::ue::Layout::ObjOuter, &w, 8);
    std::memcpy(bag.data() + pbz::ue::Layout::ObjOuter, &w, 8);
    targets = {{pawn.data(), nullptr, pbz::attr::Who::Player, cls.data()}};
}
}
namespace pbz::ue {
int Num(){return 0;}
UObject* FindClass(std::string_view n) { return !fixture::ready ? nullptr : n == "PBZBackPackSubsystem" ? fixture::cls.data() : n == "AbilitySystemBlueprintLibrary" ? fixture::lib.data() : nullptr; }
UObject* ClassOf(UObject*) { return fixture::cls.data(); }
std::string ObjName(UObject*) { return {}; }
std::string ClassName(UObject*) { return {}; }
int PropOffset(UObject*, std::string_view n) { return n == "Actor" || n == "GameplayEffect" || n=="NewInhibit" || n=="bNewMoveInput" ? 0 : n=="AttributeSetBase"?64:n == "ReturnValue" ? 8 : n == "StacksToRemove" ? 16 : -1; }
bool Live(UObject* p) { return fixture::ready && p; }
bool IsA(UObject* p, UObject*) { return p == fixture::bag.data(); }
bool IsDefault(UObject*) { return false; }
void ForEach(const std::function<bool(int, UObject*)>& fn) { if (fixture::ready) fn(0, fixture::bag.data()); }
int ObjBatch(int start, int count, UObject** out) { if (!fixture::ready || start > 0 || count < 1) return 0; out[0] = fixture::bag.data(); return 1; }  // 1.3.0 分帧扫描
bool Read(const void* p, void* out, size_t n) { if (!p) return false; std::memcpy(out,p,n); return true; }
bool Write(void* p, const void* in, size_t n) { if (!p) return false; ++fixture::writes; std::memcpy(p,in,n); return true; }  // TopUpNow 一次性写满
}
namespace pbz::pe {
ue::UObject* FindFunction(ue::UObject*, std::string_view n) {
    if (!fixture::ready) return nullptr;
    if(n=="GetUserAbilityActivationInhibited")return fixture::fnGetInhibit.data();
    if(n=="SetUserAbilityActivationInhibited")return fixture::fnSetInhibit.data();
    if(n=="SetIgnoreMoveInput")return fixture::fnMove.data();
    return n == "GetAbilitySystemComponent" ? fixture::fnAsc.data() : n == "RestoreQuickItems" ? fixture::fnQuick.data() :
           n == "RestoreShaqiWeaponBullet" ? fixture::fnBullet.data() : n == "RemoveActiveGameplayEffectBySourceEffect" ? fixture::fnRemove.data() : nullptr;
}
void Call(ue::UObject* obj, ue::UObject* fn, void* p) {
    ++fixture::writes;
    if (fn == fixture::fnAsc.data()) { auto* actor=ue::Get<ue::UObject*>(static_cast<uint8_t*>(p));auto* a = actor==fixture::boss.data()?fixture::bossAsc.data():actor==fixture::support.data()?fixture::supportAsc.data():fixture::asc.data(); std::memcpy(static_cast<uint8_t*>(p)+8,&a,8); }
    if(fn==fixture::fnGetInhibit.data())static_cast<uint8_t*>(p)[8]=obj==fixture::bossAsc.data()?fixture::bossInhibited:obj==fixture::supportAsc.data()?fixture::supportInhibited:fixture::playerInhibited;
    if(fn==fixture::fnSetInhibit.data())(obj==fixture::bossAsc.data()?fixture::bossInhibited:obj==fixture::supportAsc.data()?fixture::supportInhibited:fixture::playerInhibited)=static_cast<uint8_t*>(p)[0]!=0;
    if(fn==fixture::fnMove.data())fixture::moveLocks+=static_cast<uint8_t*>(p)[0]?1:-1;
    if (fn == fixture::fnQuick.data()) ++fixture::medicineRestores;
    if (fn == fixture::fnBullet.data()) ++fixture::bulletRestores;
}
}
namespace pbz::attr {
const std::vector<Target>& Targets() { return fixture::targets; }
bool Alive(const Target&) { return fixture::ready; }
int ActorOffset(const std::string& n) { return n=="CustomTimeDilation"?80:-1; }
int AttrOffset(const std::string& n) { return n == "Health" ? 0 : n == "Health_Max" ? 1 : n == "Shachi" ? 2 : n == "Shachi_Max" ? 3 : -1; }
float* AttrBase(const Target&, int) { return nullptr; }
float* AttrCurrent(const Target&, int off) { return &fixture::attributes.at(off); }
void Hold(const Target&, float* p, float v) { ++fixture::writes; if (fixture::commitHolds && p) *p = v; }
void Want(const Target& t, float*, Kind, float) { ++fixture::writes; if(t.obj==fixture::pawn.data())++fixture::playerFrozen;else if(t.obj==fixture::boss.data())++fixture::bossFrozen;else if(t.obj==fixture::support.data())++fixture::supportFrozen; }
}
namespace pbz::log { void Write(const std::string&) {} }
namespace towerwidgets {
pbz::ue::UObject* Pawn() { return fixture::ready ? fixture::pawn.data() : nullptr; }
pbz::ue::UObject* Controller() { return fixture::ready?fixture::pc.data():nullptr; }
pbz::ue::UObject* Default(pbz::ue::UObject* cls) { return cls; }
pbz::ue::UObject* LoadClass(const char*) { return nullptr; }  // 3.4.0 暗源效果：离线不加载
int RefreshHpBar() { return 0; }  // 血条重画：离线没有控件
bool MenuBusy() { return false; }
bool Countdown(int n) { fixture::nonzeroCountdown += n > 0; return true; }
}
namespace towerapi { bool InsideTower() { return fixture::inside; } }

int main() {
    int checks = 0, failed = 0;
    auto check = [&](bool ok, const char* why) { ++checks; if (!ok) { ++failed; std::printf("FAIL %s\n", why); } };
    using namespace towerentry;
    SetFullState(false); BeginReplay();
    check(!Holding() && PrepareReplay() == tower::entry::State::Running, "ready replay allows player interaction without a countdown lock");
    testNow += 180000;
    check(Tick(false) != tower::entry::State::Failed && !Holding(), "waiting three minutes for native interaction/intro does not trip ordinary spawn timeout");
    check(fixture::nonzeroCountdown == 0 && fixture::writes == 0, "replay waiting never freezes a player, changes an ability or displays 3/2/1");
    End(); Begin(); testNow += 90001;
    check(Tick(false) == tower::entry::State::Failed && !Holding(), "the next ordinary spawn still has its 90-second missing-readiness safety limit");
    SetFullState(true); BeginReplay();
    check(PrepareReplay() == tower::entry::State::Failed && !Holding(), "unavailable full-state preparation rejects replay without trapping player input");
    check(PrepareReplay() == tower::entry::State::Failed, "failed preparation cannot silently become ready on retry");
    End(); SetFullState(false); BeginReplay();
    check(PrepareReplay() == tower::entry::State::Running && !Holding(), "a new replay clears the previous preparation failure");
    fixture::ready = true; fixture::Init(); SetFullState(true); BeginReplay();
    check(PrepareReplay() == tower::entry::State::Waiting && !Holding(), "full-state preparation allows an arbitration frame without enabling input locks");
    check(fixture::attributes[0] == 900 && fixture::attributes[2] == 440 && PrepareReplay() == tower::entry::State::Running,
          "replay preserves full health and shachi with actual readback before the interaction-ready hint");
    check(fixture::medicineRestores == 1 && fixture::bulletRestores == 1, "medicine and shadow-weapon refill run exactly once for the replay");
    fixture::attributes[0] = 1; fixture::commitHolds = false; BeginReplay();
    check(PrepareReplay() == tower::entry::State::Waiting, "an uncommitted refill remains pending");
    testNow += 3001;
    check(PrepareReplay() == tower::entry::State::Failed && !Holding(), "a refill that never takes effect fails boundedly without unlocking an unready encounter");
    check(fixture::nonzeroCountdown == 0, "no replay path displays a countdown");
    End();SetFullState(false);Begin();
    check(Hold(fixture::pawn.data()) && Hold(fixture::boss.data()),"spawner-only replay can hold both player and adopted native boss in the real entry module");
    check(fixture::moveLocks==1 && fixture::playerInhibited && fixture::bossInhibited && fixture::playerFrozen>0 && fixture::bossFrozen>0,"both sides freeze and player movement/abilities are held before countdown");
    Hold(fixture::pawn.data());check(fixture::moveLocks==1,"repeated hold does not stack player input locks");
    End();check(fixture::moveLocks==0 && !fixture::playerInhibited && !fixture::bossInhibited,"cancel releases only this entry's paired locks");
    Begin(); fixture::supportInhibited = false;
    check(Hold(fixture::support.data(), false) && Hold(fixture::boss.data()) && fixture::supportInhibited && fixture::bossInhibited && fixture::supportFrozen > 0, "native support and boss both use the real entry freeze lifecycle");
    check(Hold(fixture::support.data(), false), "repeated support discovery does not create a second lease");
    End(); check(!fixture::supportInhibited && !fixture::bossInhibited, "entry cancellation releases support and boss");
    Begin(); fixture::supportInhibited = true; check(Hold(fixture::support.data(), false), "already inhibited support can be held");
    End(); check(fixture::supportInhibited, "release restores support's original inhibition state"); fixture::supportInhibited = false;
    std::printf("TOWER_REPLAY_ENTRY_%s %d checks (actual entry.cpp, virtual wall clock, no game)\n", failed ? "FAILED" : "OK", checks);
    return failed ? 1 : 0;
}
