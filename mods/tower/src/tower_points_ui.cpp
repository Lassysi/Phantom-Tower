// Tower currency adapter for the original graphical growth panels. No save/property
// currency writes: original dust reads become the relevant point balance only while
// a tower growth panel is active; the authoritative native transactions still charge.
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <vector>
#include "tower_points.h"
#include "tower_api.h"
#include "tower_widgets.h"
#include "../../../src/core/host/log.h"
#include "tower_perf.h"

namespace towerapi { bool InsideTower(); }
namespace towernative { int AccessoryMaxLevel(std::string_view id); }
namespace towerpoints { bool Enabled(); }
namespace towerpoints {
namespace ue = pbz::ue;
namespace pe = pbz::pe;
namespace nx = pbz::nx;
using ue::UObject;
namespace {
struct Args {
    UObject* fn; std::array<uint8_t, 0x400> bytes{};
    int Off(const char* key) const { return fn ? ue::PropOffset(fn, key) : -1; }
    uint8_t* At(const char* key) { const int o = Off(key); return o >= 0 && o < 0x300 ? bytes.data() + o : nullptr; }
};
int g_mode = 0, g_up = -1, g_no = -1, g_complete = -1;
int g_needPoints = 1;  // 当前选中物品每级要几点（兵器 / 铸魂 / 能力 1，饰品打磨 kAccessoryRefineTemper）
bool g_stateHook = false, g_costHook = false, g_rowHook = false;
std::vector<UObject*> g_pe, g_nx, g_watched;
std::vector<int> g_obs;
struct Visibility { UObject* widget; UObject* cls; uint8_t value; };
std::vector<Visibility> g_hidden;
std::map<std::string, int> g_requirements;
// 费用显示路径诊断（7c2668e：扣点成功但面板仍显示原生材料）：每个费用显示入口经 ProcessEvent 到达我们拦截的次数。
// 蓝图调用自己的蓝图函数不经过 ProcessEvent；面板开过后这里仍是 0，就说明显示走的是拦截不到的路径。面板关闭时记一行日志
struct CostHits { int state = 0, materials = 0, dust = 0, material = 0, handled = 0; } g_hits;
bool g_panelWasOpen = false;
UObject* Fn(UObject* cls, const char* name) { return cls ? pe::FindFunction(cls, name) : nullptr; }
std::string NameOf(const uint8_t* p) {
    const auto n = ue::Get<uint64_t>(p);
    auto s = ue::Name(static_cast<uint32_t>(n));
    const auto number = static_cast<uint32_t>(n >> 32);
    return number ? s + "_" + std::to_string(number - 1) : s;
}
bool Active() { return Enabled() && towerapi::InsideTower() && g_mode && towerwidgets::PanelActive(); }
bool Seen(const std::vector<UObject*>& list, UObject* fn) { return std::find(list.begin(), list.end(), fn) != list.end(); }
// 能力面板特殊材料档（用户 2026-11-05：只用修为点升级）：原生 JS 按面板蓝图变量 NeedMaterialCount[本线序号] 要求黑石 / 杀气琥珀 /
// 完成的大记忆之花个数，塔档里没有记忆之花。面板开着时把这个 int 数组就地清零（不分配内存），关面板恢复原值；
// 真正的扣点 / 前置 / 上限仍在 ActivateNode（ChargeNode）
struct NeedCounts { UObject* widget = nullptr; UObject* cls = nullptr; int32_t* data = nullptr; std::vector<int32_t> saved; };
NeedCounts g_need;
void RestoreNeedCounts() {
    if (!g_need.widget) return;
    if (ue::Live(g_need.widget) && ue::ClassOf(g_need.widget) == g_need.cls) {
        const int off = ue::PropOffset(ue::ClassOf(g_need.widget), "NeedMaterialCount");
        struct Arr { int32_t* data; int32_t num, max; };
        const Arr a = off >= 0 ? ue::Get<Arr>(g_need.widget + off) : Arr{};
        if (a.data == g_need.data && a.num == static_cast<int32_t>(g_need.saved.size())) {
            std::vector<int32_t> current(a.num);
            if (ue::Read(a.data, current.data(), current.size() * 4) &&
                std::all_of(current.begin(), current.end(), [](int32_t n) { return n == 0; }))
                ue::Write(a.data, g_need.saved.data(), g_need.saved.size() * 4);
        }
    }
    g_need = {};
}
void ZeroNeedCounts(UObject* widget) {
    RestoreNeedCounts();
    const int off = widget ? ue::PropOffset(ue::ClassOf(widget), "NeedMaterialCount") : -1;
    if (off < 0) { pbz::log::Info("影塔能力面板：没有 NeedMaterialCount（特殊材料档未接管）"); return; }
    struct Arr { int32_t* data; int32_t num, max; };
    const Arr a = ue::Get<Arr>(widget + off);
    if (!a.data || a.num <= 0 || a.num > 64) return;
    std::vector<int32_t> saved(a.num);
    if (!ue::Read(a.data, saved.data(), saved.size() * 4)) return;
    const std::vector<int32_t> zero(a.num, 0);
    if (!ue::Write(a.data, zero.data(), zero.size() * 4)) return;
    g_need.widget = widget; g_need.cls = ue::ClassOf(widget); g_need.data = a.data; g_need.saved = std::move(saved);
    pbz::log::Info("影塔能力面板：特殊材料档 {} 个清零（关面板恢复）", a.num);
}
void RestoreVisibility() {
    for (auto& v : g_hidden) if (ue::Live(v.widget) && ue::ClassOf(v.widget) == v.cls) {
        if (auto* f = Fn(v.cls, "SetVisibility")) { Args a{f}; if (a.At("InVisibility")) { *a.At("InVisibility") = v.value; pe::Call(v.widget, f, a.bytes.data()); } }
    }
    g_hidden.clear();
}
void Hide(UObject* widget) {
    if (!widget || !ue::Live(widget)) return;
    if (std::any_of(g_hidden.begin(), g_hidden.end(), [&](auto& v) { return v.widget == widget; })) return;
    auto* cls = ue::ClassOf(widget); auto* get = Fn(cls, "GetVisibility"); auto* set = Fn(cls, "SetVisibility");
    if (!get || !set) return;
    Args a{get}, b{set}; if (!a.At("ReturnValue") || !b.At("InVisibility")) return;
    pe::Call(widget, get, a.bytes.data()); g_hidden.push_back({widget, cls, *a.At("ReturnValue")});
    *b.At("InVisibility") = 1; // ESlateVisibility::Collapsed (SDK)
    pe::Call(widget, set, b.bytes.data());
}
void TextProperty(UObject* w, const char* field, const char* value) {
    const int o = ue::PropOffset(ue::ClassOf(w), field);
    if (o >= 0) if (auto* text = ue::Get<UObject*>(w + o); text && ue::Live(text)) towerwidgets::SetText(text, value);
}
bool HookNative(UObject* fn, nx::Handler cb) {
    if (!fn) return false;
    if (Seen(g_nx, fn)) return true;
    std::string why;
    // BP overrides execute through ProcessEvent; Puerts C++ bindings can enter the
    // native thunk directly. Cover both without assuming one route for every node.
    if (!Seen(g_pe, fn)) { pe::Intercept(fn, [cb](UObject* o, void* p) { return cb(o, static_cast<uint8_t*>(p), nullptr); }); g_pe.push_back(fn); }
    if (!nx::Hook(fn, std::move(cb), &why)) { pbz::log::Info("影塔原生费用：入口未接管 {}：{}", ue::ObjName(fn), why); return false; }
    g_nx.push_back(fn); return true;
}
void EnumStates() {
    if (g_up >= 0 && g_no >= 0 && g_complete >= 0) return;
    UObject* en = nullptr;
    ue::ForEach([&](int, UObject* o) { if (ue::ObjName(o) == "E_ItemUpgradeType" && !ue::IsDefault(o)) { en = o; return false; } return true; });
    auto* lib = towerwidgets::Default(towerperf::ClassNamed("KismetNodeHelperLibrary"));
    auto* f = lib ? Fn(ue::ClassOf(lib), "GetEnumeratorUserFriendlyName") : nullptr;
    if (!en || !f) return;
    for (uint8_t i = 0; i < 4; ++i) {
        Args a{f}; if (!a.At("Enum") || !a.At("EnumeratorValue") || !a.At("ReturnValue")) return;
        std::memcpy(a.At("Enum"), &en, 8); *a.At("EnumeratorValue") = i; pe::Call(lib, f, a.bytes.data());
        struct String { wchar_t* data; int32_t num, max; } s = ue::Get<String>(a.At("ReturnValue"));
        if (!s.data || s.num < 1 || s.num > 128) continue;
        std::wstring w(s.num, 0); if (!ue::Read(s.data, w.data(), s.num * 2)) continue;
        if (w.c_str() == std::wstring(L"Upgradable")) g_up = i;
        if (w.c_str() == std::wstring(L"Nonupgradable")) g_no = i;
        if (w.c_str() == std::wstring(L"CompleteUpgrade")) g_complete = i;
    }
}
// 1.1.1（玩家反馈：能力修行每条线第三个节点起点不了，"需要收集 1 朵大型记忆之花"）：
// 清零靠 W_AbilityWidget_C::BP_OnActivated 的观察，但这个类要第一次打开能力面板才加载（原生 JS 注释：会话内首次开菜单必然没加载）。
// 钟菜单 PushAbilityTreeWidget 时类还不在 → 观察挂不上、面板也没登记，原生门槛 [0,0,1,1,2,4,6,8] 原样生效。
// 兜底：打开后 8 秒内重查实例和迟加载的接口；准备阶段类不存在时，不能把空接管当成完成。
unsigned long long g_abilitySince = 0, g_abilityScan = 0;
void OnAbilityActivated(UObject* o) { if (towerapi::InsideTower()) { g_mode = 2; g_abilitySince = GetTickCount64(); g_abilityScan = 0; towerwidgets::TrackPanel(o); if (g_need.widget != o) ZeroNeedCounts(o); } }
void Watch(UObject* cls, const char* name, pe::Observer callback);
void Ensure();
void FindAbilityPanel() {
    if (g_mode != 2 || !g_abilitySince) return;
    const auto t = GetTickCount64();
    if (t - g_abilitySince > 8000) { g_abilitySince = 0; return; }
    if (t < g_abilityScan) return;
    g_abilityScan = t + 1000;
    auto* cls = towerperf::ClassNamed("W_AbilityWidget_C");
    if (!cls) return;
    Watch(cls, "BP_OnActivated", [](UObject* o, void*) { OnAbilityActivated(o); });  // 之后再开面板走正常观察
    UObject* found = nullptr;
    ue::ForEach([&](int, UObject* o) {
        if (ue::ClassOf(o) != cls || ue::IsDefault(o) || !towerwidgets::IsActivated(o)) return true;
        found = o; return false;
    });
    if (!found) return;
    if (g_need.widget != found) { towerwidgets::TrackPanel(found); ZeroNeedCounts(found); }
    // Ensure is also called after the widget and node instances actually exist.
    // It only installs missing hooks and remains scoped to the tower panel.
    Ensure();
}
void Watch(UObject* cls, const char* name, pe::Observer callback) {
    auto* fn = Fn(cls, name); if (!fn || Seen(g_watched, fn)) return;
    g_obs.push_back(pe::Observe(fn, std::move(callback))); g_watched.push_back(fn);
}
// 3.0.1（玩家反馈：能力修行最后一行第三个起经常点不了，退出重进才行）：材料门槛（HasEnoughItem / GetUsePropAmount）和末列轮回数
// （GetCycleCount）原来要等第一次打开能力面板才挂；Core 的原生拦截槽位全局共用（40 个），打了几层后战斗 / 过场 / 隔离的钩子
// 先占满，再开面板就挂不上 → 原生按真实材料判定，后面的节点点不了；重进游戏时槽位还空所以正常。
// 改为进塔就挂（它们只在能力面板开着时改答，其余时间直接放行），挂不上写日志
bool g_earlyLogged = false;
void InstallEarly() {
    // 1.1.1（用户 2026-11-07 实机：最后一列锋芒 / 凝神 / 易筋点不了，写着 30）：原生 JS 末列每级要 ceil(级/5) 次轮回
    // （PBZGameSystem::GetCycleCount），塔档是新档、轮回 0。用户定的塔规则：每通过 15 层 1 个轮回点，解锁一个末列能力（同行先点满）。
    // 只在塔内能力面板开着时改答；面板外（周目难度等）照原生
    if (auto* gs = towerperf::ClassNamed("PBZGameSystem")) {
        auto* f = Fn(gs, "GetCycleCount");
        const int ret = f ? ue::PropOffset(f, "ReturnValue") : -1;
        if (f && ret >= 0 && !Seen(g_nx, f))
            HookNative(f, [ret](UObject*, uint8_t* p, void* result) {
                if (UiMode() != 2) return false;
                // 塔规则（tower_growth.h CyclePoints）：已有末列能力、或还有没用的轮回点 → 答 6（原生末列 30 级都放行，真正的
                // 解锁条件 / 上限在 ChargeNode）；都没有 → 答 0，面板照原生显示"需要轮回"
                const auto& prof = towerapi::Growth().profile;
                const int32_t n = prof.FinalsUnlocked() > 0 || prof.CycleLeft(towerapi::Run().floor) ? 6 : 0;
                std::memcpy(p + ret, &n, 4);
                if (result) std::memcpy(result, &n, 4);
                return true;
            });
    }
    auto* bag = towerperf::ClassNamed("PBZBackPackSubsystem");
    if (auto* f = Fn(bag, "HasEnoughItem"); f && !Seen(g_nx, f)) {
        const int id = ue::PropOffset(f, "ItemId"), num = ue::PropOffset(f, "Num"), out = ue::PropOffset(f, "RequireNum"), ret = ue::PropOffset(f, "ReturnValue");
        if (id >= 0 && num >= 0 && out >= 0 && ret >= 0) HookNative(f, [id, num, out, ret](UObject*, uint8_t* p, void* result) {
            if (!Active() || g_mode != 2) return false;
            const int required = ue::Get<int32_t>(p + num); if (required < 0) return false;
            g_requirements[NameOf(p + id)] = required;
            const int missing = 0; std::memcpy(p + out, &missing, 4); p[ret] = 1; if (result) *static_cast<uint8_t*>(result) = 1; return true;
        });
    }
    if (auto* f = Fn(bag, "GetUsePropAmount"); f && !Seen(g_nx, f)) {
        const int id = ue::PropOffset(f, "NameID"), ret = ue::PropOffset(f, "ReturnValue");
        if (id >= 0 && ret >= 0) HookNative(f, [id, ret](UObject*, uint8_t* p, void* result) {
            if (!Active() || g_mode != 2) return false;
            const auto it = g_requirements.find(NameOf(p + id)); if (it == g_requirements.end()) return false;
            const int required = it->second; std::memcpy(p + ret, &required, 4); if (result) std::memcpy(result, &required, 4); return true;
        });
    }
    if (!g_earlyLogged) {
        auto* gs = towerperf::ClassNamed("PBZGameSystem"); auto* bag2 = towerperf::ClassNamed("PBZBackPackSubsystem");
        auto* a = Fn(gs, "GetCycleCount"); auto* b = Fn(bag2, "HasEnoughItem"); auto* c = Fn(bag2, "GetUsePropAmount");
        if (a && b && c) {
            g_earlyLogged = true;
            pbz::log::Info("影塔能力面板：材料门槛入口预挂 轮回数 {} / 材料够不够 {} / 消耗数 {}", Seen(g_nx, a) ? "已挂" : "未挂", Seen(g_nx, b) ? "已挂" : "未挂", Seen(g_nx, c) ? "已挂" : "未挂");
        }
    }
}
void Ensure() {
    if (!towerperf::ClassNamed("W_NeedItem_Entry_C")) towerwidgets::LoadClass("/Game/PBZContent/UI/UINew/HUD/W_NeedItem_Entry.W_NeedItem_Entry_C");
    InstallEarly();
    auto* bell = towerperf::ClassNamed("W_BellMenu_C");
    for (auto [name, mode] : {std::pair{"PushWeaponUpgrade", 1}, {"PushAbilityTreeWidget", 2}, {"PushSnapSoul", 3}})
        Watch(bell, name, [mode](UObject*, void*) { if (towerapi::InsideTower()) { std::string why; if (!PrepareUi(mode, &why)) pbz::log::Info("影塔费用面板未就绪：{}", why); } });
    auto* ability = towerperf::ClassNamed("W_AbilityWidget_C");
    auto* upgrade = towerperf::ClassNamed("W_UpgradeActivatableWidget_V1_C");
    Watch(ability, "BP_OnActivated", [](UObject* o, void*) { OnAbilityActivated(o); });
    Watch(upgrade, "BP_OnActivated", [](UObject* o, void*) { if (towerapi::InsideTower()) { if (g_mode != 3) g_mode = 1; towerwidgets::TrackPanel(o); } });
    auto* logic = towerperf::ClassNamed("BP_UIGameplayLogicComponent_C");
    EnumStates();
    if (auto* f = Fn(logic, "GetItemUpgradeState"); f && !g_stateHook && g_up >= 0 && g_no >= 0 && g_complete >= 0) {
        const int id = ue::PropOffset(f, "ItemId"), out = ue::PropOffset(f, "ItemUpgradeState"), soul = ue::PropOffset(logic, "IsZhuHunState");
        if (id >= 0 && out >= 0 && soul >= 0) {
            pe::Intercept(f, [id, out, soul](UObject* o, void* params) {
                if (!Active()) return false;
                ++g_hits.state;
                auto* p = static_cast<uint8_t*>(params); const auto key = NameOf(p + id);
                const auto& profile = towerapi::Growth().profile;
                int state = g_no;
                if (const int a = tower::growth::AccessoryIndex(key); a >= 0) {  // 饰品打磨：原生上限、淬炼点
                    const auto& accs = profile.accessories;
                    const auto it = std::find_if(accs.begin(), accs.end(), [a](const auto& x) { return x.index == a; });
                    const int cap = towernative::AccessoryMaxLevel(key);
                    if (it != accs.end() && cap > 0)
                        state = it->refine >= cap ? g_complete : towerapi::Run().phase == tower::Phase::Ready && profile.temper >= tower::growth::kAccessoryRefineTemper ? g_up : g_no;
                    p[out] = static_cast<uint8_t>(state); return true;
                }
                for (int kind : {0, 1}) {
                    const int w = tower::growth::WeaponIndex(key, kind), slot = w >= 0 ? profile.Find(w) : -1;
                    if (slot < 0) continue;
                    const auto& own = profile.weapons[slot];
                    const bool isSoul = ue::Get<uint8_t>(o + soul) != 0;
                    const int cap = towerapi::Growth().hexes.Has(tower::hex::Forge) ? 60 : tower::growth::kWeaponCap;
                    const bool complete = isSoul ? own.snapped || !tower::growth::kWeapons[w].snapSoul : own.level >= std::min(cap, tower::growth::kWeapons[w].maxLevel);
                    auto copy = profile;
                    const auto allowed = isSoul ? copy.Snap(w) : copy.Upgrade(w, cap);
                    state = complete ? g_complete : towerapi::Run().phase == tower::Phase::Ready && allowed == tower::growth::Err::Ok ? g_up : g_no;
                    break;
                }
                p[out] = static_cast<uint8_t>(state); return true;
            }); g_pe.push_back(f); g_stateHook = true;
        }
    }
    if (auto* f = Fn(logic, "GetItemMaterialsByID"); f && !g_costHook) {
        const int dust = ue::PropOffset(f, "NeedDust"), materials = ue::PropOffset(f, "Materials"), item = ue::PropOffset(f, "ItemId");
        if (dust >= 0 && materials >= 0) {
            pe::Intercept(f, [dust, materials, item](UObject*, void* params) {
                if (!Active() || g_mode == 2) return false;
                ++g_hits.materials;
                auto* p = static_cast<uint8_t*>(params);
                // Leave the engine-initialized empty out-map intact. Never memset/free a live TMap.
                const int num = ue::Get<int32_t>(p + materials + 8);
                if (num != 0) { pbz::log::Info("影塔原生费用：Materials out-map 非空，保留原生处理（{}）", num); return false; }
                g_needPoints = item >= 0 && tower::growth::AccessoryIndex(NameOf(p + item)) >= 0 ? tower::growth::kAccessoryRefineTemper : 1;
                const double points = g_needPoints; std::memcpy(p + dust, &points, 8); ++g_hits.handled; return true;
            }); g_pe.push_back(f); g_costHook = true;
        }
    }
    auto* row = towerperf::ClassNamed("W_NeedItem_Entry_C");
    if (auto* f = Fn(row, "InitNeedDust"); f && !g_rowHook) {
        const int have = ue::PropOffset(f, "CurrentDust"), need = ue::PropOffset(f, "NeedDust");
        if (have >= 0 && need >= 0) {
            pe::Intercept(f, [f, have, need](UObject* o, void* params) {
                if (!Active()) return false;
                ++g_hits.dust;
                auto* p = static_cast<uint8_t*>(params); const int balance = UiBalance();
                std::memcpy(p + have, &balance, 4);
                { const int n = g_mode == 1 ? g_needPoints : 1; std::memcpy(p + need, &n, 4); }  // 兵器 / 铸魂 / 能力每级 1 点，饰品打磨 kAccessoryRefineTemper
                pe::Call(o, f, params);
                TextProperty(o, "ItemName", g_mode == 3 ? "铸魂令" : g_mode == 2 ? "修为点" : "淬炼点");
                const int image = ue::PropOffset(ue::ClassOf(o), "ItemImage");
                if (image >= 0) Hide(ue::Get<UObject*>(o + image));
                return true;
            }); g_pe.push_back(f); g_rowHook = true;
        }
    }
    if (auto* f = Fn(row, "InitNeedMaterial"); f && !Seen(g_pe, f)) {
        pe::Intercept(f, [f](UObject* o, void* p) { if (!Active()) return false; ++g_hits.material; pe::Call(o, f, p); Hide(o); return true; }); g_pe.push_back(f);
    }
    auto* dustWidget = towerperf::ClassNamed("W_Dust_Tips_C");
    Watch(dustWidget, "ShowDustTips", [](UObject* o, void*) { if (Active()) Hide(o); });
    InstallEarly();
    // Install shared material gates before the per-node hooks compete for native slots.
    // Ability UI calls node.GetConsumeDust for the next level, including C++ overrides.
    auto* help = towerperf::ClassNamed("AbilityNodeHelpObj");
    const int idOffset = help ? ue::PropOffset(help, "NodeItemID") : -1;
    if (help && idOffset >= 0 && g_mode == 2) ue::ForEach([&](int, UObject* o) {
        if (!o || !ue::IsA(o, help) || ue::IsDefault(o)) return true;
        // GetConsumeDust 不再接管（每个节点类一份，占满 Core 的 24 个拦截槽位，后面的 GetUsePropAmount 等挂不上）：
        // 能力面板里 GetCurrentDust 有修为点就答足够大的数（tower_points.cpp），原生尘价比较总能过
        auto* max = Fn(ue::ClassOf(o), "IsMaxLevel");
        if (max && !Seen(g_nx, max)) {
            const int ret = ue::PropOffset(max, "ReturnValue");
            if (ret >= 0) HookNative(max, [ret, idOffset](UObject* node, uint8_t* p, void* result) {
                if (!Active() || g_mode != 2) return false;
                const int n = tower::growth::NodeIndex(NameOf(node + idOffset)); if (n < 0) return false;
                const uint8_t full = towerapi::Growth().profile.nodes[n] >= towerapi::Growth().profile.CapFor(n, towerapi::Run().floor);  // 3.0.0 无尽轮回点
                p[ret] = full; if (result) *static_cast<uint8_t*>(result) = full; return true;
            });
        }
        return true;
    });

}
} // namespace

bool PrepareUi(int panel, std::string* why) {
    g_mode = panel;
    if (panel == 2) { g_abilitySince = GetTickCount64(); g_abilityScan = 0; }
    if (!panel) return true;
    if (!Enabled() || !towerapi::InsideTower()) { *why = "影塔原生点数交易未就绪"; return false; }
    if (auto* free = pbz::FindFeature("res.upgrade_free"); free && free->enabled) { *why = "请先关闭修改器的升级无消耗，再使用影塔点数"; return false; }
    Ensure();
    if (panel != 2 && (!g_stateHook || !g_costHook || !g_rowHook)) { *why = "原生费用面板或枚举尚未就绪，未开放交易"; return false; }
    if (panel == 1) RefreshWeaponDust("打开兵器升级");  // 表在中途被重新载入过就补回来（菜单切换时做，全表扫一次不影响战斗帧）
    return true;
}
int UiBalance() {
    if (!Active()) return -1;
    const auto& p = towerapi::Growth().profile;
    return g_mode == 3 ? p.soulTokens : g_mode == 2 ? p.cultivate : p.temper;
}
int UiMode() { return Active() ? g_mode : 0; }
void UiTick() {
    if (!Enabled() || !towerapi::InsideTower()) return;
    { static unsigned long long next = 0; const auto now = GetTickCount64(); if (now >= next) { next = now + 2000; InstallEarly(); } }
    FindAbilityPanel();
    const bool open = towerwidgets::PanelActive();
    if (open && g_mode) g_panelWasOpen = true;
    if (!open && g_panelWasOpen) {
        g_panelWasOpen = false;
        pbz::log::Info("影塔原生费用显示：面板 {} 关闭；经 ProcessEvent 拦到 状态查询 {} 次、材料表 {} 次（替换 {} 次）、尘行 {} 次、材料行 {} 次"
                       "（显示仍是原生材料而这里为 0 = 显示走蓝图内部调用，拦截不到）",
                       g_mode == 3 ? "铸魂" : g_mode == 2 ? "能力" : "兵器", g_hits.state, g_hits.materials, g_hits.handled, g_hits.dust, g_hits.material);
        g_hits = {};
    }
    if (!open) { RestoreVisibility(); RestoreNeedCounts(); g_requirements.clear(); }
    // Watches are cheap and class-lifetime dependent; expensive node enumeration is on open only.
    auto* bell = towerperf::ClassNamed("W_BellMenu_C");
    for (auto [name, mode] : {std::pair{"PushWeaponUpgrade", 1}, {"PushAbilityTreeWidget", 2}, {"PushSnapSoul", 3}})
        Watch(bell, name, [mode](UObject*, void*) { if (towerapi::InsideTower()) { std::string why; PrepareUi(mode, &why); } });
}
void ClearUi() {
    RestoreVisibility();
    RestoreNeedCounts();
    for (auto* f : g_pe) pe::RemoveIntercept(f);
    for (auto* f : g_nx) nx::Unhook(f);
    for (int observer : g_obs) pe::Unobserve(observer);
    g_pe.clear(); g_nx.clear(); g_obs.clear(); g_watched.clear(); g_requirements.clear();
    g_mode = 0; g_earlyLogged = false; g_abilitySince = g_abilityScan = 0; g_panelWasOpen = false; g_up = g_no = g_complete = -1; g_stateHook = g_costHook = g_rowHook = false;
}
} // namespace towerpoints
