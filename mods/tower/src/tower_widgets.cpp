#include "tower_verbose.h"
#include "tower_widgets.h"
#include "tower_points.h"
#include "tower_bar_math.h"
#include "tower_api.h"
namespace towernative { int Level(std::string_view weaponId, int kind); }
#include <windows.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include "../../../src/core/host/log.h"
#include "tower_perf.h"
#include "tower_modal_logic.h"
#include "tower_l10n_text.h"

namespace towerwidgets {
namespace ue = pbz::ue;
namespace pe = pbz::pe;
namespace {
struct Args {
    UObject* fn;
    std::array<uint8_t, 0x500> bytes{};
    uint8_t* At(const char* key) {
        const int o = fn ? ue::PropOffset(fn, key) : -1;
        return o >= 0 && o < 0x400 ? bytes.data() + o : nullptr;
    }
    bool Obj(const char* key, UObject* obj) { auto* p = At(key); if (!p) return false; std::memcpy(p, &obj, 8); return true; }
};
UObject* Fn(UObject* obj, const char* name) { return obj && ue::Live(obj) ? pe::FindFunction(ue::ClassOf(obj), name) : nullptr; }
std::wstring Wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, 0);
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
void Str(uint8_t* to, std::wstring& s) {
    struct String { wchar_t* data; int32_t num, max; } a{s.data(), static_cast<int32_t>(s.size() + 1), static_cast<int32_t>(s.size() + 1)};
    std::memcpy(to, &a, sizeof a);
}
UObject* Create(UObject* cls) {
    auto* lib = Default(towerperf::ClassNamed("WidgetBlueprintLibrary"));
    auto* fn = Fn(lib, "Create");
    UObject* pc = Controller();
    if (!fn || !pc || !cls) return nullptr;
    Args a{fn};
    if (!a.Obj("WorldContextObject", pc) || !a.Obj("WidgetType", cls) || !a.Obj("OwningPlayer", pc) || !a.At("ReturnValue")) return nullptr;
    pe::Call(lib, fn, a.bytes.data());
    return ue::Get<UObject*>(a.At("ReturnValue"));
}
int g_prepare = 0, g_prepare2 = 0;
unsigned long long g_pendingUntil = 0;
UObject* g_panel = nullptr;
bool g_overlay = false;
UObject* g_countdown = nullptr;
void ClearPrepare() {
    if (g_prepare) pe::Unobserve(g_prepare);
    if (g_prepare2) pe::Unobserve(g_prepare2);
    g_prepare = g_prepare2 = 0; g_pendingUntil = 0;
}
bool Activated(UObject* widget) {
    auto* f = Fn(widget, "IsActivated");
    if (!f) return false;
    Args a{f};
    if (!a.At("ReturnValue")) return false;
    pe::Call(widget, f, a.bytes.data());
    return *a.At("ReturnValue") != 0;
}
// ed47e1f live regression: active HUD/subtitle/QTE roots support activation focus
// while the actual router remains in Game mode. Consult its active input owner,
// not every CommonActivatableWidget's capability flags. No widgets are closed.
std::unordered_set<UObject*> g_own;
UObject *g_router = nullptr, *g_routerCls = nullptr, *g_modal = nullptr;
unsigned long long g_nextCheck = 0, g_nextRouterFind = 0;
bool Mine(UObject* w) { return w == g_panel || (g_overlay && g_own.count(w) > 0); }
void ModalTick() {
    const auto now = GetTickCount64();
    if (now < g_nextCheck) return;
    g_nextCheck = now + 100;
    UObject* previous = g_modal;
    g_modal = nullptr;
    if (!g_router || !ue::Live(g_router) || !g_routerCls || !ue::Live(g_routerCls) || !ue::IsA(g_router, g_routerCls)) {
        g_router = nullptr;
        if (now < g_nextRouterFind) return;
        g_nextRouterFind = now + 2000;
        g_routerCls = towerperf::ClassNamed("PBZUIActionRouterBase");
        if (!g_routerCls) return;
        int matches = 0;
        ue::ForEach([&](int, UObject* o) {
            if (o && !ue::IsDefault(o) && ue::IsA(o, g_routerCls) && ue::Live(o)) { g_router = o; ++matches; }
            return true;
        });
        if (matches != 1) { g_router = nullptr; return; }
        pbz::log::Info("影塔原生模态：使用游戏输入路由判定（{}），不以支持焦点当作输入占用", ue::ObjName(g_router));
    }
    auto* modeFn = Fn(g_router, "GetActiveInputMode");
    auto* menuFn = Fn(g_router, "IsMenuInputModeUI");
    auto* topFn = Fn(g_router, "GetActiveWidget");
    if (!modeFn || !menuFn || !topFn) return;
    Args mode{modeFn}, menu{menuFn}, top{topFn};
    if (!mode.At("ReturnValue") || !menu.At("ReturnValue") || !top.At("ReturnValue")) return;
    pe::Call(g_router, modeFn, mode.bytes.data());
    pe::Call(g_router, menuFn, menu.bytes.data());
    pe::Call(g_router, topFn, top.bytes.data());
    const auto input = static_cast<modal::InputMode>(*mode.At("ReturnValue"));
    const bool menuInput = *menu.At("ReturnValue") != 0;
    auto* leaf = ue::Get<UObject*>(top.At("ReturnValue"));
    const bool live = leaf && ue::Live(leaf);
    // Native IsActivated handles any Boolean representation without guessed masks.
    if (modal::Blocks(true, input, menuInput, live, live && Activated(leaf), live && Mine(leaf))) g_modal = leaf;
    if (towerdev::kVerbose && previous != g_modal)
        pbz::log::Info("影塔原生模态：当前阻挡={}（输入模式 {}，菜单输入 {}，路由顶层 {}）", g_modal ? ue::ClassName(g_modal) : "-",
                       static_cast<int>(input), menuInput, live ? ue::ClassName(leaf) : "-");
}
} // namespace

UObject* NativeModal() { return g_modal && ue::Live(g_modal) && !Mine(g_modal) && Activated(g_modal) ? g_modal : nullptr; }
std::string NativeModalName() { UObject* m = NativeModal(); return m ? ue::ClassName(m) : std::string("-"); }
void Own(UObject* w) { if (w) g_own.insert(w); }

UObject* Default(UObject* cls) {
    static std::unordered_map<UObject*, UObject*> cache;
    if (!cls || !ue::Live(cls)) return nullptr;
    if (auto it = cache.find(cls); it != cache.end() && ue::Live(it->second) && ue::ClassOf(it->second) == cls && ue::IsDefault(it->second)) return it->second;
    // 第一次整表找到后记下 UClass 里默认对象指针的偏移，之后直接读并核对，每个新类省一次全表扫描
    static int cdoOffset = -1;
    UObject* out = nullptr;
    if (cdoOffset >= 0) {
        auto* c = ue::Get<UObject*>(cls + cdoOffset);
        if (c && ue::Live(c) && ue::ClassOf(c) == cls && ue::IsDefault(c)) out = c;
    }
    if (!out) {
        ue::ForEach([&](int, UObject* o) { if (ue::ClassOf(o) == cls && ue::IsDefault(o)) { out = o; return false; } return true; });
        if (out && cdoOffset < 0) for (int o = 0x80; o <= 0x1C0; o += 8) if (ue::Get<UObject*>(cls + o) == out) { cdoOffset = o; break; }
    }
    if (out) cache[cls] = out;
    return out;
}
UObject* Pawn() {
    for (auto& t : pbz::attr::Targets()) if (t.who == pbz::attr::Who::Player && pbz::attr::Alive(t)) return t.obj;
    return nullptr;
}
UObject* Controller() {
    UObject* p = Pawn();
    const int off = ue::PropOffset(towerperf::ClassNamed("Pawn"), "Controller");
    auto* pc = p && off >= 0 ? ue::Get<UObject*>(p + off) : nullptr;
    return pc && ue::Live(pc) ? pc : nullptr;
}
uint64_t Name(const char* value) {
    UObject* lib = Default(towerperf::ClassNamed("KismetStringLibrary"));
    auto* fn = Fn(lib, "Conv_StringToName");
    if (!fn) return 0;
    Args a{fn}; auto w = Wide(value);
    if (!a.At("InString") || !a.At("ReturnValue")) return 0;
    Str(a.At("InString"), w); pe::Call(lib, fn, a.bytes.data());
    return ue::Get<uint64_t>(a.At("ReturnValue"));
}
UObject* LoadClass(const char* path) {
    // Engine creates the soft reference; never synthesize FSoftObjectPtr layout.
    auto* lib = Default(towerperf::ClassNamed("PBZBlueprintFunctionLibrary"));
    auto* softFn = Fn(lib, "GetSoftClass");
    auto* sys = Default(towerperf::ClassNamed("KismetSystemLibrary"));
    auto* load = Fn(sys, "LoadClassAsset_Blocking");
    if (!softFn || !load) return nullptr;
    Args a{softFn}; auto w = Wide(path);
    if (!a.At("Path") || !a.At("ReturnValue")) return nullptr;
    Str(a.At("Path"), w); pe::Call(lib, softFn, a.bytes.data());
    Args b{load};
    if (!b.At("AssetClass") || !b.At("ReturnValue")) return nullptr;
    // SDK: a soft class property is 0x28 bytes (bell UpgradeWidgetClass -> AbilityWidgetClass).
    std::memcpy(b.At("AssetClass"), a.At("ReturnValue"), 0x28);
    pe::Call(sys, load, b.bytes.data());
    return ue::Get<UObject*>(b.At("ReturnValue"));
}

// 用户 2026-11-11：塔档是新档，原生「饰品打磨」页签要求玩家有界面可见标签 UI.VisibilityTag.Accessory（原游戏随剧情解锁），
// 没有就隐藏页签，升级面板只剩武器 / 影之武装。只在塔内、打开饰品打磨前补这一个标签（药瓶升级标签不补：药瓶按层自动升级）
bool EnsureVisibilityTag(const char* tag) {
    auto* libCls = towerperf::ClassNamed("PBZBlueprintFunctionLibrary");
    UObject* lib = libCls ? Default(libCls) : nullptr;
    auto* has = lib ? Fn(lib, "HasCommonVisibilityTag") : nullptr;
    auto* add = lib ? Fn(lib, "AddCommonVisibilityTag") : nullptr;
    UObject* pc = Controller();
    const int playerOff = ue::PropOffset(towerperf::ClassNamed("PlayerController"), "Player");
    UObject* local = pc && playerOff >= 0 ? ue::Get<UObject*>(pc + playerOff) : nullptr;
    const uint64_t name = Name(tag);
    if (!has || !add || !local || !name) { pbz::log::Info("影塔原生界面：可见标签 {} 无法补（接口 {}/{}，本地玩家 {}）", tag, !!has, !!add, !!local); return false; }
    Args h{has}; h.Obj("LocalPlayer", local); if (auto* t = h.At("CheckTag")) std::memcpy(t, &name, 8); h.Obj("ContextObject", pc);
    pe::Call(lib, has, h.bytes.data());
    if (auto* r = h.At("ReturnValue"); r && *r) return true;
    Args a{add}; a.Obj("LocalPlayer", local); if (auto* t = a.At("AddTag")) std::memcpy(t, &name, 8); a.Obj("ContextObject", pc);
    pe::Call(lib, add, a.bytes.data());
    Args h2{has}; h2.Obj("LocalPlayer", local); if (auto* t = h2.At("CheckTag")) std::memcpy(t, &name, 8); h2.Obj("ContextObject", pc);
    pe::Call(lib, has, h2.bytes.data());
    const bool ok = h2.At("ReturnValue") && *h2.At("ReturnValue");
    pbz::log::Info("影塔原生界面：补可见标签 {}：{}", tag, ok ? "成功" : "读回没有");
    return ok;
}
// E_UpgradeType 的枚举值（用户自定义枚举，值名是 NewEnumeratorN，按显示名找）；找不到 -1
int UpgradeTypeValue(const char* display) {
    static std::map<std::string, int> cache;
    if (auto it = cache.find(display); it != cache.end()) return it->second;
    UObject* en = nullptr;
    ue::ForEach([&](int, UObject* o) { if (ue::ObjName(o) == "E_UpgradeType" && !ue::IsDefault(o)) { en = o; return false; } return true; });
    auto* lib = Default(towerperf::ClassNamed("KismetNodeHelperLibrary"));
    auto* f = lib ? Fn(lib, "GetEnumeratorUserFriendlyName") : nullptr;
    if (!en || !f) return -1;
    for (uint8_t i = 0; i < 8; ++i) {
        Args a{f}; if (!a.At("Enum") || !a.At("EnumeratorValue") || !a.At("ReturnValue")) return -1;
        std::memcpy(a.At("Enum"), &en, 8); *a.At("EnumeratorValue") = i; pe::Call(lib, f, a.bytes.data());
        struct String { wchar_t* data; int32_t num, max; } str = ue::Get<String>(a.At("ReturnValue"));
        if (!str.data || str.num < 1 || str.num > 128) continue;
        std::wstring w(str.num, 0); if (!ue::Read(str.data, w.data(), str.num * 2)) continue;
        std::string name; for (wchar_t c : w) if (c) name += static_cast<char>(c);
        cache.emplace(name, i);
    }
    const auto it = cache.find(display);
    pbz::log::Info("影塔原生界面：E_UpgradeType「{}」= {}", display, it == cache.end() ? -1 : it->second);
    return it == cache.end() ? -1 : it->second;
}
bool Open(Panel panel, std::string* why) {
    if (!Pawn() || !Controller()) { *why = "玩家或控制器尚未就绪"; return false; }
    if (PanelActive()) { *why = "请先关闭当前原生面板"; return false; }
    ClearPrepare();
    // 铸魂与装备升级是同一个现代升级面板：按剧情节点 FlowNode/PushUpgradeUINode.js 的 BeforePush 初始化——
    // 装备升级 DefaultID=weapon、IsZhuHun=false；铸魂 DefaultID=weapon、IsZhuHun=true（7c2668e 实测：旧钟 PushSnapSoul
    // 只设 UpgradeType，没设 IsZhuHun，面板开成普通升级）。字段在面板构建 / 激活之前写，两处都写，幂等
    const bool upgrade = panel == Panel::Weapon || panel == Panel::Soul || panel == Panel::Accessory;
    const bool accessory = panel == Panel::Accessory;
    const char* clsName = panel == Panel::Character ? "WB_CharacterV5_C" : panel == Panel::Ability ? "W_AbilityWidget_C" : "W_UpgradeActivatableWidget_V1_C";
    const char* path = panel == Panel::Character ? "/Game/PBZContent/UI/UINew/System/Character/WB_CharacterV5.WB_CharacterV5_C" :
                       panel == Panel::Ability ? "/Game/PBZContent/UI/AbilityWidget/W_AbilityWidget.W_AbilityWidget_C" :
                       "/Game/PBZContent/UI/UINew/System/Upgrade/W_UpgradeActivatableWidget_V1.W_UpgradeActivatableWidget_V1_C";
    auto* cls = towerperf::ClassNamed(clsName);
    if (!cls) cls = LoadClass(path);
    auto* lib = Default(towerperf::ClassNamed("KismetSystemLibrary"));
    auto* convert = Fn(lib, "Conv_ClassToSoftClassReference");
    auto* async = Default(towerperf::ClassNamed("AsyncAction_PushContentToLayerForPlayer"));
    auto* push = Fn(async, "PushContentToLayerForPlayer");
    auto* onActivate = cls ? pe::FindFunction(cls, "BP_OnActivated") : nullptr;
    if (!cls || !convert || !push || !onActivate) {
        *why = std::format("原生界面不可用：{}{}{}{}", cls ? "" : std::string("界面类 ") + clsName + " 未加载；", convert ? "" : "软类转换缺失；",
                           push ? "" : "CommonUI 压入接口缺失；", cls && !onActivate ? "界面没有 BP_OnActivated；" : "");
        pbz::log::Info("影塔原生界面：{}", *why);
        return false;
    }
    const uint64_t weapon = Name(accessory ? "accessory" : "weapon");  // 原生 FirstTab 的标签 ID：weapon / shachiweapon / accessory / medicine
    const int idOff = upgrade ? ue::PropOffset(cls, "DefaultID") : -1, soulOff = upgrade ? ue::PropOffset(cls, "IsZhuHun") : -1;
    if (upgrade && (idOff < 0 || soulOff < 0 || !weapon)) { *why = "原生升级面板字段（DefaultID / IsZhuHun）不可用"; return false; }
    if (!towerpoints::PrepareUi(panel == Panel::Weapon || panel == Panel::Accessory ? 1 : panel == Panel::Soul ? 3 : panel == Panel::Ability ? 2 : 0, why)) return false;
    if (accessory) EnsureVisibilityTag("UI.VisibilityTag.Accessory");
    const bool soul = panel == Panel::Soul;
    // 只写本次压入的那个实例（在等待窗口内、第一个出现的非默认对象）
    auto init = [cls, upgrade, soul, accessory, weapon, idOff, soulOff](UObject* w) {
        if (!upgrade) return;
        ue::Write(w + idOff, &weapon, 8);
        const uint8_t zhuhun = soul ? 1 : 0;
        ue::Write(w + soulOff, &zhuhun, 1);
        // 面板实例可能被复用、残留上一次的模式：类型照默认对象复原（剧情节点不改它，由 IsZhuHun 区分）
        const int type = ue::PropOffset(cls, "UpgradeType");
        if (type < 0) return;
        uint8_t want = 0;
        if (auto* cdo = Default(cls)) want = ue::Get<uint8_t>(cdo + type);
        // 饰品打磨：和钟菜单 Push Accessory Upgrade 一样把 UpgradeType 设成 Accessory（按枚举显示名找值）
        if (accessory) if (const int v = UpgradeTypeValue("Accessory"); v >= 0) want = static_cast<uint8_t>(v);
        const uint8_t had = ue::Get<uint8_t>(w + type);
        ue::Write(w + type, &want, 1);
        // 用户 2026-11-11 实机：面板实例池化复用，一级分类（武器 / 影之武装 / 饰品 / 药瓶）按 UpgradeType 在构建时排好；
        // 复用的实例类型不同时，调面板自己的 SetUpgradeType 重新排一遍
        if (had != want) if (auto* set = pe::FindFunction(cls, "SetUpgradeType")) {
            Args a{set}; if (auto* at = a.At("UpgradeType")) { *at = want; pe::Call(w, set, a.bytes.data()); }
            pbz::log::Info("影塔原生界面：升级面板类型 {} → {}（SetUpgradeType）", had, want);
            // 实机：只调 SetUpgradeType 一级分类不变（复用实例的分类在构建时就排好了）。按新类型重跑一次面板自己的
            // PreConstruct，让它像新建实例一样重排（这里的观察回调再进来时类型已一致，不会递归）
            if (auto* pre = pe::FindFunction(cls, "PreConstruct")) { Args p{pre}; if (auto* d = p.At("IsDesignTime")) *d = 0; pe::Call(w, pre, p.bytes.data()); pbz::log::Info("影塔原生界面：升级面板按新类型重跑 PreConstruct"); }
        }
    };
    if (auto* construct = upgrade ? pe::FindFunction(cls, "PreConstruct") : nullptr)  // 这个面板没有 Construct；PreConstruct 在构建前、观察在原函数之前
        g_prepare2 = pe::Observe(construct, [cls, init](UObject* w, void*) {
            if (ue::IsA(w, cls) && !ue::IsDefault(w) && g_pendingUntil && GetTickCount64() <= g_pendingUntil) init(w);
        });
    g_prepare = pe::Observe(onActivate, [cls, init, soul](UObject* w, void*) {
        if (!ue::IsA(w, cls) || ue::IsDefault(w) || !g_pendingUntil || GetTickCount64() > g_pendingUntil) return;
        init(w);
        if (soul) pbz::log::Info("影塔原生界面：铸魂面板已按剧情节点初始化（DefaultID=weapon，IsZhuHun=true）");
        g_panel = w;
        g_pendingUntil = 0;
    });
    Args c{convert};
    if (!c.Obj("Class", cls) || !c.At("ReturnValue")) { ClearPrepare(); *why = "原生软类转换参数变化"; return false; }
    pe::Call(lib, convert, c.bytes.data());
    Args a{push};
    if (!a.Obj("OwningPlayer", Controller()) || !a.At("WidgetClass") || !a.At("LayerName") || !a.At("bSuspendInputUntilComplete") || !a.At("ReturnValue")) { ClearPrepare(); *why = "原生面板参数变化"; return false; }
    std::memcpy(a.At("WidgetClass"), c.At("ReturnValue"), 0x28);
    const auto layer = Name("UI.Layer.Menu"); std::memcpy(a.At("LayerName"), &layer, 8);
    *a.At("bSuspendInputUntilComplete") = 1;
    g_pendingUntil = GetTickCount64() + 10000;
    pe::Call(async, push, a.bytes.data());
    auto* action = ue::Get<UObject*>(a.At("ReturnValue"));
    auto* activate = Fn(action, "Activate");
    if (!activate) { ClearPrepare(); *why = "原生没有返回压入动作"; return false; }
    pe::Call(action, activate, nullptr);
    pbz::log::Info("影塔原生界面：压入 {}{}", clsName, panel == Panel::Soul ? "（铸魂）" : accessory ? "（饰品打磨）" : "");
    return true;
}
bool PanelActive() { return (g_pendingUntil && GetTickCount64() <= g_pendingUntil) || (g_panel && ue::Live(g_panel) && Activated(g_panel)) || NativeModal(); }
void OverlayBusy(bool busy) { g_overlay = busy; if (!busy) g_own.clear(); }
bool MenuBusy() { return g_overlay || PanelActive(); }
#ifdef PBZ_HEX_TEST
std::string QaCloseBlocking() {
    std::string out;
    UObject* panel = g_panel && ue::Live(g_panel) && Activated(g_panel) ? g_panel : nullptr;
    for (UObject* w : {NativeModal(), panel}) {
        if (!w) continue;
        if (auto* f = Fn(w, "DeactivateWidget")) { pe::Call(w, f, nullptr); out += (out.empty() ? "" : "、") + ue::ClassName(w); }
    }
    g_pendingUntil = 0;
    return out;
}
#endif

void TrackPanel(UObject* w) { g_panel = w; }
bool IsActivated(UObject* w) { return w && ue::Live(w) && Activated(w); }
// 武库扩建 4 格装备页调查（用户 2026-11-11）：装备页兵器格（W_CharacterButton_MainWeapon_C / _ShaChiWeapon_C）的容器结构。
// 不扫对象表（之前整表扫描每 1.5 秒耗 22 ms）：观察格子自己的构建 / 初始化函数，拿到格子后在下一帧往上走 6 层父容器，
// 每层记类、名字、子控件名单。每种格子每次进程记一次
UObject* g_equipSeen[2] = {};
UObject* g_equipCell[2] = {};  // 最近一次见到的原生兵器格（武库扩建 4+4 装备页用）
bool g_equipLogged[2] = {}, g_equipHooks = false;
unsigned long long g_equipNextHook = 0;
void ProbeEquipPage() {
    if (!towerapi::InsideTower()) return;
    const auto now = GetTickCount64();
    if (!g_equipHooks && now >= g_equipNextHook) {
        g_equipNextHook = now + 2000;
        auto* mainCls = towerperf::ClassNamed("W_ButtonBase_Weapon_C");  // 实机装备页是 WB_Character_V4（MainWeapon_Current / _Other）
        auto* shaCls = towerperf::ClassNamed("W_ButtonBase_ShaChiWeapon_C");
        if (mainCls && shaCls) {
            int n = 0;
            for (int k = 0; k < 2; ++k)
                for (const char* fn : {"PreConstruct", "InitMainWeapon", "InitShaChiWeapon", "BP_OnHovered", "GetItem", "InitUpgradeState"})
                    if (auto* f = pe::FindFunction(k ? shaCls : mainCls, fn)) { UObject* want = k ? shaCls : mainCls; pe::Observe(f, [k, want](UObject* o, void*) {
                        if (!o || !ue::IsA(o, want)) return;
                        const auto name = ue::ObjName(o);
                        if (name != (k ? "ShaChiWeapon_Current" : "MainWeapon_Current") &&
                            name != (k ? "ShaChiWeapon_Other" : "MainWeapon_Other")) return;
                        g_equipSeen[k] = o;
                        if (name == (k ? "ShaChiWeapon_Current" : "MainWeapon_Current")) g_equipCell[k] = o;
                    }); ++n; }  // 父类共用函数：只认装备页原生兵器格
            g_equipHooks = true;
            pbz::log::Info("影塔装备页调查：观察兵器格 {} 个入口", n);
        }
    }
    auto call = [](UObject* w, const char* fn, const int* arg = nullptr) -> UObject* {
        auto* f = w ? Fn(w, fn) : nullptr; if (!f) return nullptr;
        Args a{f}; if (arg) if (auto* p = a.At("Index")) std::memcpy(p, arg, 4);
        pe::Call(w, f, a.bytes.data()); auto* r = a.At("ReturnValue"); return r ? ue::Get<UObject*>(r) : nullptr;
    };
    auto count = [](UObject* w) { auto* f = w ? Fn(w, "GetChildrenCount") : nullptr; if (!f) return -1; Args a{f}; pe::Call(w, f, a.bytes.data()); auto* r = a.At("ReturnValue"); return r ? ue::Get<int32_t>(r) : -1; };
    const int slotOff = ue::PropOffset(towerperf::ClassNamed("Widget"), "Slot");
    for (int k = 0; k < 2; ++k) {
        UObject* b = g_equipSeen[k];
        if (!b || g_equipLogged[k] || !ue::Live(b) || !call(b, "GetParent")) continue;  // 还没挂到界面上：下一帧再看
        g_equipLogged[k] = true;
        UObject* w = b;
        for (int level = 0; level < 6 && w; ++level) {
            UObject* parent = call(w, "GetParent");
            UObject* slot = slotOff >= 0 ? ue::Get<UObject*>(w + slotOff) : nullptr;
            std::string kids;
            const int n = count(parent);
            for (int i = 0; i < n && i < 16; ++i) if (UObject* c = call(parent, "GetChildAt", &i)) kids += (kids.empty() ? "" : ", ") + ue::ObjName(c) + "(" + ue::ObjName(ue::ClassOf(c)) + ")";
            pbz::log::Info("影塔装备页调查：{} 第 {} 层 {}（{}）槽 {} → 父 {}（{}）子 {} 个：{}", k ? "影之武装格" : "主武器格", level, ue::ObjName(w), ue::ObjName(ue::ClassOf(w)),
                           slot ? ue::ObjName(ue::ClassOf(slot)) : "-", parent ? ue::ObjName(parent) : "-", parent ? ue::ObjName(ue::ClassOf(parent)) : "-", n, kids);
            w = parent;
        }
    }
}
// ---- 武库扩建 4+4 装备页（用户 2026-11-11）----
// 原生装备页 WB_Character_V4 的兵器格放在一个 UniformGridPanel（3 列 × 2 行：武器 / 影之武装 / 药瓶）。
// 原生脚本写死两格，不能改脚本，所以只在塔内、持有武库扩建时：武器列左边、影之武装列左边各插一列原生兵器格
// （第 3、4 把，显示轮转里的兵器），药瓶右移；原有格子宽度收窄 0.8。扩展格加入原生选中组并使用原生列表。
struct EquipGrid {
    UObject *grid = nullptr, *page = nullptr, *group = nullptr, *switcher = nullptr;
    int index = -1;
    struct Child { UObject* w; int col; double width; bool hadWidth; };
    std::vector<Child> original;
    UObject* cells[4] = {};   // 主 3、主 4、影 3、影 4
    int shown[4] = {-2, -2, -2, -2};
    UObject* preview = nullptr;
    int pendingClick = -1, picker = -1;
    bool listOpened = false;
    unsigned long long pickerDeadline = 0, restoreFocusAt = 0;
};
EquipGrid g_equip;
UObject* GridSlotOf(UObject* w) { const int o = ue::PropOffset(towerperf::ClassNamed("Widget"), "Slot"); return o >= 0 && w ? ue::Get<UObject*>(w + o) : nullptr; }
UObject* CallW(UObject* w, const char* fn) { auto* f = w ? Fn(w, fn) : nullptr; if (!f) return nullptr; Args a{f}; pe::Call(w, f, a.bytes.data()); auto* r = a.At("ReturnValue"); return r ? ue::Get<UObject*>(r) : nullptr; }
void SetInt(UObject* w, const char* fn, const char* param, int v) { auto* f = w ? Fn(w, fn) : nullptr; if (!f) return; Args a{f}; if (auto* p = a.At(param)) std::memcpy(p, &v, 4); pe::Call(w, f, a.bytes.data()); }
void SetFloat(UObject* w, const char* fn, const char* param, float v) { auto* f = w ? Fn(w, fn) : nullptr; if (!f) return; Args a{f}; if (auto* p = a.At(param)) std::memcpy(p, &v, 4); pe::Call(w, f, a.bytes.data()); }
UObject* EquipMember(UObject* owner, const char* name) {
    if (!owner || !ue::Live(owner)) return nullptr;
    const int off = ue::PropOffset(ue::ClassOf(owner), name);
    auto* member = off >= 0 ? ue::Get<UObject*>(owner + off) : nullptr;
    return member && ue::Live(member) ? member : nullptr;
}
UObject* EquipPage(UObject* cell) {
    for (int i = 0; i < 8 && cell && ue::Live(cell); ++i, cell = ue::Get<UObject*>(cell + ue::Layout::ObjOuter))
        if (ue::ClassName(cell) == "WB_Character_V4_C") return cell;
    return nullptr;
}
bool EquipPageActive(UObject* page) {
    // WB_Character_V4 is a nested system widget, not an activatable screen.
    // The input router owns the surrounding W_Systembase.
    auto* modal = NativeModal();
    if (!page || !ue::Live(page) || !modal || ue::ClassName(modal) != "W_Systembase_C") return false;
    auto* fn = Fn(page, "IsRendered");
    if (!fn) return false;
    Args a{fn};
    if (!a.At("ReturnValue")) return false;
    pe::Call(page, fn, a.bytes.data());
    return *a.At("ReturnValue") != 0;
}
UObject* EquipSwitcher(UObject* page, UObject* grid) {
    auto* cls = towerperf::ClassNamed("WidgetSwitcher");
    if (!cls) return nullptr;
    if (auto* sw = EquipMember(page, "WidgetSwitcher_53"); sw && ue::IsA(sw, cls)) return sw;
    // Some builds only expose the switcher through the widget tree.
    auto* panel = EquipMember(page, "W_SystemPanel_V1");
    int depth = 0;
    for (auto* parent = grid; parent && panel && depth++ < 16; parent = CallW(parent, "GetParent")) {
        if (!ue::IsA(parent, cls)) continue;
        int panelDepth = 0;
        for (auto* ancestor = panel; ancestor && panelDepth++ < 16; ancestor = CallW(ancestor, "GetParent"))
            if (ancestor == parent) return parent;
    }
    return nullptr;
}
int EquipListIndex(UObject* switcher) {
    if (!switcher || !ue::Live(switcher)) return -1;
    if (auto* fn = Fn(switcher, "GetActiveWidgetIndex")) {
        Args a{fn};
        if (auto* value = a.At("ReturnValue")) {
            pe::Call(switcher, fn, a.bytes.data());
            return ue::Get<int>(value, -1);
        }
    }
    const int off = ue::PropOffset(ue::ClassOf(switcher), "ActiveWidgetIndex");
    return off >= 0 ? ue::Get<int>(switcher + off, -1) : -1;
}
UObject* EquipAnchor(UObject* page, int slot) {
    return EquipMember(page, slot < 2 ? "MainWeapon_Other" : "ShaChiWeapon_Other");
}
bool EquipSelect(UObject* group, UObject* button, bool focus = false);
bool EquipPreview(UObject* anchor, UObject* button, UObject* group) {
    if (!anchor || !button || !ue::Live(button)) return false;
    // The page's Puerts selection listener accepts the selected button, unlike
    // the click listener which closes over MainWeapon_Other/ShaChiWeapon_Other.
    // Use the reflected selection parameters and only forward to the unique
    // script listener already bound by the live page.
    const int off = ue::PropOffset(ue::ClassOf(anchor), "OnSelectedChangedBase");
    struct Binding { int index, serial; uint32_t function, number; };
    struct Bindings { const Binding* data; int count, capacity; };
    static_assert(sizeof(Binding) == 16);
    if (off < 0) return false;
    const auto bindings = ue::Get<Bindings>(anchor + off);
    if (!bindings.data || bindings.count < 1 || bindings.count > 8 || bindings.capacity < bindings.count) return false;
    UObject* script = nullptr;
    for (int i = 0; i < bindings.count; ++i) {
        Binding binding{};
        if (!ue::Read(bindings.data + i, &binding, sizeof binding) || binding.index < 0 ||
            binding.serial <= 0 || binding.number || ue::Name(binding.function) != "Fire") continue;
        auto* target = ue::Obj(binding.index);
        if (!target || !ue::Live(target) || ue::ClassName(target) != "DynamicDelegateProxy") continue;
        if (script) return false;
        script = target;
    }
    auto* fire = Fn(script, "Fire");
    auto* signature = Fn(group, "OnSelectionStateChangedBase");
    if (!fire || !signature || !pe::Ready()) return false;
    Args args{signature};
    if (!args.Obj("BaseButton", button) || !args.At("bIsSelected")) return false;
    *args.At("bIsSelected") = 1;
    // Puerts dispatches Fire in its virtual ProcessEvent override. Calling the
    // core's base ProcessEvent trampoline only executes the empty Fire body.
    // Use the same ProcessEvent vtable slot the core validates at startup.
    constexpr int processEventSlot = 76;
    const auto* vtable = ue::Get<const uintptr_t*>(script);
    const auto entry = vtable ? ue::Get<uintptr_t>(vtable + processEventSlot) : 0;
    MEMORY_BASIC_INFORMATION page{};
    if (!entry || !VirtualQuery(reinterpret_cast<void*>(entry), &page, sizeof page) ||
        page.Type != MEM_IMAGE || page.AllocationBase != GetModuleHandleW(nullptr) ||
        !(page.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
    using Dispatch = void(__fastcall*)(UObject*, UObject*, void*);
    reinterpret_cast<Dispatch>(entry)(script, fire, args.bytes.data());
    return true;
}
bool OpenEquipList(EquipGrid& grid, int slot) {
    auto* button = grid.cells[slot];
    auto* anchor = EquipAnchor(grid.page, slot);
    auto* click = Fn(anchor, "HandleButtonClicked");
    if (!button || !ue::Live(button) || !click || !EquipSelect(grid.group, anchor)) return false;
    // The original replace handler resolves the destination from the selected
    // native slot. Pass the extra slot's ID to the list opener and immediately
    // restore that native slot's ID for the original confirmation handler.
    const int idOff = ue::PropOffset(ue::ClassOf(anchor), "WeaponID");
    if (idOff < 0 || ue::ClassOf(button) != ue::ClassOf(anchor)) return false;
    const auto original = ue::Get<uint64_t>(anchor + idOff);
    const auto wanted = ue::Get<uint64_t>(button + idOff);
    if (!towerapi::ArsenalNativeBegin(slot / 2, slot % 2)) return false;
    if (!ue::Write(anchor + idOff, &wanted, sizeof wanted)) {
        towerapi::ArsenalNativeBegin(-1, 0);
        return false;
    }
    Args args{click};
    pe::Call(anchor, click, args.bytes.data());
    if (ue::Live(anchor) && ue::Get<uint64_t>(anchor + idOff) == wanted)
        ue::Write(anchor + idOff, &original, sizeof original);
    return true;
}
void EquipGridInput() {
    auto& grid = g_equip;
    if (!grid.grid || !EquipPageActive(grid.page) ||
        !grid.group || !ue::Live(grid.group)) return;
    const int index = EquipListIndex(grid.switcher);
    const auto now = GetTickCount64();
    if (grid.picker >= 0) {
        if (index == 1 && !grid.listOpened) {
            grid.listOpened = true;
            pbz::log::Info("影塔武库扩建：原生兵器列表已打开，第 {} 格", grid.picker % 2 + 3);
        }
        if (index < 0 && now >= grid.pickerDeadline) {
            towerapi::ArsenalNativeBegin(-1, 0);
            grid.picker = -1;
            pbz::log::Info("影塔武库扩建：原生列表状态不可读，取消新增槽换装");
        } else if (index == 0 && (grid.listOpened || now >= grid.pickerDeadline) && !grid.restoreFocusAt) {
            towerapi::ArsenalNativeBegin(-1, 0);
            grid.restoreFocusAt = now + 60;  // Wait for native navigateBack's delayed reselection.
            if (!grid.listOpened) pbz::log::Info("影塔武库扩建：原生兵器列表未打开，第 {} 格", grid.picker % 2 + 3);
        }
        if (index == 0 && grid.restoreFocusAt && now >= grid.restoreFocusAt) {
            auto* selected = CallW(grid.group, "GetSelectedButtonBase");
            if (!selected || selected == EquipAnchor(grid.page, grid.picker))
                EquipSelect(grid.group, grid.cells[grid.picker], true);
            grid.picker = -1;
            grid.preview = nullptr;
        }
    }
    if (index != 0 || grid.picker >= 0) { grid.pendingClick = -1; return; }
    auto* selected = CallW(grid.group, "GetSelectedButtonBase");
    if (selected != grid.preview) {
        grid.preview = selected;
        for (int slot = 0; slot < 4; ++slot) {
            if (!selected || selected != grid.cells[slot]) continue;
            const bool ok = EquipPreview(EquipAnchor(grid.page, slot), selected, grid.group);
            pbz::log::Info("影塔武库扩建：第 {} 格原生预览{}", slot % 2 + 3, ok ? "已调用" : "监听不可用");
            break;
        }
    }
    if (grid.pendingClick < 0) return;
    const int slot = grid.pendingClick;
    grid.pendingClick = -1;
    if (OpenEquipList(grid, slot)) {
        grid.picker = slot;
        grid.listOpened = false;
        grid.pickerDeadline = now + 300;
        grid.restoreFocusAt = 0;
    } else {
        EquipSelect(grid.group, grid.cells[slot], true);
        pbz::log::Info("影塔武库扩建：第 {} 格原生换装入口不可用", slot % 2 + 3);
    }
}
bool EquipGroupWidget(UObject* group, const char* action, UObject* button) {
    auto* fn = Fn(group, action);
    if (!fn || !button || !ue::Live(button)) return false;
    Args a{fn};
    if (!a.Obj("InWidget", button)) return false;
    pe::Call(group, fn, a.bytes.data());
    return true;
}
int EquipButtonIndex(UObject* group, UObject* button) {
    auto* fn = Fn(group, "FindButtonIndex");
    if (!fn) return -1;
    Args a{fn};
    if (!a.Obj("ButtonToFind", button) || !a.At("ReturnValue")) return -1;
    pe::Call(group, fn, a.bytes.data());
    return ue::Get<int>(a.At("ReturnValue"), -1);
}
bool EquipSelect(UObject* group, UObject* button, bool focus) {
    const int index = EquipButtonIndex(group, button);
    if (index < 0 || !Fn(group, "SelectButtonAtIndex")) return false;
    SetInt(group, "SelectButtonAtIndex", "ButtonIndex", index);
    if (CallW(group, "GetSelectedButtonBase") != button) return false;
    if (focus) CallW(button, "SetFocus");
    return true;
}
void RestoreEquipGrid(const char* why) {
    if (!g_equip.grid) return;
    towerapi::ArsenalNativeBegin(-1, 0);
    if (ue::Live(g_equip.grid) && ue::Get<int>(g_equip.grid + ue::Layout::ObjIndex, -1) == g_equip.index) {
        for (UObject* c : g_equip.cells) if (c && ue::Live(c)) {
            EquipGroupWidget(g_equip.group, "RemoveWidget", c);
            UObject* parent = CallW(c, "GetParent");
            CallW(parent && parent != g_equip.grid ? parent : c, "RemoveFromParent");  // 新格子外面包了 SizeBox：连盒子一起移除
        }
        for (const auto& c : g_equip.original) if (ue::Live(c.w)) {
            if (UObject* slot = GridSlotOf(c.w)) SetInt(slot, "SetColumn", "InColumn", c.col);
            if (c.hadWidth) SetFloat(c.w, "SetWidthOverride", "InWidthOverride", static_cast<float>(c.width));
        }
    }
    pbz::log::Info("影塔武库扩建：装备页还原 2+2（{}）", why);
    g_equip = {};
}
void InjectEquipGrid(UObject* cell, UObject* page) {
    UObject* box = CallW(cell, "GetParent"); UObject* grid = CallW(box, "GetParent");
    auto* gridCls = towerperf::ClassNamed("UniformGridPanel");
    if (!grid || !gridCls || !ue::IsA(grid, gridCls)) return;
    auto* mainCls = towerperf::ClassNamed("W_ButtonBase_Weapon_C"); auto* shaCls = towerperf::ClassNamed("W_ButtonBase_ShaChiWeapon_C");
    auto* sizeCls = towerperf::ClassNamed("SizeBox");
    if (!mainCls || !shaCls || !sizeCls) return;
    auto* group = EquipMember(page, "ButtonGroup");
    auto* switcher = EquipSwitcher(page, grid);
    if (!group || !switcher || EquipButtonIndex(group, cell) < 0 || EquipListIndex(switcher) != 0 ||
        !Fn(group, "AddWidget") || !Fn(group, "RemoveWidget") || !Fn(group, "SelectButtonAtIndex")) return;
    // 读现有 6 格：哪一列是武器 / 影之武装 / 药瓶
    auto* countFn = Fn(grid, "GetChildrenCount"); auto* atFn = Fn(grid, "GetChildAt");
    if (!countFn || !atFn) return;
    Args c{countFn}; pe::Call(grid, countFn, c.bytes.data()); const int n = c.At("ReturnValue") ? ue::Get<int32_t>(c.At("ReturnValue")) : 0;
    const int colOff = ue::PropOffset(towerperf::ClassNamed("UniformGridSlot"), "Column"), rowOff = ue::PropOffset(towerperf::ClassNamed("UniformGridSlot"), "Row");
    const int wOff = ue::PropOffset(sizeCls, "WidthOverride");
    const int hOff = ue::PropOffset(sizeCls, "HeightOverride");
    double cellW = 0, cellH = 0;  // 原生格子（SizeBox）的宽高，新格子照这个尺寸包一层
    EquipGrid eg; eg.grid = grid; eg.page = page; eg.group = group; eg.switcher = switcher;
    eg.index = ue::Get<int>(grid + ue::Layout::ObjIndex, -1);
    int mainCol = -1, shaCol = -1; std::vector<int> rows;
    for (int i = 0; i < n && i < 16; ++i) {
        Args a{atFn}; if (auto* p = a.At("Index")) std::memcpy(p, &i, 4); pe::Call(grid, atFn, a.bytes.data());
        UObject* w = a.At("ReturnValue") ? ue::Get<UObject*>(a.At("ReturnValue")) : nullptr;
        UObject* slot = GridSlotOf(w); if (!w || !slot || colOff < 0) return;
        const int col = ue::Get<int32_t>(slot + colOff), row = rowOff >= 0 ? ue::Get<int32_t>(slot + rowOff) : 0;
        UObject* content = ue::IsA(w, sizeCls) ? CallW(w, "GetContent") : w;
        if (content && ue::IsA(content, mainCls)) mainCol = col;
        if (content && ue::IsA(content, shaCls)) shaCol = col;
        if (std::find(rows.begin(), rows.end(), row) == rows.end()) rows.push_back(row);
        eg.original.push_back({w, col, wOff >= 0 && ue::IsA(w, sizeCls) ? ue::Get<float>(w + wOff) : 0.0, wOff >= 0 && ue::IsA(w, sizeCls) && ue::Get<float>(w + wOff) > 1});
        if (ue::IsA(w, sizeCls)) { if (wOff >= 0) cellW = std::max<double>(cellW, ue::Get<float>(w + wOff)); if (hOff >= 0) cellH = std::max<double>(cellH, ue::Get<float>(w + hOff)); }
    }
    if (mainCol < 0 || shaCol < 0 || rows.size() < 2) { pbz::log::Info("影塔武库扩建：装备页结构不认识（武器列 {} 影之武装列 {}，{} 行），不改", mainCol, shaCol, rows.size()); return; }
    std::sort(rows.begin(), rows.end());
    // 新列序：主新 0 / 主原 1 / 影新 2 / 影原 3 / 其余（药瓶）4
    for (auto& ch : eg.original) {
        UObject* slot = GridSlotOf(ch.w);
        const int to = ch.col == mainCol ? 1 : ch.col == shaCol ? 3 : 4;
        SetInt(slot, "SetColumn", "InColumn", to);
        if (ch.hadWidth) SetFloat(ch.w, "SetWidthOverride", "InWidthOverride", static_cast<float>(ch.width * 0.8));
    }
    auto* add = Fn(grid, "AddChildToUniformGrid");
    for (int k = 0; k < 4; ++k) {
        UObject* cellNew = Create(k < 2 ? mainCls : shaCls);
        if (!cellNew || !add) break;
        // 用户 2026-11-11：新格子和原生格子一样大——外面包一个同尺寸的 SizeBox（原生格子宽度已收窄 0.8）
        UObject* holder = cellNew;
        if (cellW > 1 || cellH > 1) {
            auto* gs = towerperf::ClassNamed("GameplayStatics"); UObject* gsLib = gs ? Default(gs) : nullptr; auto* spawn = gsLib ? Fn(gsLib, "SpawnObject") : nullptr;
            UObject* outer = ue::Get<UObject*>(grid + ue::Layout::ObjOuter);
            if (spawn && outer) {
                Args sp{spawn}; sp.Obj("ObjectClass", sizeCls); sp.Obj("Outer", outer); pe::Call(gsLib, spawn, sp.bytes.data());
                UObject* box = sp.At("ReturnValue") ? ue::Get<UObject*>(sp.At("ReturnValue")) : nullptr;
                if (box) {
                    if (cellW > 1) SetFloat(box, "SetWidthOverride", "InWidthOverride", static_cast<float>(cellW * 0.8));
                    if (cellH > 1) SetFloat(box, "SetHeightOverride", "InHeightOverride", static_cast<float>(cellH));
                    if (auto* setContent = Fn(box, "SetContent")) { Args sc{setContent}; sc.Obj("Content", cellNew); pe::Call(box, setContent, sc.bytes.data()); holder = box; }
                }
            }
        }
        Args a{add}; a.Obj("Content", holder);
        const int row = rows[static_cast<size_t>(k % 2)], col = k < 2 ? 0 : 2;
        if (auto* p = a.At("InRow")) std::memcpy(p, &row, 4);
        if (auto* p = a.At("InColumn")) std::memcpy(p, &col, 4);
        pe::Call(grid, add, a.bytes.data());
        eg.cells[k] = cellNew;
        if (CallW(holder, "GetParent") != grid || !EquipGroupWidget(group, "AddWidget", cellNew) ||
            EquipButtonIndex(group, cellNew) < 0) break;
    }
    g_equip = std::move(eg);
    if (std::any_of(std::begin(g_equip.cells), std::end(g_equip.cells), [&](UObject* c) {
            return !c || EquipButtonIndex(group, c) < 0;
        })) { RestoreEquipGrid("原生按钮组注册失败"); return; }
    pbz::log::Info("影塔武库扩建：装备页改为 4+4（武器列 {} → 1，影之武装列 {} → 3，新格 {} 个）", mainCol, shaCol, std::count_if(std::begin(g_equip.cells), std::end(g_equip.cells), [](UObject* x) { return x != nullptr; }));
}
// 新格子显示轮转里的第 3、4 把。
bool g_equipClickHook = false;
void EquipGridTick() {
    static unsigned long long next = 0; const auto now = GetTickCount64(); if (now < next) return; next = now + 200;  // 每 0.2 秒看一次
    const bool want = towerapi::InsideTower() && towerapi::Growth().on && towerapi::Growth().hexes.Has(tower::hex::Arsenal);
    if (!want) { if (g_equip.grid) RestoreEquipGrid(towerapi::InsideTower() ? "没有武库扩建" : "离开影塔"); return; }
    // 用户 2026-11-11 实机：换层后装备页重建，新页的兵器格不一定经过观察到的入口 → 一直是 2+2。
    // 菜单开着时（原生模态 / 面板激活）主动找当前显示的「MainWeapon_Current」：只在菜单开着且还没改到这一页时找，每 2 秒最多一次
    auto pageActive = [](UObject* c) { return EquipPageActive(EquipPage(c)); };
    UObject* cell = g_equipCell[0];
    const bool menuOpen = NativeModal() != nullptr || PanelActive();
    static unsigned long long nextFind = 0;
    if (menuOpen && (!cell || !ue::Live(cell) || !pageActive(cell)) && now >= nextFind) {
        nextFind = now + 2000;
        if (auto* mainCls = towerperf::ClassNamed("W_ButtonBase_Weapon_C")) {
            UObject* found = nullptr;
            ue::ForEach([&](int, UObject* o) {
                if (ue::ClassOf(o) != mainCls || ue::IsDefault(o) || !ue::Live(o) || ue::ObjName(o) != "MainWeapon_Current") return true;
                if (pageActive(o)) { found = o; return false; }
                return true;
            });
            if (found && found != cell) { g_equipCell[0] = cell = found; pbz::log::Info("影塔武库扩建：找到当前装备页"); }
        }
    }
    auto* page = EquipPage(cell);
    if (!EquipPageActive(page)) {
        if (g_equip.grid) {
            towerapi::ArsenalNativeBegin(-1, 0);
            g_equip.pendingClick = g_equip.picker = -1;
            g_equip.preview = nullptr;
        }
        return;
    }
    UObject* box = CallW(cell, "GetParent"); UObject* grid = box ? CallW(box, "GetParent") : nullptr;
    if (!grid) return;
    if (g_equip.grid && (g_equip.grid != grid || !ue::Live(g_equip.grid) ||
        g_equip.page != page || g_equip.group != EquipMember(page, "ButtonGroup")))
        RestoreEquipGrid("装备页重建");
    if (!g_equip.grid) InjectEquipGrid(cell, page);
    if (!g_equip.grid) return;
    if (!g_equipClickHook) {
        UObject* previous = nullptr;
        for (const char* cls : {"W_ButtonBase_Weapon_C", "W_ButtonBase_ShaChiWeapon_C"})
            if (auto* c = towerperf::ClassNamed(cls)) if (auto* f = pe::FindFunction(c, "BP_OnClicked"))
                if (f != previous) {
                    previous = f;
                    pe::Observe(f, [](UObject* o, void*) {
                        for (int k = 0; k < 4; ++k)
                            if (o && o == g_equip.cells[k]) { g_equip.pendingClick = k; break; }
                    });
                }
        g_equipClickHook = true;
    }
    UObject* logic = EquipMember(g_equip.page, "UIGameplayLogicComponent");
    for (int kind = 0; kind < 2; ++kind) {
        const auto extras = towerapi::ArsenalExtras(kind);
        for (int k = 0; k < 2; ++k) {
            const int idx = kind * 2 + k, w = extras[static_cast<size_t>(k)];
            UObject* c = g_equip.cells[idx];
            if (!c || !ue::Live(c) || g_equip.shown[idx] == w) continue;
            g_equip.shown[idx] = w;
            auto* init = Fn(c, kind ? "InitShaChiWeapon" : "InitMainWeapon");
            if (!init) continue;
            Args a{init};
            const auto idText = w >= 0 ? std::string(tower::growth::kWeapons[w].id) : std::string("None");
            const uint64_t id = Name(idText.c_str());
            if (auto* p = a.At("InWeaponID")) std::memcpy(p, &id, 8);
            const int lv = w >= 0 ? std::max(1, towernative::Level(idText, kind)) : 0;
            if (auto* p = a.At("InWeaponLevel")) std::memcpy(p, &lv, 4);
            if (logic) a.Obj("GameplayUILogicComponent", logic);
            pe::Call(c, init, a.bytes.data());
            // 初始化后的按钮可能还没发生过选中变化。让原生外观函数
            // 按按钮组的真实选中态刷新底图，包括原生空槽。
            if (auto* appearance = Fn(c, "DoSelectedApperanace")) {
                Args state{appearance};
                if (auto* selected = state.At("IsSelected")) {
                    *selected = CallW(g_equip.group, "GetSelectedButtonBase") == c;
                    pe::Call(c, appearance, state.bytes.data());
                }
            }
        }
    }
}
void Tick() {
    { towerperf::Section _("装备页调查"); ProbeEquipPage(); }
    { towerperf::Section _("武库扩建装备页"); EquipGridTick(); }
    EquipGridInput();
    if (g_prepare && (!g_pendingUntil || GetTickCount64() > g_pendingUntil)) ClearPrepare();
    if (g_panel && !ue::Live(g_panel)) g_panel = nullptr;
    { towerperf::Section _("原生模态追踪"); ModalTick(); }
    towerpoints::UiTick();
}
void Reset() { RestoreEquipGrid("界面重置"); ClearPrepare(); g_panel = nullptr; g_overlay = false; g_modal = nullptr; g_own.clear(); g_nextCheck = 0; Countdown(0); }

UObject* CreateWidget(UObject* cls) { return Create(cls); }
bool GetText(UObject* widget, std::string* out) {
    if (!out || !widget || !ue::Live(widget)) return false;
    auto* lib = Default(towerperf::ClassNamed("KismetTextLibrary"));
    auto* get = Fn(widget, "GetText"); auto* convert = Fn(lib, "Conv_TextToString");
    if (!get || !convert) return false;
    Args g{get}, c{convert};
    if (!g.At("ReturnValue") || !c.At("InText") || !c.At("ReturnValue")) return false;
    pe::Call(widget, get, g.bytes.data());
    std::memcpy(c.At("InText"), g.At("ReturnValue"), 0x18);
    pe::Call(lib, convert, c.bytes.data());
    struct FStr { const wchar_t* data; int32_t num, max; };
    const auto s = ue::Get<FStr>(c.At("ReturnValue"));
    out->clear();
    if (!s.data || s.num <= 0 || s.num > 64) return true;
    for (int i = 0; i < s.num && s.data[i]; ++i) if (s.data[i] < 128) out->push_back(static_cast<char>(s.data[i]));
    return true;
}
bool SetText(UObject* widget, const std::string& value, const char* method, const char* param) {
    auto* lib = Default(towerperf::ClassNamed("KismetTextLibrary"));
    auto* convert = Fn(lib, "Conv_StringToText"); auto* set = Fn(widget, method);
    if (!convert || !set) return false;
    Args a{convert}, b{set}; auto w = Wide(tower::language::Display(value, tower::language::Current()));
    if (!a.At("InString") || !a.At("ReturnValue") || !b.At(param)) return false;
    Str(a.At("InString"), w); pe::Call(lib, convert, a.bytes.data());
    std::memcpy(b.At(param), a.At("ReturnValue"), 0x10);
    pe::Call(widget, set, b.bytes.data());
    return true;
}
namespace {
UObject* g_dead = nullptr;     // W_PlayerDead 实例（只显示）
// 原生死亡界面的逻辑事件：动画关键帧（SequenceEvent*）、撤下 / 停用、淡出、复活流送等。只对塔自己创建的那个实例吞掉，
// 玩家真正的原生死亡（塔外）不受影响。Construct 照常（界面初始化，保证书法正常显示）
void MuteDeadWidget(UObject* cls) {
    static UObject* hooked = nullptr;
    if (!cls || cls == hooked) return;
    hooked = cls;
    int n = 0;
    for (const char* name : {"SequenceEvent__ENTRYPOINTW_PlayerDead", "SequenceEvent__ENTRYPOINTW_PlayerDead_0", "SequenceEvent__ENTRYPOINTW_PlayerDead_1",
                             "SequenceEvent__ENTRYPOINTW_PlayerDead_2", "SequenceEvent__ENTRYPOINTW_PlayerDead_3", "SequenceEvent", "SequenceEvent_0",
                             "WidgetAnimationEvt_Disappear_K2Node_WidgetAnimationEvent_0", "ReadyToDeactivate", "SubtitlesPopUp", "MinusOneEvent",
                             "自定义事件", "tick2", "Tick", "Destruct", "BP_OnDeactivated", "BP_OnActivated"}) {
        UObject* f = pe::FindFunction(cls, name);
        if (!f) continue;
        pe::Intercept(f, [](UObject* self, void*) { return self && self == g_dead; });
        ++n;
    }
    pbz::log::Info("影塔殒命表现：原生死亡界面的 {} 个逻辑事件对塔实例静默（不回钟、不走原生复活）", n);
}
UObject* g_deadMontage = nullptr;
bool g_deadMove = false;
// 按路径同步加载一个资源对象（动作等）：MakeSoftObjectPath → 软引用 → LoadAsset_Blocking，由引擎自己构造软引用
UObject* LoadObject(const char* path) {
    auto* sys = Default(towerperf::ClassNamed("KismetSystemLibrary"));
    auto* mk = Fn(sys, "MakeSoftObjectPath"); auto* conv = Fn(sys, "Conv_SoftObjPathToSoftObjRef"); auto* load = Fn(sys, "LoadAsset_Blocking");
    if (!mk || !conv || !load) return nullptr;
    Args a{mk}; if (!a.At("PathString") || !a.At("ReturnValue")) return nullptr;
    auto w = Wide(path); Str(a.At("PathString"), w); pe::Call(sys, mk, a.bytes.data());
    Args b{conv}; if (!b.At("SoftObjectPath") || !b.At("ReturnValue")) return nullptr;
    std::memcpy(b.At("SoftObjectPath"), a.At("ReturnValue"), 0x20);  // FSoftObjectPath：TopLevelAssetPath 16 + SubPathString 16
    pe::Call(sys, conv, b.bytes.data());
    Args c{load}; if (!c.At("Asset") || !c.At("ReturnValue")) return nullptr;
    std::memcpy(c.At("Asset"), b.At("ReturnValue"), 0x28);  // 软引用 0x28（同 LoadClass 注释）
    pe::Call(sys, load, c.bytes.data());
    return ue::Get<UObject*>(c.At("ReturnValue"));
}
void SetVis(UObject* w, uint8_t v) {
    if (auto* f = Fn(w, "SetVisibility")) { Args a{f}; if (a.At("InVisibility")) { *a.At("InVisibility") = v; pe::Call(w, f, a.bytes.data()); } }
}
}  // namespace

namespace {
// 2026-11-11 B 批：塔改写的生命 / 杀气直接写 GAS 字段，原生血条只在自己的入口事件（属性变化委托、
// 上限动画计时器、构造 / 初始化、进度条 Tick）里按原生映射重算长度和填充；塔原来每 50 毫秒覆盖一次，
// 两个写入方交替 → 换层后长度回退、末端闪烁。现在接管这些入口：原函数先照常执行，执行完当场按塔的
// 真实值重写，同一帧里塔永远是最后写入方；控件一构造就登记，不再等最长 2 秒的扫描。
struct BarMaxSync {
    UObject* widget; int index; double hp, shachi; unsigned long long next = 0; bool warned = false;
    tower::bar::LengthTransition hpLength, shachiLength;
    UObject* hpBar = nullptr; UObject* shaBar = nullptr;     // 上次写入时的两个子进度条
    double lastHl = -1, lastSl = -1, lastHf = -1, lastSf = -1;
};
std::vector<BarMaxSync> g_barMaxSync;
struct HudRootSync { UObject* widget; int index, barOffset; };
std::vector<HudRootSync> g_hudRoots;
unsigned long long g_barMaxScan = 0;
bool g_barScanWarned = false;
// 真实值来源：当前值每次现读（原生委托触发时属性已经是新值），上限用塔每帧确认过的值
// （原生重算上限的那一刻字段会短暂回到未加成的原生值，不能拿它画长度）。
struct BarSource {
    UObject* owner = nullptr; int index = -1;
    const float *hp = nullptr, *sha = nullptr;
    float hpMax = -1, shaMax = -1; unsigned long long at = 0;
} g_src;
std::vector<UObject*> g_barHookFns;
std::array<UObject*, 3> g_barHookCls{};
int g_barHookApplies = 0;
bool g_inBarApply = false;

bool SourceValues(float* h, float* hm, float* s, float* sm) {
    if (!g_src.at || GetTickCount64() - g_src.at > 1000 || !towerapi::InsideTower() || !g_src.owner || !g_src.hp || !g_src.sha ||
        !ue::Live(g_src.owner) || ue::Get<int>(g_src.owner + ue::Layout::ObjIndex, -1) != g_src.index) return false;
    return tower::bar::Source(ue::Get<float>(g_src.hp, -1.0f), g_src.hpMax, h, hm) &&
           tower::bar::Source(ue::Get<float>(g_src.sha, -1.0f), g_src.shaMax, s, sm);
}
// 类与偏移每次调用只查一次（用户 2026-11-11 B 批实机：整表扫描里每个对象都按名字查类 + 两次反射偏移，
// 15 万对象每 2 秒卡约 300 ms）。扫描前取一次，对象先比类指针再做别的
struct HudRefs { UObject* cls = nullptr; UObject* hpCls = nullptr; UObject* shaCls = nullptr; int h = -1, s = -1; bool ok() const { return cls && hpCls && shaCls && h >= 0 && s >= 0; } };
HudRefs HudClasses() {
    HudRefs r; r.cls = towerperf::ClassNamed("WB_PlayerHPBar_V1_C");
    if (!r.cls) return r;
    static UObject* cachedCls = nullptr; static int ch = -1, cs = -1;
    if (cachedCls != r.cls) { cachedCls = r.cls; ch = ue::PropOffset(r.cls, "AnimHPBar"); cs = ue::PropOffset(r.cls, "AnimBreakmeterBar"); }
    r.h = ch; r.s = cs;
    r.hpCls = towerperf::ClassNamed("PBZAnimProgressBar1_C"); r.shaCls = towerperf::ClassNamed("PBZAnimProgressBar2_C");
    return r;
}
bool TrackHud(UObject* w, const HudRefs& r) {
    if (!w || !r.ok() || ue::ClassOf(w) != r.cls && !ue::IsA(w, r.cls)) return false;
    if (!ue::Live(w) || ue::IsDefault(w)) return false;
    auto* healthBar = ue::Get<UObject*>(w + r.h); auto* shachiBar = ue::Get<UObject*>(w + r.s);
    if (!ue::Live(healthBar) || !ue::Live(shachiBar) || !ue::IsA(healthBar, r.hpCls) || !ue::IsA(shachiBar, r.shaCls)) return false;
    const int index = ue::Get<int>(w + ue::Layout::ObjIndex, -1);
    if (index < 0) return false;
    if (std::none_of(g_barMaxSync.begin(), g_barMaxSync.end(), [w, index](const auto& b) { return b.widget == w && b.index == index; }))
        g_barMaxSync.push_back({w, index, -1, -1, 0});
    return true;
}
// Write the tower's length and fill into one native HUD. Native methods and
// materials only; the native widget keeps its own frame, textures and effects.
bool ApplyBar(BarMaxSync& bar, float health, float healthMax, float shachi, float shachiMax, unsigned long long now) {
    auto* w = bar.widget; auto* cls = ue::ClassOf(w);
    const auto off = [cls](const char* n) { return ue::PropOffset(cls, std::strcmp(n,"AnimHPBar_1")==0 ? "AnimHPBar" : n); };
    const auto d = [w,&off](const char* n) { const int o = off(n); return o >= 0 ? ue::Get<double>(w + o, -1.0) : -1.0; };
    const auto f = [w,&off](const char* n) { const int o = off(n); return o >= 0 ? ue::Get<float>(w + o, -1.0f) : -1.0f; };
    const auto obj = [w,&off](const char* n) -> UObject* { const int o = off(n); return o >= 0 ? ue::Get<UObject*>(w + o) : nullptr; };
    double hl=0, sl=0, hf=0, sf=0;
    const double hi=d("HPBarInitRatio"), si=d("ShachiBarRatio");
    const double hw=d("HPBarWhiteRatio"), hb=d("HPBarBlackRatio");
    const double sw=d("ShachiBarWhiteRatio"), sb=d("ShaChiBlackBarRatio");
    if (!tower::bar::Calculate(health,healthMax,f("OriMaxHP"),d("HPMaxAttribute"),hi,d("MaxHpPercentage"),&hl,&hf) ||
        !tower::bar::Calculate(shachi,shachiMax,f("OriMaxShachi"),d("ShachiMaxAttribute"),si,d("MaxShaChiRatio"),&sl,&sf) ||
        hw < hi || hb < hi || sw < si || sb < si) {
        if (!bar.warned) { pbz::log::Info("影塔原生血条：映射输入未就绪 hp {:.1f}/{:.1f} sha {:.1f}/{:.1f} init {:.3f}/{:.3f} margins {:.3f}/{:.3f}/{:.3f}/{:.3f}",health,healthMax,shachi,shachiMax,hi,si,hw,hb,sw,sb); bar.warned=true; }
        return false;
    }
    auto* hpBar=obj("AnimHPBar_1"); auto* shaBar=obj("AnimBreakmeterBar");
    auto* bg=obj("BG_DMI"); auto* hpBg=obj("HPBG_DMI"); auto* shaBg=obj("SCBG_DMI");
    if (!ue::Live(hpBar) || !ue::Live(shaBar) || !ue::Live(bg) || !ue::Live(hpBg) || !ue::Live(shaBg)) {
        if (!bar.warned) { pbz::log::Info("影塔原生血条：控件或材质未就绪 hp={} sha={} frame={} hpbg={} shabg={}",ue::Live(hpBar),ue::Live(shaBar),ue::Live(bg),ue::Live(hpBg),ue::Live(shaBg)); bar.warned=true; }
        return false;
    }
    // Animate the actual native bar and frame together. Once the native
    // length cap is reached, keep the cap and play its own max-change effect;
    // current/max fill remains independent.
    auto maxEffect = [](UObject* widget, double length) {
        auto* fn = Fn(widget, "PlayNiagaraOnChangeMaxValue");
        if (!fn) return;
        auto* field = ue::Get<UObject*>(fn + ue::Layout::StructChildProps);
        for (int i = 0; field && i < 128; ++i, field = ue::Get<UObject*>(field + ue::Layout::FieldNext)) {
            auto* type = ue::Get<UObject*>(field + ue::Layout::FieldClass);
            if (ue::Name(ue::Get<uint32_t>(field + ue::Layout::FieldName)) != "MaxPercentage") continue;
            if (!type || ue::Name(ue::Get<uint32_t>(type + ue::Layout::FieldClassName)) != "DoubleProperty" ||
                ue::Get<int>(field + 0x34) != 8 || !(ue::Get<uint64_t>(field + 0x38) & 0x80)) return;
            Args a{fn}; auto* value = a.At("MaxPercentage"); if (!value) return;
            std::memcpy(value, &length, sizeof length); pe::Call(widget, fn, a.bytes.data()); return;
        }
    };
    if (bar.hpLength.Update(healthMax, hl, now)) maxEffect(hpBar, hl);
    if (bar.shachiLength.Update(shachiMax, sl, now)) maxEffect(shaBar, sl);
    hl = bar.hpLength.Sample(now); sl = bar.shachiLength.Sample(now);
    auto scalar = [](UObject* material, const char* key, double value) {
        static std::unordered_map<std::string,uint64_t> names;
        auto* fn=Fn(material,"SetScalarParameterValue");
        auto it=names.find(key); if (it==names.end()) it=names.emplace(key,Name(key)).first;
        const uint64_t name=it->second;
        if (!fn || !name || !std::isfinite(value)) return false;
        Args a{fn}; auto* n=a.At("ParameterName"); auto* v=a.At("Value"); if (!n || !v) return false;
        const float x=static_cast<float>(std::clamp(value,0.0,1.0)); std::memcpy(n,&name,8); std::memcpy(v,&x,4); pe::Call(material,fn,a.bytes.data()); return true;
    };
    auto show = [](UObject* widget, double value) {
        auto* fn=Fn(widget,"SetShowUpRatio"); if (!fn) return false;
        Args a{fn}; auto* v=a.At("Value"); if (!v) return false;
        std::memcpy(v,&value,8); pe::Call(widget,fn,a.bytes.data()); return true;
    };
    auto fill = [&](UObject* widget, double value) {
        const int mo=ue::PropOffset(ue::ClassOf(widget),"MID_Bar"); auto* mid=mo>=0?ue::Get<UObject*>(widget+mo):nullptr;
        if (!ue::Live(mid)) return false;
        // Keep the native widget's cached value aligned with the authoritative
        // ratio as well as its foreground material; its own Tick positions the
        // tip from CurPercent, so both must agree.
        const int at=ue::PropOffset(ue::ClassOf(widget),"CurPercent");
        if (at>=0) {
            const double old=ue::Get<double>(widget+at,-1);
            if(!std::isfinite(old) || std::abs(old-value)>.0001) ue::Write(widget+at,&value,sizeof value);
        }
        return scalar(mid,"BarRatio_FG",value);
    };
    const bool ok=show(hpBar,hl) && show(shaBar,sl) && scalar(hpBg,"ShowUpRatio",hl+hb-hi) &&
        scalar(shaBg,"ShowUpRatio",sl+sb-si) && scalar(bg,"1stBarRatio",hl+hw-hi) &&
        scalar(bg,"2ndBarRatio",sl+sw-si) && fill(hpBar,hf) && fill(shaBar,sf);
    if (ok) {
        if (bar.hp < 0) pbz::log::Info("影塔原生血条：生命 {:.0f}/{:.0f} 长度 {:.3f}，杀气 {:.0f}/{:.0f} 长度 {:.3f}",health,healthMax,hl,shachi,shachiMax,sl);
        bar.hp=hf; bar.shachi=sf; bar.hpBar=hpBar; bar.shaBar=shaBar; bar.lastHl=hl; bar.lastSl=sl; bar.lastHf=hf; bar.lastSf=sf;
    } else if (!bar.warned) { pbz::log::Info("影塔原生血条：控件方法或材质参数不可用"); bar.warned=true; }
    return ok;
}
// 原生入口刚执行完：把这个控件（或它所属的整条 HUD）按真实值重写。child = 进度条子控件的 Tick：
// 只在原生把填充 / 长度改得和塔不一致时才重写（每帧都会来，不做无谓的材质调用）。
void ReapplyAfterNative(UObject* w, bool tick) {
    if (g_inBarApply || !w) return;
    BarMaxSync* hit = nullptr; bool hpChild = false;
    for (auto& b : g_barMaxSync) {
        if (b.widget == w) { hit = &b; break; }
        if (b.hpBar == w || b.shaBar == w) { hit = &b; hpChild = b.hpBar == w; break; }
    }
    if (!hit && tick) return;  // 子进度条属于别的界面（敌人 / Boss 血条等）：不管
    if (hit && (!ue::Live(hit->widget) || ue::Get<int>(hit->widget + ue::Layout::ObjIndex, -1) != hit->index)) return;
    float h, hm, s, sm;
    if (!SourceValues(&h, &hm, &s, &sm)) return;
    if (!hit) {
        if (!TrackHud(w, HudClasses())) return;
        hit = &g_barMaxSync.back();
    }
    if (tick) {
        static std::unordered_map<UObject*, std::pair<int, int>> offsets;  // 类 → CurPercent / CachedShowUp
        auto* cls = ue::ClassOf(w);
        auto it = offsets.find(cls);
        if (it == offsets.end()) it = offsets.emplace(cls, std::pair{ue::PropOffset(cls, "CurPercent"), ue::PropOffset(cls, "CachedShowUp")}).first;
        const auto [cur, shown] = it->second;
        if (cur >= 0 && shown >= 0 && !tower::bar::NeedsReapply(ue::Get<double>(w + cur, -1.0), ue::Get<double>(w + shown, -1.0),
                                                                hpChild ? hit->lastHf : hit->lastSf, hpChild ? hit->lastHl : hit->lastSl)) return;
    }
    const auto now = GetTickCount64();
    g_inBarApply = true;
    if (ApplyBar(*hit, h, hm, s, sm, now)) { hit->next = now + 50; ++g_barHookApplies; }
    g_inBarApply = false;
}
void HookBarClass(int slot, UObject* cls, std::initializer_list<const char*> names, std::initializer_list<const char*> prefixes, bool child) {
    if (!cls || g_barHookCls[slot] == cls) return;
    g_barHookCls[slot] = cls;
    std::vector<UObject*> fns;
    for (const char* n : names) if (auto* f = pe::FindFunction(cls, n)) fns.push_back(f);
    for (const char* p : prefixes) for (auto* f : pe::FunctionsWithPrefix(cls, p)) fns.push_back(f);
    int n = 0;
    for (auto* f : fns) {
        if (std::find(g_barHookFns.begin(), g_barHookFns.end(), f) != g_barHookFns.end()) continue;
        const bool tick = child && ue::ObjName(f) == "Tick";
        pe::Intercept(f, [f, tick](UObject* self, void* params) {
            pe::Call(self, f, params);  // 原函数照常执行（pe::Call 不再经过拦截）
            try { ReapplyAfterNative(self, tick); } catch (...) { g_inBarApply = false; }
            return true;                // 已执行过原函数，不再执行第二次
        });
        g_barHookFns.push_back(f); ++n;
    }
    pbz::log::Info("影塔原生血条：{} 接管 {} 个原生入口（执行后按真实值重写）", ue::ObjName(cls), n);
}
void InstallBarHooks() {
    HookBarClass(0, towerperf::ClassNamed("WB_PlayerHPBar_V1_C"),
                 {"Construct", "InitMaxBarValue", "InitHPBar", "OnHeathChanged", "MaxHealthChanged", "MaxAngerChanged", "UpdateMaxHealth", "UpdateMaxAnger",
                  "MaxHealthChangeFinished", "MaxAngerChangeFinished", "StartUpdateMaxHealth", "StartUpdateMaxShaChi", "ShachiRecoveredFX"},
                 {"Changed_", "Added_"}, false);
    HookBarClass(1, towerperf::ClassNamed("PBZAnimProgressBar1_C"), {"Construct", "Tick", "UpdateAnimBar"}, {}, true);
    HookBarClass(2, towerperf::ClassNamed("PBZAnimProgressBar2_C"), {"Construct", "UpdateAnimBar", "UpdateShaqiRecover", "RecoverFromBottom"}, {"EventReceived_", "Added_"}, true);
}
void RemoveBarHooks() {
    for (auto* f : g_barHookFns) pe::RemoveIntercept(f);
    g_barHookFns.clear(); g_barHookCls = {};
}
}

int RefreshHpBar() {
    auto* cls = towerperf::ClassNamed("WB_PlayerHPBar_V1_C");
    if (!cls) return 0;
    int n = 0;
    ue::ForEach([&](int, UObject* o) {
        if (!ue::IsA(o, cls) || ue::IsDefault(o) || !ue::Live(o)) return true;
        for (const char* f : {"InitMaxBarValue", "InitHPBar"})
            if (auto* fn = Fn(o, f)) { Args a{fn}; pe::Call(o, fn, a.bytes.data()); }
        ReapplyAfterNative(o, false);  // 原生初始化按自己的映射画了一遍：当场改回塔的真实长度 / 填充
        ++n;
        return true;
    });
    return n;
}

int RefreshHpBarFast(float before, float after) {
    static std::vector<UObject*> bars;
    static unsigned long long nextScan = 0;
    std::erase_if(bars, [](UObject* o) { return !ue::Live(o); });
    if (bars.empty()) {
        const auto now = GetTickCount64();
        if (now < nextScan) return 0;
        nextScan = now + 3000;
        auto* cls = towerperf::ClassNamed("WB_PlayerHPBar_V1_C");
        if (!cls) return 0;
        ue::ForEach([&](int, UObject* o) { if (ue::IsA(o, cls) && !ue::IsDefault(o) && ue::Live(o)) bars.push_back(o); return true; });
    }
    for (auto* o : bars) {
        // Direct GAS field writes do not fire the native attribute delegate.
        // Call the original HUD's health-change callback with its reflected
        // double arguments so it updates the bar instead of reinitializing it.
        auto* fn = Fn(o, "OnHeathChanged");
        bool called = false;
        if (fn) {
            Args a{fn}; auto* n = a.At("NewValue"); auto* old = a.At("OldValue");
            if (n && old) {
                const double next = after, previous = before;
                std::memcpy(n, &next, sizeof(next)); std::memcpy(old, &previous, sizeof(previous));
                pe::Call(o, fn, a.bytes.data());
                called = true;
            }
        }
        if (!called) for (const char* f : {"InitMaxBarValue", "InitHPBar"})
            if (auto* fallback = Fn(o, f)) { Args a{fallback}; pe::Call(o, fallback, a.bytes.data()); }
        ReapplyAfterNative(o, false);
    }
    return static_cast<int>(bars.size());
}

void SetBarSource(void* owner, const float* health, float healthMax, const float* shachi, float shachiMax) {
    auto* o = static_cast<UObject*>(owner);
    if (!o || !ue::Live(o) || !health || !shachi || !std::isfinite(healthMax) || !std::isfinite(shachiMax) || healthMax <= 0 || shachiMax <= 0) { g_src = {}; return; }
    g_src = {o, ue::Get<int>(o + ue::Layout::ObjIndex, -1), health, shachi, healthMax, shachiMax, GetTickCount64()};
}
void ResetMaxBarSync() {
    RemoveBarHooks();  // 先撤接管，下面的原生初始化按原生映射还原
    g_src = {};
    for (const auto& b : g_barMaxSync) if (ue::Live(b.widget) && ue::Get<int>(b.widget + ue::Layout::ObjIndex, -1) == b.index)
        for (const char* name : {"InitMaxBarValue", "InitHPBar"}) if (auto* f = Fn(b.widget, name)) { Args a{f}; pe::Call(b.widget, f, a.bytes.data()); }
    if (g_barHookApplies) pbz::log::Info("影塔原生血条：本次共在原生入口后重写 {} 次", g_barHookApplies);
    g_barMaxSync.clear(); g_hudRoots.clear(); g_barMaxScan = 0; g_barScanWarned = false; g_barHookApplies = 0;
}
void SyncMaxBars(float health, float healthMax, float shachi, float shachiMax, bool force) {
    if (!std::isfinite(healthMax) || !std::isfinite(shachiMax) || healthMax <= 0 || shachiMax <= 0) return;
    if (g_src.owner) { g_src.hpMax = healthMax; g_src.shaMax = shachiMax; g_src.at = GetTickCount64(); }
    std::erase_if(g_barMaxSync, [](const auto& b) { return !ue::Live(b.widget) || ue::Get<int>(b.widget + ue::Layout::ObjIndex, -1) != b.index; });
    std::erase_if(g_hudRoots, [](const auto& r) { return !ue::Live(r.widget) || ue::Get<int>(r.widget + ue::Layout::ObjIndex, -1) != r.index; });
    const auto now = GetTickCount64();
    InstallBarHooks();
    // A live HUD can replace its child without immediately destroying the old
    // widget. Follow the root's current child every frame; the global scan is
    // only the fallback for discovering/recreating the root itself (normally
    // the Construct hook registers a new HUD immediately).
    const HudRefs refs = HudClasses();
    for(const auto& root:g_hudRoots) TrackHud(ue::Get<UObject*>(root.widget+root.barOffset), refs);
    if (now >= g_barMaxScan) {
        g_barMaxScan = now + (g_barMaxSync.empty() ? 500 : 2000);
        // 4.0.0（每 2 秒整表扫描一次 = 准备 / 战斗中固定 20–35 ms 一顿）：改读分摊扫描的实例索引（towerperf::Instances）。
        // PropOffset compares the base FName; the engine stores the displayed
        // "_1" suffix in its separate numeric component.
        auto* rootCls=towerperf::ClassNamed("W_FrontEndHUDLayout_C");
        const int rootOff=rootCls?ue::PropOffset(rootCls,"WB_PlayerHPBar"):-1;
        if (rootCls && rootOff >= 0)
            for (auto* w : towerperf::Instances("W_FrontEndHUDLayout_C")) {
                if (!ue::Live(w) || ue::ClassOf(w) != rootCls) continue;
                const int index=ue::Get<int>(w+ue::Layout::ObjIndex,-1);
                if(index>=0 && std::none_of(g_hudRoots.begin(),g_hudRoots.end(),[w,index](const auto& r){return r.widget==w && r.index==index;}))
                    g_hudRoots.push_back({w,index,rootOff});
                TrackHud(ue::Get<UObject*>(w+rootOff), refs);
            }
        if (refs.cls) for (auto* w : towerperf::Instances("WB_PlayerHPBar_V1_C")) if (ue::Live(w) && ue::ClassOf(w) == refs.cls) TrackHud(w, refs);
        if (g_barMaxSync.empty() && !g_barScanWarned) { pbz::log::Info("影塔原生血条：未找到有效控件，将继续查找"); g_barScanWarned=true; }
    }
    // Fallback cadence: tower-side changes (rewards, regeneration written
    // directly, the growth animation) that no native entry reports.
    for (auto& bar : g_barMaxSync) {
        if (force || now >= bar.next) {
            bar.next = now + 50;
            g_inBarApply = true;
            ApplyBar(bar, health, healthMax, shachi, shachiMax, now);
            g_inBarApply = false;
        }
    }
}

int NativeBarKind(void* rawActor) {
    auto* actor = static_cast<UObject*>(rawActor);
    if (!actor || !ue::Live(actor)) return 0;
    static std::unordered_map<UObject*, int> kinds;      // 控件类 → 1 Boss / 2 头顶 / 0 其他（每个类只判一次 IsA）
    static std::unordered_map<UObject*, int> actorKind;  // 本次扫描看到的 Actor → 血条种类
    static unsigned long long nextScan = 0;
    const auto now = GetTickCount64();
    // 4.0.0：每 2 秒整表扫描（斩首行动 / 横扫千军时战斗中每 2 秒一顿）改读分摊扫描的实例索引，索引换新一轮时重建
    static int seenPass = -1;
    const auto& bossBars = towerperf::Instances("WB_BossHPBar_C");
    const auto& enemyBars = towerperf::Instances("WB_EnemyHPBar_C");
    if (now >= nextScan || seenPass != towerperf::IndexPass()) {
        nextScan = now + 2000; seenPass = towerperf::IndexPass();
        actorKind.clear();
        auto* enemy = towerperf::ClassNamed("WB_EnemyHPBar_C"); auto* boss = towerperf::ClassNamed("WB_BossHPBar_C");
        const int enemyOff = enemy ? ue::PropOffset(enemy, "SelfActor") : -1, bossOff = boss ? ue::PropOffset(boss, "Character") : -1;
        auto take = [&](UObject* w, int kind, int off) {
            if (off < 0 || !ue::Live(w)) return;
            UObject* who = ue::Get<UObject*>(w + off);
            if (who && ue::Live(who)) { int& k = actorKind[who]; if (k != 1) k = kind; }  // 同时有两种时以 Boss 血条为准
        };
        for (auto* w : enemyBars) take(w, 2, enemyOff);
        for (auto* w : bossBars) take(w, 1, bossOff);
    }
    const auto it = actorKind.find(actor);
    return it == actorKind.end() ? 0 : it->second;
}

static int RefreshEnemyBar(void* rawActor, float health, float maxHealth, const char* fnName, const char* argName);
int RefreshEnemyHpBar(void* rawActor, float health, float maxHealth) { return RefreshEnemyBar(rawActor, health, maxHealth, "ForSeq_UpdateHealth", "CurHPPercent"); }
int RefreshEnemyShachiBar(void* rawActor, float shachi, float maxShachi) { return RefreshEnemyBar(rawActor, shachi, maxShachi, "ForSeq_UpdateShachi", "CurShachiPercent"); }
static int RefreshEnemyBar(void* rawActor, float health, float maxHealth, const char* fnName, const char* argName) {
    auto* actor = static_cast<UObject*>(rawActor);
    if (!actor || !ue::Live(actor) || !std::isfinite(health) || !std::isfinite(maxHealth) || maxHealth <= 0) return 0;
    struct Entry { UObject* widget; int index; int actorOffset; };
    static std::vector<Entry> entries;
    static unsigned long long nextScan = 0;
    std::erase_if(entries, [](const Entry& e) { return !ue::Live(e.widget) || ue::Get<int>(e.widget + ue::Layout::ObjIndex, -1) != e.index; });
    const auto now = GetTickCount64();
    static int seenPass = -1;
    const auto& enemyList = towerperf::Instances("WB_EnemyHPBar_C");
    const auto& bossList = towerperf::Instances("WB_BossHPBar_C");
    if (now >= nextScan || seenPass != towerperf::IndexPass()) {  // 4.0.0：改读分摊扫描的实例索引（原每 2 秒整表扫描）
        nextScan = now + 2000; seenPass = towerperf::IndexPass();
        auto* enemy = towerperf::ClassNamed("WB_EnemyHPBar_C"); auto* boss = towerperf::ClassNamed("WB_BossHPBar_C");
        const int enemyOff = enemy ? ue::PropOffset(enemy, "SelfActor") : -1, bossOff = boss ? ue::PropOffset(boss, "Character") : -1;
        auto take = [&](UObject* w, int off) {
            if (off < 0 || !ue::Live(w) || ue::IsDefault(w) || !ue::Live(ue::Get<UObject*>(w + off))) return;
            const int index = ue::Get<int>(w + ue::Layout::ObjIndex, -1);
            if (index >= 0 && std::none_of(entries.begin(), entries.end(), [w,index](const Entry& e) { return e.widget == w && e.index == index; })) entries.push_back({w,index,off});
        };
        for (auto* w : enemyList) take(w, enemyOff);
        for (auto* w : bossList) take(w, bossOff);
    }
    int updated = 0;
    for (const auto& e : entries) {
        if (ue::Get<UObject*>(e.widget + e.actorOffset) != actor) continue;
        auto* fn = Fn(e.widget, fnName); if (!fn) continue;
        Args args{fn}; auto* v = args.At(argName); if (!v) continue;
        const double fraction = std::clamp(static_cast<double>(health / maxHealth), 0.0, 1.0);
        std::memcpy(v, &fraction, sizeof fraction); pe::Call(e.widget, fn, args.bytes.data()); ++updated;
    }
    return updated;
}

bool DeathShow(bool on) {
    auto* pawn = Pawn();
    auto* pc = Controller();
    if (!on) {
        if (g_dead && ue::Live(g_dead)) if (auto* f = Fn(g_dead, "RemoveFromParent")) pe::Call(g_dead, f, nullptr);
        g_dead = nullptr;
        if (g_deadMontage && pawn && ue::Live(g_deadMontage))
            if (auto* f = Fn(pawn, "StopAnimMontage")) { Args a{f}; a.Obj("AnimMontage", g_deadMontage); pe::Call(pawn, f, a.bytes.data()); }
        if (g_deadMove && pc) if (auto* f = Fn(pc, "ResetIgnoreMoveInput")) pe::Call(pc, f, nullptr);
        g_deadMove = false;
        return true;
    }
    bool shown = false;
    // 倒地动作（主角原生死亡动作，向前倒）
    if (!g_deadMontage || !ue::Live(g_deadMontage)) g_deadMontage = LoadObject("/PBZCombatAsset/ActorData/Players/Hun/Montage/Hit/AM_Hun_Dead_F.AM_Hun_Dead_F");
    if (g_deadMontage && pawn)
        if (auto* f = Fn(pawn, "PlayAnimMontage")) {
            Args a{f}; a.Obj("AnimMontage", g_deadMontage);
            if (auto* r = a.At("InPlayRate")) { const float one = 1.0f; std::memcpy(r, &one, 4); }
            pe::Call(pawn, f, a.bytes.data()); shown = true;
        }
    if (pc) if (auto* f = Fn(pc, "SetIgnoreMoveInput")) { Args a{f}; if (a.At("bNewMoveInput")) { *a.At("bNewMoveInput") = 1; pe::Call(pc, f, a.bytes.data()); g_deadMove = true; } }
    // 殒命书法
    auto* cls = towerperf::ClassNamed("W_PlayerDead_C");
    if (!cls) cls = LoadClass("/Game/PBZContent/UI/InGame/Dead/W_PlayerDead.W_PlayerDead_C");
    MuteDeadWidget(cls);
    UObject* w = Create(cls);
    if (!w) return shown;
    g_dead = w;  // 先登记：AddToViewport 之后的原生事件按实例吞掉
    auto* wc = ue::ClassOf(w);
    if (const int o = ue::PropOffset(wc, "NotTick"); o >= 0) { const uint8_t yes = 1; ue::Write(w + o, &yes, 1); }  // 关掉它的复活 / 流送 Tick
    for (const char* name : {"DiedTimeBox", "ReturnButton", "DeadText", "DeadRichText", "NumText", "DeltaText"})
        if (const int o = ue::PropOffset(wc, name); o >= 0) if (auto* child = ue::Get<UObject*>(w + o); child && ue::Live(child)) SetVis(child, 1);  // Collapsed
    auto* add = Fn(w, "AddToViewport");
    if (!add) return shown;
    Args a{add}; if (a.At("ZOrder")) { const int z = 9500; std::memcpy(a.At("ZOrder"), &z, 4); }
    pe::Call(w, add, a.bytes.data());
    SetVis(w, 3);  // HitTestInvisible：不抢输入
    if (const int o = ue::PropOffset(wc, "Deadpopup"); o >= 0)
        if (auto* anim = ue::Get<UObject*>(w + o); anim && ue::Live(anim))
            if (auto* play = Fn(w, "PlayAnimation")) {
                Args p{play}; p.Obj("InAnimation", anim);
                if (auto* n = p.At("NumLoopsToPlay")) { const int one = 1; std::memcpy(n, &one, 4); }
                if (auto* s = p.At("PlaybackSpeed")) { const float one = 1.0f; std::memcpy(s, &one, 4); }
                pe::Call(w, play, p.bytes.data());
            }
    return true;
}

bool Countdown(int number) {
    if (g_countdown && !ue::Live(g_countdown)) g_countdown = nullptr;
    if (number <= 0) {
        if (auto* f = Fn(g_countdown, "RemoveFromParent")) pe::Call(g_countdown, f, nullptr);
        g_countdown = nullptr;
        return true;
    }
    if (!g_countdown) {
        auto* cls = towerperf::ClassNamed("WB_TipsText_C");
        if (!cls) cls = LoadClass("/Game/PBZContent/UI/InGame/SwitchUI/WB_TipsText.WB_TipsText_C");
        g_countdown = Create(cls);
        auto* add = Fn(g_countdown, "AddToViewport");
        if (!add) { g_countdown = nullptr; return false; }
        Args a{add}; if (!a.At("ZOrder")) return false;
        const int z = 10000; std::memcpy(a.At("ZOrder"), &z, 4); pe::Call(g_countdown, add, a.bytes.data());
        auto vectorCall = [&](const char* method, const char* param, double x, double y) {
            if (auto* f = Fn(g_countdown, method)) { Args v{f}; if (auto* p = v.At(param)) { const double xy[2] = {x, y}; std::memcpy(p, xy, 16); if (auto* dpi = v.At("bRemoveDPIScale")) *dpi = 1; pe::Call(g_countdown, f, v.bytes.data()); } }
        };
        vectorCall("SetDesiredSizeInViewport", "Size", 400, 140);
        vectorCall("SetAlignmentInViewport", "Alignment", 0.5, 0.5);
        auto* layout = Default(towerperf::ClassNamed("WidgetLayoutLibrary")); auto* viewport = Fn(layout, "GetViewportSize");
        if (viewport) {
            Args size{viewport}; auto* pc = Controller();
            if (size.Obj("WorldContextObject", pc) && size.At("ReturnValue")) { pe::Call(layout, viewport, size.bytes.data()); const double* xy = reinterpret_cast<const double*>(size.At("ReturnValue")); vectorCall("SetPositionInViewport", "position", xy[0] * 0.5, xy[1] * 0.4); }
        }
        // HUD never captures focus, pointer events or CommonUI input.
        if (auto* vis = Fn(g_countdown, "SetVisibility")) { Args v{vis}; if (v.At("InVisibility")) { *v.At("InVisibility") = 3; pe::Call(g_countdown, vis, v.bytes.data()); } }
    }
    return SetText(g_countdown, std::to_string(number), "SetText", "Text");
}
std::string BarDebug() {
    for (auto& b : g_barMaxSync) {
        if (!ue::Live(b.widget) || b.lastHl < 0) continue;
        auto* cls = ue::ClassOf(b.widget);
        auto d = [&](const char* n) { const int o = ue::PropOffset(cls, n); return o >= 0 ? ue::Get<double>(b.widget + o, -1.0) : -1.0; };
        return std::format("hp长{:.3f}/满{:.3f} 上限{:.0f}(原生长度上限对应{:.0f}) sha长{:.3f}/满{:.3f} 上限{:.0f}(原生{:.0f})",
                           b.lastHl, d("MaxHpPercentage"), b.hpLength.maximum, d("HPMaxAttribute"), b.lastSl, d("MaxShaChiRatio"), b.shachiLength.maximum, d("ShachiMaxAttribute"));
    }
    return "血条未接管";
}
UObject* LoadAsset(const char* path) { return path && *path ? LoadObject(path) : nullptr; }
} // namespace towerwidgets
