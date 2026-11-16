// 驿站大面板（游戏端）：运行时用 UMG 基础控件搭一个列表面板，挂在原生 WB_TipsText（读秒同款）里替换它的根控件。
//   标题（魂玉余额）/ 结果条 / 6 行商品（左：名称 + 效果 + 价格，右：购买）/ 底栏（上一页 · 页码 · 下一页 · 刷新货架 · 离开驿站）
// 证据等级：静态（SDK：GameplayStatics.SpawnObject、PanelWidget.AddChildTo*、ContentWidget.SetContent、Button.IsPressed、
// WidgetBlueprintLibrary.SetInputMode_GameAndUIEx；UserWidget 是抽象类，所以借原生蓝图控件当宿主）。
// 按钮点击靠每帧轮询 IsPressed 的"按下 → 松开"（模块不能绑原生多播委托）。任何一步搭不起来都返回 false，界面退回旧的确认框驿站。
#include <windows.h>

#include <array>
#include <cstring>
#include <format>
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../../src/core/host/log.h"
#include "../../../src/core/sdk/pbz_module.h"
#include "tower_api.h"
#include "tower_perf.h"
#include "tower_shop_panel.h"
#include "tower_shop_ui.h"
#include "tower_widgets.h"
#include "tower_language.h"
#include "tower_l10n_layout.h"
#include "tower_l10n_offer.h"

namespace towershop {
namespace {
namespace ue = pbz::ue;
namespace pe = pbz::pe;
using ue::UObject;
using tower::shoppanel::View;

struct Args {
    UObject* fn; std::array<uint8_t, 0x200> bytes{};
    uint8_t* At(const char* name) { const int o = fn ? ue::PropOffset(fn, name) : -1; return o >= 0 && o < 0x180 ? bytes.data() + o : nullptr; }
};
UObject* Fn(UObject* o, const char* f) { return o && ue::Live(o) ? pe::FindFunction(ue::ClassOf(o), f) : nullptr; }
UObject* Lib(const char* cls) { return towerwidgets::Default(towerperf::ClassNamed(cls)); }

// 调一个只有一个参数的函数（参数按原样拷进缓冲）
bool Call1(UObject* o, const char* fn, const char* param, const void* value, size_t size, UObject** ret = nullptr) {
    auto* f = Fn(o, fn);
    if (!f) return false;
    Args a{f};
    if (param && !a.At(param)) return false;
    if (param) std::memcpy(a.At(param), value, size);
    pe::Call(o, f, a.bytes.data());
    if (ret) *ret = a.At("ReturnValue") ? ue::Get<UObject*>(a.At("ReturnValue")) : nullptr;
    return true;
}

struct Panel {
    UObject *host = nullptr, *tree = nullptr, *root = nullptr, *title = nullptr, *banner = nullptr, *pageText = nullptr;
    struct RowW { UObject *box = nullptr, *info = nullptr, *buy = nullptr, *buyText = nullptr; };
    std::array<RowW, tower::shoppanel::kRows> rows{};
    UObject *prev = nullptr, *next = nullptr;
    std::array<UObject*, 4> foot{}, footText{};  // 底栏按钮（从左到右）
    std::vector<std::pair<UObject*, bool>> pressed;  // 按钮 → 上一帧是否按着
    std::array<uint8_t, 0x60> font{};
    bool hasFont = false;
};
Panel g;
bool g_open = false;
int g_page = 0;
std::function<void(const std::string&, const std::string&)> g_send;
std::function<void()> g_closed;
uint8_t g_cursorWas = 0;
std::string g_lastDrawn;
uint64_t g_openedAt = 0;                       // 打开后 0.8 秒内不受理点击（击杀那一下的按键不能穿透成领奖，第 4 层 158 ms 教训）
std::vector<std::function<void()>> g_deferred;

UObject* g_outer = nullptr;  // 非空：控件建在这棵树上（HUD），否则建在列表面板的树上
UObject* Spawn(const char* cls) {
    auto* c = towerperf::ClassNamed(cls);
    auto* lib = Lib("GameplayStatics");
    auto* f = Fn(lib, "SpawnObject");
    UObject* outer = g_outer ? g_outer : g.tree;
    if (!c || !f || !outer) return nullptr;
    Args a{f};
    if (!a.At("ObjectClass") || !a.At("Outer") || !a.At("ReturnValue")) return nullptr;
    std::memcpy(a.At("ObjectClass"), &c, 8);
    std::memcpy(a.At("Outer"), &outer, 8);
    pe::Call(lib, f, a.bytes.data());
    return ue::Get<UObject*>(a.At("ReturnValue"));
}
UObject* Add(UObject* panel, const char* how, UObject* child) {
    UObject* slot = nullptr;
    return panel && child && Call1(panel, how, "Content", &child, 8, &slot) ? slot : nullptr;
}
void Pad(UObject* slot, float l, float t, float r, float b) { const float m[4] = {l, t, r, b}; if (slot) Call1(slot, "SetPadding", "InPadding", m, 16); }
void Size(UObject* slot, bool fill) {
    struct { float value; uint8_t rule; uint8_t pad[3]; } s{fill ? 1.0f : 0.0f, static_cast<uint8_t>(fill ? 1 : 0), {}};
    if (slot) Call1(slot, "SetSize", "InSize", &s, 8);
}
void VAlign(UObject* slot, uint8_t v) { if (slot) Call1(slot, "SetVerticalAlignment", "InVerticalAlignment", &v, 1); }
void Visible(UObject* w, bool on) { const uint8_t v = on ? 0 : 1; if (w) Call1(w, "SetVisibility", "InVisibility", &v, 1); }  // Visible / Collapsed
void Enabled(UObject* w, bool on) { const uint8_t v = on; if (w) Call1(w, "SetIsEnabled", "bInIsEnabled", &v, 1); }
void Text(UObject* t, const std::string& s) { if (t) towerwidgets::SetText(t, s); }

// 字体：照抄游戏里一个已有 TextBlock 的字体（默认 Roboto 没有中文字形），只改字号
// 用户 2026-11-05：只用游戏自己的字体 ZH_FZKaiTi_Font（方正楷体）。"用得最多的字体"会随加载时机变（09:32 取到 Roboto，
// HUD 变成黑体）；游戏字体还没加载就先不定，HUD 等它出现再套上
void FindFont() {
    if (g.hasFont) return;
    auto* tb = towerperf::ClassNamed("TextBlock");
    const int off = tb ? ue::PropOffset(tb, "Font") : -1;
    if (off < 0) return;
    UObject* sample = nullptr;
    std::unordered_map<UObject*, bool> isText;  // 每个类只判一次 IsA
    ue::ForEach([&](int, UObject* o) {
        UObject* c = ue::ClassOf(o);
        if (!c) return true;
        auto [it, fresh] = isText.try_emplace(c, false);
        if (fresh) it->second = c == tb || ue::IsA(o, tb);
        if (!it->second) return true;
        if (ue::IsDefault(o) || !ue::Live(o)) return true;
        auto* font = ue::Get<UObject*>(o + off);
        if (!font || ue::ObjName(font) != "ZH_FZKaiTi_Font") return true;
        sample = o;
        return false;
    });
    if (!sample) return;
    ue::Read(sample + off, g.font.data(), g.font.size());
    g.hasFont = true;
    pbz::log::Info("影塔界面字体：游戏字体 ZH_FZKaiTi_Font");
}
void ApplyFont(UObject* t, float size) {
    if (!t || !g.hasFont) return;
    auto f = g.font;
    std::memcpy(f.data() + 0x48, &size, 4);  // SlateFontInfo.Size（SDK：+0x48）
    Call1(t, "SetFont", "InFontInfo", f.data(), f.size());
}
UObject* Label(float size, bool wrap) {
    auto* t = Spawn("TextBlock");
    if (!t) return nullptr;
    if (g.hasFont) {
        auto f = g.font;
        std::memcpy(f.data() + 0x48, &size, 4);  // SlateFontInfo.Size（SDK：+0x48）
        Call1(t, "SetFont", "InFontInfo", f.data(), f.size());
    }
    if (wrap) { const uint8_t on = 1; Call1(t, "SetAutoWrapText", "InAutoTextWrap", &on, 1); }
    return t;
}
UObject* ButtonWith(UObject** text, float size) {
    auto* b = Spawn("Button");
    auto* t = Label(size, false);
    if (!b || !t) return nullptr;
    UObject* slot = nullptr;
    Call1(b, "SetContent", "Content", &t, 8, &slot);
    Pad(slot, 18, 6, 18, 6);
    g.pressed.push_back({b, false});
    *text = t;
    return b;
}

bool Build() {
    g = {};
    FindFont();
    auto* cls = towerperf::ClassNamed("WB_TipsText_C");
    if (!cls) cls = towerwidgets::LoadClass("/Game/PBZContent/UI/InGame/SwitchUI/WB_TipsText.WB_TipsText_C");
    g.host = towerwidgets::CreateWidget(cls);
    const int treeOff = g.host ? ue::PropOffset(towerperf::ClassNamed("UserWidget"), "WidgetTree") : -1;
    g.tree = treeOff >= 0 ? ue::Get<UObject*>(g.host + treeOff) : nullptr;
    const int rootOff = g.tree ? ue::PropOffset(ue::ClassOf(g.tree), "RootWidget") : -1;
    if (!g.tree || rootOff < 0) return false;
    g.root = Spawn("CanvasPanel");
    auto* border = Spawn("Border");
    auto* list = Spawn("VerticalBox");
    if (!g.root || !border || !list) return false;
    // 全屏遮罩（1–32 层第 5 层：背后的原生药详情透出来）：盖住整屏、挡住鼠标悬停 / 点击落到底下的原生界面
    if (auto* scrim = Spawn("Border")) {
        if (auto* ss = Add(g.root, "AddChildToCanvas", scrim)) {
            const double a0[2] = {0, 0}, a1[2] = {1, 1};
            const float z[4] = {0, 0, 0, 0};
            Call1(ss, "SetMinimum", "InMinimumAnchors", a0, 16);
            Call1(ss, "SetMaximum", "InMaximumAnchors", a1, 16);
            Call1(ss, "SetOffsets", "InOffset", z, 16);
        }
        const float dim[4] = {0, 0, 0, 0.72f};
        Call1(scrim, "SetBrushColor", "InBrushColor", dim, 16);
        Visible(scrim, true);  // Visible：命中测试到这里为止
    }
    // 面板占屏幕中间 70% × 76%，深色不透明底
    auto* bslot = Add(g.root, "AddChildToCanvas", border);
    if (!bslot) return false;
    const double mn[2] = {0.15, 0.12}, mx[2] = {0.85, 0.88};
    Call1(bslot, "SetMinimum", "InMinimumAnchors", mn, 16);
    Call1(bslot, "SetMaximum", "InMaximumAnchors", mx, 16);
    const float zero[4] = {0, 0, 0, 0};
    Call1(bslot, "SetOffsets", "InOffset", zero, 16);
    const float bg[4] = {0.02f, 0.02f, 0.03f, 1.0f};
    Call1(border, "SetBrushColor", "InBrushColor", bg, 16);
    const float bp[4] = {32, 24, 32, 24};
    Call1(border, "SetPadding", "InPadding", bp, 16);
    UObject* cslot = nullptr;
    Call1(border, "SetContent", "Content", &list, 8, &cslot);
    g.title = Label(30, false); Pad(Add(list, "AddChildToVerticalBox", g.title), 0, 0, 0, 6);
    g.banner = Label(22, true); Pad(Add(list, "AddChildToVerticalBox", g.banner), 0, 0, 0, 12);
    for (auto& r : g.rows) {
        r.box = Spawn("HorizontalBox");
        r.info = Label(20, true);
        r.buy = ButtonWith(&r.buyText, 20);
        if (!r.box || !r.info || !r.buy) return false;
        auto* rs = Add(list, "AddChildToVerticalBox", r.box); Pad(rs, 0, 6, 0, 6);
        auto* is = Add(r.box, "AddChildToHorizontalBox", r.info); Size(is, true); VAlign(is, 2); Pad(is, 0, 0, 24, 0);
        auto* bs = Add(r.box, "AddChildToHorizontalBox", r.buy); Size(bs, false); VAlign(bs, 2);
    }
    auto* foot = Spawn("HorizontalBox");
    UObject *prevT = nullptr, *nextT = nullptr;
    g.prev = ButtonWith(&prevT, 20);
    g.pageText = Label(20, false);
    g.next = ButtonWith(&nextT, 20);
    for (int k = 0; k < 4; ++k) g.foot[k] = ButtonWith(&g.footText[k], 22);
    if (!foot || !g.prev || !g.next || !g.foot[0] || !g.foot[1] || !g.foot[2] || !g.foot[3]) return false;
    Pad(Add(list, "AddChildToVerticalBox", foot), 0, 18, 0, 0);
    for (auto* w : {g.prev, g.pageText, g.next}) { auto* s = Add(foot, "AddChildToHorizontalBox", w); VAlign(s, 2); Pad(s, 0, 0, 12, 0); }
    auto* gap = Spawn("Spacer"); if (gap) Size(Add(foot, "AddChildToHorizontalBox", gap), true);
    for (auto* w : g.foot) { auto* s = Add(foot, "AddChildToHorizontalBox", w); VAlign(s, 2); Pad(s, 12, 0, 0, 0); }
    Text(prevT, "上一页"); Text(nextT, "下一页");
    ue::Write(g.tree + rootOff, &g.root, 8);  // 宿主原来的根控件不再显示（仍由宿主持有）
    return true;
}

// 面板内容来源：驿站 / 背包共用同一个列表面板（6 行 + 翻页 + 底栏两个固定按钮）
struct Source {
    virtual ~Source() = default;
    virtual bool Alive() = 0;                              // false = 自动关面板（离店 / 离开准备阶段 / 世界切换）
    virtual std::string Title() = 0;
    virtual std::string Banner() = 0;
    virtual int Count() = 0;                               // 全部行数（翻页由面板算）
    virtual std::string RowText(int i) = 0;
    virtual std::string RowButton(int i) = 0;
    virtual bool RowEnabled(int i) = 0;
    virtual void OnRow(int i) = 0;
    virtual std::string Foot(int k) = 0;                   // 底栏按钮 0–3（从左到右；空 = 隐藏）
    virtual bool FootEnabled(int) { return true; }
    virtual bool OnFoot(int k) = 0;                        // 返回 true = 关面板
    virtual bool Busy() = 0;                               // 有交易在等结果：所有按钮不受理
    virtual std::string Key() = 0;                         // 内容变化键（变了才重画）
    virtual void Tick() {}
};
std::unique_ptr<Source> g_src;

void Draw(bool force) {
    auto& src = *g_src;
    const int total = src.Count();
    const int perPage = tower::language::RowsPerPage(tower::language::Current() == tower::language::Language::English, tower::shoppanel::kRows);
    const int pages = tower::language::Pages(total, perPage);
    g_page = std::clamp(g_page, 0, pages - 1);
    const bool busy = src.Busy();
    const std::string key = std::format("{}|{}|{}|{}", src.Key(), g_page, busy, total);
    if (!force && key == g_lastDrawn) return;
    g_lastDrawn = key;
    Text(g.title, src.Title());
    Text(g.banner, src.Banner());
    for (int r = 0; r < tower::shoppanel::kRows; ++r) {
        auto& w = g.rows[r];
        const int i = tower::language::RowIndex(g_page, r, perPage);
        const bool on = r < perPage && i < total;
        Visible(w.box, on);
        if (!on) continue;
        Text(w.info, src.RowText(i));
        const auto label = src.RowButton(i);
        Visible(w.buy, !label.empty());
        Text(w.buyText, label);
        Enabled(w.buy, !busy && src.RowEnabled(i));
    }
    Text(g.pageText, pages > 1 ? std::format("{}/{}", g_page + 1, pages) : std::string(" "));
    Visible(g.prev, pages > 1); Visible(g.next, pages > 1);
    Enabled(g.prev, !busy && g_page > 0); Enabled(g.next, !busy && g_page + 1 < pages);
    for (int k = 0; k < 4; ++k) {
        const auto label = src.Foot(k);
        Visible(g.foot[k], !label.empty()); Text(g.footText[k], label); Enabled(g.foot[k], !busy && src.FootEnabled(k));
    }
}

bool Clicked(UObject* b) {
    for (auto& [btn, was] : g.pressed) {
        if (btn != b) continue;
        bool now = false;
        if (auto* f = Fn(b, "IsPressed")) { Args a{f}; pe::Call(b, f, a.bytes.data()); now = a.At("ReturnValue") && *a.At("ReturnValue"); }
        const bool click = was && !now;
        was = now;
        return click;
    }
    return false;
}

void InputMode(bool ui) {
    auto* pc = towerwidgets::Controller();
    auto* lib = Lib("WidgetBlueprintLibrary");
    if (!pc || !lib) return;
    const int cur = ue::PropOffset(ue::ClassOf(pc), "bShowMouseCursor");  // 位域的第一位（SDK：与 bEnableClickEvents 等同字节）
    if (ui) {
        if (auto* f = Fn(lib, "SetInputMode_GameAndUIEx")) {
            Args a{f};
            if (a.At("PlayerController")) std::memcpy(a.At("PlayerController"), &pc, 8);
            if (a.At("InWidgetToFocus")) std::memcpy(a.At("InWidgetToFocus"), &g.root, 8);
            if (a.At("bFlushInput")) *a.At("bFlushInput") = 1;
            pe::Call(lib, f, a.bytes.data());
        }
        if (cur >= 0) { g_cursorWas = ue::Get<uint8_t>(pc + cur); const uint8_t v = g_cursorWas | 1; ue::Write(pc + cur, &v, 1); }
    } else {
        if (auto* f = Fn(lib, "SetInputMode_GameOnly")) {
            Args a{f};
            if (a.At("PlayerController")) std::memcpy(a.At("PlayerController"), &pc, 8);
            pe::Call(lib, f, a.bytes.data());
        }
        if (cur >= 0) { const uint8_t v = static_cast<uint8_t>((ue::Get<uint8_t>(pc + cur) & ~1) | (g_cursorWas & 1)); ue::Write(pc + cur, &v, 1); }
    }
}

void Cmd(const std::string& cmd, const std::string& extra) { if (g_send) g_send(cmd, extra); }

// ---- 驿站
struct ShopSource : Source {
    std::string banner;
    tower::shoppanel::Guard guard;
    struct Pending { std::string what; int price = 0, before = 0; bool refresh = false; } pending;
    bool hadPending = false, busy = false;
    const tower::rewards::Shop& S() { return towerapi::Growth().shop; }
    View V() { return tower::shoppanel::Build(S(), towerapi::Run().jade, 0); }
    std::vector<tower::shoppanel::Row> All() {  // 全部真实商品（翻页由面板做）
        View all;
        for (int p = 0;; ++p) {
            const View v = tower::shoppanel::Build(S(), towerapi::Run().jade, p);
            all.rows.insert(all.rows.end(), v.rows.begin(), v.rows.end());
            if (p + 1 >= v.pages) break;
        }
        return all.rows;
    }
    bool Alive() override { return S().Open(); }
    void Tick() override {
        const int jade = towerapi::Run().jade;
        busy = guard.Busy(S().version, jade, GetTickCount64());
        if (hadPending && !busy) { hadPending = false; banner = tower::shoppanel::Result(pending.what, pending.price, pending.before, jade, pending.refresh); }
    }
    std::string Title() override { return std::format("驿站　魂玉 {}", towerapi::Run().jade); }
    std::string Banner() override { return !banner.empty() ? banner : V().total ? std::string(" ") : std::string("暂无商品"); }
    int Count() override { return V().total; }
    std::string RowText(int i) override {
        const auto r = All()[i];
        const auto effect = r.slot >= 0 && r.slot < static_cast<int>(S().shelf.size())
                                ? tower::language::OfferEffect(S().shelf[r.slot].offer) : r.effect;
        return std::format("{}　{} 魂玉\n{}", r.name, r.price, effect);
    }
    std::string RowButton(int i) override { const auto r = All()[i]; return r.sold ? "已购买" : "购买"; }
    bool RowEnabled(int i) override { return !All()[i].sold; }  // 魂玉不足仍可点：给出明确的"不足"结果
    void OnRow(int i) override {
        const auto r = All()[i];
        const int jade = towerapi::Run().jade;
        pending = {r.name, r.price, jade, false};
        guard.Arm(S().version, jade, GetTickCount64());
        hadPending = busy = true;
        Cmd("shop", std::format(R"(,"value":"buy","slot":{},"ver":{})", r.slot, S().version));
    }
    std::string Foot(int k) override { return k == 3 ? std::string("离开驿站") : std::string(); }
    bool OnFoot(int k) override { if (k != 3) return false; Cmd("shop", R"(,"value":"leave")"); return true; }
    bool Busy() override { return busy; }
    std::string Key() override { return std::format("{}|{}|{}", S().version, towerapi::Run().jade, banner); }
};

// ---- 背包（用户 2026-11-01：Ins → 背包，道具行右侧"使用（预装）"；没有战斗中即时道具）
struct BagSource : Source {
    struct Line { int item = -1, shentong = -1; };
    std::string banner;
    std::vector<Line> Lines() {
        std::vector<Line> out;
        const auto& r = towerapi::Run();
        const auto& prep = towerapi::Growth().prep;
        for (int i = 0; i < tower::kItemCount; ++i) {
            const auto t = tower::rewards::ItemTiming(tower::kItems[i].item);
            if (t == tower::rewards::Timing::Retired) continue;
            if (r.items[i] > 0 || prep.Armed(i)) out.push_back({i, -1});
        }
        for (int k = 0; k < static_cast<int>(r.shentong.size()); ++k) out.push_back({-1, k});
        return out;
    }
    bool Alive() override { return towerapi::Run().phase == tower::Phase::Ready && towerapi::Growth().on; }
    std::string Title() override {
        const auto& prep = towerapi::Growth().prep;
        std::string armed;
        for (int i : prep.armed) armed += (armed.empty() ? "" : "、") + std::string(tower::kItems[i].name);
        return std::format("MOD 背包　{}", armed.empty() ? std::string("未准备") : "待生效：" + armed);
    }
    std::string Banner() override {
        return banner.empty() ? std::string("每层准备一件，下一层开战时生效，本层结束清除") : banner;
    }
    int Count() override { return static_cast<int>(Lines().size()); }
    std::string RowText(int i) override {
        const auto l = Lines()[i];
        const auto& r = towerapi::Run();
        if (l.shentong >= 0) {
            const auto& st = tower::kShentong[r.shentong[l.shentong]];
            return std::format("神通·{}\n{}；作用于下一层", st.name, st.text);
        }
        const auto& d = tower::kItems[l.item];
        const bool armed = towerapi::Growth().prep.Armed(l.item);
        return std::format("{} ×{}{}\n{}", d.name, r.items[l.item], armed ? "　待生效" : "", d.text);
    }
    std::string RowButton(int i) override {
        const auto l = Lines()[i];
        if (l.shentong >= 0) return "使用";
        const auto t = tower::rewards::ItemTiming(tower::kItems[l.item].item);
        if (t == tower::rewards::Timing::Passive) return "自动生效";
        if (t == tower::rewards::Timing::Settlement) return "奖励时用";
        if (t == tower::rewards::Timing::Use) return "使用";
        if (towerapi::Growth().prep.Armed(l.item)) return "撤销";
        return towerapi::Growth().prep.Why(l.item, towerapi::Run().items).empty() ? std::string("准备") : std::string("更换");
    }
    bool RowEnabled(int i) override {
        const auto l = Lines()[i];
        if (l.shentong >= 0) return true;
        if (tower::rewards::ItemTiming(tower::kItems[l.item].item) == tower::rewards::Timing::Use) return towerapi::Run().items[l.item] > 0;
        if (tower::rewards::ItemTiming(tower::kItems[l.item].item) != tower::rewards::Timing::Prep) return false;
        return towerapi::Run().items[l.item] > 0 || towerapi::Growth().prep.Armed(l.item);  // 已准备了别的：点"更换"先撤销再准备
    }
    void OnRow(int i) override {
        const auto l = Lines()[i];
        if (l.shentong >= 0) { Cmd("st", std::format(R"(,"slot":{})", l.shentong)); banner = "已使用神通：" + std::string(tower::kShentong[towerapi::Run().shentong[l.shentong]].name) + "（下一层生效）"; return; }
        const auto& d = tower::kItems[l.item];
        if (tower::rewards::ItemTiming(d.item) == tower::rewards::Timing::Use) { Cmd("prep", std::format(R"(,"value":"use","item":"{}")", d.id)); banner = "已使用：" + std::string(d.name); return; }
        if (towerapi::Growth().prep.Armed(l.item)) { Cmd("prep", std::format(R"(,"value":"disarm","item":"{}")", d.id)); banner = "已撤销：" + std::string(d.name); return; }
        if (!towerapi::Growth().prep.armed.empty()) Cmd("prep", R"(,"value":"disarm")");  // 更换：先撤销已准备的那件
        Cmd("prep", std::format(R"(,"value":"arm","item":"{}")", d.id));
        banner = "已准备：" + std::string(d.name);
    }
    std::string Foot(int k) override { return k == 3 ? std::string("返回") : std::string(); }
    bool OnFoot(int k) override { return k == 3; }
    bool Busy() override { return false; }
    std::string Key() override {
        const auto& prep = towerapi::Growth().prep;
        std::string k = banner;
        for (int i : prep.armed) k += std::format("|a{}", i);
        for (int i = 0; i < tower::kItemCount; ++i) k += std::format("|{}", towerapi::Run().items[i]);
        return k + std::format("|s{}", towerapi::Run().shentong.size());
    }
};

// ---- 通用菜单（tower_ui 填内容）
struct MenuSource : Source {
    MenuSpec spec;
    std::vector<MenuRow> cache;
    uint64_t cachedAt = 0;
    explicit MenuSource(MenuSpec s) : spec(std::move(s)) {}
    const std::vector<MenuRow>& Rows() {
        const uint64_t now = GetTickCount64();
        if (now - cachedAt > 200 || cache.empty()) { cache = spec.rows ? spec.rows() : std::vector<MenuRow>{}; cachedAt = now; }
        return cache;
    }
    bool Alive() override { return !spec.alive || spec.alive(); }
    std::string Title() override { return spec.title ? spec.title() : std::string(); }
    std::string Banner() override { const auto b = spec.banner ? spec.banner() : std::string(); return b.empty() ? std::string(" ") : b; }
    int Count() override { return static_cast<int>(Rows().size()); }
    std::string RowText(int i) override { return i < Count() ? Rows()[i].text : std::string(); }
    std::string RowButton(int i) override { return i < Count() ? Rows()[i].button : std::string(); }
    bool RowEnabled(int i) override { return i < Count() && Rows()[i].enabled && Rows()[i].act != nullptr; }
    void OnRow(int i) override { if (i < Count() && Rows()[i].act) { auto act = Rows()[i].act; cachedAt = 0; act(); } }
    std::string Foot(int k) override { return k >= 0 && k < 4 ? spec.foot[k] : std::string(); }
    bool OnFoot(int k) override { return k >= 0 && k < 4 && spec.onFoot[k] ? spec.onFoot[k]() : false; }
    bool Busy() override { return false; }
    std::string Key() override {
        std::string k = Title() + "|" + Banner();
        for (auto& r : Rows()) k += "|" + r.text + "#" + r.button + (r.enabled ? "1" : "0");
        return k;
    }
};

bool OpenWith(std::unique_ptr<Source> src, std::function<void(const std::string&, const std::string&)> send, std::function<void()> closed, const char* what) {
    if (g_open) Close();
    if (!src->Alive()) return false;
    towerperf::Section _("列表面板搭建");
    if (!Build()) { pbz::log::Info("影塔{}面板：搭不起来（控件类 / 函数缺失），退回确认框", what); g = {}; return false; }
    auto* add = Fn(g.host, "AddToViewport");
    if (!add) { g = {}; return false; }
    Args a{add}; if (a.At("ZOrder")) { const int z = 9000; std::memcpy(a.At("ZOrder"), &z, 4); }
    pe::Call(g.host, add, a.bytes.data());
    g_src = std::move(src);
    g_send = std::move(send);
    g_closed = std::move(closed);
    g_open = true; g_page = 0; g_lastDrawn.clear(); g_openedAt = GetTickCount64();
    InputMode(true);
    towerwidgets::OverlayBusy(true);
    Draw(true);
    pbz::log::Info("影塔{}面板：已打开（{} 行，字体 {}）", what, g_src->Count(), g.hasFont ? "沿用游戏字体" : "默认字体");
    return true;
}

}  // namespace

bool Open(std::function<void(const std::string&, const std::string&)> send, std::function<void()> closed) {
    if (g_open && dynamic_cast<ShopSource*>(g_src.get())) { Draw(true); return true; }
    return OpenWith(std::make_unique<ShopSource>(), std::move(send), std::move(closed), "驿站");
}
bool OpenBag(std::function<void(const std::string&, const std::string&)> send, std::function<void()> closed) {
    if (g_open && dynamic_cast<BagSource*>(g_src.get())) { Draw(true); return true; }
    return OpenWith(std::make_unique<BagSource>(), std::move(send), std::move(closed), "背包");
}

bool OpenMenu(MenuSpec spec, std::function<void()> closed) {
    const std::string what = spec.what;
    if (g_open) if (auto* m = dynamic_cast<MenuSource*>(g_src.get()); m && m->spec.what == what) { m->spec = std::move(spec); m->cachedAt = 0; Draw(true); return true; }
    return OpenWith(std::make_unique<MenuSource>(std::move(spec)), [](const std::string&, const std::string&) {}, std::move(closed), what.c_str());
}
bool MenuOpen(const std::string& what) {
    auto* m = g_open ? dynamic_cast<MenuSource*>(g_src.get()) : nullptr;
    return m && m->spec.what == what;
}

void Close() {
    if (!g_open) return;
    g_open = false;
    if (auto* f = Fn(g.host, "RemoveFromParent")) pe::Call(g.host, f, nullptr);
    InputMode(false);
    towerwidgets::OverlayBusy(false);
    g = {};
    g_src.reset();
    pbz::log::Info("影塔列表面板：已关闭");
    if (auto done = std::move(g_closed)) done();
}

bool IsOpen() { return g_open; }

void Defer(std::function<void()> act) { if (act) g_deferred.push_back(std::move(act)); }

void RunDeferred() {
    auto todo = std::move(g_deferred);
    g_deferred.clear();
    for (auto& f : todo) f();
}
void TickPanel();
void Tick() {
    TickPanel();
    if (!g_deferred.empty()) RunDeferred();
}
void TickPanel() {
    if (!g_open) return;
    towerperf::Section _("列表面板");
    if (!ue::Live(g.host) || !g_src->Alive()) { Close(); return; }  // 世界切换 / 已离店 / 离开准备阶段
    g_src->Tick();
    if (GetTickCount64() - g_openedAt < 800) {  // 刚打开：只跟踪按钮状态，不受理
        for (auto& [btn, was] : g.pressed) Clicked(btn);
        Draw(false);
        return;
    }
    const bool busy = g_src->Busy();
    const int total = g_src->Count();
    const int perPage = tower::language::RowsPerPage(tower::language::Current() == tower::language::Language::English, tower::shoppanel::kRows);
    const int pages = tower::language::Pages(total, perPage);
    for (int r = 0; r < tower::shoppanel::kRows; ++r) {
        const int i = tower::language::RowIndex(g_page, r, perPage);
        if (Clicked(g.rows[r].buy) && !busy && r < perPage && i < total && g_src->RowEnabled(i)) g_src->OnRow(i);
    }
    if (Clicked(g.prev) && !busy && g_page > 0) --g_page;
    if (Clicked(g.next) && !busy && g_page + 1 < pages) ++g_page;
    for (int k = 0; k < 4; ++k)
        if (Clicked(g.foot[k]) && !busy && !g_src->Foot(k).empty() && g_src->FootEnabled(k) && g_src->OnFoot(k)) { Close(); return; }
    Draw(false);
}

}  // namespace towershop

// ---------------------------------------------------------------- 左上 HUD（方案第 5.1 节）：层数 / 层型 / 计时 / 限时与附加挑战
// 不拦输入、不抢焦点（根控件 SelfHitTestInvisible），宿主同读秒控件；内容变化才重写文字
namespace towerhud {
namespace {
using namespace towershop;
struct Hud { UObject *host = nullptr, *floor = nullptr, *clock = nullptr, *goal = nullptr; std::string last; uint64_t next = 0, fontTry = 0, waitSince = 0; bool broken = false, fontSet = false; } h;
bool BuildHud() {
    FindFont();
    auto* cls = towerperf::ClassNamed("WB_TipsText_C");
    if (!cls) cls = towerwidgets::LoadClass("/Game/PBZContent/UI/InGame/SwitchUI/WB_TipsText.WB_TipsText_C");
    h.host = towerwidgets::CreateWidget(cls);
    const int treeOff = h.host ? ue::PropOffset(towerperf::ClassNamed("UserWidget"), "WidgetTree") : -1;
    UObject* tree = treeOff >= 0 ? ue::Get<UObject*>(h.host + treeOff) : nullptr;
    const int rootOff = tree ? ue::PropOffset(ue::ClassOf(tree), "RootWidget") : -1;
    if (!tree || rootOff < 0) return false;
    g_outer = tree;
    UObject* root = Spawn("CanvasPanel");
    UObject* box = Spawn("VerticalBox");
    h.floor = Label(26, false); h.clock = Label(30, false); h.goal = Label(18, true);
    g_outer = nullptr;
    if (!root || !box || !h.floor || !h.clock || !h.goal) return false;
    auto* slot = Add(root, "AddChildToCanvas", box);
    if (!slot) return false;
    const double at[2] = {0.015, 0.16};
    Call1(slot, "SetMinimum", "InMinimumAnchors", at, 16);
    Call1(slot, "SetMaximum", "InMaximumAnchors", at, 16);
    const uint8_t autoSize = 1; Call1(slot, "SetAutoSize", "InbAutoSize", &autoSize, 1);
    Add(box, "AddChildToVerticalBox", h.floor);
    Add(box, "AddChildToVerticalBox", h.clock);
    Pad(Add(box, "AddChildToVerticalBox", h.goal), 0, 4, 0, 0);
    const uint8_t passThrough = 4; Call1(root, "SetVisibility", "InVisibility", &passThrough, 1);  // SelfHitTestInvisible
    ue::Write(tree + rootOff, &root, 8);
    auto* add = Fn(h.host, "AddToViewport");
    if (!add) return false;
    Args a{add}; if (a.At("ZOrder")) { const int z = 8000; std::memcpy(a.At("ZOrder"), &z, 4); }
    pe::Call(h.host, add, a.bytes.data());
    pbz::log::Info("影塔 HUD：已显示（左上层数 / 计时）");
    return true;
}
void Hide() {
    if (h.host && ue::Live(h.host)) if (auto* f = Fn(h.host, "RemoveFromParent")) pe::Call(h.host, f, nullptr);
    const bool broken = h.broken;
    h = {};
    h.broken = broken;
}
}  // namespace

void Tick() {
    const uint64_t now = GetTickCount64();
    if (now < h.next) return;
    h.next = now + 100;
    const std::string floor = towerapi::HudFloor();
    if (floor.empty() || !towerwidgets::Pawn()) { if (h.host) Hide(); return; }
    if (h.host && !ue::Live(h.host)) h = {};  // 换了世界：重建
    // 游戏字体：没找到就每 2 秒找一次；HUD 最多等 15 秒再先用默认字体显示，找到后换上
    if (!g.hasFont && now >= h.fontTry) { h.fontTry = now + 2000; FindFont(); }
    if (!h.host) {
        if (h.broken) return;
        if (!g.hasFont) { if (!h.waitSince) h.waitSince = now; if (now - h.waitSince < 15000) return; }
        if (!BuildHud()) { pbz::log::Info("影塔 HUD：搭不起来（控件类 / 函数缺失），本次不显示"); Hide(); h.broken = true; return; }
    }
    if (g.hasFont && !h.fontSet) { ApplyFont(h.floor, 26); ApplyFont(h.clock, 30); ApplyFont(h.goal, 18); h.fontSet = true; h.last.clear(); }
    const std::string clock = towerapi::HudClock(), goal = towerapi::HudGoal();
    const std::string key = floor + "|" + clock + "|" + goal +
                            (tower::language::Current() == tower::language::Language::English ? "|en" : "|zh");
    if (key == h.last) return;
    h.last = key;
    Text(h.floor, floor);
    Text(h.clock, clock.empty() ? std::string(" ") : clock);
    Text(h.goal, goal.empty() ? std::string(" ") : goal);
}
void Reset() { Hide(); h.broken = false; }

}  // namespace towerhud
