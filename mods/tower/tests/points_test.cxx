// Exercise the actual tower_points.cpp handlers and lifecycle with reflected-function
// fixtures. No game, injection, process discovery, disk save or native hook writes.
#include <map>
#include <memory>
#include <cstdio>
#include <array>
#include "../src/tower_points.cpp"

// 武器上限钩子是独立模块（只在 QA 构建生效）；这里只需要生命周期被调用，不碰原生入口
namespace towerweaponcap {
int enables = 0, disables = 0; bool ready = false;
bool Enable() { ++enables; ready = true; return true; }
void Disable() { ++disables; ready = false; }
bool Ready() { return ready; }
}

namespace fixture {
using pbz::ue::UObject;
struct Object {
    std::array<uint8_t, 0x180> bytes{};
    std::string name;
    std::map<std::string, int> properties;
    std::map<std::string, UObject*> methods;
    UObject* Ptr() { return bytes.data(); }
};
std::vector<std::unique_ptr<Object>> objects;
std::map<std::string, UObject*> classes;
std::map<UObject*, Object*> byPointer;
std::map<UObject*, pbz::nx::Handler> hooks;
std::map<UObject*, pbz::pe::Interceptor> intercepts;
std::vector<std::function<void()>> deferred;
bool inside = true, freeUpgrade = false;
int points = 5, levels = 1, node = 0, charges = 0, refunds = 0, soul = 0, mode = 0;
Object& Make(std::string name) { auto o = std::make_unique<Object>(); o->name = name; auto* p = o.get(); objects.push_back(std::move(o)); byPointer[p->Ptr()] = p; return *p; }
Object& Class(std::string name) { auto& o = Make(name); classes[name] = o.Ptr(); return o; }
void Method(Object& cls, const char* name, std::initializer_list<std::pair<const char*, int>> args) {
    auto& f = Make(name); for (auto& [key, off] : args) f.properties[key] = off; cls.methods[name] = f.Ptr();
}
UObject* Function(const char* cls, const char* fn) { return byPointer[classes.at(cls)]->methods.at(fn); }
void Init() {
    auto& bag = Class("PBZBackPackSubsystem");
    Method(bag, "UpgradeWeapon", {{"WeaponID", 0}, {"IsNormalWeapon", 8}, {"UpgradeLevel", 12}});
    Method(bag, "SnapSoulWeapon", {{"WeaponID", 0}, {"IsNormalWeapon", 8}});
    Method(bag, "ResetWeaponToLevel", {{"WeaponID", 0}, {"bIsNormalWeapon", 8}, {"TargetLevel", 12}, {"OutRefundedDust", 16}});
    Method(bag, "UpgradeMedicine", {{"UpgradeLevel", 8}}); Method(bag, "UpgradeAccessory", {{"AccessoryID", 0}, {"UpgradeLevel", 8}});
    Method(bag, "RemovePropsFromBackPack", {{"ItemId", 0}});
    Method(bag, "HasEnoughItems", {{"RequiredItems", 0}, {"ReturnValue", 80}});
    auto& nodes = Class("PBZAbilityNodesComponent");
    Method(nodes, "ActivateNode", {{"NodeID", 0}, {"bActivateByInit", 8}});
    Method(nodes, "DeactivateNode", {{"NodeID", 0}}); Method(nodes, "DeactivateAllNodes", {{"ASC", 0}});
    auto& base = Class("PBZCharacter_Player"); Method(base, "GetCurrentDust", {{"ReturnValue", 0}});
    auto& bp = Class("BP_RenewalPlayer_C"); Method(bp, "GetCurrentDust", {{"ReturnValue", 0}});
    Method(bp, "ChangeDustNum", {{"bIsAdd", 0}, {"Num", 4}});
    Method(bp, "ChangeDustNumInternal", {{"ChangeDustNum", 0}});
}
bool Call(const char* cls, const char* fn, std::array<uint8_t, 256>& p) { return hooks.at(Function(cls, fn))(nullptr, p.data(), nullptr); }
void Flush() { auto list = std::move(deferred); deferred.clear(); for (auto& f : list) f(); }
std::array<uint8_t, 256> WeaponArgs(int n = 1) { std::array<uint8_t, 256> p{}; const uint64_t name = 1; std::memcpy(p.data(), &name, 8); p[8] = 1; std::memcpy(p.data() + 12, &n, 4); return p; }
}
namespace pbz::ue {
UObject* FindClass(std::string_view n) { auto it = fixture::classes.find(std::string(n)); return it == fixture::classes.end() ? nullptr : it->second; }
int Num() { return 0; }
// 1.0.1 武器表尘价补丁用到的对象表接口：离线没有数据表，ForEach 不给任何对象（补丁什么都不改）
void ForEach(const std::function<bool(int, UObject*)>&) {}
bool IsA(UObject*, UObject*) { return false; }
bool IsDefault(UObject*) { return false; }
std::string ObjName(UObject*) { return {}; }
bool Live(UObject* o) { return o != nullptr; }
bool Write(void* p, const void* in, size_t n) { if (!p) return false; std::memcpy(p, in, n); return true; }
int PropOffset(UObject* o, std::string_view n) { auto it = fixture::byPointer.find(o); if (it == fixture::byPointer.end()) return -1; auto p = it->second->properties.find(std::string(n)); return p == it->second->properties.end() ? -1 : p->second; }
std::string Name(uint32_t id) { return id == 1 ? "Tang" : id == 7 ? "TU_19003" : "Weapon_Material_01"; }
bool Read(const void* p, void* out, size_t n) { if (!p) return false; std::memcpy(out, p, n); return true; }
}
namespace pbz::pe {
ue::UObject* FindFunction(ue::UObject* cls, std::string_view name) { auto it = fixture::byPointer.find(cls); if (it == fixture::byPointer.end()) return nullptr; auto f = it->second->methods.find(std::string(name)); return f == it->second->methods.end() ? nullptr : f->second; }
void Call(ue::UObject*, ue::UObject*, void*) {}
void Intercept(ue::UObject* f, Interceptor cb) { fixture::intercepts[f] = std::move(cb); }
void RemoveIntercept(ue::UObject* f) { fixture::intercepts.erase(f); }
}
namespace pbz::nx {
bool Hook(ue::UObject* f, Handler cb, std::string*) { fixture::hooks[f] = std::move(cb); return true; }
void Unhook(ue::UObject* f) { fixture::hooks.erase(f); }
}
namespace pbz::gamethread { void Post(std::function<void()> fn) { fixture::deferred.push_back(std::move(fn)); } }
namespace pbz { IFeature* FindFeature(const std::string&) { return nullptr; } }
namespace towernative { int Level(std::string_view, int) { return fixture::levels; } int NodeLevel(std::string_view) { return fixture::node; } int Snapped(std::string_view, int) { return fixture::soul; } int AccessoryLevel(std::string_view) { return -1; } }
namespace towerpoints { void ClearUi() {} int UiBalance() { return fixture::inside ? fixture::points : -1; } int UiMode() { return fixture::inside ? fixture::mode : 0; } }
namespace towerapi {
bool InsideTower() { return fixture::inside; }
bool TemperMasterHeld() { return false; }
int TemperLevels(const std::string&, int, int levels) { return levels; }
float MeltMulOf(const std::string&, int) { return 1.0f; }
float MeltPowerMul(void*) { return 1.0f; }
bool ChargeWeapon(const std::string&, int, int n, std::string*) { if (n > fixture::points) return false; fixture::points -= n; ++fixture::charges; return true; }
bool ChargeNode(const std::string&, std::string*) { if (!fixture::points) return false; --fixture::points; ++fixture::charges; return true; }
bool ChargeSoul(const std::string&, int, std::string*) { return false; }
void RefundSoul(const std::string&, int) { ++fixture::refunds; }
bool ChargeAccessory(const std::string&, int, std::string*) { return false; }
void RefundAccessory(const std::string&, int) { ++fixture::refunds; }
void Refund(const std::string&, int, int n) { fixture::points += n; ++fixture::refunds; }
bool NativeDowngrade(const std::string&, int, int, std::string*) { return true; }
bool NativeResetNodes(const std::string&, bool, std::string*) { return true; }
void Notify(const std::string&, const std::string&) {}
}

int main() {
    fixture::Init(); int checks = 0, failed = 0;
    auto check = [&](bool ok, const char* msg) { ++checks; if (!ok) { ++failed; std::printf("FAIL %s\n", msg); } };
    using namespace fixture;
    towerpoints::Enable(true, true);
    check(towerpoints::Enabled() && hooks.size() == 12, "all required native transaction and both dust getters installed");
    check(intercepts.count(Function("BP_RenewalPlayer_C", "ChangeDustNum")) && intercepts.count(Function("BP_RenewalPlayer_C", "ChangeDustNumInternal")), "dust writes found on the actual BP owner");
    auto p = WeaponArgs();
    check(!Call("PBZBackPackSubsystem", "UpgradeWeapon", p) && points == 4 && charges == 1, "actual upgrade handler charges once and lets native execute");
    check(Call("PBZBackPackSubsystem", "UpgradeWeapon", p) && points == 4, "second hold callback before readback cannot charge twice");
    ++levels; Flush(); check(refunds == 0 && points == 4, "native success readback retains one point charge");
    Call("PBZBackPackSubsystem", "UpgradeWeapon", p); Flush();
    check(refunds == 1 && points == 4, "native failure refunds exactly once");
    points = 0;
    check(Call("PBZBackPackSubsystem", "UpgradeWeapon", p) && deferred.empty(), "insufficient points neither execute native nor queue a refund");
    points = 4;
    { towerpoints::Internal internal; check(!Call("PBZBackPackSubsystem", "UpgradeWeapon", p) && points == 4 && deferred.empty(), "internal synchronization bypasses UI charging"); }
    inside = false;
    check(!Call("PBZBackPackSubsystem", "UpgradeWeapon", p) && points == 4, "outside tower native transaction is unchanged");
    inside = true;
    auto dust = WeaponArgs();
    check(Call("PBZCharacter_Player", "GetCurrentDust", dust) && pbz::ue::Get<int32_t>(dust.data()) == 4, "base native dust getter returns exact point balance");
    dust = WeaponArgs();
    check(intercepts.at(Function("BP_RenewalPlayer_C", "GetCurrentDust"))(nullptr, dust.data()) && pbz::ue::Get<int32_t>(dust.data()) == 4, "BP ProcessEvent dust override returns the same balance");
    std::array<uint8_t, 256> empty{};
    check(Call("PBZBackPackSubsystem", "HasEnoughItems", empty) && empty[80] == 1, "empty tower point recipe bypasses vanilla empty-map failure");
    check(Call("PBZBackPackSubsystem", "SnapSoulWeapon", p) && soul == 0, "no soul token cannot alter native soul state");
    // 铸魂视图的长按门：HasEnoughItems({TU_19003:1})（一元素 TMap：数据指针 / 元素数，内联位标志 +0x10，位数 +0x28 / 容量 +0x2c / 空闲 +0x34）
    std::array<uint8_t, 20> element{}; const uint64_t soulName = 7; std::memcpy(element.data(), &soulName, 8); const int one = 1; std::memcpy(element.data() + 8, &one, 4);
    auto soulMap = [&] { std::array<uint8_t, 256> m{}; auto* data = element.data(); std::memcpy(m.data(), &data, 8); const int32_t num = 1, flags = 1, bits = 1, cap = 32;
        std::memcpy(m.data() + 8, &num, 4); std::memcpy(m.data() + 12, &num, 4); std::memcpy(m.data() + 0x10, &flags, 4); std::memcpy(m.data() + 0x28, &bits, 4); std::memcpy(m.data() + 0x2c, &cap, 4); return m; };
    auto q = soulMap(); mode = 1;
    check(!Call("PBZBackPackSubsystem", "HasEnoughItems", q), "outside the soul view a TU_ soul material keeps the native answer");
    q = soulMap(); mode = 3; points = 1;
    check(Call("PBZBackPackSubsystem", "HasEnoughItems", q) && q[80] == 1, "soul view with one universal token passes the weapon-specific soul material gate");
    q = soulMap(); points = 0;
    check(Call("PBZBackPackSubsystem", "HasEnoughItems", q) && q[80] == 0, "soul view without a token still reports materials short");
    mode = 0; points = 4;
    check(Call("PBZBackPackSubsystem", "UpgradeMedicine", p), "player cannot manually upgrade reward-managed medicines");
    { towerpoints::Internal internal; check(!Call("PBZBackPackSubsystem", "UpgradeMedicine", p), "floor reward can still apply native medicine growth"); }
    // 2026-11-11 饰品打磨：记账（ChargeAccessory）失败就拦下；内部同步（奖励精炼写原生）放行
    check(Call("PBZBackPackSubsystem", "UpgradeAccessory", p), "accessory refine without tempering charge is blocked");
    { towerpoints::Internal internal; check(!Call("PBZBackPackSubsystem", "UpgradeAccessory", p), "internal sync can apply native accessory level"); }
    Call("PBZBackPackSubsystem", "UpgradeWeapon", p);
    const int refundedBeforeExit = refunds;
    towerpoints::Enable(false, false); Flush();
    check(hooks.empty() && intercepts.empty() && !towerpoints::Enabled(), "exit removes native and BP hooks together");
    check(refunds == refundedBeforeExit, "deferred old-session readback cannot refund after exit");
    auto* missing = Function("BP_RenewalPlayer_C", "ChangeDustNum");
    byPointer[classes.at("BP_RenewalPlayer_C")]->methods.erase("ChangeDustNum");
    towerpoints::Enable(true, true);
    check(!towerpoints::Enabled() && hooks.empty() && intercepts.empty(), "missing BP dust blocker rolls back the whole transaction adapter");
    (void)missing;
    std::printf("TOWER_NATIVE_POINTS_%s %d checks (no game process)\n", failed ? "FAILED" : "OK", checks);
    return failed ? 1 : 0;
}
