#include "tower_native.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../../src/core/host/log.h"
#include "../../../src/core/ipc/pipe.h"
#include "../../../src/core/sdk/pbz_module.h"
#include "../../../src/core/services/attributes.h"
#include "../../../src/core/services/inventory.h"
#include "growth_data.generated.h"
#include "tower_json.h"
#include "tower_save.h"
#include "tower_trip.h"
#include "tower_points.h"
#include "tower_api.h"
#include "tower_widgets.h"
#include "tower_entry.h"
#include "tower_title.h"
#include "tower_travel_api.h"
#include "tower_perf.h"
#include "tower_isolation.h"
#include "tower_weapon_grant.h"

namespace towersb { bool Set(bool on, std::string* why, bool force); bool Active(); bool Dirty(); uint64_t Activity(); }
namespace towergate { bool Veto(bool on, std::string* why); bool Snapshot(std::string* why); bool Restore(std::string* why);
                      bool Put(const std::wstring& text, bool clean, std::string* why); bool BaselineText(std::wstring* out); void ConfirmOriginal();
                      bool Release(std::string* why); void StopWatching(); bool SaveClean(std::string* why); }
namespace towerprobe { std::string Ach(const std::string& v); }

namespace towernative {
namespace {

using pbz::ue::UObject;
namespace ue = pbz::ue;
namespace pe = pbz::pe;
namespace attr = pbz::attr;

struct FArr { void* data; int32_t num, max; };

// 参数缓冲：按参数名取偏移（UFunction 也是 UStruct）
struct Args {
    UObject* fn;
    std::vector<uint8_t> b;
    explicit Args(UObject* f) : fn(f), b(0x400, 0) {}
    uint8_t* At(std::string_view name) {
        const int o = fn ? ue::PropOffset(fn, name) : -1;
        static uint8_t sink[0x80];
        if (o < 0 || o + 0x40 > static_cast<int>(b.size())) { std::memset(sink, 0, sizeof sink); return sink; }
        return b.data() + o;
    }
};

UObject* Cdo(UObject* cls) {
    UObject* found = nullptr;
    if (cls) ue::ForEach([&](int, UObject* o) { if (ue::ClassOf(o) == cls && ue::IsDefault(o)) { found = o; return false; } return true; });
    return found;
}

std::string NameStr(uint64_t n) {
    if (!(n & 0xFFFFFFFF)) return {};
    auto s = ue::Name(static_cast<uint32_t>(n));
    const auto num = static_cast<uint32_t>(n >> 32);
    return num ? s + "_" + std::to_string(num - 1) : s;
}

// Map<FName, UObject*>：TSparseArray 的元素 = {FName 8, 值 8, 哈希链 8}；空闲位的值指针是旧值，用对象表身份过滤
std::vector<std::pair<uint64_t, UObject*>> NameObjMap(const uint8_t* addr) {
    std::vector<std::pair<uint64_t, UObject*>> out;
    const auto a = ue::Get<FArr>(addr);
    if (a.num < 0 || a.num > 4096 || a.max < a.num || (a.num && !a.data)) return out;
    const int bits = ue::Get<int32_t>(addr + 0x28, -1), capacity = ue::Get<int32_t>(addr + 0x2c, -1), free = ue::Get<int32_t>(addr + 0x34, -1);
    auto* heap = ue::Get<uint8_t*>(addr + 0x20);
    if (bits != a.num || capacity < bits || capacity > 1048576 || free < 0 || free > a.num || (!heap && capacity > 128)) return out;
    std::vector<uint32_t> flags((a.num + 31) / 32);
    if (!flags.empty() && !ue::Read(heap ? heap : addr + 0x10, flags.data(), flags.size() * 4)) return out;
    for (int i = 0; a.data && i < a.num && i < 4096; ++i) {
        if (!((flags[i / 32] >> (i % 32)) & 1)) continue;
        const auto* e = static_cast<const uint8_t*>(a.data) + i * 24;
        const auto key = ue::Get<uint64_t>(e);
        auto* val = ue::Get<UObject*>(e + 8);
        if (val && ue::Live(val)) out.push_back({key, val});
    }
    return out;
}

// 3.3.0（用户：回收 / 熔铸后武器还在背包能用）：RemoveWeaponByID 只把武器从装备栏卸下，背包的武器信息表
// （MainWeaponInfos / ShaqiWeaponInfos，HasWeapon 和背包列表都读它）里那条还在。这里按 UE5 TSet 布局删掉这一条：
// 元素 = {FName 8, 值 8, HashNextId 4, HashIndex 4}；+0x10 分配位（内联）/ +0x20 堆、+0x28 位数；+0x30 FirstFreeIndex、+0x34 NumFreeIndices；
// 哈希 +0x38 内联 / +0x40 堆、+0x48 HashSize。任何一处对不上就不动，返回 false
bool EraseNameKey(uint8_t* map, uint64_t key) {
    const auto a = ue::Get<FArr>(map);
    if (a.num <= 0 || a.num > 4096 || a.max < a.num || !a.data) return false;
    if (ue::Get<int32_t>(map + 0x28, -1) != a.num) return false;
    auto* flagHeap = ue::Get<uint8_t*>(map + 0x20);
    auto* flags = reinterpret_cast<uint32_t*>(flagHeap ? flagHeap : map + 0x10);
    auto* data = static_cast<uint8_t*>(a.data);
    int idx = -1;
    for (int i = 0; i < a.num; ++i) {
        uint32_t w = 0;
        if (!ue::Read(flags + i / 32, &w, 4)) return false;
        if (((w >> (i % 32)) & 1) && ue::Get<uint64_t>(data + i * 24) == key) { idx = i; break; }
    }
    if (idx < 0) return false;
    uint8_t* e = data + idx * 24;
    const int32_t next = ue::Get<int32_t>(e + 0x10), bucket = ue::Get<int32_t>(e + 0x14);
    const int32_t hashSize = ue::Get<int32_t>(map + 0x48, 0);
    auto* hashHeap = ue::Get<uint8_t*>(map + 0x40);
    auto* hash = reinterpret_cast<int32_t*>(hashHeap ? hashHeap : map + 0x38);
    if (hashSize <= 0 || bucket < 0 || bucket >= hashSize) return false;
    int32_t* link = hash + bucket;
    for (int guard = 0; ue::Get<int32_t>(reinterpret_cast<uint8_t*>(link), -2) != idx; ++guard) {
        const int32_t at = ue::Get<int32_t>(reinterpret_cast<uint8_t*>(link), -2);
        if (at < 0 || at >= a.num || guard > a.num) return false;  // 哈希链对不上：不动
        link = reinterpret_cast<int32_t*>(data + at * 24 + 0x10);
    }
    if (!ue::Write(link, &next, 4)) return false;
    const int32_t firstFree = ue::Get<int32_t>(map + 0x30, -1), numFree = ue::Get<int32_t>(map + 0x34, 0);
    const int32_t none = -1, nextFree = numFree > 0 ? firstFree : -1;
    if (numFree > 0 && firstFree >= 0 && firstFree < a.num) ue::Write(data + firstFree * 24, &idx, 4);  // 旧表头的 PrevFreeIndex
    ue::Write(e, &none, 4); ue::Write(e + 4, &nextFree, 4);
    ue::Write(map + 0x30, &idx, 4);
    const int32_t nf = numFree + 1; ue::Write(map + 0x34, &nf, 4);
    uint32_t w = 0; ue::Read(flags + idx / 32, &w, 4); w &= ~(1u << (idx % 32)); ue::Write(flags + idx / 32, &w, 4);
    return true;
}

class Native final : public tower::session::IGame {
public:
    std::function<bool(std::string*)> backup;

    bool Resolve() {
        if (resolved_) return ready_;
        resolved_ = true;
        bagCls_ = towerperf::ClassNamed("PBZBackPackSubsystem");
        nodesCls_ = towerperf::ClassNamed("PBZAbilityNodesComponent");
        helpCls_ = towerperf::ClassNamed("AbilityNodeHelpObj");
        medInfoCls_ = towerperf::ClassNamed("PBZMedicineInfo");
        auto* strLib = towerperf::ClassNamed("KismetStringLibrary");
        auto* asLib = towerperf::ClassNamed("AbilitySystemBlueprintLibrary");
        strCdo_ = Cdo(strLib);
        asCdo_ = Cdo(asLib);
        toName_ = strLib ? pe::FindFunction(strLib, "Conv_StringToName") : nullptr;
        getAsc_ = asLib ? pe::FindFunction(asLib, "GetAbilitySystemComponent") : nullptr;
        auto F = [&](UObject* cls, const char* n) { return cls ? pe::FindFunction(cls, n) : nullptr; };
        has_ = F(bagCls_, "HasWeapon"); level_ = F(bagCls_, "GetWeaponLevel"); upgrade_ = F(bagCls_, "UpgradeWeapon");
        reset_ = F(bagCls_, "ResetWeaponToLevel"); snap_ = F(bagCls_, "SnapSoulWeapon"); snapped_ = F(bagCls_, "IsWeaponSnapSouled");
        upAcc_ = F(bagCls_, "UpgradeAccessory"); accMax_ = F(bagCls_, "GetAccessoryMaxRefineLevel");
        accInfoCls_ = towerperf::ClassNamed("PBZAccessoryInfo"); offAccLevel_ = accInfoCls_ ? ue::PropOffset(accInfoCls_, "CurrentLevel") : -1;
        pushMed_ = F(bagCls_, "PushMedicineToInventory"); upMed_ = F(bagCls_, "UpgradeMedicine"); pushAcc_ = F(bagCls_, "PushAccessoryToInventory"); addProps_ = F(bagCls_, "AddPropsToBackPack"); itemNum_ = F(bagCls_, "GetItemNumInfo");
        accNum_ = F(bagCls_, "GetCanAssembleAccessoryNum");
        activate_ = F(nodesCls_, "ActivateNode"); deactivate_ = F(nodesCls_, "DeactivateNode"); deactivateAll_ = F(nodesCls_, "DeactivateAllNodes");
        offMain_ = ue::PropOffset(bagCls_, "MainWeapons"); offShaqi_ = ue::PropOffset(bagCls_, "ShaqiWeapons");
        offMedB_ = ue::PropOffset(bagCls_, "BottomMedicineID"); offMedT_ = ue::PropOffset(bagCls_, "TopMedicineID");
        offMeds_ = ue::PropOffset(bagCls_, "MedicineInfos"); offAccs_ = ue::PropOffset(bagCls_, "Accessories");
        offAssembly_ = ue::PropOffset(bagCls_, "AssemblyAccessory");
        offNodeMap_ = ue::PropOffset(nodesCls_, "AbilityNodeMap");
        offNodeLevel_ = ue::PropOffset(helpCls_, "Level");
        offMedLevel_ = ue::PropOffset(medInfoCls_, "CurrentLevel"); offMedTime_ = ue::PropOffset(medInfoCls_, "CurrentTime");
        ready_ = bagCls_ && strCdo_ && toName_ && has_ && level_ && upgrade_ && reset_ && offMain_ >= 0 && offShaqi_ >= 0;
        pbz::log::Info("影塔原生成长：背包 {} 能力组件 {}（激活 {} 取消 {}，节点表 {} 等级 {}）药 {}/{} 饰品 {}，ASC {}", ready_, !!nodesCls_, !!activate_, !!deactivate_,
                       offNodeMap_, offNodeLevel_, !!pushMed_, !!upMed_, !!pushAcc_, !!getAsc_);
        return ready_;
    }

    UObject* Bag() {
        if (!Resolve()) return nullptr;
        if (bag_ && ue::Live(bag_) && ue::ClassOf(bag_) == bagCls_) return bag_;
        bag_ = nullptr;
        ue::ForEach([&](int, UObject* o) { if (ue::ClassOf(o) == bagCls_ && !ue::IsDefault(o)) { bag_ = o; return false; } return true; });
        return bag_;
    }
    UObject* Pawn() {
        for (auto& t : attr::Targets()) if (t.who == attr::Who::Player && attr::Alive(t)) return t.obj;
        return nullptr;
    }
    uint64_t Name(std::string_view s) {
        if (!Resolve()) return 0;
        if (auto it = names_.find(std::string(s)); it != names_.end()) return it->second;
        std::wstring w(s.begin(), s.end());  // 原生 ID 全是 ASCII
        Args a(toName_);
        FArr str{w.data(), static_cast<int32_t>(w.size() + 1), static_cast<int32_t>(w.size() + 1)};
        std::memcpy(a.At("InString"), &str, sizeof str);
        pe::Call(strCdo_, toName_, a.b.data());
        const auto n = ue::Get<uint64_t>(a.At("ReturnValue"));
        if (n) names_[std::string(s)] = n;
        return n;
    }

    // 能力：玩家自己的组件（GetOwner 是玩家或其控制器；不能拿 _GEN_VARIABLE 模板）+ 玩家 ASC
    UObject* Nodes(UObject* pawn) {
        if (!nodesCls_ || !pawn) return nullptr;
        if (nodes_ && ue::Live(nodes_) && nodesOwner_ == pawn) return nodes_;
        nodes_ = nullptr; nodesOwner_ = pawn;
        auto* getOwner = pe::FindFunction(towerperf::ClassNamed("ActorComponent"), "GetOwner");
        if (!getOwner) return nullptr;
        const int offCtrl = ue::PropOffset(towerperf::ClassNamed("Pawn"), "Controller");
        UObject* ctrl = offCtrl >= 0 ? ue::Get<UObject*>(pawn + offCtrl) : nullptr;
        ue::ForEach([&](int, UObject* o) {
            if (!ue::IsA(o, nodesCls_) || ue::IsDefault(o)) return true;
            uint8_t r[16] = {};
            pe::Call(o, getOwner, r);
            UObject* owner = *reinterpret_cast<UObject**>(r);
            if (owner && (owner == pawn || owner == ctrl)) { nodes_ = o; return false; }
            return true;
        });
        return nodes_;
    }
    // Actor.GetComponentByClass：直接问 Actor 自己的组件，不扫对象表（开战时多只 Boss 逐个整表扫描会卡一下）
    UObject* Component(UObject* actor, UObject* cls) {
        if (!actor || !cls || !ue::Live(actor)) return nullptr;
        if (!getComp_) getComp_ = pe::FindFunction(towerperf::ClassNamed("Actor"), "GetComponentByClass");
        if (!getComp_) return nullptr;
        Args a(getComp_);
        std::memcpy(a.At("ComponentClass"), &cls, 8);
        pe::Call(actor, getComp_, a.b.data());
        UObject* c = ue::Get<UObject*>(a.At("ReturnValue"));
        return c && ue::Live(c) ? c : nullptr;
    }
    // 玩家是否在原生剧情镜头锁里（CharacterState.StoryCameraLock.*：Boss 开场台词期间玩家只能慢走）。
    // 读玩家自己的 ActorStateComponent（GetOwner 是玩家），InChildState / InExactState 只读查询。-1 = 接口不可用
    int StoryLocked() {
        UObject* pawn = Pawn();
        if (!Resolve() || !pawn) return -1;
        if (!stateCls_) {
            stateCls_ = towerperf::ClassNamed("ActorStateComponent");
            inChild_ = stateCls_ ? pe::FindFunction(stateCls_, "InChildState") : nullptr;
            inExact_ = stateCls_ ? pe::FindFunction(stateCls_, "InExactState") : nullptr;
        }
        if (!inChild_ || !inExact_) return -1;
        if (!state_ || !ue::Live(state_) || stateOwner_ != pawn) { stateOwner_ = pawn; state_ = Component(pawn, stateCls_); }
        if (!state_) return -1;
        auto ask = [&](UObject* fn, const char* param, const char* tag) {
            const uint64_t n = Name(tag);
            if (!n) return false;
            Args a(fn);
            std::memcpy(a.At(param), &n, 8);  // FGameplayTag{FName TagName}
            pe::Call(state_, fn, a.b.data());
            return ue::Get<uint8_t>(a.At("ReturnValue")) != 0;
        };
        return ask(inChild_, "ParentStateTag", "CharacterState.StoryCameraLock") || ask(inExact_, "StateTag", "CharacterState.StoryCameraLock") ||
               ask(inExact_, "StateTag", "CharacterState.StoryCameraLock.ForceWalk") ? 1 : 0;
    }
    // 玩家当前全部 ActorState（诊断用：51 层开场台词到底挂了什么状态）。GetCurrentStates 的输出数组由引擎分配，诊断期每秒一次，量很小
    // 3.6.0 反震：玩家当前锁定的敌人（TargetingComponent.LockTarget = {SceneComponent*, Socket}，取组件的 Owner）。只读；没锁定 / 读不到返回空
    UObject* LockTarget() {
        UObject* pawn = Pawn();
        if (!pawn) return nullptr;
        if (!targetingCls_) {
            targetingCls_ = towerperf::ClassNamed("TargetingComponent");
            lockOff_ = targetingCls_ ? ue::PropOffset(targetingCls_, "LockTarget") : -1;
            compOwner_ = pe::FindFunction(towerperf::ClassNamed("ActorComponent"), "GetOwner");
        }
        if (!targetingCls_ || lockOff_ < 0 || !compOwner_) return nullptr;
        if (!targeting_ || !ue::Live(targeting_) || targetingOwner_ != pawn) { targetingOwner_ = pawn; targeting_ = Component(pawn, targetingCls_); }
        if (!targeting_) return nullptr;
        UObject* comp = ue::Get<UObject*>(targeting_ + lockOff_);
        if (!comp || !ue::Live(comp)) return nullptr;
        uint8_t r[16] = {};
        pe::Call(comp, compOwner_, r);
        UObject* owner = *reinterpret_cast<UObject**>(r);
        return owner && owner != pawn && ue::Live(owner) ? owner : nullptr;
    }
    std::string PlayerStates() {
        UObject* pawn = Pawn();
        if (StoryLocked() < 0 || !state_) return "（读不到）";
        if (!getStates_) getStates_ = pe::FindFunction(stateCls_, "GetCurrentStates");
        if (!getStates_) return "（无 GetCurrentStates）";
        (void)pawn;
        Args a(getStates_);
        pe::Call(state_, getStates_, a.b.data());
        struct Arr { uint8_t* data; int32_t num, max; } arr{};
        std::memcpy(&arr, a.At("CurrentStates"), sizeof arr);
        std::string out;
        for (int i = 0; arr.data && i < arr.num && i < 32; ++i) {
            uint64_t n = 0;
            if (!ue::Read(arr.data + i * 8, &n, 8) || !(n & 0xFFFFFFFF)) continue;
            out += (out.empty() ? "" : "、") + ue::Name(static_cast<uint32_t>(n));
        }
        return out.empty() ? "（空）" : out;
    }
    // 让 Boss 自己的 AI 组件收到"玩家进入领域"（原生遭遇开打的入口）。塔静默了战斗组的领域事件，
    // 左殇真龙之躯这类只认领域、不靠视觉的 Boss 会一直站着等挨打（用户 2026-11-06，51 层）。
    // 只调 Boss 自己的 SGameAIComponent（组件拥有者是 Boss 或它的控制器），不碰战斗组。返回 1 已调 / 0 没找到组件 / -1 接口不可用
    int EnterDomain(UObject* boss) {
        UObject* pawn = Pawn();
        if (!Resolve() || !pawn || !boss || !ue::Live(boss)) return -1;
        if (!aiCls_) { aiCls_ = towerperf::ClassNamed("SGameAIComponent"); enterDomain_ = aiCls_ ? pe::FindFunction(aiCls_, "OnActorEnterDomain") : nullptr; }
        if (!aiCls_ || !enterDomain_) return -1;
        const int offCtrl = ue::PropOffset(towerperf::ClassNamed("Pawn"), "Controller");
        UObject* ctrl = offCtrl >= 0 ? ue::Get<UObject*>(boss + offCtrl) : nullptr;
        UObject* comp = Component(boss, aiCls_);
        if (!comp && ctrl && ue::Live(ctrl)) comp = Component(ctrl, aiCls_);
        if (!comp) return 0;
        Args a(enterDomain_);
        std::memcpy(a.At("OtherActor"), &pawn, 8);
        pe::Call(comp, enterDomain_, a.b.data());
        return 1;
    }
    UObject* Asc(UObject* pawn) {
        if (!getAsc_ || !asCdo_ || !pawn) return nullptr;
        Args a(getAsc_);
        std::memcpy(a.At("Actor"), &pawn, 8);
        pe::Call(asCdo_, getAsc_, a.b.data());
        return ue::Get<UObject*>(a.At("ReturnValue"));
    }
    UObject* NodeObj(std::string_view id) {
        auto* comp = Nodes(Pawn());
        if (!comp || offNodeMap_ < 0) return nullptr;
        const uint64_t key = Name(id);
        for (auto& [k, v] : NameObjMap(comp + offNodeMap_)) if (k == key) return v;
        return nullptr;
    }
    bool Ach(bool block) { towerprobe::Ach(block ? "on" : "off"); return true; }

    // ---- IGame ----
    bool BackupSave(std::string* where) override { return backup ? backup(where) : (*where = "没有备份接口", false); }
    bool Sandbox(bool on) override {
        std::string why;
        if (on) {
            if (!towersb::Active() && (!towersb::Set(true, &why, false) || !towergate::Veto(true, &why))) { pbz::log::Info("影塔原生成长：开沙盒失败：{}", why); return false; }
            if (!towergate::Snapshot(&why)) { pbz::log::Info("影塔原生成长：进塔基准取不到：{}", why); return false; }
            return true;
        }
        if (!towersb::Set(false, &why, false)) { pbz::log::Info("影塔原生成长：关沙盒被拒绝：{}", why); return false; }
        towergate::Veto(false, &why);
        return true;
    }
    // 出塔：把进塔基准放回游戏内存（2026-10-31 实机：restore → 回标题 → 继续，成长逐项回到基准）
    bool SandboxReadThrough() override {
        std::string why;
        if (!towergate::Restore(&why)) { pbz::log::Info("影塔原生成长：还原基准失败：{}", why); return false; }
        return true;
    }
    void Achievements(bool blocked) override { Ach(blocked); }

    int WeaponLevel(std::string_view id, int kind) override {
        UObject* bag = Bag();
        if (!bag) return -1;
        const uint64_t n = Name(id);
        Args h(has_);
        std::memcpy(h.At("WeaponID"), &n, 8);
        *h.At("bIsNormalWeapon") = kind == 0;
        pe::Call(bag, has_, h.b.data());
        if (!*h.At("ReturnValue")) return -1;
        Args l(level_);
        std::memcpy(l.At("WeaponID"), &n, 8);
        *l.At("bIsNormalWeapon") = kind == 0;
        pe::Call(bag, level_, l.b.data());
        return ue::Get<int32_t>(l.At("ReturnValue"));
    }
    // 升：UpgradeWeapon(ID, 普通, N) 免费加 N 级（实机）；降：ResetWeaponToLevel（实机降级，返还物塔不入账）
    bool SetWeaponLevel(std::string_view id, int kind, int level) override {
        towerpoints::Internal internal;
        UObject* bag = Bag();
        const int now = WeaponLevel(id, kind);
        if (!bag || now < 0) return false;
        if (level == now) return true;
        const uint64_t n = Name(id);
        if (level > now) {
            Args a(upgrade_);
            std::memcpy(a.At("WeaponID"), &n, 8);
            *a.At("IsNormalWeapon") = kind == 0;
            const int32_t d = level - now;
            std::memcpy(a.At("UpgradeLevel"), &d, 4);
            pe::Call(bag, upgrade_, a.b.data());
        } else {
            Args a(reset_);
            std::memcpy(a.At("WeaponID"), &n, 8);
            *a.At("bIsNormalWeapon") = kind == 0;
            const int32_t t = std::max(1, level);
            std::memcpy(a.At("TargetLevel"), &t, 4);
            pe::Call(bag, reset_, a.b.data());  // 返还的材料表由引擎分配，留在缓冲里不释放（模块没有引擎分配器）
        }
        const int after = WeaponLevel(id, kind);
        if (after != level) pbz::log::Info("影塔原生成长：{} 等级 {} → 目标 {}，读回 {}", id, now, level, after);
        return after == level;
    }
    bool GrantWeapon(std::string_view id, int kind) override {
        if (kind!=0 && kind!=1) return false;
        if (WeaponLevel(id,kind)>=0) return true; // No duplicate grant/popup for an already-held weapon.
        auto* bag=Bag();
        if (!towerweapongrant::Grant(bag,Name(id),kind==0)) return false;
        const bool ok = WeaponLevel(id, kind) >= 0;
        pbz::log::Info("影塔原生成长：发放兵器 {} → {}（原生获得界面；{}）",id,ok?"已持有":"未持有",ok?"实际持有已读回":"勿重复授予，保留现场核查");
        return ok;
    }
    // 3.1.0 驿站交易：PBZBackPackSubsystem.RemoveWeaponByID(WeaponID, bIsNormalWeapon)（SDK 有，待实机）；读回不再持有才算成功
    bool RemoveWeapon(std::string_view id, int kind) override {
        if (kind != 0 && kind != 1) return false;
        if (WeaponLevel(id, kind) < 0) return true;
        UObject* bag = Bag();
        static UObject* fn = nullptr;
        if (!fn && bagCls_) fn = pe::FindFunction(bagCls_, "RemoveWeaponByID");
        if (!bag || !fn) { pbz::log::Info("影塔交易：原生删兵器接口不可用"); return false; }
        Args a(fn);
        if (ue::PropOffset(fn, "WeaponID") < 0 || ue::PropOffset(fn, "bIsNormalWeapon") < 0) { pbz::log::Info("影塔交易：RemoveWeaponByID 参数不匹配"); return false; }
        const uint64_t n = Name(id);
        std::memcpy(a.At("WeaponID"), &n, 8);
        *a.At("bIsNormalWeapon") = kind == 0;
        pe::Call(bag, fn, a.b.data());
        // 3.3.0（用户：回收后武器还在背包、还能用）：RemoveWeaponByID 只卸下装备栏，背包信息表还在 → 删表项，读回核实
        bool gone = WeaponLevel(id, kind) < 0;
        if (!gone) {
            const int off = ue::PropOffset(bagCls_, kind == 0 ? "MainWeaponInfos" : "ShaqiWeaponInfos");
            const bool erased = off >= 0 && EraseNameKey(bag + off, n);
            gone = WeaponLevel(id, kind) < 0;
            pbz::log::Info("影塔交易：原生删兵器 {} → 卸下后背包表仍有，删表项{}，读回{}", id, erased ? "成功" : "失败", gone ? "已不持有" : "仍持有（不成交）");
        } else pbz::log::Info("影塔交易：原生删兵器 {} → 已删除", id);
        return gone;
    }
    // N9i 驿站回收药 / 饰品：RemovePropsFromBackPack(Num, ItemId, bSendBroadcast)（SDK 有，待实机）。读回只记日志（兵器同帧读回不准的教训），接口调到就算成交
    bool RemoveProp(std::string_view id, const char* what) {
        UObject* bag = Bag();
        static UObject* fn = nullptr;
        if (!fn && bagCls_) fn = pe::FindFunction(bagCls_, "RemovePropsFromBackPack");
        if (!bag || !fn || ue::PropOffset(fn, "ItemId") < 0 || ue::PropOffset(fn, "Num") < 0) { pbz::log::Info("影塔交易：原生删{}接口不可用", what); return false; }
        Args a(fn);
        const uint64_t n = Name(id); const int32_t one = 1;
        std::memcpy(a.At("ItemId"), &n, 8); std::memcpy(a.At("Num"), &one, 4);
        if (a.At("bSendBroadcast")) *a.At("bSendBroadcast") = 1;
        pe::Call(bag, fn, a.b.data());
        return true;
    }
    // N9j（实机：回收药葫芦后药栏里还在）：药不是道具，背包没有删药接口。做法：
    //   1) 两个药格里是这瓶的先卸下（TakeOffMedicineByIndex）；2) 用剩下的药重建药表（InitMedicines）；
    //   3) 剩下的药按原等级补回、另一格原来装的重新装上。读回仍持有就算失败（不成交，塔档不扣）
    bool RemoveMedicine(std::string_view id) override {
        if (!HasMedicine(id)) return true;
        UObject* bag = Bag();
        auto fn = [this](const char* n) { return bagCls_ ? pe::FindFunction(bagCls_, n) : nullptr; };
        static UObject *getIdx = nullptr, *takeOff = nullptr, *init = nullptr, *replace = nullptr;
        if (!getIdx) getIdx = fn("GetMedicineByIndex");
        if (!takeOff) takeOff = fn("TakeOffMedicineByIndex");
        if (!init) init = fn("InitMedicines");
        if (!replace) replace = fn("ReplaceMedicineByIndex");
        if (!bag || !getIdx || !init || offMeds_ < 0) { pbz::log::Info("影塔交易：原生删药接口不可用"); return false; }
        const uint64_t key = Name(id);
        auto slotOf = [&](int idx) { Args a(getIdx); std::memcpy(a.At("Index"), &idx, 4); pe::Call(bag, getIdx, a.b.data()); uint64_t n = 0; std::memcpy(&n, a.At("ReturnValue"), 8); return n; };
        const uint64_t slot[2] = {slotOf(0), slotOf(1)};
        std::vector<uint64_t> keep; std::vector<std::pair<uint64_t, int>> levels;
        for (auto& [k, v] : NameObjMap(bag + offMeds_)) if (k != key) { keep.push_back(k); levels.push_back({k, v && offMedLevel_ >= 0 ? ue::Get<int32_t>(v + offMedLevel_) : 0}); }
        if (takeOff) for (int idx = 0; idx < 2; ++idx) if (slot[idx] == key) { Args a(takeOff); std::memcpy(a.At("Index"), &idx, 4); pe::Call(bag, takeOff, a.b.data()); }
        {
            Args a(init);
            struct Arr { uint64_t* data; int32_t num, max; } arr{keep.data(), static_cast<int32_t>(keep.size()), static_cast<int32_t>(keep.size())};
            if (ue::PropOffset(init, "MedicineIDs") < 0) { pbz::log::Info("影塔交易：InitMedicines 参数不匹配"); return false; }
            std::memcpy(a.At("MedicineIDs"), &arr, sizeof arr);
            { towerisolate::GrantScope grant; pe::Call(bag, init, a.b.data()); }
            std::memset(a.At("MedicineIDs"), 0, sizeof arr);  // 数组是我们的内存，不让参数析构碰它
        }
        // 等级补回（原生从 0 起；UpgradeMedicine 是增量）
        for (auto& [k, lv] : levels) {
            UObject* info = nullptr;
            for (auto& [k2, v] : NameObjMap(bag + offMeds_)) if (k2 == k) info = v;
            const int now = info && offMedLevel_ >= 0 ? ue::Get<int32_t>(info + offMedLevel_) : -1;
            if (now >= 0 && now < lv && upMed_) { Args a(upMed_); std::memcpy(a.At("MedicineID"), &k, 8); const int32_t d = lv - now; std::memcpy(a.At("UpgradeLevel"), &d, 4); towerpoints::Internal internal; towerisolate::GrantScope grant; pe::Call(bag, upMed_, a.b.data()); }
        }
        // 另一格原来装的重新装上
        if (replace) for (int idx = 0; idx < 2; ++idx) if (slot[idx] && slot[idx] != key && slotOf(idx) != slot[idx]) {
            Args a(replace); std::memcpy(a.At("Index"), &idx, 4); std::memcpy(a.At("NewMedicineID"), &slot[idx], 8);
            if (a.At("bRefillToFull")) *a.At("bRefillToFull") = 1;
            pe::Call(bag, replace, a.b.data());
        }
        const bool gone = !HasMedicine(id);
        pbz::log::Info("影塔交易：原生删药 {} → {}（药表剩 {} 种；药格 {} / {}）", id, gone ? "已删除" : "读回仍持有，未成交", keep.size(), ue::Name(static_cast<uint32_t>(slotOf(0))), ue::Name(static_cast<uint32_t>(slotOf(1))));
        return gone;
    }
    // N9j：饰品先从饰品格卸下（TakeOffAccessory），再按道具删（RemovePropsFromBackPack）；读回饰品表（直接读 map，可靠）仍有就算失败、不成交
    bool RemoveAccessory(std::string_view id) override {
        if (!HasAccessory(id)) return true;
        UObject* bag = Bag();
        static UObject *getId = nullptr, *takeOff = nullptr;
        if (!getId && bagCls_) getId = pe::FindFunction(bagCls_, "GetAccessoryID");
        if (!takeOff && bagCls_) takeOff = pe::FindFunction(bagCls_, "TakeOffAccessory");
        const uint64_t key = Name(id);
        if (bag && getId && takeOff) for (int idx = 0; idx < 8; ++idx) {
            Args g(getId); std::memcpy(g.At("Index"), &idx, 4); pe::Call(bag, getId, g.b.data());
            uint64_t n = 0; std::memcpy(&n, g.At("ReturnValue"), 8);
            if (n == key) { Args t(takeOff); std::memcpy(t.At("Index"), &idx, 4); pe::Call(bag, takeOff, t.b.data()); }
        }
        if (!RemoveProp(id, "饰品")) return false;
        const bool gone = !HasAccessory(id);
        pbz::log::Info("影塔交易：原生删饰品 {} → {}", id, gone ? "已删除" : "读回仍持有，未成交");
        return gone;
    }
    // 3.3.2：回收 / 熔铸掉正在装备的兵器后，当前手持（CurrentWeaponIndex / CurrentShaqiWeaponIndex）可能指向空槽，
    // 原生界面 / HUD 读到空兵器会卡住。切到第一把还在的兵器（SetCurrentWeaponByIndex）
    // 3.3.4：PBZBackPackSubsystem.AddShaChiWeaponBullet(当前影之武装 ID, n)；上限交给原生。读回 GetCurrentShaqiWeaponBulletNum
    int ShaqiBullets() override {
        UObject* bag = Bag();
        if (!bag || !bagCls_) return -1;
        static UObject* cur = nullptr;
        if (!cur) cur = pe::FindFunction(bagCls_, "GetCurrentShaqiWeaponBulletNum");
        if (!cur) return -1;
        Args a(cur); pe::Call(bag, cur, a.b.data()); int32_t v = -1; std::memcpy(&v, a.At("ReturnValue"), 4);
        return v;
    }
    int AddShaqiBullet(int n) override {
        UObject* bag = Bag();
        if (!bag || !bagCls_ || n <= 0) return -1;
        static UObject *add = nullptr, *cur = nullptr;
        if (!add) add = pe::FindFunction(bagCls_, "AddShaChiWeaponBullet");
        if (!cur) cur = pe::FindFunction(bagCls_, "GetCurrentShaqiWeaponBulletNum");
        const auto l = Read();
        const int off = ue::PropOffset(bagCls_, "CurrentShaqiWeaponIndex");
        const int at = off >= 0 ? ue::Get<int32_t>(bag + off, -1) : -1;
        if (!add || !l.ok || at < 0 || at >= static_cast<int>(l.shaqi.size()) || l.shaqi[at].empty() || tower::ids::Equal(l.shaqi[at], "None")) {
            static int bad = 0;
            if (++bad <= 10) pbz::log::Info("影塔暗源：加暗源失败（函数 {}，背包 {}，当前影之武装槽 {} / {}）", add != nullptr, l.ok, at, l.shaqi.size());
            return -1;
        }
        auto now = [&] { if (!cur) return -1; Args a(cur); pe::Call(bag, cur, a.b.data()); int32_t v = -1; std::memcpy(&v, a.At("ReturnValue"), 4); return static_cast<int>(v); };
        const int before = now();
        Args a(add);
        const uint64_t id = Name(l.shaqi[at]);
        std::memcpy(a.At("ShaChiWeaponID"), &id, 8); std::memcpy(a.At("BulletNum"), &n, 4);
        { towerisolate::GrantScope grant; pe::Call(bag, add, a.b.data()); }
        const int after = now();
        static int logs = 0;
        if (++logs <= 20) pbz::log::Info("影塔暗源：{} +{}，暗源 {} → {}", l.shaqi[at], n, before, after);
        return after;
    }
    void FixCurrentSlots() override {
        UObject* bag = Bag();
        if (!bag || !bagCls_) return;
        static UObject* setIdx = nullptr;
        if (!setIdx) setIdx = pe::FindFunction(bagCls_, "SetCurrentWeaponByIndex");
        const auto l = Read();
        if (!l.ok) return;
        for (int normal = 1; normal >= 0; --normal) {
            const auto& slots = normal ? l.main : l.shaqi;
            const int off = ue::PropOffset(bagCls_, normal ? "CurrentWeaponIndex" : "CurrentShaqiWeaponIndex");
            if (off < 0) continue;
            const int at = ue::Get<int32_t>(bag + off, -1);
            auto filled = [&](int i) { return i >= 0 && i < static_cast<int>(slots.size()) && !slots[i].empty() && !tower::ids::Equal(slots[i], "None"); };
            if (filled(at)) continue;
            int to = -1;
            for (int i = 0; i < static_cast<int>(slots.size()); ++i) if (filled(i)) { to = i; break; }
            if (to < 0) continue;
            if (setIdx && ue::PropOffset(setIdx, "WeaponIndex") >= 0) {
                Args a(setIdx); std::memcpy(a.At("WeaponIndex"), &to, 4);
                if (a.At("IsNormalWeapon")) *a.At("IsNormalWeapon") = static_cast<uint8_t>(normal);
                pe::Call(bag, setIdx, a.b.data());
            }
            if (ue::Get<int32_t>(bag + off, -1) != to) ue::Write(bag + off, &to, 4);
            pbz::log::Info("影塔交易：{} 当前手持指向空槽 {}，已切到第 {} 槽", normal ? "主兵器" : "影之武装", at, to);
        }
    }
    bool SnapWeapon(std::string_view id, int kind) override {  // 未实测：调用后读回 IsWeaponSnapSouled
        towerpoints::Internal internal;
        UObject* bag = Bag();
        if (!bag || !snap_) return false;
        const uint64_t n = Name(id);
        Args a(snap_);
        std::memcpy(a.At("WeaponID"), &n, 8);
        *a.At("IsNormalWeapon") = kind == 0;
        pe::Call(bag, snap_, a.b.data());
        bool ok = false;
        if (snapped_) {
            Args q(snapped_);
            std::memcpy(q.At("WeaponID"), &n, 8);
            *q.At("IsNormalWeapon") = kind == 0;
            pe::Call(bag, snapped_, q.b.data());
            ok = *q.At("ReturnValue") != 0;
        }
        pbz::log::Info("影塔原生成长：铸魂 {} → {}（未实测接口）", id, ok ? "已铸魂" : "读回未铸魂");
        return ok;
    }
    int Snapped(std::string_view id, int kind) {
        auto* bag = Bag();
        if (!bag || !snapped_) return -1;
        Args q(snapped_); const auto n = Name(id);
        std::memcpy(q.At("WeaponID"), &n, 8); *q.At("IsNormalWeapon") = kind == 0;
        pe::Call(bag, snapped_, q.b.data()); return *q.At("ReturnValue") ? 1 : 0;
    }
    std::vector<std::string> OwnedWeapons(int kind) override {
        std::vector<std::string> out;
        for (const auto& w : tower::growth::kWeapons)
            if (w.kind == kind && WeaponLevel(w.id, kind) >= 0) out.push_back(std::string(w.id));
        return out;
    }
    int NodeLevel(std::string_view id) override {
        UObject* node = NodeObj(id);
        return node && offNodeLevel_ >= 0 ? ue::Get<int32_t>(node + offNodeLevel_) : (Nodes(Pawn()) ? 0 : -1);
    }
    // 升：ActivateNode 每次 +1 级（实机：取玩家组件 + 玩家 ASC）；降：DeactivateNode 后再升回目标（每步读回）
    bool SetNodeLevel(std::string_view id, int level) override {
        towerpoints::Internal internal;
        UObject* pawn = Pawn();
        UObject* comp = Nodes(pawn);
        UObject* asc = Asc(pawn);
        if (!comp || !asc || !activate_) return false;
        const uint64_t n = Name(id);
        int now = NodeLevel(id);
        for (int guard = 0; now > level && deactivate_ && guard < 4; ++guard) {
            Args a(deactivate_);
            std::memcpy(a.At("NodeID"), &n, 8);
            std::memcpy(a.At("ASC"), &asc, 8);
            pe::Call(comp, deactivate_, a.b.data());
            const int after = NodeLevel(id);
            if (after == now) break;  // 不再变化：停下，别死循环
            now = after;
        }
        // DeactivateNode 未实测：降不下来就用实机验证过的 DeactivateAllNodes，再把其余节点按原等级点回去
        if (now > level && deactivateAll_) {
            std::vector<std::pair<std::string, int>> keep;
            for (const auto& d : tower::growth::kNodes)
                if (d.id != id) if (const int lv = NodeLevel(d.id); lv > 0) keep.push_back({std::string(d.id), lv});
            Args a(deactivateAll_);
            std::memcpy(a.At("ASC"), &asc, 8);
            pe::Call(comp, deactivateAll_, a.b.data());
            now = NodeLevel(id);
            pbz::log::Info("影塔原生成长：节点 {} 单独取消无效，改为全部取消后重点 {} 个节点", id, keep.size());
            for (auto& [other, lv] : keep) Raise(comp, asc, other, lv);
        }
        now = Raise(comp, asc, id, level);
        if (now != level) pbz::log::Info("影塔原生成长：节点 {} 目标 {} 读回 {}", id, level, now);
        return now == level;
    }
    // ActivateNode 每次 +1 级，直到目标或不再变化；返回读回的等级
    int Raise(UObject* comp, UObject* asc, std::string_view id, int level) {
        const uint64_t n = Name(id);
        int now = NodeLevel(id);
        for (int guard = 0; now < level && guard < 40; ++guard) {
            Args a(activate_);
            std::memcpy(a.At("NodeID"), &n, 8);
            std::memcpy(a.At("ASC"), &asc, 8);
            pe::Call(comp, activate_, a.b.data());
            const int after = NodeLevel(id);
            if (after <= now) break;
            now = after;
        }
        return now;
    }
    UObject* MedInfo(std::string_view id) {
        UObject* bag = Bag();
        if (!bag || offMeds_ < 0) return nullptr;
        const uint64_t key = Name(id);
        for (auto& [k, v] : NameObjMap(bag + offMeds_)) if (k == key) return v;
        return nullptr;
    }
    bool HasMedicine(std::string_view id) override { return MedInfo(id) != nullptr; }
    // 禁药挑战：全部药瓶的当前剩余次数（MedicineInfo.CurrentTime）之和；读不到 -1
    int MedicineUsesLeft() override {
        UObject* bag = Bag();
        if (!bag || offMeds_ < 0 || offMedTime_ < 0) return -1;
        int sum = 0;
        for (auto& [k, v] : NameObjMap(bag + offMeds_)) if (v && ue::Live(v)) sum += std::max(0, ue::Get<int32_t>(v + offMedTime_));
        return sum;
    }
    bool GrantMedicine(std::string_view id) override {
        UObject* bag = Bag();
        if (!bag || !pushMed_) return false;
        const uint64_t n = Name(id);
        Args a(pushMed_);
        std::memcpy(a.At("MedicineID"), &n, 8);
        *a.At("bAutoEquip") = 0;
        *a.At("bShowPickupUI") = 1;  // 原生"获得物品"弹窗
        { towerisolate::GrantScope grant; pe::Call(bag, pushMed_, a.b.data()); }
        return HasMedicine(id);
    }
    // 原生 CurrentLevel 从 0 起（药葫芦 0 级 = 2 次，2026-10-31 基准存档核对）；塔档药瓶等级从 1 起
    int MedicineLevel(std::string_view id) override {
        UObject* info = MedInfo(id);
        return info && offMedLevel_ >= 0 ? ue::Get<int32_t>(info + offMedLevel_) + 1 : -1;
    }
    // UpgradeMedicine 静态语义是增量：只往上加，加完读回；降级没有原生接口（出塔由基准还原）
    bool SetMedicineLevel(std::string_view id, int level) override {
        towerpoints::Internal internal;
        UObject* bag = Bag();
        const int now = MedicineLevel(id);
        if (!bag || now < 0 || !upMed_) return false;
        if (level <= now) return level == now;
        const uint64_t n = Name(id);
        Args a(upMed_);
        std::memcpy(a.At("MedicineID"), &n, 8);
        const int32_t d = level - now;
        std::memcpy(a.At("UpgradeLevel"), &d, 4);
        { towerisolate::GrantScope grant; pe::Call(bag, upMed_, a.b.data()); }
        const int after = MedicineLevel(id);
        if (after != level) pbz::log::Info("影塔原生成长：药 {} 等级 {} → 目标 {}，读回 {}（UpgradeMedicine 未实测）", id, now, level, after);
        return after == level;
    }
    // 回归基本功：所有药瓶剩余次数清零（每层开打时塔会照常补满，持有期间每 0.5 秒再清一次）
    void ZeroMedicineUses() {
        UObject* bag = Bag();
        if (!bag || offMeds_ < 0 || offMedTime_ < 0) return;
        const int32_t zero = 0;
        for (auto& [k, v] : NameObjMap(bag + offMeds_)) if (v && ue::Live(v) && ue::Get<int32_t>(v + offMedTime_) != 0) ue::Write(v + offMedTime_, &zero, 4);
    }
    UObject* AccInfo(std::string_view id) {
        UObject* bag = Bag();
        if (!bag || offAccs_ < 0) return nullptr;
        const uint64_t key = Name(id);
        for (auto& [k, v] : NameObjMap(bag + offAccs_)) if (k == key) return v;
        return nullptr;
    }
    int AccessoryLevel(std::string_view id) override {
        if (offAccLevel_ < 0 && accInfoCls_ == nullptr) { accInfoCls_ = towerperf::ClassNamed("PBZAccessoryInfo"); offAccLevel_ = accInfoCls_ ? ue::PropOffset(accInfoCls_, "CurrentLevel") : -1; }
        UObject* info = AccInfo(id);
        return info && ue::Live(info) && offAccLevel_ >= 0 ? ue::Get<int32_t>(info + offAccLevel_) + 1 : -1;
    }
    // 原生可打磨上限（塔内约定 +1）；接口缺失返回 -1
    int AccessoryMaxLevel(std::string_view id) {
        UObject* bag = Bag();
        if (!bag || !accMax_) return -1;
        const uint64_t n = Name(id);
        Args a(accMax_);
        std::memcpy(a.At("AccessoryID"), &n, 8);
        pe::Call(bag, accMax_, a.b.data());
        return ue::Get<int32_t>(a.At("ReturnValue")) + 1;
    }
    bool SetAccessoryLevel(std::string_view id, int level) override {
        towerpoints::Internal internal;
        UObject* bag = Bag();
        const int now = AccessoryLevel(id);
        if (!bag || now < 0 || !upAcc_) return false;
        if (level <= now) return level == now;
        const uint64_t n = Name(id);
        Args a(upAcc_);
        std::memcpy(a.At("AccessoryID"), &n, 8);
        const int32_t d = level - now;
        std::memcpy(a.At("UpgradeLevel"), &d, 4);
        { towerisolate::GrantScope grant; pe::Call(bag, upAcc_, a.b.data()); }
        const int after = AccessoryLevel(id);
        pbz::log::Info("影塔原生成长：饰品 {} 等级 {} → 目标 {}，读回 {}", id, now, level, after);
        return after == level;
    }
    bool HasAccessory(std::string_view id) override {
        UObject* bag = Bag();
        if (!bag || offAccs_ < 0) return false;
        const uint64_t key = Name(id);
        for (auto& [k, v] : NameObjMap(bag + offAccs_)) if (k == key) return true;
        return false;
    }
    bool GrantAccessory(std::string_view id) override {
        UObject* bag = Bag();
        if (!bag || !pushAcc_) return false;
        if (HasAccessory(id)) return true;
        const uint64_t n = Name(id);
        // v14（用户 2026-11-05：饰品也要有获得窗口）：PushAccessoryToInventory 没有弹窗参数；先走原生道具发放
        // AddPropsToBackPack(bPushWidget=1)（原生拾取 / GetItems 同一入口），读回饰品到账才算；没到账再退回无弹窗的饰品接口
        if (addProps_ && ue::PropOffset(addProps_, "ItemId") >= 0 && ue::PropOffset(addProps_, "Num") >= 0) {
            Args w(addProps_);
            std::memcpy(w.At("ItemId"), &n, 8);
            const int32_t one = 1;
            std::memcpy(w.At("Num"), &one, 4);
            if (w.At("bPushWidget")) *w.At("bPushWidget") = 1;
            { towerisolate::GrantScope grant; pe::Call(bag, addProps_, w.b.data()); }
            if (HasAccessory(id)) { pbz::log::Info("影塔原生成长：发放饰品 {}（原生获得界面，已读回）", id); return true; }
            pbz::log::Info("影塔原生成长：饰品 {} 走道具发放未到账，改用饰品接口（无获得界面）", id);
        }
        Args a(pushAcc_);
        std::memcpy(a.At("AccessoryID"), &n, 8);
        *a.At("bAutoEquip") = 0;
        { towerisolate::GrantScope grant; pe::Call(bag, pushAcc_, a.b.data()); }
        return HasAccessory(id);
    }
    // 道具持有数（GetItemNumInfo 读回 CurrentNum）；接口缺失 / 参数对不上返回 -1
    int ItemCount(std::string_view id) override {
        UObject* bag = Bag();
        if (!bag || !itemNum_ || ue::PropOffset(itemNum_, "PropID") < 0 || ue::PropOffset(itemNum_, "CurrentNum") < 0) return -1;
        const uint64_t n = Name(id);
        Args a(itemNum_);
        std::memcpy(a.At("PropID"), &n, 8);
        pe::Call(bag, itemNum_, a.b.data());
        int32_t cur = 0; std::memcpy(&cur, a.At("CurrentNum"), 4);
        return cur;
    }
    // Boss 原生道具掉落：原生 AddPropsToBackPack（bPushWidget=1 弹原生获得界面），读回持有数增加才算发到
    bool GrantItem(std::string_view id, int count) override {
        UObject* bag = Bag();
        if (!bag || !addProps_ || count <= 0 || ue::PropOffset(addProps_, "ItemId") < 0 || ue::PropOffset(addProps_, "Num") < 0) return false;
        const int before = ItemCount(id);
        if (before < 0) return false;
        const uint64_t n = Name(id);
        Args a(addProps_);
        std::memcpy(a.At("ItemId"), &n, 8);
        const int32_t num = count;
        std::memcpy(a.At("Num"), &num, 4);
        *a.At("bPushWidget") = 1;
        { towerisolate::GrantScope grant; pe::Call(bag, addProps_, a.b.data()); }
        const int after = ItemCount(id);
        pbz::log::Info("影塔Boss掉落：道具 {} ×{} → 持有 {} → {}（原生获得界面；{}）", id, count, before, after, after > before ? "已读回" : "未读回，勿重复授予");
        return after > before;
    }
    // 出塔第二步由玩家完成（实机通过的路径）：回标题 → 继续。这里记下旧背包，等新背包出现、玩家稳定 3 秒算进完
    bool ReloadFromTitle() override { Arm(false); return true; }
    // 开始等"重新载入"：记下当前背包；keepTitle=true（在标题上重新放存档）时保留"已到过标题"
    void Arm(bool keepTitle) {
        reloadBag_ = bag_ && ue::Live(bag_) ? bag_ : nullptr;
        if (!keepTitle) titleSeen_ = false;
        stableSince_ = 0;
    }
    // 标题画面：标题世界（PBZ_StartMenu…，2026-10-31 实机关卡名 PBZ_StartMenu_CE_WP）存在且没有玩家角色
    bool AtTitle() {
        if (Pawn()) return false;
        if (!worldCls_) worldCls_ = towerperf::ClassNamed("World");
        bool title = false;
        if (worldCls_)
            ue::ForEach([&](int, UObject* o) {
                if (ue::ClassOf(o) != worldCls_ || !ue::Live(o)) return true;
                if (ue::ObjName(o).rfind("PBZ_StartMenu", 0) == 0) { title = true; return false; }
                return true;
            });
        if (title) titleSeen_ = true;
        return title;
    }
    // 重新载入完成：Arm 之后到过标题，现在背包是新对象、玩家在且稳定 3 秒（跨图传送不经过标题，不算）
    bool ReloadFinished() override {
        bag_ = nullptr;  // 每次重新找：回标题后旧背包会被释放
        UObject* bag = Bag();
        UObject* pawn = Pawn();
        const bool fresh = titleSeen_ && bag && bag != reloadBag_ && pawn;
        if (!fresh) { stableSince_ = 0; return false; }
        if (!stableSince_) stableSince_ = GetTickCount64();
        return GetTickCount64() - stableSince_ >= 3000;
    }

    // 核验：游戏里的实际内容是否就是这份存档（装备、兵器持有与等级、药、饰品、能力、尘、升级材料）。
    // diff：前几处不一致。饰品里塔池排除的权益件（聚宝盆等由原生权益补发）只要求"有则一致"，不当成差异
    bool Verify(const std::string& expect, std::string* diff) {
        tower::json::Value e;
        if (!tower::json::Parse(expect, &e)) { *diff = "目标存档解析失败"; return false; }
        const auto* inv = e.Find("Inventory");
        const auto* player = e.Find("Player");
        const auto* nodesSec = e.Find("AbilityNode_RenewalHun");
        const auto* nodes = nodesSec ? nodesSec->Find("AbilityNodes") : nullptr;
        if (!inv || !player || !nodes) { *diff = "目标存档缺少 Inventory / Player / AbilityNodes"; return false; }
        std::vector<std::string> bad;
        auto note = [&](std::string s) { if (bad.size() < 8) bad.push_back(std::move(s)); };
        const auto live = Read();
        if (!live.ok) { *diff = "读不到背包"; return false; }
        auto arr = [](const tower::json::Value* a) { std::vector<std::string> v; if (a) for (auto& x : a->items) v.push_back(x.text); return v; };
        if (!tower::ids::SameSlots(arr(inv->Find("MainWeapons")), live.main)) note("主兵器槽");
        if (!tower::ids::SameSlots(arr(inv->Find("ShaqiWeapons")), live.shaqi)) note("影之武装槽");
        auto accessories = arr(inv->Find("AccessoryIDs"));
        std::erase_if(accessories, [](const std::string& id) { return id.empty() || tower::ids::Equal(id, "None"); });
        if (!tower::ids::Equal(accessories, live.accessories)) note("饰品装备槽");
        // 当前手持槽（CurrentWeaponIndex / CurrentShaqiWeaponIndex）：载入后游戏自己会改（2026-11-05 ui5 按保存装备继续时读回不是 0），
        // 不是存档内容；只要求指向一个有兵器的槽
        for (const char* field : {"CurrentWeaponIndex", "CurrentShaqiWeaponIndex"}) if (inv->Find(field)) {
            const int offset = ue::PropOffset(bagCls_, field);
            const auto& slots = std::string_view(field) == "CurrentWeaponIndex" ? live.main : live.shaqi;
            const int at = offset >= 0 ? ue::Get<int32_t>(Bag() + offset, -1) : -1;
            const bool anyWeapon = std::any_of(slots.begin(), slots.end(), [](const std::string& x) { return !x.empty() && !tower::ids::Equal(x, "None"); });
            const bool ok = !anyWeapon || (at >= 0 && at < static_cast<int>(slots.size()) && !slots[at].empty() && !tower::ids::Equal(slots[at], "None"));
            if (!ok) pbz::log::Info("影塔塔档核验：当前装备索引 {} = {} 指向空槽（游戏载入后自行调整，不作为差异）", field, at);
        }
        auto str = [](const tower::json::Value* v) { return v && v->kind == tower::json::Value::String ? v->text : std::string(); };
        auto med = [](std::string s) { return s.empty() ? std::string("None") : s; };
        if (!tower::ids::Equal(med(str(inv->Find("BottomMedicine"))), med(live.medBottom)) || !tower::ids::Equal(med(str(inv->Find("TopMedicine"))), med(live.medTop))) note("药瓶槽");
        const auto* mains = inv->Find("MainWeaponInfos");
        const auto* shaqi = inv->Find("ShaChiWeaponInfo");
        for (const auto& w : tower::growth::kWeapons) {
            const auto* infos = w.kind == 0 ? mains : shaqi;
            const auto lookup = tower::ids::Find(infos, w.id);
            if (lookup.ambiguous) { note(std::format("兵器键冲突 {}", w.id)); continue; }
            const auto* info = lookup.value;
            const int want = info ? static_cast<int>(info->Find(w.kind == 0 ? "WeaponLevel" : "ShaChiWeaponLevel") ? info->Find(w.kind == 0 ? "WeaponLevel" : "ShaChiWeaponLevel")->Int(1) : 1) : -1;
            const int got = WeaponLevel(w.id, w.kind);
            if (want != got) note(std::format("{} {}→{}", w.id, want, got));
            if (info) if (const auto* snapped = info->Find("AlreadySnapSoul"); snapped && Snapped(w.id, w.kind) != (snapped->Int() ? 1 : 0)) note(std::format("铸魂状态 {}", w.id));
        }
        for (const auto& [id, v] : nodes->fields)
            if (const int got = NodeLevel(id); got >= 0 && got != v.Int()) note(std::format("{} {}→{}", id, v.Int(), got));
        const auto* meds = inv->Find("MedicineInfos");
        for (const auto& m : tower::growth::kMedicines) {
            const auto lookup = tower::ids::Find(meds, m.id);
            if (lookup.ambiguous) { note(std::format("药品键冲突 {}", m.id)); continue; }
            const auto* info = lookup.value;
            const int want = info ? static_cast<int>((info->Find("CurrentLevel") ? info->Find("CurrentLevel")->Int() : 0) + 1) : -1;
            const int got = MedicineLevel(m.id);
            if (want != got) note(std::format("{} {}→{}", m.id, want, got));
        }
        const auto* accs = inv->Find("AccessoryInfos");
        for (const auto& a : tower::growth::kAccessories) {
            const auto lookup = tower::ids::Find(accs, a.id);
            if (lookup.ambiguous) { note(std::format("饰品键冲突 {}", a.id)); continue; }
            const bool want = lookup.value != nullptr;
            const bool got = HasAccessory(a.id);
            const bool exempt = std::find(tower::growth::kAccessoryExcluded.begin(), tower::growth::kAccessoryExcluded.end(), a.id) != tower::growth::kAccessoryExcluded.end();
            // 平台赠送（DLC / 预购）的饰品由游戏按账号发放，载入时没有不算塔档错误
            const bool entitled = want && lookup.value->Find("GrantedByEntitlement") && lookup.value->Find("GrantedByEntitlement")->b;
            if (want != got && !(exempt && got && !want) && !(entitled && !got)) note(std::format("饰品 {}{}", a.id, want ? "缺少" : "多出"));
        }
        // 尘与升级材料：用核心物品编辑的读回（与修改器物品页同一来源）
        tower::json::Value l;
        if (tower::json::Parse(pbz::inv::List(), &l) && l.Find("ok") && l.Find("ok")->b) {
            const long long wantDust = player->Find("Dust") ? player->Find("Dust")->Int() : 0;
            const long long gotDust = l.Find("dust") ? l.Find("dust")->Int() : -1;
            if (wantDust != gotDust) note(std::format("尘 {}→{}", wantDust, gotDust));
            const auto* items = inv->Find("InventoryInfos");
            const auto* stack = l.Find("stack");
            auto lower = [](std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; };
            std::map<std::string, long long> want, got;
            if (items) for (auto& [id, v] : items->fields) if (tower::save::IsUpgradeMaterial(id)) want[lower(id)] = v.Find("CurrentAmount") ? v.Find("CurrentAmount")->Int() : 0;
            if (stack) for (auto& [id, v] : stack->fields) if (tower::save::IsUpgradeMaterial(id)) got[lower(id)] = v.Int();
            // 3.0.1（玩家反馈：出塔「原档核验未通过（材料 xx 50→0 …）」，每人列的材料不同）：核心物品读取对「当前数 > 累计获得数」的
            // 条目判为读取异常、不放进 stack（修改器改过 / 旧版本迁移的材料常见），这里当成 0 → 误判。读回不一致时改用原生
            // GetItemNumInfo 的当前数再比一次；两边都对不上才算差异
            int rescued = 0;
            auto liveCount = [&](const std::string& id, long long fromList) -> long long {
                const int native = ItemCount(id);
                if (native >= 0 && native != fromList) ++rescued;
                return native >= 0 ? native : fromList;
            };
            for (auto& [id, n] : want) {
                long long g = got.count(id) ? got[id] : 0;
                if (g != n) g = liveCount(id, g);
                if (g != n) note(std::format("材料 {} {}→{}", id, n, g));
            }
            for (auto& [id, n] : got) if (!want.count(id) && n && liveCount(id, n) != 0) note(std::format("材料 {} 0→{}", id, liveCount(id, n)));
            if (rescued) pbz::log::Info("影塔独立塔档：核验时 {} 种材料改用原生持有数（核心读取跳过了这些条目）", rescued);
        } else note("读不到尘 / 材料");
        if (bad.empty()) return true;
        std::string d;
        for (auto& b : bad) d += (d.empty() ? "" : "、") + b;
        *diff = d;
        return false;
    }

    // 饰品槽（用户 2026-11-05）：原生能力节点开槽走 PBZBackPackSubsystem.AddAccessorySlotTag(System.Player.AddAccessorySlot.*)，
    // 四个标签各开一格（导出 GA_AddAccessorySlot_*：HeiShi / HuPo / MemoryFlower / RongShu），基础 2 格，开满 6 格。
    // 只加不减（Set 去重，能力节点开的同一标签不会重复算）；当前可装数已够就不调用。返回读回的可装数，-1 = 接口不可用
    int EnsureAccessorySlots(int unlocks) {
        UObject* bag = Bag();
        if (!bag || !accNum_) return -1;
        if (!addSlot_) addSlot_ = pe::FindFunction(bagCls_, "AddAccessorySlotTag");
        auto count = [&] { uint8_t r[16] = {}; pe::Call(bag, accNum_, r); return *reinterpret_cast<int32_t*>(r); };
        const int before = count();
        unlocks = std::clamp(unlocks, 0, 4);
        if (!addSlot_ || before >= 2 + unlocks) return before;
        static constexpr const char* kTags[4] = {"System.Player.AddAccessorySlot.HeiShi", "System.Player.AddAccessorySlot.HuPo",
                                                 "System.Player.AddAccessorySlot.MemoryFlower", "System.Player.AddAccessorySlot.RongShu"};
        for (int i = 0; i < unlocks; ++i) {
            const uint64_t tag = Name(kTags[i]);
            if (!tag) continue;
            uint8_t args[32] = {};
            std::memcpy(args, &tag, 8);  // FGameplayTag{FName TagName}
            pe::Call(bag, addSlot_, args);
        }
        const int after = count();
        pbz::log::Info("影塔饰品槽：目标 {} 格（开放 {} 个），{} → {}", 2 + unlocks, unlocks, before, after);
        return after;
    }

    Loadout Read() {
        Loadout l;
        UObject* bag = Bag();
        if (!bag) return l;
        auto names = [&](int off) {
            std::vector<std::string> v;
            const auto a = ue::Get<FArr>(bag + off);
            for (int i = 0; a.data && i < a.num && i < 8; ++i) v.push_back(NameStr(ue::Get<uint64_t>(static_cast<uint8_t*>(a.data) + i * 8)));
            return v;
        };
        l.main = names(offMain_);
        l.shaqi = names(offShaqi_);
        if (offMedB_ >= 0) l.medBottom = NameStr(ue::Get<uint64_t>(bag + offMedB_));
        if (offMedT_ >= 0) l.medTop = NameStr(ue::Get<uint64_t>(bag + offMedT_));
        if (offAssembly_ >= 0) for (auto& s : names(offAssembly_)) if (!s.empty() && s != "None") l.accessories.push_back(s);
        if (accNum_) { uint8_t r[16] = {}; pe::Call(bag, accNum_, r); l.accessorySlots = *reinterpret_cast<int32_t*>(r); }
        l.ok = true;
        return l;
    }


private:
    bool resolved_ = false, ready_ = false;
    UObject *bagCls_ = nullptr, *nodesCls_ = nullptr, *helpCls_ = nullptr, *medInfoCls_ = nullptr, *strCdo_ = nullptr, *asCdo_ = nullptr;
    UObject *toName_ = nullptr, *getAsc_ = nullptr, *has_ = nullptr, *level_ = nullptr, *upgrade_ = nullptr, *reset_ = nullptr, *snap_ = nullptr, *snapped_ = nullptr;
    UObject *addProps_ = nullptr, *itemNum_ = nullptr;  // Boss 原生道具掉落（AddPropsToBackPack / GetItemNumInfo）
    UObject *addSlot_ = nullptr, *pushMed_ = nullptr, *upMed_ = nullptr, *pushAcc_ = nullptr, *accNum_ = nullptr, *activate_ = nullptr, *deactivate_ = nullptr, *deactivateAll_ = nullptr;
    int offMedTime_ = -1;
    UObject *upAcc_ = nullptr, *accMax_ = nullptr, *accInfoCls_ = nullptr; int offAccLevel_ = -1;
    int offMain_ = -1, offShaqi_ = -1, offMedB_ = -1, offMedT_ = -1, offMeds_ = -1, offAccs_ = -1, offAssembly_ = -1, offNodeMap_ = -1, offNodeLevel_ = -1, offMedLevel_ = -1;
    UObject *bag_ = nullptr, *nodes_ = nullptr, *nodesOwner_ = nullptr, *reloadBag_ = nullptr, *worldCls_ = nullptr;
    bool titleSeen_ = false;
    unsigned long long stableSince_ = 0;
    std::unordered_map<std::string, uint64_t> names_;
    UObject* getComp_ = nullptr; UObject* getStates_ = nullptr; UObject* aiCls_ = nullptr; UObject* enterDomain_ = nullptr;
    UObject* targetingCls_ = nullptr; UObject* targeting_ = nullptr; UObject* targetingOwner_ = nullptr; UObject* compOwner_ = nullptr; int lockOff_ = -1;
    UObject* stateCls_ = nullptr; UObject* inChild_ = nullptr; UObject* inExact_ = nullptr; UObject* state_ = nullptr; UObject* stateOwner_ = nullptr;
};

Native g_native;

std::string Utf8(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring Wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// 进出独立塔档的游戏端：沙盒 / 存档锁 / 基准 / ReplaceSaveString / 重新载入检测
namespace { void TemplateAnchor() {} }
// 世界模板：和 PBZTower.dll 同目录的 world-template.json（安装器 / 手动包随附）；开发版模块从副本目录载入时没有，退回原档
bool ReadWorldTemplate(std::string* json) {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&TemplateAnchor), &self)) return false;
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(self, path, MAX_PATH)) return false;
    std::wstring p(path);
    p.resize(p.find_last_of(L"\\/") + 1);
    p += L"world-template.json";
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart > 64 && size.QuadPart < (64ll << 20);
    if (ok) {
        json->resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = ReadFile(h, json->data(), static_cast<DWORD>(json->size()), &got, nullptr) && got == json->size();
        if (ok && json->size() >= 3 && static_cast<unsigned char>((*json)[0]) == 0xEF) json->erase(0, 3);  // BOM
    }
    CloseHandle(h);
    return ok;
}

class TripIo final : public tower::trip::IIo {
public:
    std::function<bool(std::string*)> backup;
    bool Backup(std::string* why) override { return backup ? backup(why) : (*why = "没有备份接口", false); }
    bool SandboxOn(bool snapshot, std::string* why) override {
        if (!towersb::Active() && (!towersb::Set(true, why, false) || !towergate::Veto(true, why))) return false;
        return !snapshot || towergate::Snapshot(why);
    }
    bool WorldTemplate(std::string* json) override { return ReadWorldTemplate(json); }
    bool Baseline(std::string* json) override {
        std::wstring w;
        if (!towergate::BaselineText(&w)) return false;
        *json = Utf8(w);
        return true;
    }
    bool Replace(const std::string& json, bool isBaseline, std::string* why) override {
        const bool atTitle = g_native.AtTitle();
        if (!towergate::Put(Wide(json), isBaseline, why)) return false;
        g_native.Arm(atTitle);  // 重新等"回标题 → 新背包"；在标题上重放时保留"已到过标题"
        pbz::log::Info("影塔独立塔档：{}已放进内存（{}）", isBaseline ? "原档基准" : "塔内存档", atTitle ? "在标题画面" : "在游戏里，等回标题");
        return true;
    }
    bool AtTitle() override { return g_native.AtTitle(); }
    bool Reloaded() override { return g_native.ReloadFinished(); }
    bool Verify(const std::string& expect, std::string* diff) override {
        const bool ok = g_native.Verify(expect, diff);
        pbz::log::Info("影塔独立塔档：载入内容核验{}{}", ok ? "通过" : "未通过：", ok ? "" : *diff);
        return ok;
    }
    bool SandboxOff(std::string* why) override {
        towergate::ConfirmOriginal();  // 已核验是原档：之后再开沙盒可以记新基准
        if (!towersb::Set(false, why, false)) return false;
        towergate::Veto(false, why);
        towergate::StopWatching();
        return true;
    }
    // 出塔收尾（fa164d1 反例）：先只放开存档锁、沙盒保持，等原生存档落稳
    bool SettleSaves() override { return true; }
    bool ReleaseLock(std::string* why) override { return towergate::Release(why); }
    uint64_t SandboxActivity() override { return towersb::Activity(); }
    bool SaveClean(std::string* why) override { return towergate::SaveClean(why); }
    void Achievements(bool blocked) override { g_native.Achievements(blocked); }
    // 自动切档：标题上只看原生加载；游戏里还要玩家在、目的地遮罩已清、没有点传送悬停 / 影塔旅行
    bool Calm() override {
        if (g_native.AtTitle()) return towertitle::Quiet();
        // 1.1.1（两次实机闪退：战斗中暂离 / 塔内重开，移除 Boss 后 0.15–1.4 秒就 OpenLevel 回标题，2 秒内游戏工作线程读坏内存）：
        // 移除场上 Boss 后至少等 3 秒，让它的技能 / 特效 / 投射物收尾，再卸场景
        const auto cleared = towerapi::LastEncounterClear();
        if (cleared && GetTickCount64() - cleared < 3000) return false;
        return towerwidgets::Pawn() && towertitle::Quiet() && towerentry::LoadingClear() && !pbz::TeleportBusy() && !towertravel::Busy();
    }
    bool ToTitle(std::string* why) override { return towertitle::ToTitle(why); }
    bool TitleReady() override { return towertitle::Quiet() && towertitle::MenuReady(); }
    bool Continue(std::string* why) override { return towertitle::Continue(why); }
    uint64_t Now() override { return GetTickCount64(); }
};
TripIo g_trip;

}  // namespace

tower::session::IGame& Game(std::function<bool(std::string*)> backup) {
    if (backup) g_native.backup = std::move(backup);
    return g_native;
}
// 用户 2026-11-08：换波很卡——每种新怪第一次刷时，运行库在游戏线程同步加载蓝图（每只 200 多毫秒）。
// 提前用引擎的异步加载（LoadAssetClass，和蓝图的 Async Load Class Asset 同一个）放到后台，真正刷怪时资源已在内存
void PreloadClass(const std::string& path) {
    static bool broken = false;
    static std::unordered_map<std::string, unsigned long long> recent;
    if (broken) return;
    const auto dot = path.rfind('.');
    auto* pawn = towerwidgets::Pawn();
    if (dot == std::string::npos || !pawn) return;
    const auto now = GetTickCount64();
    if (auto it = recent.find(path); it != recent.end() && now - it->second < 15000) return;
    auto* ksl = towerperf::ClassNamed("KismetSystemLibrary");
    auto* str = towerperf::ClassNamed("KismetStringLibrary");
    auto* lib = ksl ? towerwidgets::Default(ksl) : nullptr;
    auto* strLib = str ? towerwidgets::Default(str) : nullptr;
    auto* toName = str ? pe::FindFunction(str, "Conv_StringToName") : nullptr;
    auto* toSoft = ksl ? pe::FindFunction(ksl, "Conv_SoftClassPathToSoftClassRef") : nullptr;
    auto* load = ksl ? pe::FindFunction(ksl, "LoadAssetClass") : nullptr;
    bool ok = lib && strLib && toName && toSoft && load;
    if (ok) for (const char* p : {"WorldContextObject", "AssetClass", "OnLoaded", "LatentInfo"}) ok = ok && ue::PropOffset(load, p) >= 0;
    if (!ok) { broken = true; pbz::log::Info("影塔预加载：引擎异步加载接口不可用，刷怪时照常同步加载"); return; }
    struct Str { const wchar_t* data; int32_t num, max; };
    auto name = [&](const std::string& text) {
        std::wstring wide(text.begin(), text.end()); wide.push_back(L'\0');
        Args a(toName);
        const Str s{wide.data(), static_cast<int32_t>(wide.size()), static_cast<int32_t>(wide.size())};
        std::memcpy(a.At("InString"), &s, sizeof s);
        pe::Call(strLib, toName, a.b.data());
        return ue::Get<uint64_t>(a.At("ReturnValue"));
    };
    const uint64_t package = name(path.substr(0, dot)), asset = name(path.substr(dot + 1));
    Args soft(toSoft);
    std::memcpy(soft.At("SoftClassPath"), &package, 8);
    std::memcpy(soft.At("SoftClassPath") + 8, &asset, 8);
    pe::Call(lib, toSoft, soft.b.data());
    Args a(load);
    std::memcpy(a.At("WorldContextObject"), &pawn, 8);
    std::memcpy(a.At("AssetClass"), soft.At("ReturnValue"), 40);
    // FLatentActionInfo：Linkage = -1（加载完不回调）、UUID 每次不同、ExecutionFunction = None、CallbackTarget
    static int32_t uuid = 0x50425A00;
    uint8_t* info = a.At("LatentInfo");
    const int32_t linkage = -1, id = ++uuid;
    std::memcpy(info, &linkage, 4);
    std::memcpy(info + 4, &id, 4);
    std::memcpy(info + 16, &pawn, 8);
    pe::Call(lib, load, a.b.data());
    recent[path] = now;
}
Loadout ReadLoadout() { return g_native.Read(); }
tower::trip::IIo& Trip(std::function<bool(std::string*)> backup) {
    if (backup) g_trip.backup = std::move(backup);
    return g_trip;
}
int Level(std::string_view weaponId, int kind) { return g_native.WeaponLevel(weaponId, kind); }
int AccessoryLevel(std::string_view id) { return g_native.AccessoryLevel(id); }
void ZeroMedicineUses() { g_native.ZeroMedicineUses(); }
int AccessoryMaxLevel(std::string_view id) { return g_native.AccessoryMaxLevel(id); }
int NodeLevel(std::string_view nodeId) { return g_native.NodeLevel(nodeId); }
int Snapped(std::string_view id, int kind) { return g_native.Snapped(id, kind); }
int StoryLocked() { return g_native.StoryLocked(); }
void* LockTarget() { return g_native.LockTarget(); }
std::string PlayerStates() { return g_native.PlayerStates(); }
int EnterDomain(void* boss) { return g_native.EnterDomain(static_cast<UObject*>(boss)); }
int EnsureAccessorySlots(int unlocks) { return g_native.EnsureAccessorySlots(unlocks); }
bool OpenCharacterPage(std::string* why) { return towerwidgets::Open(towerwidgets::Panel::Character, why); }

bool WorldTemplateText(std::string* json) { return ReadWorldTemplate(json); }
}  // namespace towernative
