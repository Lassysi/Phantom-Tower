// 影塔游戏内界面 UI-A：Insert 弹出游戏原生对话框（CommonGame 的确认框，游戏自己的字体 / 面板 / 按钮音效），
// 按当前阶段给最多三个按钮；通过本层后自动弹三选一，通关 / 失败自动弹结算。
// 做法：AsyncAction_ShowConfirmation.ShowConfirmationYesNo 生成对话框描述（引擎分配的按钮数组有余量），
// 改写按钮文字、必要时补第三个按钮，再 Activate 弹出；按钮被点时游戏经 ProcessEvent 调 CommonButtonBase.HandleButtonClicked
// （2026-10-30 实机调用记录：对话框按钮是 W_MenuButton_V2_SelectedVersion_Decorator2_C，蓝图覆盖了 BP_OnClicked，
// 所以观察基类的 BP_OnClicked 收不到；HandleButtonClicked 没被覆盖，函数对象就是基类那个），
// 按按钮在对话框按钮列表里的序号对应到我们的选项。Esc 关闭不算选择。
// UI-B：钟菜单 W_BellMenu_C 激活时，在 MainBox 里加一个同类按钮"影塔·百战"（放在"离开"之前），点它关掉钟菜单、弹影塔主菜单。
#include <windows.h>
#pragma comment(lib, "user32.lib")  // Insert 热键与前台窗口判断

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <sstream>
#include <functional>
#include <unordered_map>
#include <string>
#include <vector>

#include "../../../src/core/host/log.h"
#include "../../../src/core/sdk/pbz_module.h"
#include "../../../src/core/services/gamethread.h"
#include "boss_pool.generated.h"
#include "tower_api.h"
#include "tower_attack_speed.h"
#include "tower_diagmode.h"
#include "tower_hotkey.h"
#include "tower_native.h"
#include "tower_ui_logic.h"
#include "tower_widgets.h"
#include "tower_shop_ui.h"
#include "tower_floorplay.h"
#include "tower_entry.h"
#include "tower_perf.h"
#include "tower_reward_text.h"
#include "tower_menu_text.h"
#include "tower_input_gate.h"
#include "tower_sony_pad.h"
#include "tower_shop_panel.h"
#include "tower_language.h"
#include "tower_l10n_text.h"
#include "tower_l10n_offer.h"
#include "tower_leech.h"
#include "tower_weapon_config.generated.h"
#include "tower_item_icons.generated.h"

namespace {

using pbz::ue::UObject;
namespace ue = pbz::ue;
namespace pe = pbz::pe;
namespace attr = pbz::attr;

constexpr size_t kMaxButtons = 3;  // 原生确认框只有三种结果；第四个战斗按钮由 DynamicEntryBox 单独创建。
constexpr int kTextSize = 0x10;    // FText（本版本 0x10：ConfirmationDialogAction 0x18 = Result + 填充 8 + FText）
constexpr int kActionSize = 0x18;  // ConfirmationDialogAction

// UFunction 参数缓冲：按参数名取偏移
struct Args {
    UObject* fn;
    std::vector<uint8_t> buf;
    explicit Args(UObject* f) : fn(f), buf(0x300, 0) {}
    uint8_t* At(std::string_view name) {
        const int o = fn ? ue::PropOffset(fn, name) : -1;
        static uint8_t sink[64];
        return o >= 0 && o < 0x2C0 ? buf.data() + o : sink;
    }
};

std::wstring Wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

UObject* Cdo(UObject* cls) {
    UObject* found = nullptr;
    const std::string want = "Default__" + ue::ObjName(cls);
    ue::ForEach([&](int, UObject* o) {
        if (ue::ClassOf(o) == cls && ue::ObjName(o) == want) { found = o; return false; }
        return true;
    });
    return found;
}

struct Option { std::string label; std::function<void()> act; };

class TowerUi : public pbz::IFeature {
public:
    const char* Id() const override { return "tower.ui"; }
    bool IsToggle() const override { return true; }
    bool DefaultOn() const override { return true; }

    bool Resolve(std::string* why) override {
        textLib_ = towerperf::ClassNamed("KismetTextLibrary");
        asyncCls_ = towerperf::ClassNamed("AsyncAction_ShowConfirmation");
        descCls_ = towerperf::ClassNamed("CommonGameDialogDescriptor");
        btnCls_ = towerperf::ClassNamed("CommonButtonBase");
        screenCls_ = towerperf::ClassNamed("PBZConfirmationScreen");
        activatable_ = towerperf::ClassNamed("CommonActivatableWidget");
        entryCls_ = towerperf::ClassNamed("DynamicEntryBoxBase");
        if (!textLib_ || !asyncCls_ || !descCls_ || !btnCls_ || !screenCls_ || !activatable_ || !entryCls_) { *why = "原生对话框类还没加载"; return false; }
        toText_ = pe::FindFunction(textLib_, "Conv_StringToText");
        yesNo_ = pe::FindFunction(asyncCls_, "ShowConfirmationYesNo");
        activate_ = pe::FindFunction(asyncCls_, "Activate");
        clicked_ = pe::FindFunction(btnCls_, "HandleButtonClicked");
        entries_ = pe::FindFunction(entryCls_, "GetAllEntries");
        isActivated_ = pe::FindFunction(activatable_, "IsActivated");
        actDesc_ = ue::PropOffset(asyncCls_, "Descriptor");
        descActions_ = ue::PropOffset(descCls_, "ButtonActions");
        descDialog_ = ue::PropOffset(descCls_, "CachedDialog");
        screenEntries_ = ue::PropOffset(screenCls_, "EntryBox_Buttons");
        if (!toText_ || !yesNo_ || !activate_ || !clicked_ || !entries_ || !isActivated_ || actDesc_ < 0 || descActions_ < 0 || descDialog_ < 0 || screenEntries_ < 0) {
            *why = "原生对话框接口缺失"; return false;
        }
        return true;
    }
    void Enable() override {
        enabled = true;
        // 默认开启的功能没有持有者，修改器"全部还原"会把它当成用户开的关掉（2026-10-30 实测）：由影塔自己持有
        pbz::HoldFeature(this, "tower");
        if (!observeBase_ && !towerdiag::SandboxOnly()) HookClicks();
    }
    void Disable() override {
        enabled = false;
        Quiet();
    }
    // 摘掉全部观察（停用、或进入诊断模式"只开沙盒"）：界面对游戏完全不活动
    void Quiet() {
        towerwidgets::Reset();
        if (observeBase_) { pe::RemoveIntercept(clicked_); observeBase_ = 0; }
        if (observeOverride_) { pe::Unobserve(observeOverride_); observeOverride_ = 0; }
        if (observeBell_ > 0) pe::Unobserve(observeBell_);
        if (observeBell2_ > 0) pe::Unobserve(observeBell2_);
        observeBell_ = observeBell2_ = 0; nextBellTry_ = 0;
        open_ = false;
    }

    void Tick(float) override {
        towerperf::Section _("影塔界面");
        towershop::Tick();
        towerhud::Tick();
        if (!enabled) return;
        towerwidgets::Tick();
        if (towerdiag::SandboxOnly()) {
            if (observeBase_ || observeBell_ > 0 || observeBell2_ > 0) { Quiet(); pbz::log::Info("影塔界面：诊断模式，已摘掉钟菜单入口与按钮观察"); }
            return;
        }
        if (!observeBase_) HookClicks();
        CheckClosed();
        if (open_) { gate_.Sample(GetTickCount64(), ConfirmInputs()); if (UObject* d = Dialog()) towerwidgets::Own(d); }  // 塔自己的对话框不算原生模态
        else gate_.Close();
        TickRows();
        TickFourthButton();
        TickShopPanelReturn();
        towerwidgets::OverlayBusy(open_);
        const auto& run = towerapi::Run();
        // 阶段变化时自动弹下一步：开局 / 继续 / 选完奖励 → "开下一层"；打完一层 → 三选一；通关 / 失败 → 结算
        // （2026-10-30 实测：开局后没有任何反馈，看起来像没反应）
        if (run.phase != lastPhase_) {
            const auto prev = lastPhase_;
            lastPhase_ = run.phase;
            const bool next = run.phase == tower::Phase::Ready && (prev == tower::Phase::Idle || prev == tower::Phase::Reward || prev == tower::Phase::Lost ||
                                                                   (prev == tower::Phase::Fighting && !towerapi::FloorNote().empty()));  // 本层取消 / 坏层跳过：弹出说明和"下一层"
            const bool done = prev != tower::Phase::Idle &&
                              (run.phase == tower::Phase::Reward || run.phase == tower::Phase::Won || run.phase == tower::Phase::Lost);
            // 等上一个对话框的关闭动画走完；玩家正开着菜单（例如战斗中按了 Insert）就等它关掉再弹
            if (next || done) pendingShow_ = GetTickCount64() + 800;
            pbz::log::Info("影塔界面阶段：{}→{} pending_show={} native_panel={} native_modal={} wall_ms={}", tower::PhaseName(prev), tower::PhaseName(run.phase), pendingShow_, towerwidgets::PanelActive(),
                           towerwidgets::NativeModalName(), GetTickCount64());
        }
        {   // 2.0.0：驿站打开（小怪打完 / 读档后点下一层）时自动弹出
            const bool shopNow = towerapi::Growth().on && towerapi::Growth().shop.Open();
            if (shopNow && !shopSeen_) {
                shopDismissed_ = false;
                if (run.phase == tower::Phase::Ready && !pendingShow_) pendingShow_ = GetTickCount64() + 800;
            }
            shopSeen_ = shopNow;
        }
        if (pendingAct_ && GetTickCount64() >= pendingActAt_ && !open_) { auto act = std::move(pendingAct_); pendingAct_ = nullptr; act(); }
        if (pendingShow_ && GetTickCount64() >= pendingShow_ && !open_ && !pendingAct_ && !towerwidgets::PanelActive()) { pendingShow_ = 0; ShowMain(); }
        {   // 开不了层 / 操作被拒的原因：弹原生确认框（原来只发管道，发布版玩家看不到，像"点了没反应"）
            int seq = 0;
            auto err = towerapi::LastError(&seq);
            if (seq != errSeen_) { errSeen_ = seq; pendingErr_ = std::move(err); pendingErrAt_ = GetTickCount64(); }
            // 原生面板状态残留时也要能看到（"请先关闭原生整备面板"本身就是要报的情况）：等 3 秒仍被挡就照弹
            const bool blocked = pendingAct_ || pendingShow_ || (towerwidgets::PanelActive() && GetTickCount64() - pendingErrAt_ < 3000);
            if (!pendingErr_.empty() && !open_ && !blocked) {
                const auto msg = std::move(pendingErr_); pendingErr_.clear();
                Show("影塔·百战 · 提示", msg, {{"知道了", [this] { if (towerapi::Run().phase == tower::Phase::Ready) ShowMain(); }}});
            }
        }
        HookBellMenu();
        // 每秒：推进出塔（通关 / 失败后阶段不是空闲，也要能走完）；空闲时的换回兵器等由 Housekeep 自己判断阶段
        if (GetTickCount64() >= nextHousekeep_) {
            nextHousekeep_ = GetTickCount64() + 1000;
            towerapi::Housekeep();
            // 饰品槽按层开放已去掉（用户 2026-11-05：能力修行里有原生开槽节点）
        }
        // Insert：只在游戏窗口在前台时响应。1.3.5：另加 PBZTowerHotkey.exe 设的自定义键（hotkey.txt，改了 2 秒内生效）
        const int customVk = hotkey_.Vk(GetTickCount64(), [](const std::string& m) { pbz::log::Info("{}", m); });
        const bool down = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0 || (customVk && (GetAsyncKeyState(customVk) & 0x8000) != 0);
        // 用户 2026-11-05：Insert 再按一次关掉影塔界面（对话框 / 旧面板）
        if (down && !insertDown_ && Foreground()) {
            if (open_) { pbz::log::Info("影塔界面：Insert 关闭对话框"); CloseDialog(); pendingShow_ = 0; }
            else if (towershop::IsOpen()) towershop::Close();
            else if (!towerwidgets::PanelActive()) ShowMain();
        }
        insertDown_ = down;
        PadTick();
    }

private:
    // 按钮点击走拦截（不是观察）：武装前的点击要吞掉，对话框留着（第 4 层 158 ms 被选走）
    void HookClicks() {
        pe::Intercept(clicked_, [this](UObject* obj, void*) { return GateClick(obj); });
        observeBase_ = 1;
    }
    bool GateClick(UObject* button) {
        ++uiClicks_;
        if (open_ && (DialogIndex(button) >= 0 || RowIndex(button) >= 0)) {
            const auto now = GetTickCount64();
            if (!gate_.Accept(now, ConfirmInputs())) {
                if (gateLogs_++ < 20) pbz::log::Info("影塔界面：对话框打开 {} ms 内的点击被新输入门挡下（打开前按住或连击；松开 {} ms 后的新输入才算）", now - openedAt_,
                                                    tower::inputgate::Gate::kQuietMs);
                return true;
            }
        }
        if (button && button == fourthButton_ && open_ && opts_.size() == 4) {
            auto act = opts_[3].act;
            pbz::log::Info("影塔界面：选择第 4 项「{}」", opts_[3].label);
            CloseDialog();
            pendingAct_ = std::move(act); pendingActAt_ = GetTickCount64() + 700;
            return true; // 第四项没有原生 Result，阻止确认框按 Result 再次处理
        }
        // Injected rows belong to Tower, not the native dialog's result list.
        // Letting the original handler run after our action can close the shop.
        if (open_ && RowIndex(button) >= 0) { OnRowClicked(button); return true; }
        OnClicked(button);
        return false;
    }
    // 手柄（XInput 第一个手柄）：按键位、扳机、左摇杆；没有手柄返回 false
    struct Pad { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; bool chord = false; };
    static_assert(offsetof(Pad, chord) == 16);  // XINPUT_STATE occupies the first 16 bytes
    // 用户 2026-11-06 玩家反馈"手柄不兼容"：原来只读 0 号手柄，Steam 输入 / 多个设备时手柄可能在 1~3 号。
    // 四个槽都读、按键合并；没接手柄的槽 XInputGetState 很慢，所以断开的槽每 2 秒才重新探一次
    static bool ReadPad(Pad* pad) {
        using GetState = DWORD(WINAPI*)(DWORD, Pad*);
        static GetState get = [] {
            for (const wchar_t* dll : {L"xinput1_4.dll", L"xinput9_1_0.dll", L"xinput1_3.dll"})
                if (HMODULE h = LoadLibraryW(dll)) if (auto f = reinterpret_cast<GetState>(GetProcAddress(h, "XInputGetState"))) return f;
            return static_cast<GetState>(nullptr);
        }();
        static bool live[4] = {};
        static unsigned long long probeAt = 0, cachedAt = ~0ull;
        static DWORD probeSlot = 0;
        static Pad cached{};
        static bool cachedAny = false;
        *pad = {};
        const auto now = GetTickCount64();
        // 同一毫秒里（同一帧的新输入门采样 + 手柄处理）只读一次
        if (now == cachedAt) { *pad = cached; return cachedAny; }
        // 没接手柄的槽 XInputGetState 很慢：每 500 ms 只探一个空槽（四个槽轮流），已连上的每次都读
        int probe = -1;
        if (now >= probeAt) {
            probeAt = now + 500;
            for (int k = 0; k < 4; ++k) { const DWORD s = (probeSlot + k) % 4; if (!live[s]) { probe = static_cast<int>(s); probeSlot = (s + 1) % 4; break; } }
        }
        bool any = false;
        for (DWORD i = 0; get && i < 4; ++i) {
            if (!live[i] && static_cast<int>(i) != probe) continue;
            Pad p{};
            live[i] = get(i, &p) == ERROR_SUCCESS;
            if (!live[i]) continue;
            any = true;
            pad->buttons |= p.buttons;
            pad->chord |= (p.buttons & 0x00c0) == 0x00c0;
            pad->lt = std::max(pad->lt, p.lt); pad->rt = std::max(pad->rt, p.rt);
            if (std::abs(p.lx) + std::abs(p.ly) > std::abs(pad->lx) + std::abs(pad->ly)) { pad->lx = p.lx; pad->ly = p.ly; }
        }
        // A DS4 can be visible to the game without an XInput slot. Read it
        // non-exclusively while the game is foreground, including when a
        // modal menu routes keys away from PlayerController.
        if (!any && Foreground()) {
            static tower::sony_pad::Reader sony;
            tower::sony_pad::Sample s;
            if (sony.Read(&s)) {
                pad->buttons = s.buttons; pad->lx = s.x; pad->ly = s.y;
                pad->chord = s.chord; any = true;
            }
        }
        cachedAt = now; cached = *pad; cachedAny = any;
        return any;
    }
    // 确认类输入：鼠标左右键、回车 / 空格 / E / F / J / K / L，手柄 A / B / X / Y / 肩键 / 扳机。
    // 手柄只算"刚按下 1 秒内"：插着手柄时有的手柄某个键位 / 扳机一直报按下（漂移、第三方手柄），
    // 原来会让新输入门永远不安静、鼠标点击全被吞（2026-11-06 玩家反馈"插上手柄用鼠标也不好点"）
    static uint32_t ConfirmInputs() {
        uint32_t m = 0; int bit = 0;
        for (int vk : std::initializer_list<int>{VK_LBUTTON, VK_RBUTTON, VK_RETURN, VK_SPACE, 'E', 'F', 'J', 'K', 'L'}) { if (GetAsyncKeyState(vk) & 0x8000) m |= 1u << bit; ++bit; }
        static WORD prevButtons = 0; static bool prevTrig = false;
        static unsigned long long buttonsAt = 0, trigAt = 0;
        Pad pad{};
        ReadPad(&pad);
        const auto now = GetTickCount64();
        const WORD btn = pad.buttons & 0xF300;  // A B X Y LB RB
        const bool trig = pad.lt > 30 || pad.rt > 30;
        if (btn & ~prevButtons) buttonsAt = now;
        if (trig && !prevTrig) trigAt = now;
        prevButtons = btn; prevTrig = trig;
        if (btn && now - buttonsAt < 1000) m |= 1u << 16;
        if (trig && now - trigAt < 1000) m |= 1u << 17;
        return m;
    }
    // ---- 手柄（用户 2026-11-06：大部分玩家用手柄）----
    // L3 + R3 同时按下 = Insert（打开 / 关闭影塔）；对话框开着、在用手柄时保证有一个按钮拿到焦点（之后上下左右、A 确认走原生导航）；
    // B：原生没关掉对话框时由塔处理——有"返回 / 知道了"就执行它，否则关闭
    void PadTick() {
        Pad pad{};
        const bool have = ReadPad(&pad);
        const WORD b = have ? pad.buttons : 0;
        const auto now = GetTickCount64();
        // "在用手柄"只看新动作：键位刚按下、摇杆从回中推到底。一直报按下的键 / 漂移的摇杆不算
        // （原来按住状态就算，插着手柄时界面一直以为在用手柄，焦点被反复拉走，鼠标点着乱跳）
        const bool stick = std::abs(pad.lx) > 20000 || std::abs(pad.ly) > 20000;
        // 摇杆方向当成虚拟十字键（推到底的那一下算一次）
        WORD dir = b & 0x000F;
        if (pad.ly > 20000) dir |= 0x0001; else if (pad.ly < -20000) dir |= 0x0002;
        if (pad.lx < -20000) dir |= 0x0004; else if (pad.lx > 20000) dir |= 0x0008;
        const WORD pressed = static_cast<WORD>((b & ~padPrev_) | (dir & ~padDir_));
        if (pressed) lastPadAt_ = now;
        padPrev_ = b; padStick_ = stick; padDir_ = dir;
        {   // 鼠标动过 / 点过：以鼠标为准，不替玩家挪焦点。光标隐藏时（手柄模式游戏会藏光标、重设位置）的移动不算
            CURSORINFO ci{sizeof(ci)};
            POINT c{};
            GetCursorPos(&c);
            const bool shown = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING);
            const bool moved = cursorValid_ && std::abs(c.x - cursor_.x) + std::abs(c.y - cursor_.y) > 3;
            if ((shown && moved) || (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000)) lastMouseAt_ = now;
            cursor_ = c; cursorValid_ = true;
        }
        if (open_ && shopConfirmFocusPending_ && lastPadAt_ > lastMouseAt_) {
            const auto buttons = DialogButtons();
            if (!buttons.empty()) {
                FocusOn(buttons.front());  // Cancel is deliberately first.
                padFocused_ = true;
                shopConfirmFocusPending_ = false;
            }
        }
        PadAssist(pressed, b, now);
        const bool chord = have && pad.chord;  // 同一手柄左右摇杆按下
        if (chord && !padChord_ && Foreground()) {
            if (open_) { pbz::log::Info("影塔界面：手柄 L3+R3 关闭对话框"); CloseDialog(); pendingShow_ = 0; }
            else if (towershop::IsOpen()) towershop::Close();
            else if (!towerwidgets::PanelActive()) ShowMain();
        }
        padChord_ = chord;
        // 每个对话框最多补一次焦点（原来每 200 ms 检查一次，而 CommonUI 按钮的焦点在内部按钮上、外层 HasAnyUserFocus 为假，
        // 于是焦点每 200 ms 被拉回第一项：玩家方向键移到"整备"，按 A 却选中了"下一层"——2026-11-06 玩家反馈）
        if (open_ && !padFocused_ && lastPadAt_ > lastMouseAt_ && now - lastPadAt_ < 10000 && now >= nextFocusFix_ && now - openedAt_ > 350) { nextFocusFix_ = now + 200; EnsurePadFocus(); }
        const bool back = Foreground() && (b & 0x2000) != 0;
        if (back && !padBack_ && open_) padBackAt_ = now;
        padBack_ = back;
        if (padBackAt_ && now - padBackAt_ > 250) {
            padBackAt_ = 0;
            if (Foreground() && open_ && Dialog()) PadBack();
        }
    }
    // ---- 手柄兜底（1.0.1 玩家反馈：手柄在影塔对话框里按确认进不去 / 方向键不动）----
    // 键位（XInput；PS 手柄经 Steam 输入映射同位）：确认 = A（PS ✕）或 X（PS □）；返回 = B（PS ○）；
    // 选择 = 十字键 / 左摇杆；打开 / 关闭影塔 = 同时按下左右摇杆（L3 + R3）。
    // 做法：按下后等 150 ms——游戏原生导航已经处理（有点击到达 / 焦点移动）就什么都不做，没处理才由塔来点 / 移焦点；不会重复触发
    std::vector<UObject*> PadTargets() {
        std::vector<UObject*> targets;
        for (auto* r : rowBtns_) if (r && ue::Live(r) && BoolCall(r, "IsVisible")) targets.push_back(r);
        for (auto* d : DialogButtons()) targets.push_back(d);
        return targets;
    }
    UObject* FocusedTarget(const std::vector<UObject*>& targets) {
        for (auto* t : targets) if (Focused(t)) return t;
        return nullptr;
    }
    void FocusOn(UObject* t) {
        if (!t || !ue::Live(t)) return;
        if (auto* f = pe::FindFunction(ue::ClassOf(t), "SetFocus")) { Args a(f); pe::Call(t, f, a.buf.data()); }
    }
    void PadAssist(WORD pressed, WORD held, unsigned long long now) {
        if (!open_ || !Foreground() || lastMouseAt_ > lastPadAt_ || now - openedAt_ < 400) { padConfirmAt_ = padConfirmReleasedAt_ = padNavAt_ = 0; padConfirmTarget_ = nullptr; return; }
        if (pressed & (0x1000 | 0x4000)) {
            padConfirmAt_ = now; padConfirmReleasedAt_ = 0; padClicksAtPress_ = uiClicks_;
            padConfirmTarget_ = FocusedTarget(PadTargets());
        }
        if (pressed & 0x000F) {
            padNavAt_ = now;
            padNavDir_ = (pressed & (0x0001 | 0x0004)) ? -1 : 1;
            // Native navigation can run before this game-thread tick. Compare
            // with the preceding frame, not the already-moved current focus.
            padNavFrom_ = padLastFocus_ ? padLastFocus_ : FocusedTarget(PadTargets());
        }
        if (padConfirmAt_ && !(held & (0x1000 | 0x4000)) && !padConfirmReleasedAt_) padConfirmReleasedAt_ = now;
        if (padConfirmAt_ && padConfirmReleasedAt_ && now - padConfirmReleasedAt_ >= 150) {
            padConfirmAt_ = padConfirmReleasedAt_ = 0;
            if (uiClicks_ != padClicksAtPress_ || !open_) return;  // 原生已经处理了这次确认
            const auto targets = PadTargets();
            UObject* t = padConfirmTarget_; padConfirmTarget_ = nullptr;
            if (t && std::find(targets.begin(), targets.end(), t) != targets.end() && ue::Live(t) && Focused(t)) {
                if (!gate_.Accept(now, ConfirmInputs())) return;
                // pe::Call deliberately invokes the original ProcessEvent and
                // bypasses our interceptor. Dispatch our own action exactly
                // once; a native click observed during release cancels this.
                if (padAssistLogs_++ < 20) pbz::log::Info("影塔界面：手柄确认由塔直接执行（原生未处理）");
                if (RowIndex(t) >= 0) { ++uiClicks_; OnRowClicked(t); }
                else {
                    const int i = DialogIndex(t);
                    if (i < 0 || i >= static_cast<int>(opts_.size())) return;
                    auto act = opts_[i].act; ++uiClicks_;
                    CloseDialog(); pendingAct_ = std::move(act); pendingActAt_ = now + 700;
                }
            } else if (!targets.empty()) {
                FocusOn(targets[0]);  // 没有焦点：第一下先选中第一项，看得见再按确认
                if (padAssistLogs_++ < 20) pbz::log::Info("影塔界面：手柄确认时没有选中项，先选中第一项");
            }
        }
        if (padNavAt_ && now - padNavAt_ >= 150) {
            padNavAt_ = 0;
            const auto targets = PadTargets();
            if (targets.empty()) return;
            UObject* cur = FocusedTarget(targets);
            if (cur && cur != padNavFrom_) return;  // 原生导航已经移动了焦点
            int i = -1;
            for (size_t k = 0; k < targets.size(); ++k) if (targets[k] == cur) i = static_cast<int>(k);
            const int n = static_cast<int>(targets.size());
            const int next = i < 0 ? 0 : std::clamp(i + padNavDir_, 0, n - 1);
            if (next != i) FocusOn(targets[next]);
        }
        if (open_ && now >= padNextFocusSample_) {
            padNextFocusSample_ = now + 30;
            if (UObject* t = FocusedTarget(PadTargets())) padLastFocus_ = t;
        }
    }
    std::vector<UObject*> DialogButtons() {
        std::vector<UObject*> out;
        UObject* d = Dialog();
        UObject* box = d ? ue::Get<UObject*>(d + screenEntries_) : nullptr;
        if (!box || !entries_) return out;
        Args a(entries_);
        pe::Call(box, entries_, a.buf.data());
        struct Arr { UObject** data; int32_t num, max; };
        const Arr list = ue::Get<Arr>(a.At("ReturnValue"));
        for (int i = 0; list.data && i < list.num && i < 8; ++i) if (auto* w = ue::Get<UObject*>(list.data + i); w && ue::Live(w)) out.push_back(w);
        return out;
    }
    bool BoolCall(UObject* w, const char* fn) {
        auto* f = w && ue::Live(w) ? pe::FindFunction(ue::ClassOf(w), fn) : nullptr;
        if (!f) return false;
        Args a(f); pe::Call(w, f, a.buf.data());
        return a.At("ReturnValue") && *a.At("ReturnValue") != 0;
    }
    bool Focused(UObject* w) { return BoolCall(w, "HasAnyUserFocus") || BoolCall(w, "HasKeyboardFocus") || BoolCall(w, "HasFocusedDescendants"); }
    void EnsurePadFocus() {
        std::vector<UObject*> targets;
        for (auto* r : rowBtns_) if (r && ue::Live(r) && BoolCall(r, "IsVisible")) targets.push_back(r);
        for (auto* d : DialogButtons()) targets.push_back(d);
        if (targets.empty()) return;  // 按钮还没生成：下次再试
        if (rows_ && !rowsInjected_) return;  // 等原生按钮列插好，免得先落到底部按钮
        padFocused_ = true;
        if (UObject* d = Dialog(); d && Focused(d)) return;  // 焦点已在对话框里（原生按钮或插入的行）：不动
        for (auto* t : targets) if (Focused(t)) return;
        if (auto* f = pe::FindFunction(ue::ClassOf(targets[0]), "SetFocus")) {
            Args a(f); pe::Call(targets[0], f, a.buf.data());
            if (padFocusLogs_++ < 10) pbz::log::Info("影塔界面：手柄焦点落到「{}」的第一项", rowsHeader_.empty() ? std::string("对话框") : rowsHeader_);
        }
    }
    void PadBack() {
        for (size_t i = 0; i < opts_.size(); ++i) {
            const auto& l = opts_[i].label;
            if (l == "返回" || l == "知道了" || l == "继续战斗" || l == "取消") {
                auto act = opts_[i].act;
                CloseDialog();
                pendingAct_ = act; pendingActAt_ = GetTickCount64() + 700;
                pbz::log::Info("影塔界面：手柄 B 执行「{}」", l);
                return;
            }
        }
        pbz::log::Info("影塔界面：手柄 B 关闭对话框");
        CloseDialog(); pendingShow_ = 0;
    }
    static bool Foreground() {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        return pid == GetCurrentProcessId();
    }
    static std::string Cmd(const std::string& cmd, const std::string& extra = "") {
        return std::format(R"({{"op":"tower","cmd":"{}","client":"ingame","takeover":true{}}})", cmd, extra);
    }
    static void Send(const std::string& cmd, const std::string& extra = "") {
        const auto line = Cmd(cmd, extra);
        pbz::gamethread::Post([line] { towerapi::Command(line); });  // 下一帧执行，不在按钮回调里改状态
    }

    // ---- 菜单内容 ----
    static std::string Join(const std::vector<std::string>& v) {
        std::string o;
        for (auto& x : v) o += (o.empty() ? "" : "、") + x;
        return o.empty() ? std::string("无") : o;
    }
    static std::string WeaponName(const tower::Run& r, int w) {
        return std::string(tower::kWeaponDrops[w].name) + (w == r.wearMain || w == r.wearShaqi ? "（装备中）" : "");
    }
    // 用户 2026-11-06：攻击力 / 破防能力是作用于全部武器的百分比加成（本局属性奖励 × 锐锋符），不显示角色固定数值
    static std::string Pct(double v) {
        auto s = std::format("{:.1f}", v);
        if (s.ends_with(".0")) s.resize(s.size() - 2);
        if (s == "-0") s = "0";
        return (v < 0 && s != "0" ? "" : "+") + s + "%";  // 用户 2026-11-11：负数显示「-30%」，不是「+-30%」
    }
    static std::string AtkBreakLines() {
        const auto& g = towerapi::Growth();
        const auto science = g.on && towerapi::Run().phase == tower::Phase::Fighting ? g.hexes.ScientistBoost(towerapi::ShownNow(), towerapi::Run().seed) : tower::hex::State::ScienceBoost{};
        std::string out = std::format("攻击力加成 {}\n破防能力加成 {}", Pct((tower::hex::Scale(g.attrs.Mul(tower::rewards::AttrKind::Attack), g.hexes.Attack() + science.attack) - 1) * 100),
                           Pct((tower::hex::Scale(g.attrs.Mul(tower::rewards::AttrKind::Break), g.hexes.Break() + science.armorBreak) - 1) * 100));
        if (g.on) out += std::format("\n生命上限加成 {}\n杀气上限加成 {}", Pct((tower::hex::LifeScale(g.attrs.Mul(tower::rewards::AttrKind::Life), g.hexes.Health() + science.health) - 1) * 100),
                                      Pct((tower::hex::Scale(g.attrs.Mul(tower::rewards::AttrKind::Shachi), science.shachiCap) - 1) * 100));
        // 用户 2026-11-08：伤害减免、生命吸取也是常规属性，没有时也显示 0%
        if (g.on) out += std::format("\n伤害减免 {:g}%{}\n生命吸取 {:g}%", (1.0 - tower::hex::DamageFactor(g.attrs.pm[static_cast<int>(tower::rewards::AttrKind::Defense)], g.hexes.Defense(), g.hexes.MinTaken())) * 100,
                                     tower::hex::ReductionCapped(g.attrs.pm[static_cast<int>(tower::rewards::AttrKind::Defense)], g.hexes.Defense(), g.hexes.MinTaken()) ? "（已达上限）" : "",
                                     (g.attrs.pm[static_cast<int>(tower::rewards::AttrKind::LifeSteal)] + g.hexes.LifeSteal() + tower::leech::kPlaytestLeech) / 10.0);
        if (g.on) out += std::format("\n移动速度 +{:g}%", (g.hexes.Speed() + science.move) * 100);
        if (g.on) out += std::format("\n攻击速度 +{:g}%", (science.attackSpeed + g.hexes.AttackSpeed()) * 100.0);
        if (g.on && g.hexes.Has(tower::hex::Ocean)) out += std::format("\n持续回血 {:g}% / 秒", g.hexes.OceanRate() * 100);
        if (g.on && g.hexes.DamageTaken() != 1.0) out += std::format("\n受到伤害 ×{:g}", g.hexes.DamageTaken());
        if (g.on) for (const auto& r : tower::rewards::kResists)  // 用户 2026-11-11：抗性有了才显示
            if (const int pm = g.attrs.pm[static_cast<int>(r.kind)]; pm > 0) out += std::format("\n{} {:g}%", tower::rewards::kAttrs[static_cast<int>(r.kind)].name, pm / 10.0);
        if (g.on && g.hexes.EnemyTaken(false) != 1.0) out += std::format("\nBoss 受伤 ×{:g}", g.hexes.EnemyTaken(false));
        if (g.on && g.hexes.EnemyTaken(true) != 1.0) out += std::format("\n小怪受伤 ×{:g}", g.hexes.EnemyTaken(true));
        return out;
    }
    // 实时属性（统一起点 + 本局加成 + 修改器倍率之后的实际值）
    std::string Live() {
        const auto* p = Player();
        if (!p) return "";
        auto v = [&](const char* n) { const int o = attr::AttrOffset(n); return o >= 0 ? ue::Get<float>(attr::AttrCurrent(*p, o)) : 0.0f; };
        return std::format("{}\n生命 {:.0f}/{:.0f}\n杀气 {:.0f}", AtkBreakLines(), v("Health"), v("Health_Max"), v("Shachi_Max"));
    }
    // Ins 主界面（用户 2026-11-01 最终要求）：有实际数值的直接读角色当前值（生命 / 杀气 / 攻击 / 杀气攻击）；
    // 抽象项（攻速、削杀气、杀气恢复速度、受伤）没有可对照的原生总值，只显示本局加成 +0% / +X%——奖励里出现的属性这里都查得到。
    // 不列兵器（兵器一律 Esc 看）。313987b 用户：一行一个数据，不靠自动折行
    std::string MainStats() {
        const auto* p = Player();
        if (!p) return "（读不到角色属性）";
        auto v = [&](const char* n) { const int o = attr::AttrOffset(n); return o >= 0 ? ue::Get<float>(attr::AttrCurrent(*p, o)) : 0.0f; };
        return std::format("{}\n生命 {:.0f}/{:.0f}\n杀气 {:.0f}/{:.0f}", AtkBreakLines(), v("Health"), v("Health_Max"), v("Shachi"), v("Shachi_Max"));
    }
    std::string BriefStats() {
        const auto* p = Player();
        if (!p) return "（读不到角色属性）";
        auto v = [&](const char* n) { const int o = attr::AttrOffset(n); return o >= 0 ? ue::Get<float>(attr::AttrCurrent(*p, o)) : 0.0f; };
        return std::format("{}\n生命 {:.0f}/{:.0f}\n杀气 {:.0f}/{:.0f}", AtkBreakLines(), v("Health"), v("Health_Max"), v("Shachi"), v("Shachi_Max"));
    }
    // 本局加成：与引擎 ApplyGrowth / 敌方削杀气同一套乘法与封顶（旧卡 × 祝福）
    static std::string BonusLine(const tower::Run& r) {
        const auto& m = r.mods;
        const auto& g = towerapi::Growth();
        if (g.on) {  // v14：本轮属性奖励累计（整轮上限）
            std::string out = "本轮属性奖励";
            for (int i = 0; i < tower::rewards::kAttrCount; ++i)
                if (i <= static_cast<int>(tower::rewards::AttrKind::LifeSteal) || g.attrs.pm[i] > 0)  // 抗性有了才列
                    out += std::format("\n{} +{:g}%", tower::rewards::kAttrs[i].name, g.attrs.pm[i] / 10.0);
            return out;
        }
        const tower::growth::Blessings none{};
        const auto& b = g.on ? g.bless : none;
        auto pct = [](double mul) { const int x = static_cast<int>(std::lround((mul - 1) * 100)); return std::format("{}{}%", x >= 0 ? "+" : "", x); };
        return std::format("本局加成\n　攻击/杀气攻击 {}\n　生命上限 {}\n　受伤 {}\n　削杀气 {}\n　杀气恢复速度 {}\n　攻速 {}",
                           pct(std::min(1.5, m.DealtMul() * b.AttackMul())), pct(std::min(1.5, m.MaxHpMul() * b.MaxHpMul())), pct(std::max(0.7, m.TakenMul() * b.TakenMul())),
                           pct(std::min(1.6, m.PostureMul() * b.PostureMul())), pct(std::min(1.6, m.ShachiMul() * b.ShachiMul())), pct(std::min(1.1, m.SpeedMul() * b.SpeedMul())));
    }
    // 道具 / 神通的简短一行（空的不显示）
    static std::string ShortBag(const tower::Run& r) {
        std::vector<std::string> items, st;
        for (int i = 0; i < tower::kItemCount; ++i)
            if (r.items[i] > 0) items.push_back(std::format("{}×{}", tower::kItems[i].name, r.items[i]));
        for (int i : r.shentong) st.push_back(std::string(tower::kShentong[i].name));
        std::string s;
        if (!items.empty()) s += "道具：" + Join(items);
        if (!st.empty()) s += (s.empty() ? "" : "\n") + std::string("神通：") + Join(st);
        return s;
    }
    static std::string ModsLine(const tower::Mods& m) {
        std::vector<std::string> v;
        if (m.dealt) v.push_back(std::format("兵器淬炼 攻击 +{:.0f}%", (m.DealtMul() - 1) * 100));
        if (m.taken) v.push_back(std::format("受伤 ×{:.2f}", m.TakenMul()));
        if (m.maxHp) v.push_back(std::format("生命上限 ×{:.2f}", m.MaxHpMul()));
        if (m.posture) v.push_back(std::format("兵器开刃 削杀气 +{:.0f}%", (m.PostureMul() - 1) * 100));
        if (m.shachi) v.push_back(std::format("杀气恢复速度 ×{:.2f}", m.ShachiMul()));
        if (m.speed) v.push_back(std::format("攻速 ×{:.2f}", m.SpeedMul()));
        if (m.heal) v.push_back(std::format("每层回复 {:.0f}%", m.HealFrac() * 100));
        return "加成：" + Join(v);
    }
    // 背包一览：魂玉 / 道具 / 神通 / 兵器
    static std::string BagText(const tower::Run& r) {
        std::vector<std::string> items, st, weapons;
        for (int i = 0; i < tower::kItemCount; ++i)
            if (r.items[i] > 0) items.push_back(std::format("{}×{}", tower::kItems[i].name, r.items[i]));
        for (int i : r.shentong) st.push_back(std::string(tower::kShentong[i].name));
        for (int w : r.weapons) weapons.push_back(WeaponName(r, w));
        return std::format("魂玉 {}\n道具：{}\n神通：{}\n兵器：{}", r.jade, Join(items), Join(st), Join(weapons));
    }
    std::string Summary(const tower::Run& r) {
        std::string s = std::format("第 {}/{} 层（第 {} 区）", towerapi::ShownNow(), tower::floorplay::ShownTotal(r.floors), r.Zone() + 1);
        if (!r.bosses.empty() && r.phase == tower::Phase::Fighting) {
            std::vector<std::string> b;
            for (int i : r.bosses) b.push_back(std::string(tower::kBossPool[i].name));
            s += "\nBoss：" + Join(b);
        }
        if (towerapi::Growth().on) {  // 成长模式（战斗中 Ins 也走这里）：实际数值 + 本局加成 + 道具 / 神通，不列兵器
            s += "\n" + MainStats() + "\n" + BonusLine(r);
            if (const auto bl = BlessLine(towerapi::Growth().bless); !bl.empty()) s += "\n" + bl;
            if (const auto fx = towerapi::Effects(); !fx.empty()) s += "\n本层生效：" + fx;
            if (const auto bag = ShortBag(r); !bag.empty()) s += "\n" + bag;
            return s;
        }
        if (const auto live = Live(); !live.empty()) s += "\n" + live;
        s += "\n" + ModsLine(r.mods);
        if (const auto fx = towerapi::Effects(); !fx.empty()) s += "\n本层生效：" + fx;
        return s + "\n" + BagText(r);
    }
    static std::string OfferLabel(const tower::Offer& o) {
        switch (o.kind) {
            case tower::OfferKind::Card: return o.index >= 0 ? std::format("{}：{}", tower::kCards[o.index].name, tower::kCards[o.index].text) : "";
            case tower::OfferKind::Item: return o.index >= 0 ? std::format("{}：{}", tower::kItems[o.index].name, tower::kItems[o.index].text) : "";
            case tower::OfferKind::Jade: return std::format("魂玉 +{}", o.amount);
        }
        return "";
    }
    // 本层三选一第 i 项的文字：成长模式用新奖励模型的一组，否则旧十二卡
    static std::string RewardLabel(const tower::Run& r, int i) {
        const auto& g = towerapi::Growth();
        if (g.on) return g.offered ? tower::rewards::Name(g.offers[i]) : std::string();
        return OfferLabel(r.offer[i]);
    }
    // 三选一的正文：每项名称一行、效果另起一行
    static std::string RewardEntries(const tower::Run& r) {
        const auto& g = towerapi::Growth();
        std::string s;
        for (int i = 0; i < 3; ++i) {
            if (g.on) {
                if (!g.offered) continue;
                const auto& o = g.offers[i];
                s += std::format("\n{}. {}", i + 1, tower::rewards::Name(o));
                if (o.kind != tower::rewards::Kind::Weapon)
                    if (const auto effect = tower::language::OfferEffect(o); !effect.empty()) s += "\n　　" + effect;
            }
            else if (const auto label = OfferLabel(r.offer[i]); !label.empty()) s += std::format("\n{}. {}", i + 1, label);
        }
        return s;
    }
    static std::string BlessLine(const tower::growth::Blessings& b) {
        std::vector<std::string> v;
        for (int i = 0; i < tower::growth::kBlessCount; ++i) if (b.stacks[i]) v.push_back(std::format("{}×{}", tower::growth::kBlessings[i].name, b.stacks[i]));
        return v.empty() ? std::string() : "祝福：" + Join(v);
    }
    // 背包里现在能点的东西：道具 / 神通 / 换兵器（兵器只在开层前换）
    struct Entry { std::string label; std::function<void()> act; std::string detail; };  // label 只放短名；detail 在正文名称下另起一行
    std::vector<Entry> BagEntries(const tower::Run& r) {
        std::vector<Entry> all;
        const bool fighting = r.phase == tower::Phase::Fighting, ready = r.phase == tower::Phase::Ready;
        if (!fighting && !ready) return all;
        auto act = [this](std::string cmd, std::string extra) { return [this, cmd, extra] { Send(cmd, extra); Reopen(); }; };
        // 用户 2026-11-01：没有战斗中即时使用的道具。成长模式只走下面的预装；旧模式只剩回春丹这类非预装道具
        if (!towerapi::Growth().on)
            for (int i = 0; i < tower::kItemCount; ++i) {
                const auto& d = tower::kItems[i];
                if (r.items[i] <= 0 || tower::rewards::ItemTiming(d.item) != tower::rewards::Timing::Retired || d.item == tower::Item::Intel || d.item == tower::Item::Borrow) continue;
                all.push_back({std::format("{}×{}", d.name, r.items[i]), act("use", std::format(R"(,"item":"{}")", d.id)), std::string(d.text)});
            }
        for (size_t k = 0; k < r.shentong.size(); ++k) {
            const auto& st = tower::kShentong[r.shentong[k]];
            all.push_back({std::format("神通·{}", st.name), act("st", std::format(R"(,"slot":{})", k)), std::format("{}{}", st.text, ready ? "；作用于下一层" : "；本层生效")});
        }
        if (ready && towerapi::Growth().on) {  // 准备道具：只登记，下一层正式开战才扣（取消 / 生成失败保留）
            const auto& prep = towerapi::Growth().prep;
            for (int i : prep.armed)
                all.push_back({std::format("取消预装 {}", tower::kItems[i].name), act("prep", std::format(R"(,"value":"disarm","item":"{}")", tower::kItems[i].id)), "已预装，下一层开战时扣除并生效"});
            for (int i = 0; i < tower::kItemCount; ++i)
                if (r.items[i] > 0 && !prep.Armed(i) && prep.Why(i, r.items).empty())
                    all.push_back({std::format("预装 {}", tower::kItems[i].name),
                                   act("prep", std::format(R"(,"value":"arm","item":"{}")", tower::kItems[i].id)),
                                   std::format("库存 {}；{}；下一层正式开战时才扣并生效，取消入场不扣；不同种类可一起用，同一种每层一张", r.items[i], tower::kItems[i].text)});
        }
        if (ready && !towerapi::Growth().on) {
            for (int w : r.weapons) {
                if (w == r.wearMain || w == r.wearShaqi) continue;
                const auto& d = tower::kWeaponDrops[w];
                all.push_back({std::format("换上{}（{}）", d.name, d.shaqi ? "杀气兵器" : "主兵器"), act("equip", std::format(R"(,"weapon":"{}")", d.weapon))});
            }
            if (r.wearMain >= 0 || r.wearShaqi >= 0) all.push_back({"换回自己的兵器", act("unequip", "")});
        }
        return all;
    }
    // 背包操作完：开层前回到准备菜单（战斗中不弹，免得挡住战斗）
    void Reopen() { if (towerapi::Run().phase != tower::Phase::Fighting) pendingShow_ = GetTickCount64() + 900; }
    // 发命令后回到指定菜单（等命令在下一帧执行完、上一个对话框关完）
    void Then(std::function<void()> menu) { pendingAct_ = std::move(menu); pendingActAt_ = GetTickCount64() + 900; }
    void Grow(const std::string& action, int index, std::function<void()> back, int to = 1) {
        Send("grow", std::format(R"(,"action":"{}","index":{},"to":{})", action, index, to));
        Then(std::move(back));
    }

    // ---- 原生成长菜单（v2）
    static std::string Loadout() {
        const auto l = towernative::ReadLoadout();
        if (!l.ok) return "（读不到装备）";
        auto name = [](const std::string& id, int kind) {
            if (id.empty() || id == "None") return std::string("空");
            const int w = tower::growth::WeaponIndex(id, kind);
            const int lv = towernative::Level(id, kind);
            return std::format("{}{}", w >= 0 ? std::string(tower::growth::kWeapons[w].name) : id, lv > 0 ? std::format(" {} 级", lv) : std::string());
        };
        std::vector<std::string> main, shaqi;
        for (auto& m : l.main) main.push_back(name(m, 0));
        for (auto& m : l.shaqi) shaqi.push_back(name(m, 1));
        auto med = [](const std::string& id) {
            const int m = tower::growth::MedicineIndex(id);
            return id.empty() || id == "None" ? std::string("空") : m >= 0 ? std::string(tower::growth::kMedicines[m].name) : id;
        };
        return std::format("主兵器：{}\n影之武装：{}\n药瓶：{} / {}\n饰品：{}/{} 件", Join(main), Join(shaqi), med(l.medBottom), med(l.medTop),
                           l.accessories.size(), l.accessorySlots);
    }
    // 准备：配置（原生 Esc → 角色）。进出配置都停在准备，只有点"开第 N 层"才开打
    void OpenNative(towerwidgets::Panel panel) {
        pendingShow_ = 0;
        std::string why;
        if (!towerwidgets::Open(panel, &why)) Show("影塔·百战", why, {{"返回准备", [this] { ShowMain(); }}});
    }
    void ShowFightInfo() {
        ShowInfo(0);
    }
    void ShowFightMore() {
        std::vector<Option> opts;
        opts.push_back({"信息", [this] { ShowFightInfo(); }});
        opts.push_back({"保存并退出", [this] { ShowSaveExitConfirm(); }});
        opts.push_back({"返回", [this] { ShowMain(); }});
        Show("影塔·百战 · 战斗中", "", std::move(opts));
    }
    void ShowSkipConfirm() {
        std::vector<Option> opts;
        opts.push_back({"取消", [this] { ShowMain(); }});
        opts.push_back({"确认跳过本层", [] { Send("skipfloor"); }});
        const auto& r = towerapi::Run();
        const std::string detail = (r.inMobWave || towerapi::FailedStageIsWave())
            ? "确认跳过本层小怪？跳过后不发本层小怪魂玉，可继续下一层。"
            : "确认跳过本层 Boss？可获得本层固定收获，但不发 Boss 兵器和三选一奖励。";
        Show("影塔·百战 · 跳过本层", detail, std::move(opts));
    }
    void ShowSaveExitConfirm() {
        Show("影塔·百战 · 保存并退出", "保存本局并返回原进度？战斗中退出会从本层重新开始。",
             {{"取消", [this] { ShowMain(); }}, {"确认保存并退出", [] { Send("suspend"); }}});  // 手柄焦点落在第一项：取消放前面，防误点
    }
    void ShowInfo(int page) {
        const auto& r = towerapi::Run();
        const auto& g = towerapi::Growth();
        auto rows = tower::menutext::Lines(BriefStats() + (g.on ? std::string() : "\n" + BonusLine(r)));
        for (int i=0;i<tower::growth::kBlessCount;++i) if (g.bless.stacks[i])
            rows.push_back(std::format("{}×{}：{}", tower::growth::kBlessings[i].name, g.bless.stacks[i], tower::growth::kBlessings[i].text));
        for (int i=0;i<tower::kItemCount;++i) if(r.items[i]>0)
            rows.push_back(std::format("{}×{}：{}", tower::kItems[i].name, r.items[i], tower::kItems[i].text));
        for (int i:r.shentong) rows.push_back(std::format("{}：{}", tower::kShentong[i].name, tower::kShentong[i].text));
        if (const auto fx=towerapi::Effects();!fx.empty()) rows.push_back("本层生效："+fx);
        // Keep the player's status and active effects together on one page.
        const int per=std::max(1, static_cast<int>(rows.size()));
        const int pages=std::max(1,(static_cast<int>(rows.size())+per-1)/per);
        page=std::clamp(page,0,pages-1);
        std::string body;
        for(int i=page*per;i<std::min((page+1)*per,static_cast<int>(rows.size()));++i)body+=(body.empty()?"":"\n")+rows[i];
        std::vector<Option> opts;
        if(page>0)opts.push_back({"上一页",[this,page]{ShowInfo(page-1);}});
        if(page+1<pages)opts.push_back({"下一页",[this,page]{ShowInfo(page+1);}});
        opts.push_back({"返回",[this]{ShowMain();}});
        Show("影塔·百战 · 信息",body,std::move(opts));
    }
    void ShowReadyOptions() {
        Show("影塔·百战", "", {
            {"整备",[this]{ShowUpgrade();}}, {"信息",[this]{ShowInfo(0);}}, {"返回",[this]{ShowMain();}}
        });
    }
    void ShowEquip() {
        OpenNative(towerwidgets::Panel::Character);
    }
    void ShowSnap(int page) {
        (void)page;
        OpenNative(towerwidgets::Panel::Soul);
    }
    // 本层收获里的二选一 / 三选一（新药、饰品匣）：领完回准备
    bool ShowGrowthChoice() {
        const auto& g = towerapi::Growth();
        std::vector<Option> opts;
        if (g.forgeBatch.Active() && !forgeDeferred_) { ShowForgeBatch(); return true; }  // 3.6.1 批量锻造（点「退出」后不再自动弹，从驿站「锻造」行回来）
        if (g.hexAttributes.remaining) {
            const int generation = g.hexAttributes.claimed;
            for (int i = 0; i < static_cast<int>(g.hexAttributes.offers.size()); ++i)
                opts.push_back({tower::rewards::Label(g.hexAttributes.offers[i]), [this, i, generation] {
                    Grow("hex_attr", i, [this] { ShowMain(); }, generation);
                }});
            // 3.1.0（用户）：标题写清楚是哪个海克斯给的这一页；投掷狂人让速战速决也选两次，第二页写投掷狂人
            const bool second = g.hexAttributes.blitz && g.hexAttributes.claimed >= 1;
            Show(!g.hexAttributes.blitz ? "属性叠属性" : second ? "投掷狂人 · 速战速决第 2 次" : "速战速决奖励",
                 g.hexAttributes.blitz ? "限时通关奖励：请选择一项属性" : "请选择一项奖励", std::move(opts));
            return true;
        }
        if (!g.bonus.empty()) {  // 1.0.1 用户 2026-11-07：额外挑战成功的额外三选一；药 / 饰品 / MOD 道具在上面写效果
            std::string body;
            for (size_t i = 0; i < g.bonus.size() && i < 3; ++i) {
                AddFx(body, FxLine(g.bonus[i]));
                opts.push_back({std::string(tower::rewards::Name(g.bonus[i])), [this, i] { Grow("bonus", static_cast<int>(i), [this] { ShowMain(); }); }});
            }
            Show(g.hexBonusSecond ? std::format("投掷狂人 · 额外挑战奖励第 {} 次", g.hexBonusSecond + 1) : std::string("影塔·百战 · 额外挑战奖励（三选一）"), body, std::move(opts));
            return true;
        }
        if (!g.med.empty()) {
            std::string body;
            for (size_t i = 0; i < g.med.size() && i < 3; ++i) {
                const auto& m = tower::growth::kMedicines[g.med[i]];
                AddFx(body, std::format("{}：{}", tower::language::Display(m.name, tower::language::Current()),
                                       tower::language::Display(m.text, tower::language::Current())));  // 药的名称和多行效果分别在显示边界翻译
                opts.push_back({std::string(m.name), [this, i] { Grow("med", static_cast<int>(i), [this] { ShowMain(); }); }});
            }
            Show("影塔·百战 · 选一种新药", body, std::move(opts));
            return true;
        }
        if (!g.acc.empty()) {
            std::string body;
            for (size_t i = 0; i < g.acc.size() && i < 3; ++i) {
                const auto& a = tower::growth::kAccessories[g.acc[i]];
                AddFx(body, std::format("{}：{}", tower::language::Display(a.name, tower::language::Current()),
                                       tower::language::Display(a.text, tower::language::Current())));
                opts.push_back({std::string(a.name), [this, i] { Grow("acc", static_cast<int>(i), [this] { ShowMain(); }); }});
            }
            Show("影塔·百战 · 饰品匣", body, std::move(opts));
            return true;
        }
        return false;
    }

    // ---- 列表菜单（用户 2026-11-05：整备 / 驿站 / 背包都要和原生确认框一个样子）：
    // 同一个原生确认框，正文下面插一列游戏自己的确认框按钮（同类、同按钮组），每行一个；底部仍是原生的最多三个按钮。
    // 行按钮不关对话框（就地刷新文字），要换界面的行先关对话框、等关闭动画走完再打开下一个
    static constexpr int kRowsPerPage = 5;  // 用户 2026-11-05：一页 6 行时驿站对话框超出屏幕下沿
    // 用户 2026-11-05：MOD 道具 / 药 / 饰品在正文下面列效果（固定排版，不随悬停变）；兵器不写
    static std::string FxLine(const tower::rewards::Offer& o) {
        using K = tower::rewards::Kind;
        if (tower::rewards::Empty(o)) return "";
        const auto line = [](std::string_view name, std::string_view text) {
            const auto lang = tower::language::Current();
            return std::format("{}：{}", tower::language::Display(name, lang), tower::language::Display(text, lang));
        };
        switch (o.kind) {
            case K::Item: return line(tower::kItems[o.index].name, tower::kItems[o.index].text) + tower::language::OfferItemHint(o);
            case K::Medicine: return line(tower::growth::kMedicines[o.index].name, tower::growth::kMedicines[o.index].text);
            case K::Accessory:
            case K::Refine: return line(tower::growth::kAccessories[o.index].name, tower::growth::kAccessories[o.index].text);
            default: return "";
        }
    }
    static void AddFx(std::string& body, const std::string& line) { if (!line.empty()) body += (body.empty() ? "" : "\n") + line; }

    // 整备：一行一项，点了关掉对话框打开原生界面
    void ShowUpgrade() {
        const auto& g = towerapi::Growth();
        if (!g.on) { ShowMain(); return; }
        const auto cycle = tower::growth::GetCycleProgress(towerapi::Run().floor, g.profile.FinalsUnlocked(), g.profile.FinalExtra());
        const std::string progress = cycle.current >= 50  // 3.0.0 无尽：每 15 层 1 个轮回点，末列可继续升级（最高 30 级）
            ? std::format("下一个轮回点：第 {} 层（还需 {} 层）\n可用轮回点 {}（末列能力可继续升级）", cycle.next + 49, cycle.remaining, cycle.available)
            : cycle.next
            ? std::format("Boss关卡进度 {}/{}，还需 {} 关\n可用轮回点 {}", cycle.current, cycle.next, cycle.remaining, cycle.available)
            : std::format("Boss关卡进度 {}，轮回点目标已达成\n可用轮回点 {}", cycle.current, cycle.available);
        const std::string body = std::format("淬炼点 {}　修为点 {}　铸魂令 {}\n{}", g.profile.temper, g.profile.cultivate, g.profile.soulTokens, progress);
        if (rowsBroken_) {  // 列表插不进去：退回三个按钮
            Show("整备", body,
                 {{"武器升级", [this] { OpenNative(towerwidgets::Panel::Weapon); }}, {"铸魂", [this] { OpenNative(towerwidgets::Panel::Soul); }}, {"MOD背包", [this] { ShowBag(0); }}});
            return;
        }
        struct Item { std::string label, detail; std::function<void()> act; };
        auto items = std::make_shared<std::vector<Item>>();
        auto native = [this](towerwidgets::Panel p) { return [this, p] { OpenNative(p); }; };
        items->push_back({"武器升级", std::format("淬炼点 {}", g.profile.temper), native(towerwidgets::Panel::Weapon)});
        items->push_back({"铸魂", std::format("铸魂令 {}", g.profile.soulTokens), native(towerwidgets::Panel::Soul)});
        items->push_back({"饰品打磨", std::format("淬炼点 {}（每次 {} 点）", g.profile.temper, tower::growth::kAccessoryRefineTemper), native(towerwidgets::Panel::Accessory)});  // 用户 2026-11-11
        items->push_back({"能力修行", std::format("修为点 {}", g.profile.cultivate), native(towerwidgets::Panel::Ability)});  // 用户 2026-11-06：原生界面，一个修为点长按升一级
        items->push_back({"MOD背包", "准备下一层要用的 MOD 道具", [this] { ShowBag(0); }});
        Rows rows;
        rows.count = [items] { return static_cast<int>(items->size()); };
        rows.label = [items](int i) { return i < static_cast<int>(items->size()) ? (*items)[i].label : std::string(); };
        rows.detail = [items](int i) { return i < static_cast<int>(items->size()) ? (*items)[i].detail : std::string(); };
        rows.closes = [](int) { return true; };
        rows.act = [items](int i) { if (i < static_cast<int>(items->size())) (*items)[i].act(); };
        rows.alive = [] { return towerapi::Run().phase == tower::Phase::Ready && towerapi::Growth().on; };
        Show("整备", body,
             {{"返回", [this] { ShowMain(); }}}, std::move(rows));
    }

    // 驿站（成长模式）：选择商品先查看说明和确认，确认后才发送购买。
    struct ShopState { tower::shoppanel::Guard guard; std::string what; int price = 0, before = 0; size_t beforeBought = 0; bool pending = false; };
    std::shared_ptr<ShopState> shopState_ = std::make_shared<ShopState>();
    int shopFocusItem_ = -1;
    void ReturnToShop(const tower::shoppanel::Quote& q, int page, bool fallback) {
        const auto& s = towerapi::Growth().shop;
        if (towerapi::Run().id != q.run || !s.Open() || s.floor != q.floor || s.seed != q.seed) { ShowMain(); return; }
        if (fallback || rowsBroken_) ShowShop(page); else ShowShopRows(page);
    }
    void FinishShopPurchase(tower::shoppanel::Quote q, int page, bool fallback) {
        auto st = shopState_;
        const auto& s = towerapi::Growth().shop;
        if (towerapi::Run().id != q.run || !s.Open() || s.floor != q.floor || s.seed != q.seed) { st->pending = false; st->guard.pending = false; ShowMain(); return; }
        if (s.version == q.version && GetTickCount64() < st->guard.until) {
            Then([this, q, page, fallback] { FinishShopPurchase(q, page, fallback); });
            return;
        }
        st->pending = false;
        st->guard.pending = false;
        const bool recorded = tower::shoppanel::Recorded(q, s, st->beforeBought, st->before, towerapi::Run().jade);
        shopNote_ = recorded
            ? tower::shoppanel::Result(st->what, st->price, st->before, towerapi::Run().jade, false)
            : "交易未确认，请检查货架与魂玉";
        // 用户 2026-11-12：淬炼点 / 修为点买完直接进原生武器升级 / 能力修行，关掉后回驿站
        if (recorded && q.offer.kind == tower::rewards::Kind::TemperPack) { OpenNativeFromShop(towerwidgets::Panel::Weapon, page); return; }
        if (recorded && q.offer.kind == tower::rewards::Kind::CultivatePack) { OpenNativeFromShop(towerwidgets::Panel::Ability, page); return; }
        ReturnToShop(q, page, fallback);
    }
    // 从驿站打开原生面板：面板关掉（且影塔对话框没开）后自动回到驿站同一页
    int shopPanelReturn_ = 0, shopPanelPage_ = 0; unsigned long long shopPanelSince_ = 0;
    void OpenNativeFromShop(towerwidgets::Panel panel, int page) {
        shopPanelReturn_ = 1; shopPanelPage_ = page; shopPanelSince_ = GetTickCount64();
        OpenNative(panel);
    }
    void TickShopPanelReturn() {
        if (!shopPanelReturn_) return;
        const bool active = towerwidgets::PanelActive();
        if (shopPanelReturn_ == 1) {
            if (active) shopPanelReturn_ = 2;
            else if (GetTickCount64() - shopPanelSince_ > 5000) shopPanelReturn_ = 0;  // 面板没打开（OpenNative 已弹原因）
            return;
        }
        if (active || open_) return;
        shopPanelReturn_ = 0;
        if (towerapi::Run().phase == tower::Phase::Ready && towerapi::Growth().shop.Open()) { pendingShow_ = 0; shopDismissed_ = false; ShowShopRows(shopPanelPage_); }
    }
    // 确认购买后重新检查报价，再发送购买请求。
    void BuyNow(tower::shoppanel::Quote q, int page, bool fallback) {
        const auto check = tower::shoppanel::Check(q, towerapi::Growth().shop, towerapi::Run().id, towerapi::Run().jade);
        if (check != tower::shoppanel::QuoteStatus::Ready) {
            shopNote_ = check == tower::shoppanel::QuoteStatus::NoJade ? "魂玉不足" : "货架已变化，未购买";
            ReturnToShop(q, page, fallback);
            return;
        }
        auto st = shopState_;
        const int before = towerapi::Run().jade;
        st->what = tower::rewards::Name(q.offer); st->price = q.price; st->before = before; st->pending = true;
        st->beforeBought = towerapi::Growth().shop.bought.size();
        st->guard.Arm(q.version, before, GetTickCount64());
        Send("shop", std::format(R"(,"value":"buy","slot":{},"ver":{})", q.slot, q.version));
        Then([this, q, page, fallback] { FinishShopPurchase(q, page, fallback); });
    }
    void ShowShopConfirm(tower::shoppanel::Quote q, int page, bool fallback, int effectPage = 0) {
        using namespace tower::shoppanel;
        const auto status = Check(q, towerapi::Growth().shop, towerapi::Run().id, towerapi::Run().jade);
        const auto lang = tower::language::Current();
        const auto pages = EffectPages(tower::language::OfferEffect(q.offer));
        effectPage = std::clamp(effectPage, 0, static_cast<int>(pages.size()) - 1);
        const std::string name = tower::language::Display(tower::rewards::Name(q.offer), lang);
        const bool en = lang == tower::language::Language::English;
        std::string body = name + "\n" + pages[effectPage] + (en
            ? std::format("\n\nPrice: {} Soul Jade　Available: {}", q.price, towerapi::Run().jade)
            : std::format("\n\n价格：{} 魂玉　当前：{} 魂玉", q.price, towerapi::Run().jade));
        if (status == QuoteStatus::NoJade) body += en ? "\nNot enough Soul Jade." : "\n魂玉不足。";
        else if (status != QuoteStatus::Ready) body += en ? "\nStock has changed. No purchase was made." : "\n货架已变化，未购买。";
        std::vector<Option> opts;
        opts.push_back({"取消", [this, q, page, fallback] { ReturnToShop(q, page, fallback); }});
        if (pages.size() > 1)
            opts.push_back({"下一页", [this, q, page, fallback, effectPage, n = static_cast<int>(pages.size())] {
                ShowShopConfirm(q, page, fallback, (effectPage + 1) % n);
            }});
        if (status == QuoteStatus::Ready) opts.push_back({"确认购买", [this, q, page, fallback] { BuyNow(q, page, fallback); }});
        const std::string header = pages.size() > 1
            ? std::format("商品介绍（{}/{}）", effectPage + 1, pages.size()) : "商品介绍";
        Show(header, body, std::move(opts));
        if (open_) shopConfirmFocusPending_ = true;
    }
    static int ShopTabOf(const tower::shoppanel::Row& r) {
        using K = tower::rewards::Kind;
        const auto& shelf = towerapi::Growth().shop.shelf;
        if (r.slot < 0 || r.slot >= static_cast<int>(shelf.size())) return 0;
        const auto& o = shelf[r.slot].offer;
        switch (o.kind) {
            case K::Weapon: return o.quality != tower::rewards::kRareQuality && tower::growth::kWeapons[o.index].kind == 1 ? 2 : 1;  // 3.2.1 随机武器一律在「武器」页，不透露是不是影之武装
            case K::Accessory: case K::Refine: return 3;
            case K::Medicine: return 4;
            case K::Item: return 5;
            default: return 0;
        }
    }
    // ---- 3.1.0 驿站【交易】（用户 2026-11-13）：分类里三行（回收 / 以旧换新 / 熔铸），点进去是兵器列表，最后一页左右对照确认
    static constexpr int kTradeTab = 6;
    static std::string WeaponLine(int w) {
        const auto& p = towerapi::Growth().profile;
        const int s = p.Find(w);
        const auto& d = tower::growth::kWeapons[w];
        std::string line = std::format("{} Lv{}（{}）", d.name, s >= 0 ? p.weapons[s].level : 1, d.kind == 1 ? "影之武装" : "主武器");
        if (const int n = p.MeltCount(w); n > 0) line += std::format("　熔铸 {} 次 +{:.0f}%", n, (tower::trade::MeltMul(n) - 1) * 100);
        return line;
    }
    static std::vector<int> OwnedWeaponList() {
        std::vector<int> out;
        for (const auto& w : towerapi::Growth().profile.weapons) out.push_back(w.index);
        std::stable_sort(out.begin(), out.end(), [](int a, int b) { return tower::growth::kWeapons[a].kind < tower::growth::kWeapons[b].kind; });
        return out;
    }
    static std::vector<tower::shoppanel::Row> TradeRows() {
        const auto& g = towerapi::Growth();
        std::vector<tower::shoppanel::Row> rows;
        // 3.1.0 N8（用户）：只留回收、熔铸，去掉以旧换新；说明从简
        const char* text[3] = {"交出兵器，换回淬炼点和魂玉。", "", "牺牲兵器强化另一把，牺牲品不再出现。"};
        for (int op : {static_cast<int>(tower::trade::Recycle), static_cast<int>(tower::trade::Melt)}) {
            const bool used = g.tradeVisit.Used(g.shop.floor, static_cast<tower::trade::Op>(op));
            rows.push_back({-10 - op, std::string(tower::trade::kOpName[op]), text[op], 0, used, true});
        }
        return rows;
    }
    // 交易页（3.1.0 用户 2026-11-13）：一页两栏对照。左栏 = 交出的兵器（以旧换新选 2 把、熔铸选牺牲品），右栏 = 目标
    // （回收：获得；以旧换新：货架兵器；熔铸：被强化的兵器）。每栏按主武器 / 影之武装分组，每行左侧是原生武器图标；点一下选中 / 取消，
    // 正文就地显示选中兵器的说明与这笔交易的所得；底部按钮确认
    struct TradeCell { int id = -1; bool shelf = false; std::string label, why, icon; bool header = false; };
    struct TradeState { int op = 0; std::vector<int> give; int target = -1; uint32_t ver = 0; int page = 0, pageR = 0; int cat = 0; };  // cat：回收分类（武器 / 影之武装 / 药 / 饰品）；page：回收翻页 / 熔铸左列翻页；pageR：熔铸右列
    TradeState trade_;
    static std::string WeaponIcon(int w) {
        if (w < 0 || w >= tower::growth::kWeaponCount) return {};
        const auto& d = tower::growth::kWeapons[w];
        for (const auto& e : tower::weaponcfg::kConfigs) if (e.weapon == d.id && e.kind == d.kind) return std::string(e.icon);
        return {};
    }
    std::vector<TradeCell> TradeColumn(bool right) const {
        namespace td = tower::trade;
        const auto& g = towerapi::Growth();
        const auto& p = g.profile;
        std::vector<TradeCell> out;
        auto group = [&](int kind, const std::vector<TradeCell>& cells) {
            if (cells.empty()) return;
            out.push_back({-1, false, kind == 1 ? "· 影之武装 ·" : "· 主武器 ·", "", "", true});
            out.insert(out.end(), cells.begin(), cells.end());
        };
        auto owned = [&](bool target) {
            std::vector<TradeCell> cells[2];
            for (const auto& w : p.weapons) {
                const auto& d = tower::growth::kWeapons[w.index];
                if (target && std::find(trade_.give.begin(), trade_.give.end(), w.index) != trade_.give.end()) continue;  // 熔铸：牺牲品不能同时是目标
                // N9g（用户）：任何兵器都能点；保底（每类留一把）在点选时弹提示，不再灰掉
                TradeCell c{w.index, false, std::format("{} Lv{}", d.name, w.level), target ? td::MeltTargetBlocked(p, w.index) : std::string(), WeaponIcon(w.index)};
                if (const int n = p.MeltCount(w.index); n > 0) c.label += std::format(" +{:.0f}%", (td::MeltMul(n) - 1) * 100);
                cells[d.kind].push_back(c);
            }
            group(0, cells[0]); group(1, cells[1]);
        };
        if (!right) { owned(false); return out; }
        if (trade_.op == td::Recycle) {
            if (trade_.give.empty()) { out.push_back({-1, false, "← 选要回收的兵器", "", "", true}); return out; }
            const auto gain = td::RecycleManyGain(p, trade_.give, towerapi::TradeWeaponPrice());
            out.push_back({-1, false, std::format("{} 把", trade_.give.size()), "", "", true});
            out.push_back({-1, false, std::format("淬炼点 +{}", gain.temper), "", "", true});
            out.push_back({-1, false, std::format("魂玉 +{}", gain.jade), "", "", true});
            if (gain.soul) out.push_back({-1, false, std::format("铸魂令 +{}", gain.soul), "", "", true});
            return out;
        }
        if (trade_.op == td::Exchange) {
            std::vector<TradeCell> cells[2];
            const auto& shelf = g.shop.shelf;
            for (int i = 0; i < static_cast<int>(shelf.size()); ++i) {
                const auto& w = shelf[i];
                if (w.sold || w.offer.kind != tower::rewards::Kind::Weapon) continue;
                const auto& d = tower::growth::kWeapons[w.offer.index];
                cells[d.kind].push_back({i, true, std::format("{}", d.name), "", WeaponIcon(w.offer.index)});
            }
            if (cells[0].empty() && cells[1].empty()) out.push_back({-1, false, "本次货架没有兵器", "", "", true});
            group(0, cells[0]); group(1, cells[1]);
            return out;
        }
        owned(true);  // 熔铸：右栏选被强化的兵器
        return out;
    }
    // 左边最多选几把：以旧换新 2 把；回收不限；熔铸到被强化的满 10 次为止（N8）
    int TradeGiveNeed() const {
        if (trade_.op == tower::trade::Exchange) return 2;
        if (trade_.op == tower::trade::Melt) return tower::trade::MeltRoom(towerapi::Growth().profile, trade_.target);
        return 999;
    }
    std::string TradeBody() const {
        namespace td = tower::trade;
        const auto& g = towerapi::Growth();
        const auto& p = g.profile;
        (void)WeaponLine;
        std::string give, get;
        // 交出：最多列 3 个名字，多了写「等 N 把」
        for (size_t i = 0; i < trade_.give.size() && i < 3; ++i) give += (give.empty() ? "" : "、") + std::string(td::CodeName(trade_.give[i]));
        if (trade_.give.size() > 3) give += std::format(" 等 {} 件", trade_.give.size());
        if (trade_.op == td::Recycle && !trade_.give.empty()) {
            const auto gain = td::RecycleMixGain(p, trade_.give, towerapi::TradePrices());
            get = std::format("淬炼点 +{}　魂玉 +{}{}", gain.temper, gain.jade, gain.soul ? std::format("　铸魂令 +{}", gain.soul) : "");
        } else if (trade_.op == td::Exchange && trade_.target >= 0 && trade_.target < static_cast<int>(g.shop.shelf.size())) {
            const int want = g.shop.shelf[trade_.target].offer.index;
            int refund = 0; for (int w : trade_.give) refund += td::Refund(p, w);
            get = std::format("{}（{}）Lv1　淬炼点 +{}", tower::growth::kWeapons[want].name, tower::growth::kWeapons[want].kind == 1 ? "影之武装" : "主武器", refund);
        } else if (trade_.op == td::Melt && trade_.target >= 0) {
            const int n = p.MeltCount(trade_.target), k = std::max<int>(1, static_cast<int>(trade_.give.size()));
            get = std::format("{}：伤害 / 破防加成 +{:.0f}% → +{:.0f}%", tower::growth::kWeapons[trade_.target].name, (td::MeltMul(n) - 1) * 100, (td::MeltMul(n + k) - 1) * 100);
            if (!trade_.give.empty()) { const auto gain = td::MeltManyGain(p, trade_.target, trade_.give); get += std::format("　淬炼点 +{}{}", gain.temper, gain.soul ? std::format("　铸魂令 +{}", gain.soul) : ""); }
        }
        std::string hint = trade_.op == td::Recycle ? "可多选，武器和影之武装可一起回收" : trade_.op == td::Exchange ? "左边选 2 把，右边选想要的" : "左边选材料（可多选），右边选 1 把强化";
        if (trade_.op == td::Recycle) { const int pages = RecyclePages(); if (pages > 1) hint += std::format("　第 {}/{} 页", std::clamp(trade_.page, 0, pages - 1) + 1, pages); }
        std::string body = std::format("{}\n交出：{}\n获得：{}", hint, give.empty() ? "—" : give, get.empty() ? "—" : get);
        if (!shopNote_.empty()) body += "\n" + shopNote_;
        return body;
    }
    bool TradeReady() const {
        if (trade_.op == tower::trade::Exchange) return trade_.give.size() == 2 && trade_.target >= 0;
        if (trade_.op == tower::trade::Melt) return !trade_.give.empty() && trade_.target >= 0 && static_cast<int>(trade_.give.size()) <= TradeGiveNeed();
        return !trade_.give.empty();
    }
    void ShowTrade(int op) {
        trade_ = TradeState{op, {}, -1, towerapi::Growth().shop.version, 0, 0};
        shopNote_.clear();
        ShowTradePage();
    }
    // 3.1.0 N8（用户 2026-11-13 定版）：
    //   回收：所有兵器一排一排排开（3 列 × 5 行，主武器在前、影之武装另起一排），点了标【选中】、可多选，底部确认；多了用底部翻页
    //   熔铸：两列——左列 = 已有兵器（熔铸材料，可多选），右列 = 已有兵器（强化目标，选 1 把）；每列最后一格是这一列自己的小翻页
    // 3.1.0 实机（用户截图：图标和字互相挡住）：4 列时一格约 275 像素，图标 + 「亡命斩马刀 Lv1」放不下、文字溢到下一格压住下一格的图标。
    // 改 3 列（一页 15 把），图标缩到 44，按钮占满格子剩余宽度
    // N9g（用户 2026-11-13：左右排版混乱）：每格上图标、下文字；4 列 × 3 行。回收一页 12 把；
    // 熔铸左 2 列 = 材料、右 2 列 = 目标，每边 6 格，放不下时每边最后一格是这一边的翻页
    static constexpr int kTradeGridCols = 4, kTradeRows = 3, kMeltSideCols = 2, kMeltSide = kMeltSideCols * kTradeRows;
    // 去掉分组标题，影之武装另起一排（width = 这一块有几列）
    std::vector<TradeCell> TradeGrid(bool right, int width) const {
        std::vector<TradeCell> out;
        for (const auto& c : TradeColumn(right)) {
            if (c.header) { if (!out.empty()) while (out.size() % static_cast<size_t>(width)) out.push_back({}); continue; }
            out.push_back(c);
        }
        return out;
    }
    // N9i（用户）：回收页分类 武器 / 影之武装 / 药 / 饰品；每格上图标、下名字和等级；已选的排在最前
    std::vector<TradeCell> RecycleGrid() const {
        namespace td = tower::trade;
        const auto& p = towerapi::Growth().profile;
        std::vector<TradeCell> out;
        if (trade_.cat <= 1) {
            for (const auto& w : p.weapons) {
                const auto& d = tower::growth::kWeapons[w.index];
                if (d.kind != trade_.cat) continue;
                TradeCell c{w.index, false, std::format("{} Lv{}", d.name, w.level), "", WeaponIcon(w.index)};
                if (const int n = p.MeltCount(w.index); n > 0) c.label += std::format(" +{:.0f}%", (td::MeltMul(n) - 1) * 100);
                out.push_back(c);
            }
        } else if (trade_.cat == 2) {
            for (int m : p.medicines) out.push_back({td::kMedCode + m, false, std::string(tower::growth::kMedicines[m].name), "", std::string(tower::itemicon::Of(tower::growth::kMedicines[m].id))});
        } else {
            for (const auto& a : p.accessories)
                out.push_back({td::kAccCode + a.index, false, std::string(tower::growth::kAccessories[a.index].name) + (a.refine > 1 ? std::format(" +{}", a.refine - 1) : ""), "",
                               std::string(tower::itemicon::Of(tower::growth::kAccessories[a.index].id))});
        }
        std::stable_partition(out.begin(), out.end(), [this](const TradeCell& c) { return std::find(trade_.give.begin(), trade_.give.end(), c.id) != trade_.give.end(); });
        return out;
    }
    int RecyclePages() const { return std::max(1, (static_cast<int>(RecycleGrid().size()) + kTradeGridCols * kTradeRows - 1) / (kTradeGridCols * kTradeRows)); }
    // N9h（用户 2026-11-13 定版）：熔铸只分两列。每列 = 标题 + 6 把（图标在左）+ 底部一行小箭头（前一页 / 后一页）。
    // 左列标题「熔铸材料（永久消失）」、右列「熔铸武器」；主武器 / 影之武装用列内小标题分开
    static constexpr int kMeltPer = 6, kMeltRows = kMeltPer + 3;  // 标题 + 6 把 + ＜ + ＞（最后两格并成一行）
    static bool MeltRight(int i) { return i % 2 == 1; }
    int MeltPages(bool right) const { const int n = static_cast<int>(TradeColumn(right).size()); return std::max(1, (n + kMeltPer - 1) / kMeltPer); }
    TradeCell MeltCell(bool right, int row) const {
        const int pages = MeltPages(right);
        int& page = right ? const_cast<TradeState&>(trade_).pageR : const_cast<TradeState&>(trade_).page;
        page = std::clamp(page, 0, pages - 1);
        if (row == 0) {
            std::string t = right ? "熔铸武器" : "熔铸材料（永久消失）";
            if (pages > 1) t += std::format("  {}/{}", page + 1, pages);
            return {-1, false, t, "", "", true};
        }
        if (row == kMeltRows - 2) return pages > 1 ? TradeCell{-3, false, "＜", "", "", false} : TradeCell{};
        if (row == kMeltRows - 1) return pages > 1 ? TradeCell{-2, false, "＞", "", "", false} : TradeCell{};
        const auto list = TradeColumn(right);
        const int x = page * kMeltPer + row - 1;
        return x < static_cast<int>(list.size()) ? list[x] : TradeCell{};
    }
    void ShowTradePage() {
        namespace td = tower::trade;
        if (!towerapi::Growth().shop.Open()) { ShowMain(); return; }
        const bool melt = trade_.op == td::Melt;
        const int cols = melt ? 2 : kTradeGridCols, perPage = cols * (melt ? kMeltRows : kTradeRows);
        auto cells = [this, melt, perPage](int i) -> TradeCell {
            if (melt) return MeltCell(MeltRight(i), i / 2);
            const auto grid = RecycleGrid();
            const int k = trade_.page * perPage + i;
            return k < static_cast<int>(grid.size()) ? grid[k] : TradeCell{};
        };
        int pages = 1;
        if (!melt) { pages = RecyclePages(); trade_.page = std::clamp(trade_.page, 0, pages - 1); }
        Rows rows;
        rows.columns = cols; rows.perColumn = melt ? kMeltRows : kTradeRows; rows.noCollapse = true; rows.iconAbove = !melt; rows.pagerPair = melt; rows.splitAt = melt ? 1 : 0;
        if (!melt) {
            rows.tabs = {"武器", "影之武装"};  // N9k（用户）：药、饰品不回收
            rows.tab = std::clamp(trade_.cat, 0, 1);
            rows.onTab = [this](int tab) { trade_.cat = std::clamp(tab, 0, 1); trade_.page = 0; };
        }
        rows.count = [perPage] { return perPage; };
        rows.label = [this, cells, melt](int i) {
            const auto c = cells(i);
            if (c.label.empty()) return std::string();
            if (c.header || c.id == -2 || c.id == -3) return c.label;
            const bool picked = (!melt || !MeltRight(i)) ? std::find(trade_.give.begin(), trade_.give.end(), c.id) != trade_.give.end() : trade_.target == c.id;
            return (picked ? "已选 " : "") + c.label + (c.why.empty() ? "" : "（" + c.why + "）");
        };
        rows.icon = [cells](int i) { return cells(i).icon; };
        rows.enabled = [cells](int i) { const auto c = cells(i); return c.id == -2 || c.id == -3 || (!c.header && c.id >= 0 && c.why.empty()); };
        // N9g：点了会把某一类交到一把不剩 → 先关掉交易页再弹提示（知道了 → 回到交易页，已选的保留）
        auto warn = [this, cells, melt](int i) -> std::string {
            const auto c = cells(i);
            if (c.header || c.id < 0 || (melt && MeltRight(i)) || !tower::trade::IsWeapon(c.id)) return {};
            const auto& gv = trade_.give;
            if (std::find(gv.begin(), gv.end(), c.id) != gv.end()) return {};
            std::vector<int> more;
            for (int x : gv) if (tower::trade::IsWeapon(x)) more.push_back(x);
            more.push_back(c.id);
            return tower::trade::GiveManyBlocked(towerapi::Growth().profile, more, {});
        };
        rows.closes = [warn](int i) { return !warn(i).empty(); };
        rows.act = [this, cells, melt, warn](int i) {
            if (const auto why = warn(i); !why.empty()) {
                Show("影塔·百战 · 提示", why + "，这把不能" + std::string(melt ? "熔铸" : "回收") + "。", {{"知道了", [this] { ShowTradePage(); }}});
                return;
            }
            const auto c = cells(i);
            if (c.id == -2 || c.id == -3) {  // 这一列自己翻页：＞ 后一页、＜ 前一页（首尾循环）
                const bool right = MeltRight(i); const int pages = MeltPages(right);
                int& pg = right ? trade_.pageR : trade_.page; pg = (pg + (c.id == -2 ? 1 : pages - 1)) % pages; return;
            }
            if (c.header || c.id < 0 || !c.why.empty()) return;
            shopNote_.clear();
            if (!melt || !MeltRight(i)) {  // 回收的格子 / 熔铸左边：材料，可多选
                auto& gv = trade_.give;
                if (auto it = std::find(gv.begin(), gv.end(), c.id); it != gv.end()) gv.erase(it);
                else {
                    // N9g（用户）：交到某一类一把不剩时弹提示（多选到全部也一样）
                    if (static_cast<int>(gv.size()) < TradeGiveNeed()) gv.push_back(c.id);
                    else shopNote_ = std::format("这把最多再熔 {} 把", TradeGiveNeed());
                }
                if (melt && trade_.target >= 0 && std::find(gv.begin(), gv.end(), trade_.target) != gv.end()) trade_.target = -1;
            } else {  // 熔铸右列：目标，选 1 把
                trade_.target = trade_.target == c.id ? -1 : c.id;
                if (static_cast<int>(trade_.give.size()) > TradeGiveNeed()) trade_.give.resize(static_cast<size_t>(TradeGiveNeed()));
            }
        };
        rows.body = [this] { return TradeBody(); };
        rows.alive = [] { return towerapi::Growth().shop.Open(); };
        std::vector<Option> opts;
        opts.push_back({"返回驿站", [this] { shopTab_ = kTradeTab; ShowShopRows(0); }});
        // N9g（用户）：回收页上下翻页（后期几十把，一页 12 把放不下）；首页 / 末页循环
        // N9i：分类切换后页数会变，翻页按当时的页数算；页码写在正文
        if (!melt) {
            (void)pages;
            opts.push_back({"上一页", [this] { const int n = RecyclePages(); trade_.page = (std::clamp(trade_.page, 0, n - 1) + n - 1) % n; ShowTradePage(); }});
            opts.push_back({"下一页", [this] { const int n = RecyclePages(); trade_.page = (std::clamp(trade_.page, 0, n - 1) + 1) % n; ShowTradePage(); }});
        }
        opts.push_back({std::string("确认") + std::string(td::kOpName[trade_.op]), [this, melt] {
            if (!TradeReady()) { shopNote_ = melt ? "左边选材料，右边选要强化的兵器" : "先选要回收的兵器"; ShowTradePage(); return; }
            const int a = melt ? trade_.target : trade_.give[0];
            const int b = melt ? trade_.give[0] : -1;
            std::string list;
            for (int w : trade_.give) list += (list.empty() ? "" : ",") + std::to_string(w);
            Send("trade", std::format(R"(,"kind":"{}","a":{},"b":{},"target":-1,"ver":{},"list":"{}")", melt ? "melt" : "recycle", a, b, towerapi::Growth().shop.version, list));
            Then([this] { shopNote_ = towerapi::TradeNote(); shopTab_ = kTradeTab; ShowShopRows(0); });
        }});
        Show(std::string(td::kOpName[trade_.op]), TradeBody(), std::move(opts), std::move(rows));
    }
    // 货架上这一格现在为什么不能买（能买返回空）；只看货架格（slot ≥ 0），锻造 / 交易行不管
    static std::string ShelfWhyNot(int slot) {
        const auto& g = towerapi::Growth();
        if (slot < 0 || slot >= static_cast<int>(g.shop.shelf.size())) return {};
        const auto& r = towerapi::Run();
        return std::string(tower::rewards::WhyNot(g.shop.shelf[slot].offer, g.Ctx(r.floor, r.floors, r.items)));
    }
    static std::string ShopIcon(int slot) {
        const auto& s = towerapi::Growth().shop;
        if (slot < 0 || slot >= static_cast<int>(s.shelf.size())) return {};
        const auto& o = s.shelf[slot].offer;
        if (tower::rewards::Mystery(o) || o.quality == tower::rewards::kRareQuality) return {};  // 珍品随机位：文字
        using K = tower::rewards::Kind;
        switch (o.kind) {
            case K::Weapon: return WeaponIcon(o.index);
            case K::Medicine: return o.index >= 0 && o.index < tower::growth::kMedicineCount ? std::string(tower::itemicon::Of(tower::growth::kMedicines[o.index].id)) : std::string();
            case K::Accessory: case K::Refine: return o.index >= 0 && o.index < tower::growth::kAccessoryCount ? std::string(tower::itemicon::Of(tower::growth::kAccessories[o.index].id)) : std::string();
            default: return {};  // MOD 道具、点数、铸魂令等自制：文字
        }
    }
    static std::vector<tower::shoppanel::Row> ShopRows(int tab) {
        std::vector<tower::shoppanel::Row> all;
        const auto& s = towerapi::Growth().shop;
        for (int p = 0;; ++p) {
            const auto v = tower::shoppanel::Build(s, towerapi::Run().jade, p);
            all.insert(all.end(), v.rows.begin(), v.rows.end());
            if (p + 1 >= v.pages) break;
        }
        if (tower::language::Current() == tower::language::Language::English)
            for (auto& row : all) if (row.slot >= 0 && row.slot < static_cast<int>(s.shelf.size()))
                row.effect = tower::rewards::Mystery(s.shelf[row.slot].offer) ? tower::rewards::Effect(s.shelf[row.slot].offer) : tower::language::OfferEffect(s.shelf[row.slot].offer);
        std::erase_if(all, [tab](const tower::shoppanel::Row& r) { return ShopTabOf(r) != tab; });
        if (tab == kTradeTab) return TradeRows();
        if (tab == 0 && towerapi::Growth().hexes.Has(tower::hex::AttributeStack))
        {
            const auto& fb = towerapi::Growth().forgeBatch;
            if (fb.Active())  // 3.6.1：已付款、还没选完的批量锻造
                all.insert(all.begin(), {-2, std::format("锻造（{} 组待选）", fb.rows.size()), "已付款，点这里接着选择成长属性。", 0, false, true});
            else
                all.insert(all.begin(), {-2, "锻造", std::format("每个 {} 魂玉，可一次买多个。", tower::hex::ForgePrice(towerapi::Run().floor, towerapi::Run().floors)), tower::hex::ForgePrice(towerapi::Run().floor, towerapi::Run().floors), false, towerapi::Run().jade >= tower::hex::ForgePrice(towerapi::Run().floor, towerapi::Run().floors)});
        }
        return all;
    }
    void ShowAttributeForgeConfirm(std::string run, int floor, uint64_t seed, uint32_t version, int page) {
        const auto& s = towerapi::Growth().shop;
        const bool valid = s.Open() && towerapi::Run().id == run && s.floor == floor && s.seed == seed && s.version == version &&
                           towerapi::Growth().hexes.Has(tower::hex::AttributeStack) && !towerapi::Growth().hexAttributes.remaining && !towerapi::Growth().forgeBatch.Active();
        const int price = tower::hex::ForgePrice(towerapi::Run().floor, towerapi::Run().floors);
        const int most = price > 0 ? towerapi::Run().jade / price : 0;  // 3.4.1（玩家：魂玉够 1500 却显示魂玉不足）：确认页按当前价判断
        const int groups = towerapi::Growth().hexes.ThrowerMul();
        // 3.6.1（用户：后期一次只能买一个太麻烦；要能直接输入数量，再配加减按钮方便手柄 / 键鼠微调）：
        // 上面一个数量输入框（键盘直接打数字），下面原生按钮：−10 / −1 / +1 / +10，最少 / 最多 / 购买。买得起的全部都能买，不设上限
        forgeQty_ = std::clamp(forgeQty_, 1, std::max(1, most));
        Rows rows;
        rows.columns = 4; rows.perColumn = 2; rows.noCollapse = true; rows.numberInput = true;
        rows.count = [] { return 8; };
        rows.inputText = [this] { return std::to_string(forgeQty_); };
        rows.onInput = [this, most](const std::string& typed) {
            long long v = 0; bool digit = false;
            for (char ch : typed) if (ch >= '0' && ch <= '9') { digit = true; v = std::min(v * 10 + (ch - '0'), 1000000000LL); }
            if (digit) forgeQty_ = static_cast<int>(std::clamp(v, 1LL, static_cast<long long>(std::max(1, most))));
        };
        static constexpr std::array<int, 4> kStep{-10, -1, 1, 10};
        rows.label = [this, groups](int i) -> std::string {
            if (i < 4) return kStep[static_cast<size_t>(i)] > 0 ? std::format("+{}", kStep[static_cast<size_t>(i)]) : std::format("−{}", -kStep[static_cast<size_t>(i)]);
            if (i == 4) return "最少";
            if (i == 5) return "最多";
            if (i == 7) return std::format("购买 ×{}", forgeQty_);
            return {};
        };
        rows.enabled = [valid, most](int i) { return valid && most > 0 && i != 6; };
        rows.closes = [](int i) { return i == 7; };
        rows.act = [this, most](int i) {
            if (i < 4) forgeQty_ = std::clamp(forgeQty_ + kStep[static_cast<size_t>(i)], 1, std::max(1, most));
            else if (i == 4) forgeQty_ = 1;
            else if (i == 5) forgeQty_ = std::max(1, most);
        };
        rows.body = [this, price, groups, most, valid] {
            const int n = forgeQty_;
            const std::string extra = groups > 1 ? std::format("，投掷狂人额外 +{} 组，共 {} 组", n * (groups - 1), n * groups) : std::string();
            return std::format("每个 {} 魂玉。当前魂玉 {}，最多可买 {} 个。\n购买 {} 个{}，花费 {} 魂玉。\n买了不退，选完才能开下一层{}", price, towerapi::Run().jade, most, n, extra,
                               static_cast<long long>(n) * price, !valid ? "\n货架已变化" : most <= 0 ? "\n魂玉不足" : "");
        };
        rows.prepare = [this, run, floor, seed, version, page](int i) -> std::function<void()> {
            if (i != 7) return {};
            const int n = forgeQty_;
            return [this, run, floor, seed, version, page, n] {
                const auto& current = towerapi::Growth().shop;
                if (!current.Open() || towerapi::Run().id != run || current.floor != floor || current.seed != seed || current.version != version) { shopNote_ = "货架已变化，请重试"; ShowShopRows(page); return; }
                forgeSel_.clear(); forgePage_ = 0;
                Send("shop", std::format(R"(,"value":"forge","ver":{},"slot":{})", version, n));
                Then([this, page] {
                    if (towerapi::Growth().forgeBatch.Active()) { shopDismissed_ = false; ShowForgeBatch(); }
                    else { shopNote_ = "锻造未成交，请检查魂玉与货架"; ShowShopRows(page); }
                });
            };
        };
        std::vector<Option> opts{{"取消", [this, page] { ShowShopRows(page); }}};
        Show("锻造", "", std::move(opts), std::move(rows));
    }
    int forgeQty_ = 1;  // 3.6.1：锻造数量页上次选的数量
    // 3.6.1 批量锻造选择页：一页 5 组、每组一行三个属性，每行只能选一个（选中的按钮常亮原生选中底图，文字加【】）；
    // 最下一行：上一页 / 下一页 / 确定；底部原生按钮「退出」（已付款的锻造保留，回到影塔菜单会再弹出这一页）。确定后一起加到属性上
    static constexpr int kForgeRowsPerPage = 5;
    std::vector<int> forgeSel_;
    int forgePage_ = 0;
    bool forgeDeferred_ = false;
    void ShowForgeBatch() {
        const auto& fb = towerapi::Growth().forgeBatch;
        if (!fb.Active()) { ShowMain(); return; }
        const int groups = static_cast<int>(fb.rows.size());
        if (static_cast<int>(forgeSel_.size()) != groups) forgeSel_ = fb.pick;
        const int pages = std::max(1, (groups + kForgeRowsPerPage - 1) / kForgeRowsPerPage);
        forgePage_ = std::clamp(forgePage_, 0, pages - 1);
        Rows rows;
        rows.columns = 3; rows.perColumn = kForgeRowsPerPage + 1; rows.noCollapse = true; rows.forgeGrid = true;
        rows.count = [] { return 3 * (kForgeRowsPerPage + 1); };
        auto group = [this](int i) { return forgePage_ * kForgeRowsPerPage + i / 3; };
        rows.label = [this, group](int i) -> std::string {
            const auto& b = towerapi::Growth().forgeBatch;
            const int n = static_cast<int>(b.rows.size()), pages = std::max(1, (n + kForgeRowsPerPage - 1) / kForgeRowsPerPage);
            if (i >= 3 * kForgeRowsPerPage) {
                const int c = i % 3;
                if (c == 0) return forgePage_ > 0 ? std::string("上一页") : std::string();
                if (c == 1) return pages > 1 ? std::format("下一页 {}/{}", forgePage_ + 1, pages) : std::string();
                const int left = static_cast<int>(std::count(forgeSel_.begin(), forgeSel_.end(), -1));
                return left ? std::format("还有 {} 组未选", left) : std::string("确定");  // 用户 2026-11-15：没选完不能确定
            }
            const int g = group(i), c = i % 3;
            if (g >= n || c >= static_cast<int>(b.rows[static_cast<size_t>(g)].size())) return {};
            const auto text = tower::rewards::Label(b.rows[static_cast<size_t>(g)][static_cast<size_t>(c)]);
            return g < static_cast<int>(forgeSel_.size()) && forgeSel_[static_cast<size_t>(g)] == c ? "【" + text + "】" : text;
        };
        rows.lit = [this, group](int i) {
            if (i >= 3 * kForgeRowsPerPage) return false;
            const int g = group(i);
            return g < static_cast<int>(forgeSel_.size()) && forgeSel_[static_cast<size_t>(g)] == i % 3;
        };
        rows.enabled = [this](int i) {
            if (i < 3 * kForgeRowsPerPage) return true;
            const int n = static_cast<int>(towerapi::Growth().forgeBatch.rows.size()), pages = std::max(1, (n + kForgeRowsPerPage - 1) / kForgeRowsPerPage);
            const int c = i % 3;
            if (c == 2) return std::count(forgeSel_.begin(), forgeSel_.end(), -1) == 0;  // 全部选完才能确定
            return (c == 0 && forgePage_ > 0) || (c == 1 && forgePage_ + 1 < pages);
        };
        rows.alive = [] { return towerapi::Growth().forgeBatch.Active(); };
        rows.closes = [](int i) { return i == 3 * kForgeRowsPerPage + 2; };  // 确定：关对话框后提交
        rows.body = [this] {
            const auto& b = towerapi::Growth().forgeBatch;
            const int n = static_cast<int>(b.rows.size());
            const int from = forgePage_ * kForgeRowsPerPage + 1, to = std::min(n, from + kForgeRowsPerPage - 1);
            const std::string head = b.Extra() > 0 ? std::format("锻造器 {} 个，投掷狂人额外获得 {} 组，共 {} 组", b.forges, b.Extra(), n) : std::format("锻造器 {} 个，共 {} 组", b.forges, n);
            return std::format("{}；本页第 {}–{} 组。\n每组选一项，全部选完才能点「确定」，一起加到属性上。伤害减免、抗性有上限，超出部分作废。", head, from, to);
        };
        rows.act = [this, group](int i) {
            const auto& b = towerapi::Growth().forgeBatch;
            const int n = static_cast<int>(b.rows.size()), pages = std::max(1, (n + kForgeRowsPerPage - 1) / kForgeRowsPerPage);
            if (i >= 3 * kForgeRowsPerPage) {
                const int c = i % 3;
                if (c == 0 && forgePage_ > 0) --forgePage_;
                if (c == 1 && forgePage_ + 1 < pages) ++forgePage_;
                return;
            }
            const int g = group(i), c = i % 3;
            if (g < n && g < static_cast<int>(forgeSel_.size()) && c < static_cast<int>(b.rows[static_cast<size_t>(g)].size())) forgeSel_[static_cast<size_t>(g)] = c;
        };
        rows.prepare = [this](int i) -> std::function<void()> {
            if (i != 3 * kForgeRowsPerPage + 2) return {};
            std::string picks;
            for (size_t k = 0; k < forgeSel_.size(); ++k) picks += (k ? "," : "") + std::to_string(forgeSel_[k]);
            return [this, picks] {
                Send("forge_pick", std::format(R"(,"picks":"{}")", picks));
                Then([this] { forgeSel_.clear(); forgePage_ = 0; ShowMain(); });
            };
        };
        forgeDeferred_ = false;
        std::vector<Option> opts{{"退出", [this] {
            forgeDeferred_ = true;  // 已付款的锻造保留：驿站「锻造」行（N 组待选）回来接着选；没选完不能开下一层
            if (towerapi::Growth().shop.Open()) ShowShopRows(0); else ShowMain();
        }}};
        Show("锻造 · 选择成长属性", "", std::move(opts), std::move(rows));
    }
    // 分类独立成两行，给原生按钮留足宽度；商品减少到三行，为分类腾出高度。
    // 驿站上图下字（N13）
    static constexpr bool kShopIcons = true;  // 3.5.0（用户）：正式启用
    static constexpr int kShopColumns = kShopIcons ? 3 : 2;  // 上图下字时 3 列 × 2 行（每页仍 6 件，降低总高度）
    static constexpr int kShopRows = kShopIcons ? 2 : 3;
    static constexpr int kShopPerPage = kShopRows * kShopColumns;
    int shopTab_ = 0, shopPage_ = 0, shopTabFloor_ = -1;
    std::string ShopTitle() const {
        const int total = static_cast<int>(ShopRows(shopTab_).size());
        const int pages = std::max(1, (total + kShopPerPage - 1) / kShopPerPage);
        return pages > 1 ? std::format("驿站（{}/{}）", std::min(shopPage_ + 1, pages), pages) : std::string("驿站");
    }
    void ShowShopRows(int page) {
        if (const int f = towerapi::Growth().shop.floor; f != shopTabFloor_) { shopTabFloor_ = f; shopTab_ = 0; }  // 新开的店从第一个分类看起
        const int total = static_cast<int>(ShopRows(shopTab_).size());
        const int pages = std::max(1, (total + kShopPerPage - 1) / kShopPerPage);
        shopPage_ = std::clamp(page, 0, pages - 1);
        auto st = shopState_;
        Rows rows;
        rows.first = shopPage_ * kShopPerPage;
        rows.columns = kShopColumns;
        rows.perColumn = kShopRows;
        rows.tabs = {"属性", "武器", "影之武装", "饰品", "药", "MOD道具", "交易"};
        rows.tab = shopTab_;
        if (shopFocusItem_ >= 0 && total > 0)
            rows.focusIndex = std::clamp(shopFocusItem_, shopPage_ * kShopPerPage, std::min(total - 1, (shopPage_ + 1) * kShopPerPage - 1));
        shopFocusItem_ = -1;
        rows.onTab = [this](int tab) { shopTab_ = tab; shopPage_ = 0; rowsHeader_ = ShopTitle(); };
        rows.count = [this] { return static_cast<int>(ShopRows(shopTab_).size()); };
        rows.label = [this](int i) {
            const auto all = ShopRows(shopTab_);
            if (i >= static_cast<int>(all.size())) return std::string();
            const auto& r = all[i];
            if (r.slot <= -10) return r.sold ? std::format("{}　本次已用", r.name) : r.name;  // 交易
            if (r.sold) return std::format("{}　已购买", r.name);
            if (const auto why = ShelfWhyNot(r.slot); !why.empty()) return std::format("{}　{}", r.name, why);  // 3.1.0：不能买的写原因并置灰
            return std::format("{}　{} 魂玉", r.name, r.price);
        };
        rows.detail = [this](int i) { const auto all = ShopRows(shopTab_); return i < static_cast<int>(all.size()) ? all[i].effect : std::string(); };
        rows.enabled = [this, st](int i) {
            const auto all = ShopRows(shopTab_);
            if (i >= static_cast<int>(all.size()) || all[i].sold || !ShelfWhyNot(all[i].slot).empty()) return false;
            return !st->guard.Busy(towerapi::Growth().shop.version, towerapi::Run().jade, GetTickCount64());
        };
        // 3.4.3（用户：驿站做成上图下字）：武器 / 影之武装 / 饰品 / 药用原生图标；珍品随机位、MOD 道具、点数、锻造、交易不贴图（格子照样占位，不错位）
        if (kShopIcons) {
            rows.iconAbove = true;
            rows.icon = [this](int i) { const auto all = ShopRows(shopTab_); return i < static_cast<int>(all.size()) ? ShopIcon(all[i].slot) : std::string(); };
        }
        rows.closes = [](int) { return true; };
        rows.prepare = [this](int i) -> std::function<void()> {
            const auto all = ShopRows(shopTab_);
            if (i < 0 || i >= static_cast<int>(all.size())) return {};
            if (all[i].slot <= -10) {  // 交易：进二级菜单
                const int op = -10 - all[i].slot;
                return [this, op] { ShowTrade(op); };
            }
            if (all[i].slot == -2) {
                const auto& s = towerapi::Growth().shop; const auto run = towerapi::Run().id;
                const int page = shopPage_, floor = s.floor; const auto seed = s.seed; const auto version = s.version;
                if (towerapi::Growth().forgeBatch.Active()) return [this] { ShowForgeBatch(); };  // 3.6.1：已付款待选的批量锻造
                return [this, run, floor, seed, version, page] { ShowAttributeForgeConfirm(run, floor, seed, version, page); };
            }
            const auto q = tower::shoppanel::Capture(towerapi::Growth().shop, towerapi::Run().id, all[i].slot);
            if (!q) return {};
            shopFocusItem_ = i;
            const int page = shopPage_;
            return [this, q = *q, page] { ShowShopConfirm(q, page, false); };
        };
        rows.body = [this] {
            std::string b = std::format("魂玉 {}", towerapi::Run().jade);
            if (!shopNote_.empty()) b += "\n" + shopNote_;
            const auto all = ShopRows(shopTab_);
            if (all.empty()) b += "\n暂无商品";
            return b;
        };
        rows.alive = [] { return towerapi::Growth().shop.Open(); };
        std::vector<Option> opts;
        opts.push_back({"上一页", [this] { ShowShopRows(shopPage_ - 1); }});
        opts.push_back({"下一页", [this] { ShowShopRows(shopPage_ + 1); }});
        opts.push_back({"离开驿站", [this] { shopNote_.clear(); shopDismissed_ = true; Send("shop", R"(,"value":"leave")"); Reopen(); }});
        const auto body = rows.body();
        Show(ShopTitle(), body, std::move(opts), std::move(rows));
    }

    // MOD 背包（成长模式准备阶段）：一行一件，点了准备 / 撤销 / 更换（就地刷新）
    struct BagLine { int item = -1, shentong = -1; };
    static std::vector<BagLine> BagLines() {
        std::vector<BagLine> out;
        const auto& r = towerapi::Run();
        const auto& prep = towerapi::Growth().prep;
        for (int i = 0; i < tower::kItemCount; ++i) {
            if (tower::rewards::ItemTiming(tower::kItems[i].item) == tower::rewards::Timing::Retired) continue;
            if (r.items[i] > 0 || prep.Armed(i)) out.push_back({i, -1});
        }
        for (int k = 0; k < static_cast<int>(r.shentong.size()); ++k) out.push_back({-1, k});
        return out;
    }
    static std::string BagVerb(const BagLine& l) {
        if (l.shentong >= 0) return "使用";
        const auto t = tower::rewards::ItemTiming(tower::kItems[l.item].item);
        if (t == tower::rewards::Timing::Passive) return "自动生效";
        if (t == tower::rewards::Timing::Settlement) return "领奖时使用";
        if (t == tower::rewards::Timing::Use) return "使用";
        if (towerapi::Growth().prep.Armed(l.item)) return "撤销";
        return "准备";  // 3.6.0：不同种类可以一起准备，不再「更换」
    }
    void ShowBagRows(int page) {
        const int total = static_cast<int>(BagLines().size());
        const int pages = std::max(1, (total + kRowsPerPage - 1) / kRowsPerPage);
        page = std::clamp(page, 0, pages - 1);
        Rows rows;
        rows.first = page * kRowsPerPage;
        rows.count = [] { return static_cast<int>(BagLines().size()); };
        rows.label = [](int i) {
            const auto all = BagLines();
            if (i >= static_cast<int>(all.size())) return std::string();
            const auto& l = all[i];
            const auto& r = towerapi::Run();
            if (l.shentong >= 0) return std::format("神通·{}　{}", tower::kShentong[r.shentong[l.shentong]].name, BagVerb(l));
            const bool armed = towerapi::Growth().prep.Armed(l.item);
            return std::format("{} ×{}{}　{}", tower::kItems[l.item].name, r.items[l.item], armed ? "（待生效）" : "", BagVerb(l));
        };
        rows.detail = [](int i) {
            const auto all = BagLines();
            if (i >= static_cast<int>(all.size())) return std::string();
            const auto& l = all[i];
            if (l.shentong >= 0) return std::string(tower::kShentong[towerapi::Run().shentong[l.shentong]].text) + "；作用于下一层";
            return std::string(tower::kItems[l.item].text);
        };
        rows.enabled = [](int i) {
            const auto all = BagLines();
            if (i >= static_cast<int>(all.size())) return false;
            const auto& l = all[i];
            if (l.shentong >= 0) return true;
            if (tower::rewards::ItemTiming(tower::kItems[l.item].item) == tower::rewards::Timing::Use) return towerapi::Run().items[l.item] > 0;
            if (tower::rewards::ItemTiming(tower::kItems[l.item].item) != tower::rewards::Timing::Prep) return false;
            return towerapi::Run().items[l.item] > 0 || towerapi::Growth().prep.Armed(l.item);
        };
        rows.act = [this](int i) {
            const auto all = BagLines();
            if (i >= static_cast<int>(all.size())) return;
            const auto& l = all[i];
            if (l.shentong >= 0) { Send("st", std::format(R"(,"slot":{})", l.shentong)); bagNote_ = "已使用神通：" + std::string(tower::kShentong[towerapi::Run().shentong[l.shentong]].name); return; }
            const auto& d = tower::kItems[l.item];
            if (tower::rewards::ItemTiming(d.item) == tower::rewards::Timing::Use) { Send("prep", std::format(R"(,"value":"use","item":"{}")", d.id)); bagNote_ = "已使用：" + std::string(d.name); return; }
            if (towerapi::Growth().prep.Armed(l.item)) { Send("prep", std::format(R"(,"value":"disarm","item":"{}")", d.id)); bagNote_ = "已撤销：" + std::string(d.name); return; }
            Send("prep", std::format(R"(,"value":"arm","item":"{}")", d.id));
            bagNote_ = "已准备：" + std::string(d.name);
        };
        rows.body = [this, total, page] {
            const auto& prep = towerapi::Growth().prep;
            std::string armed;
            for (int i : prep.armed) armed += (armed.empty() ? "" : "、") + std::string(tower::kItems[i].name);
            std::string b = "待生效：" + (armed.empty() ? std::string("无") : armed);
            if (!bagNote_.empty()) b += "\n" + bagNote_;
            if (!total) b += "\n没有可用的道具";
            std::string fx;
            const auto all = BagLines();
            for (int i = page * kRowsPerPage; i < std::min<int>((page + 1) * kRowsPerPage, static_cast<int>(all.size())); ++i) {
                const auto& l = all[i];
                if (l.shentong >= 0) { const auto& t = tower::kShentong[towerapi::Run().shentong[l.shentong]]; AddFx(fx, std::format("{}：{}", t.name, t.text)); }
                else AddFx(fx, std::format("{}：{}", tower::kItems[l.item].name, tower::kItems[l.item].text));
            }
            if (!fx.empty()) b += "\n\n" + fx;
            return b;
        };
        rows.alive = [] { return towerapi::Run().phase == tower::Phase::Ready && towerapi::Growth().on; };
        std::vector<Option> opts;
        if (page > 0) opts.push_back({"上一页", [this, page] { ShowBagRows(page - 1); }});
        if (page + 1 < pages) opts.push_back({"下一页", [this, page] { ShowBagRows(page + 1); }});
        opts.push_back({"返回", [this] { bagNote_.clear(); ShowUpgrade(); }});
        const auto body = rows.body();
        Show(pages > 1 ? std::format("MOD背包（{}/{}）", page + 1, pages) : std::string("MOD背包"), body, std::move(opts), std::move(rows));
    }
    std::string bagNote_;

    int hexListPage_ = 0;
    std::vector<int> ChosenHexes() const {
        std::vector<int> ids;
        for (int id : towerapi::Growth().hexes.picks)
            if (tower::hex::Playable(id) && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        for (int id : towerapi::Growth().hexes.extra)  // 3.0.1：15 / 20 层一个的额外栏位
            if (tower::hex::Playable(id) && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        return ids;
    }
    void ShowOwnedHex(int id) {
        if (!tower::hex::Playable(id)) { ShowMain(); return; }
        const auto& h = towerapi::Growth().hexes;
        const int rank = h.Rank(static_cast<tower::hex::Id>(id));
        std::string body(tower::hex::TextAt(id, rank));
        // 3.6.0 海克斯升级：等级只写在标题（名字 LvN）；不写最高等级，让玩家自己摸索（用户 2026-11-15）
        // 用户 2026-11-11：可叠层 / 有条件的海克斯，详情里写清楚叠了多少层、现在加了多少
        namespace hx = tower::hex;
        const int L = h.Levels(static_cast<hx::Id>(id));
        auto AttackProbe = [](const auto& st) -> std::string {  // 读回诊断：只在 QA 构建显示
#ifdef PBZ_HEX_TEST
            return std::format("\n原生攻击力：游戏当前读回 {:.0f}（影塔总倍率 ×{:.2f}）", st.liveAttack, st.liveAtkMul);
#else
            (void)st; return {};
#endif
        };
        auto stacked = [&](int n, double step, const char* what) { body += std::format("\n\n已叠加 {} 层：{} +{:g}%", n, what, n * step * 100); };
        switch (id) {
            case hx::Tank: stacked(L, h.TankStep(), "生命上限"); break;  // 3.5.0：跟生效值同源
            case hx::Swift: body += std::format("\n\n已叠加 {} 层：移动速度、攻击速度各 +{:g}%（缩小引擎本身最多 +{:g}%）", L, h.SwiftBonus() * 100, hx::kSwiftCap * 100); break;
            case hx::Icathia: stacked(L, .01, "伤害减免"); break;
            case hx::Battle: stacked(L, h.BattleStep(), "攻击"); break;
            case hx::BreakGrowth: stacked(L, h.BreakStep(), "破防"); break;
            case hx::Grit: body += std::format("\n\n本层已叠加 {} 次：伤害减免 +{}%（最多 {} 次）", h.gritStacks, h.gritStacks, h.GritCap()); break;
            case hx::Escape: body += h.escapeActive ? "\n\n本层已触发：移动速度 +80%" : "\n\n本层尚未触发"; break;
            case hx::SnowBall: body += std::format("\n\n已通关 {} 段：魂玉 +{:g}%", L, h.SnowBallBonus() * 100); break;
            case hx::Ascetic: stacked(h.asceticStacks, h.AsceticStep(), "生命上限"); break;
            case hx::Flawless: stacked(h.flawlessStacks, h.FlawlessStep(), "攻击"); break;
            // 3.2.1（用户：描述后面接了一坨「原生攻击力」话术）：读回诊断只在 QA 构建显示
            case hx::BackWater: body += std::format("\n\n当前：攻击力 +{:.0f}%", h.backWaterBonus * 100) + AttackProbe(h); break;
            case hx::TreasureBowl: body += std::format("\n\n满血通关额外 +{:g}% 魂玉，按通关时剩余生命比例递减", h.BowlRate() * 100); break;
            case hx::WarFeed: body += std::format("\n\n本层已击败 {} 个：攻击力 +{:.0f}%", h.warStacks, h.warStacks * h.WarFeedStep() * 100) + AttackProbe(h); break;
            case hx::Fury: body += h.furyActive ? "\n\n正在生效" : "\n\n未生效（生命低于 30% 时生效）"; break;
            case hx::Opening: body += h.openingActive ? "\n\n正在生效" : std::format("\n\n未生效（每层开战后 {:g} 秒）", h.OpeningSeconds()); break;
            case hx::Echo: body += (h.echoActive ? std::format("\n\n当前 {} 层，攻击力 +{:.0f}%，剩余 {:.1f} 秒", h.echoStacks, h.echoStacks * h.EchoStep() * 100, h.echoLeft) : std::string("\n\n未生效")) + AttackProbe(h); break;
            case hx::Interest: body += std::format("\n\n按现在的魂玉，下次驿站开张发 {}", hx::InterestOn(towerapi::Run().jade, tower::balance::InterestCap(towerapi::ShownNext()), h.InterestRate())); break;
            case hx::Charmsmith: { const int every = h.CharmEvery(), next = (towerapi::ShownNow() / every + 1) * every; body += std::format("\n\n下一次：通关第 {} 层（已有复活符时不给）", next); break; }
            case hx::Arsenal: {
                std::string names[2];
                for (int kind = 0; kind < 2; ++kind) for (int w : towerapi::ArsenalExtras(kind)) if (w >= 0) names[kind] += (names[kind].empty() ? "" : "、") + std::string(tower::growth::kWeapons[w].name);
                body += std::format("\n\n武器第 3、4 把：{}\n影之武装第 3、4 把：{}", names[0].empty() ? "空" : names[0], names[1].empty() ? "空" : names[1]);
                break;
            }
            default: break;
        }
        Show(tower::hex::NameAt(id, rank), body, {{"返回", [this] { ShowMain(); }}});
    }
    void ShowHexChoices() {
        const auto& h = towerapi::Growth().hexes;
        if (!h.pending) { ShowMain(); return; }
        std::vector<Option> opts;
        std::string body;
        for (int i = 0; i < 3; ++i) {
            const int id = h.offers[i];
            if (!tower::hex::Playable(id)) continue;
            const int lv = h.OfferLevel(id);  // 3.6.0：卡面写这张卡选中后的等级和该级效果
            const std::string name = tower::hex::NameAt(id, lv);
            body += std::format("{}：{}\n", name, tower::hex::TextAt(id, lv));
            const bool owned = !h.CanPick(static_cast<tower::hex::Id>(id), h.pending);
            opts.push_back({name + (owned ? "（已拥有）" : ""), [this, i, id, floor = h.pending] { Send("hex_pick", std::format(R"(,"slot":{},"hex_id":{},"hex_floor":{})", i, id, floor)); Reopen(); }});
        }
        Show(std::format("第 {} 层 · 选择海克斯", h.pending), body, std::move(opts));
    }
    void ShowHex() {
        const auto& h = towerapi::Growth().hexes;
        if (!h.pending) { ShowMain(); return; }
        if (rowsBroken_) { ShowHexChoices(); return; }
        Rows rows;
        rows.columns = 3; rows.perColumn = 2; rows.hexCards = true;
        rows.count = [] { return 6; };
        rows.card = [](int slot) {
            const auto& x = towerapi::Growth().hexes;
            const int id = x.offers[slot];
            if (!tower::hex::Playable(id)) return std::string();
            return tower::hex::NameAt(id, x.OfferLevel(id));  // 3.6.0「名字 LvN」
        };
        rows.cardDetail = [](int slot) {
            const auto& x = towerapi::Growth().hexes;
            const int id = x.offers[slot];
            return tower::hex::Playable(id) ? std::string(tower::hex::TextAt(id, x.OfferLevel(id))) : std::string();
        };
        rows.label = [](int i) {
            const auto& x = towerapi::Growth().hexes;
            if (i < 3) return (x.rerolled & (1u << i)) ? std::string() : std::string("刷新");
            return std::string("选择");
        };
        rows.enabled = [](int i) {
            const auto& x = towerapi::Growth().hexes; const int slot = i % 3, id = x.offers[slot];
            return id >= 0 && id < tower::hex::Count && (i < 3 || x.CanPick(static_cast<tower::hex::Id>(id), x.pending));
        };
        rows.alive = [] { return towerapi::Growth().hexes.pending != 0; };
        rows.closes = [](int i) { return i >= 3; };
        rows.act = [](int i) {
            if (i >= 3) return;
            const auto& x = towerapi::Growth().hexes;
            Send("hex_refresh", std::format(R"(,"slot":{},"hex_floor":{},"hex_rng":"{}")", i, x.pending, x.rng));
        };
        rows.prepare = [this](int i) -> std::function<void()> {
            if (i < 3) return {};
            const auto& x = towerapi::Growth().hexes; const int slot = i - 3, id = x.offers[slot], floor = x.pending;
            return [this, slot, id, floor] { Send("hex_pick", std::format(R"(,"slot":{},"hex_id":{},"hex_floor":{})", slot, id, floor)); Reopen(); };
        };
        std::vector<Option> opts{{"保存并退出", [this] { ShowSaveExitConfirm(); }}};
#ifdef PBZ_HEX_TEST
        Show(std::format("测试版 · 第 {} 层海克斯", h.pending), "", std::move(opts), std::move(rows));
#else
        Show(std::format("第 {} 层 · 海克斯", h.pending), "", std::move(opts), std::move(rows));
#endif
    }

    void ShowMain(bool allowLanguageFallback = true) {
        const auto& r = towerapi::Run();
        std::vector<Option> opts;
        std::string header = "影塔·百战", body;
        switch (r.phase) {
            case tower::Phase::Idle:
                if (towerapi::TripPhase() == static_cast<int>(tower::trip::Phase::Exiting)) {
                    body = towerapi::TripText();
                    if (towerapi::TripFailed()) opts.push_back({"重试", [] { Send("recover"); }});
                    opts.push_back({"知道了", [] {}});
                    break;
                }
                if (towerapi::TripPhase() == static_cast<int>(tower::trip::Phase::Stale)) {
                    body = "上次影塔未正常结束";
                    opts.push_back({"恢复原进度", [] { Send("recover"); }});
                    opts.push_back({"已是原进度", [] { Send("forget"); }});
                    break;
                }
                body = "";
                // v14：第 51 层入口解锁后固定在最左（失败 / 退出 / 重进存档都在）；确认框只有三个按钮，有入口时不再列试玩
                if (towerapi::HiddenUnlocked()) opts.push_back({"第 100 层", [] { Send("hidden"); }});
                if (towerapi::ClimbAvailable()) opts.push_back({"继续上一局", [] { Send("resume"); }});
                opts.push_back({"开始新局", [this] { ShowStarter(50); }});
                opts.push_back({"无尽模式", [this] { ShowEndlessMenu(); }});  // 3.5.0
                if (opts.size() < 3) opts.push_back({"试玩 3 层", [this] { ShowStarter(3); }});
                break;
            case tower::Phase::Ready: {
                if (towerapi::TripPhase() == static_cast<int>(tower::trip::Phase::Entering)) {
                    header = "影塔·百战 · 进入影塔";
                    body = towerapi::TripText();
                    if (towerapi::TripFailed()) opts.push_back({"重试", [] { Send("recover"); }});
                    opts.push_back({"知道了", [] {}});
                    opts.push_back({"取消，回到原进度", [] { Send("stop"); }});
                    break;
                }
                if (towerapi::Growth().on) {  // 原生成长：先领本层固定药 / 饰品匣，再驿站（开店层），再层间主面板；开打只由玩家点"下一层"
                    if (towerapi::Growth().hexes.pending) { ShowHex(); return; }
                    if (ShowGrowthChoice()) return;
                    if (towerapi::Growth().shop.Open() && !shopDismissed_) { ShowShop(0); return; }
                    const auto& g = towerapi::Growth();
                    if (r.floor == 50 && (r.endless || (!r.hidden && r.floors > 50))) {  // 通关 50 层：结算页 + 下一层（无尽模式从这里接着打）
                        header = "影塔·百战 · 通关";
                        const auto st = towerapi::Settlement();
                        body = st.empty() ? Summary(r) : tower::menutext::CompactSettlement(st);
                        opts.push_back({"下一层", [this] { SendNext(); }});
                        opts.push_back({"整备", [this] { ShowUpgrade(); }});
                        opts.push_back({"退出影塔", [] { Send("finish"); Send("stop"); }});
                        break;
                    }
                    header = std::format("影塔·百战　第 {} 层（{}）", towerapi::ShownNow(), tower::floorplay::DifficultyName(r.difficulty));
                    body = std::format("魂玉 {}\n{}\n淬炼点 {}\n修为点 {}\n铸魂令 {}", r.jade, MainStats(), g.profile.temper, g.profile.cultivate, g.profile.soulTokens);
                    if (const auto next = towerapi::NextBossPreview(); !next.empty()) body = next + "\n" + body;
                    if (const auto note = towerapi::FloorNote(); !note.empty()) body = note + "\n" + body;
                    opts.push_back({"下一层", [this] { if (SendNext()) Then([this] { if (towerapi::Run().phase == tower::Phase::Ready) ShowMain(); }); }});
                    if (towerapi::FailedStageAvailable()) {
                        opts.push_back({"整备", [this] { ShowUpgrade(); }});
                        opts.push_back({"保存并退出", [this] { ShowSaveExitConfirm(); }});
                        opts.push_back({"本层问题", [this] { ShowSkipConfirm(); }});
                        break;
                    }
                    if (rowsBroken_ && g.shop.Open()) opts.push_back({"整备 / 驿站", [this] { Show("整备 / 驿站", "", {{"整备", [this] { ShowUpgrade(); }}, {"进入驿站", [this] { ShowShop(0); }}, {"返回", [this] { ShowMain(); }}}); }});
                    else opts.push_back({"整备", [this] { ShowUpgrade(); }});
                    opts.push_back({"保存并退出", [this] { ShowSaveExitConfirm(); }});
                    break;
                }
                // 每层开打前自动弹出：属性、背包一览，有能用的东西就提示先用
                header = std::format("影塔·百战 · 准备第 {} 层", towerapi::ShownNext());
                body = Summary(r);
                const size_t usable = BagEntries(r).size();
                opts.push_back({std::format("开第 {} 层", towerapi::ShownNext()), [this] { SendNext(); }});
                opts.push_back({usable ? std::format("背包（{} 项可用）", usable) : std::string("背包"), [this] { ShowBag(0); }});
                opts.push_back({"属性 / 离开…", [this] { ShowMore(); }});
                break;
            }
            case tower::Phase::Fighting:
                header = std::format("影塔·百战 · 第 {} 层", towerapi::ShownNow());
                if (towerapi::Growth().on) {
                    body = towerentry::Holding() ? "入场准备中" : std::format("魂玉 {}", r.jade);
                    opts.push_back({"继续战斗", [] {}});
                    // 313987b 实测战斗中压原生角色页失败（"原生界面资产或 CommonUI 接口不可用"）：战斗中只看塔自己的信息页，不压原生菜单
                    opts.push_back({"信息", [this] { ShowFightInfo(); }});
                    opts.push_back({"保存并退出", [this] { ShowSaveExitConfirm(); }});
                    opts.push_back({"本层问题", [this] { ShowSkipConfirm(); }});
                    break;
                }
                body = Summary(r);
                opts.push_back({"背包", [this] { ShowBag(0); }});
                opts.push_back({"Boss 有问题", [this] { ShowBadBoss(); }});
                opts.push_back({"属性 / 离开…", [this] { ShowMore(); }});
                break;
            case tower::Phase::Reward:
                pbz::log::Info("影塔奖励显示：floor={} wall_ms={}", r.floor, GetTickCount64());
                // 原生获取界面（Boss 掉落 / 挑战奖励）还开着：等它关掉再弹领奖面板，不互相覆盖
                if (towerapi::Growth().on && towerwidgets::NativeModal()) { pendingShow_ = GetTickCount64() + 600; return; }
                header = towerapi::Growth().on && towerapi::Growth().hexRewardSecond ? std::format("投掷狂人 · 第 {} 层奖励第 {} 次", towerapi::ShownNow(), towerapi::Growth().hexRewardSecond + 1)
                                                                                      : std::format("第 {} 层通过", towerapi::ShownNow());
                body = std::format("本层收获：魂玉 +{}", r.last.jade);
                if (r.floor >= 1 && r.floor <= static_cast<int>(r.times.size()) && r.times[r.floor - 1] > 0)
                    body = std::format("本层用时 {}\n", tower::floorplay::Clock(r.times[r.floor - 1] / 10.0)) + body;
                if (const auto bt = towerapi::BonusText(); !bt.empty()) body += "\n" + bt;
                if (const auto gains = towerapi::Gains(); towerapi::Growth().on && !gains.empty()) body += "\n" + gains;
                if (towerapi::Growth().on && towerapi::Growth().refresh && towerapi::Growth().hexes.Has(tower::hex::Fortune)) body += "\n天赐良机：本页可免费刷新 1 次";
                for (int w : r.last.weapons) body += std::format("\n兵器：{}", tower::kWeaponDrops[w].name);
                for (int i : r.last.shentong) body += std::format("\n神通：{}\n　　{}", tower::kShentong[i].name, tower::kShentong[i].text);
                // 累计加成只放信息页，奖励页保留本层收获和三个选项。
                // 刷新令：对话框只显示三个按钮，不能做第四个。有刷新令时先问一步：看奖励 / 重抽（阶段不变，刷完自己再弹一次）
                if (const int n = r.items[static_cast<int>(tower::Item::Reroll)]; !towerapi::Growth().on && n > 0 && !rewardChoosing_) {
                    body += "\n\n本次三选一：" + RewardEntries(r);
                    opts.push_back({"选择奖励", [this] { rewardChoosing_ = true; ShowMain(); }});
                    opts.push_back({std::format("刷新令×{}：重抽", n), [this] { Send("reroll"); Reopen(); }});
                    break;
                }
                rewardChoosing_ = false;
                if (!towerapi::Growth().on) body += "\n" + RewardEntries(r);
                else if (towerapi::Growth().offered) {  // 成长模式：名字在按钮上；道具 / 药 / 饰品在正文下面列效果
                    std::string fx;
                    for (int i = 0; i < 3; ++i) AddFx(fx, FxLine(towerapi::Growth().offers[i]));
                    if (!fx.empty()) body += "\n\n" + fx;
                }
                for (int i = 0; i < 3; ++i)
                    if (const auto label = RewardLabel(r, i); !label.empty())
                        opts.push_back({label, [this, i] {
                            const auto& g = towerapi::Growth();
                            const bool more = g.on && g.hexRewardSecond < g.hexes.ThrowerExtraPages();
                            Send("pick", std::format(R"(,"slot":{})", i));
                            if (more) Reopen();
                        }});
                // v14：刷新令出现时作为正文下面的一行原生按钮（底部三个按钮留给三个奖励）
                if (towerapi::Growth().on && towerapi::Growth().refresh && !rowsBroken_) {
                    Rows rows;
                    rows.count = [] { return towerapi::Growth().refresh ? 1 : 0; };
                    rows.label = [](int) { return std::string("使用刷新令"); };
                    rows.closes = [](int) { return true; };
                    rows.act = [this](int) { Send("reroll"); Reopen(); };
                    rows.alive = [] { return towerapi::Run().phase == tower::Phase::Reward; };
                    Show(header, body, std::move(opts), std::move(rows));
                    return;
                }
                break;
            case tower::Phase::Won:
            case tower::Phase::Lost:
                header = r.phase == tower::Phase::Won ? (r.hidden || r.floor > 50 ? std::string("影塔·百战 · 登临绝顶") : std::string("影塔·百战 · 通关")) : std::format("影塔·百战 · 止步第 {} 层", towerapi::ShownNow());
                if (const auto st = towerapi::Settlement(); !st.empty()) {  // 独立塔档：结算快照（死亡 / 通关那一刻），点退出才出塔恢复原档
                    if (r.phase == tower::Phase::Lost && towerapi::RebirthOffered()) {  // 1.0.1：有重生符先问
                        header = std::format("影塔·百战 · 殒命　第 {} 层", towerapi::ShownNow());
                        const int cp = towerapi::CheckpointShown();
                        body.clear();
                        if (towerapi::RebirthCount() > 0)
                            body += std::format("持有重生符 ×{}\n使用一个：回到第 {} 层的准备页继续（{}）\n", towerapi::RebirthCount(), towerapi::RebirthNext(),
                                                towerapi::ShownNow() > 11 ? "退后 10 层" : "倒在第 11 层以内，从第 1 层开始");
                        if (cp > 0) body += std::format("从第 {} 层开始：整局回到第 {} 层开打前（武器、点数、海克斯、魂玉、道具都回到那一刻）\n", cp, cp);
                        body += "退出：本局结束，进入结算";
                        if (towerapi::RebirthCount() > 0) opts.push_back({"使用重生符", [] { Send("rebirth"); }});
                        if (cp > 0) opts.push_back({std::format("从第 {} 层开始", cp), [] { Send("checkpoint"); }});
                        opts.push_back({"退出", [this] { Send("rebirth_no"); pendingShow_ = GetTickCount64() + 900; }});
                        break;
                    }
                    if (r.phase == tower::Phase::Lost) {  // 肉鸽一条命（用户 2026-11-05）：倒下本局结束；重新开始 = 出塔回原档后自动开新局
                        body = st;
                        opts.push_back({"重新开始", [] { Send("restart"); }});
                        opts.push_back({"退出影塔", [] { Send("stop"); }});
                        break;
                    }
                    body = tower::menutext::CompactSettlement(st);
                    opts.push_back({"退出影塔", [] { Send("stop"); }});
                    break;
                }
                body = Summary(r);
                opts.push_back({"再来一局", [this] { ShowStarter(50); }});
                opts.push_back({"试玩 3 层", [this] { ShowStarter(3); }});
                opts.push_back({"退出影塔", [] { Send("stop"); }});  // 回到正常游戏：属性还原，兵器换回
                break;
        }
        // Put a language control before the main choices without taking one of
        // the three native confirmation buttons or changing their callbacks.
        // Reward/settlement screens retain their existing choices and layout.
        const bool canChangeLanguage = r.phase == tower::Phase::Idle || r.phase == tower::Phase::Ready || r.phase == tower::Phase::Fighting;
        if (rowsBroken_ && allowLanguageFallback && canChangeLanguage) {
            Show("语言 / Language", "", {
                {tower::language::Current() == tower::language::Language::English ? "Language: English → 中文" : "语言：中文 → English", [this] {
                    const auto next = tower::language::Current() == tower::language::Language::Chinese
                                          ? tower::language::Language::English : tower::language::Language::Chinese;
                    if (!tower::language::Set(next)) {
                        Show("语言 / Language", "语言设置保存失败 / Could not save language preference.", {{"返回", [this] { ShowMain(); }}});
                    } else ShowMain();
                }},
                {"继续 / Continue", [this] { ShowMain(false); }}
            });
        } else if (!rowsBroken_ && canChangeLanguage && towerapi::Growth().on &&
                   (r.phase == tower::Phase::Ready || r.phase == tower::Phase::Fighting) && !ChosenHexes().empty()) {
            // Two rows of native Hex buttons below two columns of statistics.
            const auto phase = r.phase;
            const bool canReturnToShop = phase == tower::Phase::Ready && towerapi::Growth().shop.Open() && shopDismissed_;
            Rows rows; rows.statsGrid = true; rows.columns = 1; rows.perColumn = 9;
            // 0–4 海克斯（固定 5 格）｜5 上一页 6 下一页（只有超过 5 个才出现，留给无尽模式）｜7 语言 8 驿站
            rows.count = [] { return 9; };
            const auto pagesOf = [this] { return std::max(1, static_cast<int>((ChosenHexes().size() + 4) / 5)); };
            rows.label = [this, canReturnToShop, pagesOf](int i) {
                const auto ids = ChosenHexes(); const int pages = pagesOf();
                hexListPage_ = std::clamp(hexListPage_, 0, pages - 1);
                if (i < 5) { const int n = hexListPage_ * 5 + i; return n < static_cast<int>(ids.size()) ? std::string(tower::hex::defs[ids[n]].name) : std::string(); }
                if (i == 5) return pages > 1 ? std::string("上一页") : std::string();
                if (i == 6) return pages > 1 ? std::format("下一页 {}/{}", hexListPage_ + 1, pages) : std::string();
                if (i == 8) return canReturnToShop ? std::string("进入驿站") : std::string();
                return tower::language::Current() == tower::language::Language::English ? std::string("Language / 语言: English") : std::string("语言 / Language：中文");
            };
            rows.enabled = [this, pagesOf](int i) {
                if (i == 5) return hexListPage_ > 0;
                if (i == 6) return hexListPage_ + 1 < pagesOf();
                return i >= 7 || hexListPage_ * 5 + i < static_cast<int>(ChosenHexes().size());
            };
            rows.closes = [](int i) { return i != 5 && i != 6; };
            rows.act = [this, pagesOf](int i) {
                if (i == 5) hexListPage_ = std::max(0, hexListPage_ - 1);
                if (i == 6) hexListPage_ = std::min(pagesOf() - 1, hexListPage_ + 1);
            };
            rows.prepare = [this](int i) -> std::function<void()> {
                if (i < 5) { const auto ids = ChosenHexes(); const int n = hexListPage_ * 5 + i; if (n >= static_cast<int>(ids.size())) return {}; const int id = ids[n]; return [this, id] { ShowOwnedHex(id); }; }
                if (i == 7) return [this] {
                    const auto next = tower::language::Current() == tower::language::Language::Chinese ? tower::language::Language::English : tower::language::Language::Chinese;
                    if (!tower::language::Set(next)) Show("语言 / Language", "语言设置保存失败 / Could not save language preference.", {{"返回", [this] { ShowMain(); }}});
                    else ShowMain();
                };
                if (i == 8) return [this] { ShowShop(0); };
                return {};
            };
            rows.alive = [phase] { return towerapi::Run().phase == phase; };
            Show(header, body, std::move(opts), std::move(rows));
        } else if (!rowsBroken_ && canChangeLanguage) {
            const auto phase = r.phase;
            Rows rows;
            const bool canReturnToShop = phase == tower::Phase::Ready && towerapi::Growth().on && towerapi::Growth().shop.Open() && shopDismissed_;
            rows.count = [canReturnToShop] { return canReturnToShop ? 2 : 1; };
            rows.label = [canReturnToShop](int i) { if (canReturnToShop && i == 1) return std::string("进入驿站"); return tower::language::Current() == tower::language::Language::English
                                               ? std::string("Language / 语言: English") : std::string("语言 / Language：中文"); };
            rows.closes = [](int) { return true; };
            rows.act = [this, canReturnToShop](int i) {
                if (canReturnToShop && i == 1) { ShowShop(0); return; }
                const auto next = tower::language::Current() == tower::language::Language::Chinese
                                      ? tower::language::Language::English : tower::language::Language::Chinese;
                if (!tower::language::Set(next)) {
                    Show("语言 / Language", "语言设置保存失败 / Could not save language preference.",
                         {{"返回", [this] { ShowMain(); }}});
                } else ShowMain();
            };
            rows.alive = [phase] { return towerapi::Run().phase == phase; };
            Show(header, body, std::move(opts), std::move(rows));
        } else Show(header, body, std::move(opts));
    }

    // 开局（用户 2026-11-03）：原游戏新档起手，没有选项——黑伤 + 黑伤双刃、羽弓、药葫芦 + 凝锋丹，生命 / 杀气为原游戏初值
    static std::string WName(std::string_view id, int kind) { const int w = tower::growth::WeaponIndex(id, kind); return w >= 0 ? std::string(tower::growth::kWeapons[w].name) : std::string(id); }
    static std::string MName(std::string_view id) { const int m = tower::growth::MedicineIndex(id); return m >= 0 ? std::string(tower::growth::kMedicines[m].name) : std::string(id); }
    // ---- 3.5.0 无尽模式入口 ----
    // 第 99 层打完、这局还不是无尽存档：先确认（无尽存档只有一个，会覆盖旧的 / 导入的），再开第 100 层；其它情况照常下一层
    bool SendNext() {
        const auto& r = towerapi::Run();
        if (towerapi::Growth().on && r.phase == tower::Phase::Ready && r.endless && !r.endlessSave && r.floor == 50) { ShowEndlessCommit(); return false; }
        Send("next");
        return true;
    }
    void ShowEndlessCommit() {
        std::string body = "进入无尽模式后，这一局会成为你唯一的无尽存档：武器、海克斯、属性、魂玉、道具全部保留，之后的 Boss、海克斯和驿站照常随机。";
        if (towerapi::EndlessNext() > 0) body += std::format("\n\n你已有一个无尽存档（下一层第 {} 层），会被这一局覆盖。", towerapi::EndlessNext());
        std::string author; int shown = 0, diff = 0;
        if (towerapi::EndlessImport(&author, &shown, &diff)) body += "\n\n已导入但还没使用的无尽档也会被清除。";
        body += "\n\n之后在开局菜单选「开始新局」，就是重新开一局 100 层爬塔；选「无尽模式」继续这个无尽存档。";
        std::vector<Option> opts;
        opts.push_back({"进入无尽模式", [] { Send("endless_commit"); }});
        opts.push_back({"返回", [this] { ShowMain(); }});
        Show("影塔·百战 · 进入无尽模式", body, std::move(opts));
    }
    void ShowEndlessMenu() {
        std::string author; int shown = 0, diff = 0;
        const bool imported = towerapi::EndlessImport(&author, &shown, &diff);
        const int next = towerapi::EndlessNext();
        std::vector<Option> opts;
        std::string body;
        if (next > 0) {
            body = std::format("无尽存档：下一层第 {} 层。", next);
            opts.push_back({std::format("继续无尽（第 {} 层）", next), [] { Send("endless_resume"); }});
        }
        if (imported) {
            body += std::format("{}已导入无尽档：{}，第 {} 层起，{}。", body.empty() ? "" : "\n\n", author.empty() ? "佚名" : author, shown, diff ? "简单" : "困难");
            opts.push_back({"用导入的无尽档开局", [this, next] {
                if (next <= 0) { Send("endless_import"); return; }
                Show("影塔·百战 · 无尽模式", std::format("用导入的无尽档开局，会覆盖你现在的无尽存档（下一层第 {} 层），不能恢复。", next),
                     {{"确定覆盖并开局", [] { Send("endless_import"); }}, {"返回", [this] { ShowEndlessMenu(); }}});
            }});
        }
        if (opts.empty()) {
            Show("影塔·百战 · 无尽模式", "暂未开启无尽模式。\n\n打完第 99 层后进入无尽模式，或在安装器的「无尽档」里导入别人的无尽档，就能在这里开始。", {{"知道了", [this] { ShowMain(); }}});
            return;
        }
        opts.push_back({"返回", [this] { ShowMain(); }});
        Show("影塔·百战 · 无尽模式", body, std::move(opts));
    }
    void ShowStarter(int floors, bool offerImported = true) {
        std::string mains, shaqi, meds;
        for (auto id : tower::growth::kStarterKitMain) mains += (mains.empty() ? "" : "、") + WName(id, 0);
        for (auto id : tower::growth::kStarterKitShaqi) shaqi += (shaqi.empty() ? "" : "、") + WName(id, 1);
        for (auto id : tower::growth::kStarterKitMeds) meds += (meds.empty() ? "" : "、") + MName(id);
        std::string body = std::format("主兵器：{}\n影之武装：{}\n药：{}", mains, shaqi, meds);
        std::vector<Option> opts;
        uint64_t imported = 0; int diff = 0;
        const int seedState = floors >= 50 && offerImported && towerapi::TripPhase() == static_cast<int>(tower::trip::Phase::Outside) ? towerapi::ImportedSeed(&imported, &diff) : 0;
        if (seedState == 1) {
            body += std::format("\n\n已导入种子 {}（{}）。从第 1 层开始，旧塔档将备份。", imported, diff ? "简单" : "困难");
            opts.push_back({"开始种子局", [] { Send("start", R"(,"floors":50,"seed_import":"1")"); }});
            opts.push_back({"普通开局", [this, floors] { ShowStarter(floors, false); }});
        } else if (floors >= 50) {  // 普通新局仍保留两种难度
            if (seedState < 0) body += "\n\n导入的种子无效或版本不一致，请重新导入。";
            opts.push_back({std::format("{} 层（困难）", tower::floorplay::ShownTotal(floors)), [floors] { Send("start", std::format(R"(,"floors":{},"difficulty":"hard")", floors)); }});
            opts.push_back({std::format("{} 层（简单）", tower::floorplay::ShownTotal(floors)), [floors] { Send("start", std::format(R"(,"floors":{},"difficulty":"easy")", floors)); }});
        } else {
            opts.push_back({std::format("进入影塔 {} 层", tower::floorplay::ShownTotal(floors)), [floors] { Send("start", std::format(R"(,"floors":{})", floors)); }});
        }
        opts.push_back({"返回", [this] { ShowMain(); }});
        Show("影塔·百战 · 起手", body, std::move(opts));
    }

    void ShowBadBoss() {
        std::vector<Option> opts;
        opts.push_back({"确认：作废本层并禁用这个 Boss", [] { Send("badboss"); }});
        opts.push_back({"返回", [this] { ShowMain(); }});
        Show("影塔·百战 · Boss 有问题", "", std::move(opts));
    }

    void ShowMore() {
        std::vector<Option> opts;
        opts.push_back({"属性", [this] { ShowStats(); }});
        opts.push_back({"离开影塔…", [this] { ShowLeave(); }});
        opts.push_back({"返回", [this] { ShowMain(); }});
        Show("影塔·百战", "", std::move(opts));
    }

    // 驿站：对话框只有三个按钮——每页两件货 + 下一页；最后一页 刷新货架 / 离开 / 回第一页。
    // 买 / 刷新都带货架版本号：页面过期（别处已买 / 已刷新）时不会成交
    void ShowShop(int page) {
        const auto& g = towerapi::Growth();
        const auto& s = g.shop;
        if (!s.Open()) { ShowMain(); return; }
        // v14 用户：驿站和原生确认框一个样子（确认框 + 一列原生按钮）。列表插不进去才退回下面的分页确认框驿站
        if (!rowsBroken_) { ShowShopRows(page); return; }
        const auto& r = towerapi::Run();
        std::string body = std::format("魂玉 {}\n", r.jade);
        if (!shopNote_.empty()) body += shopNote_ + "\n";
        // 空货位一律隐藏，按真实商品重排计数（32ffb7d 用户：不能"货架 6 件"里夹"暂无合适的货"）；全空才显示一次空状态
        std::vector<int> goods;
        for (int i = 0; i < static_cast<int>(s.shelf.size()); ++i) if (!tower::rewards::Empty(s.shelf[i].offer)) goods.push_back(i);
        const int shelfPages = (static_cast<int>(goods.size()) + 1) / 2;
        if (goods.empty()) body += "\n暂无商品\n";
        else if (page < shelfPages) body += std::format("商品 {} 件，第 {}/{} 页\n", goods.size(), page + 1, shelfPages);
        else body += std::format("刷新货架换一批货，或离开驿站\n");
        for (size_t k = 0; k < goods.size(); ++k) {
            if (page >= shelfPages || static_cast<int>(k) / 2 != page) continue;
            const auto& w = s.shelf[goods[k]];
            body += std::format("\n{}. {}　{} 魂玉{}", k + 1, tower::rewards::Name(w.offer), w.price, w.sold ? "（已售）" : "");
        }
        std::vector<Option> opts;
        const int pages = shelfPages;
        const uint32_t ver = s.version;
        if (page < pages) {
            for (int k = page * 2; k < page * 2 + 2 && k < static_cast<int>(goods.size()); ++k) {
                const int i = goods[k];  // 后台真实货位（显示重排不影响成交对象）
                const auto& w = s.shelf[i];
                if (w.sold) continue;
                const auto q = tower::shoppanel::Capture(s, r.id, i);
                if (!q) continue;
                opts.push_back({std::format("买 {}（{}）", tower::rewards::Name(w.offer), w.price),
                                [this, q = *q, page] { ShowShopConfirm(q, page, true); }});
            }
            opts.push_back({page + 1 < pages ? "下一页" : "刷新 / 离开…", [this, page] { ShowShop(page + 1); }});
        } else {
            const int cost = tower::rewards::RefreshCost(s.refreshes, s.floor);
            opts.push_back({std::format("刷新货架（{} 魂玉）", cost), [this, ver, cost] {
                const int before = towerapi::Run().jade;
                Send("shop", std::format(R"(,"value":"refresh","ver":{})", ver));
                Then([this, cost, before] { ShopResult("刷新货架", cost, before); ShowShop(0); }); }});
            opts.push_back({"离开驿站", [this] { shopNote_.clear(); shopDismissed_ = true; Send("shop", R"(,"value":"leave")"); Reopen(); }});
            opts.push_back({"回第一页", [this] { ShowShop(0); }});
        }
        Show(std::format("影塔·百战 · 驿站（第 {} 页）", std::min(page, pages) + 1), body, std::move(opts));
    }

    // 驿站成交结果：按魂玉实际读回判断（扣了 = 成功；没扣 = 魂玉不足或货架已变，原因写明）
    void ShopResult(const std::string& what, int price, int before) {
        const int now = towerapi::Run().jade;
        if (now == before - price) shopNote_ = std::format("【成功】{}，花费 {} 魂玉", what, price);
        else if (before < price) shopNote_ = std::format("【未成交】魂玉不足：{} 需要 {}，现有 {}", what, price, before);
        else shopNote_ = std::format("【未成交】{}：货架已变或已售出，没有扣魂玉", what);
    }
    std::string shopNote_;
    // 属性：实时数值、本局加成明细、生效中的效果、下一层（战斗中为本层）Boss 强度
    void ShowStats() {
        if (towerapi::Growth().on) { OpenNative(towerwidgets::Panel::Character); return; }
        const auto& r = towerapi::Run();
        std::string body = Live();
        if (body.empty()) body = "（读不到角色属性）";
        body += "\n" + ModsLine(r.mods);
        if (r.mods.dealt) body += std::format("\n兵器淬炼：攻击 {:.0f} → {:.0f}，杀气攻击 {:.0f} → {:.0f}", r.standard.attack, r.standard.attack * r.mods.DealtMul(),
                                              r.standard.shachiAttack, r.standard.shachiAttack * r.mods.DealtMul());
        std::vector<std::string> cards;
        for (int i : r.picked) cards.push_back(std::format("{}：{}", tower::kCards[i].name, tower::kCards[i].text));
        body += "\n已选：" + Join(cards);
        if (const auto fx = towerapi::Effects(); !fx.empty()) body += "\n本层生效：" + fx;
        std::vector<std::string> worn;
        if (r.wearMain >= 0) worn.push_back(std::string(tower::kWeaponDrops[r.wearMain].name));
        if (r.wearShaqi >= 0) worn.push_back(std::string(tower::kWeaponDrops[r.wearShaqi].name));
        body += "\n塔内兵器：" + (worn.empty() ? std::string("未换") : Join(worn));
        tower::Run next = r;  // 复制一份算 Boss 强度（含加成补偿与自适应），不动真正的局
        if (r.phase == tower::Phase::Ready) ++next.floor;
        const int count = tower::BossCount(std::max(1, next.floor), next.floors);
        body += std::format("\n{} Boss：血量约 {:.0f}{}，伤害 ×{:.2f}（难度系数 {:.2f}）", r.phase == tower::Phase::Ready ? "下一层" : "本层",
                            next.BossHpTarget(count), count > 1 ? std::format(" ×{} 只", count) : "", next.BossDmg(count), r.adapt);
        std::vector<Option> opts;
        opts.push_back({"返回", [this] { ShowMain(); }});
        Show(std::format("影塔·百战 · 属性（第 {}/{} 层）", towerapi::ShownNow(), tower::floorplay::ShownTotal(r.floors)), body, std::move(opts));
    }

    // 离开：暂离会保存，下次从钟或 Insert 选"继续上一局"；放弃则本局作废
    void ShowLeave() {
        const auto& r = towerapi::Run();
        const bool fighting = r.phase == tower::Phase::Fighting;
        std::vector<Option> opts;
        opts.push_back({fighting ? "暂离，本层重来" : "暂离", [this] { ShowSaveExitConfirm(); }});
        opts.push_back({"放弃本局，退出影塔", [] { Send("stop"); }});
        opts.push_back({"返回", [this] { ShowMain(); }});
        Show("影塔·百战 · 离开", "", std::move(opts));
    }

    // 背包：每页两项，第三个按钮是"下一页 / 返回"
    void ShowBag(int page) {
        const auto& r = towerapi::Run();
        // 成长模式准备阶段：原生确认框 + 一列原生按钮；列表插不进去才退回下面的分页确认框背包
        if (towerapi::Growth().on && r.phase == tower::Phase::Ready && !rowsBroken_) { ShowBagRows(page); return; }
        auto all = BagEntries(r);
        std::vector<Option> opts;
        const size_t from = static_cast<size_t>(page) * 2;
        for (size_t k = from; k < all.size() && k < from + 2; ++k) opts.push_back({all[k].label, all[k].act});
        const bool growth = towerapi::Growth().on && r.phase == tower::Phase::Ready;
        if (from + 2 < all.size()) opts.push_back({"下一页", [this, page] { ShowBag(page + 1); }});
        else opts.push_back({growth ? "返回整备" : "返回", [this, growth] { if (growth) ShowUpgrade(); else ShowMain(); }});
        std::string body = BagText(r);
        for (size_t k = from; k < all.size() && k < from + 2; ++k)
            if (!all[k].detail.empty()) body += "\n\n" + all[k].label + "\n　　" + all[k].detail;
        std::vector<std::string> later;
        for (int i = 0; i < tower::kItemCount; ++i) {
            const auto& d = tower::kItems[i];
            if (r.items[i] <= 0) continue;
            if (d.item == tower::Item::Revive) later.push_back(std::string(d.name) + "：倒下时自动生效");
            else if (d.item == tower::Item::Reroll) later.push_back(std::string(d.name) + "：三选一时使用");

        }
        if (!later.empty()) body += "\n\n" + Join(later);
        Show(std::format("影塔·百战 · {}{}", growth ? "Mod 背包" : "背包", page ? std::format("（第 {} 页）", page + 1) : ""), body, std::move(opts));
    }

    // ---- 原生对话框 ----
    UObject* TextCdo() {
        return text_.Get([this] { return Cdo(textLib_); }, [this](UObject* o) { return ue::Live(o) && ue::ClassOf(o) == textLib_; });
    }
    // 文本库默认对象按需取（首次打开钟菜单时还没弹过对话框）；取不到就返回 false，调用方放弃本次
    bool MakeText(const std::string& s, uint8_t* out) {
        UObject* cdo = TextCdo();
        if (!cdo || !toText_) { pbz::log::Info("影塔界面：文本库不可用，跳过（默认对象 {} 函数 {}）", !!cdo, !!toText_); return false; }
        std::wstring w = Wide(tower::language::Display(s, tower::language::Current()));
        Args a(toText_);
        struct FStr { const wchar_t* data; int32_t num, max; } str{w.c_str(), static_cast<int32_t>(w.size() + 1), static_cast<int32_t>(w.size() + 1)};
        std::memcpy(a.At("InString"), &str, sizeof str);
        pe::Call(cdo, toText_, a.buf.data());
        std::memcpy(out, a.At("ReturnValue"), kTextSize);  // 所有权转给 out（参数缓冲不析构）
        return true;
    }
    const attr::Target* Player() {
        for (auto& t : attr::Targets())
            if (t.who == attr::Who::Player && attr::Alive(t)) return &t;
        return nullptr;
    }
    UObject* PlayerPawn() {
        for (auto& t : attr::Targets())
            if (t.who == attr::Who::Player && attr::Alive(t)) return t.obj;
        return nullptr;
    }

    // ---- 原生确认框里的一列原生按钮（整备 / 驿站 / 背包 / 刷新令）----
    // 对话框弹出后，把正文（RichText_Description）就地换成一个竖排容器：[正文, 按钮列]，容器沿用正文原来的槽位设置；
    // 按钮是对话框自己按钮的同一个类（WidgetBlueprintLibrary.Create，和钟菜单加按钮同一做法），加进对话框的按钮组（悬停 / 手柄导航）。
    // 点击走同一个 HandleButtonClicked 拦截。插不进去（2.5 秒内）就记下 rowsBroken_，之后退回分页的三按钮确认框
    struct Rows {
        int first = 0;                                   // 本页第一行的序号
        std::function<int()> count;                      // 全部行数
        std::function<std::string(int)> label, detail;   // 按钮文字 / 悬停时正文下面的说明
        std::function<bool(int)> enabled, closes;        // closes：点了先关对话框，关完再执行
        std::function<void(int)> act;
        std::function<std::function<void()>(int)> prepare; // snapshot before closing; execute after animation
        std::function<std::string()> body;               // 正文（就地刷新）；空 = 用弹出时的正文
        std::function<bool()> alive;                     // false = 自动关
        int columns = 1;                                 // 每行几列
        int perColumn = kRowsPerPage;                     // 每列可见行数
        std::vector<std::string> tabs;                   // 独立行中的分类；空 = 没有
        int tab = -1;
        int focusIndex = -1;                             // optional logical row to restore for controller
        std::function<void(int)> onTab;                  // 点分类：就地换内容，不重开窗口
        bool hexCards = false;
        bool sideBySide = false;                        // 原生正文在左、海克斯名称在右
        bool statsGrid = false;                         // 双列属性，下方一行海克斯按钮
        std::function<std::string(int)> card;
        std::function<std::string(int)> cardDetail;
        // 3.1.0 驿站交易：每行左侧原生图标（贴图资产路径，空 = 不显示）；noCollapse：内容少也保持多列（左右两栏对照）
        std::function<std::string(int)> icon;
        bool noCollapse = false;
        bool iconAbove = false;                          // N9g：图标在上、文字在下（交易页）
        bool pagerPair = false;                          // N9h：每列最后两格并成一行（＜ ＞ 小箭头）
        int splitAt = 0;                                 // N9g：从第几列起是右边一块（熔铸），前面留缝
        bool forgeGrid = false;                          // 3.6.1 批量锻造：3 列 × 6 行（5 组 + 翻页 / 确定）
        std::function<bool(int)> lit;                    // 3.6.1：这一格常亮原生选中底图（已选的属性）
        bool numberInput = false;                        // 3.6.1：按钮上方放一个数字输入框（锻造数量）
        std::function<std::string()> inputText;          // 输入框应显示的数字
        std::function<void(const std::string&)> onInput; // 玩家在输入框里打了字
    };
    UObject* rowInput_ = nullptr;
    std::string rowInputSeen_, rowInputWant_;
    uint64_t rowInputAt_ = 0;
    std::vector<uint8_t> rowLit_;                        // 3.6.1：上一帧常亮的格子
    std::vector<UObject*> rowIcons_;                   // 3.1.0：与内容按钮一一对应的图标（没有 = nullptr）
    std::unordered_map<std::string, UObject*> iconTex_;
    // 4.1.0（用户：驿站图标有时是白块 / 绿块）：图标贴图按需加载、只存指针，没有任何东西持有它。驿站关掉后贴图被 GC 回收，
    // 引擎会把同一地址、同一对象表槽位分给别的对象——Live() 照样通过，下次开驿站把别的对象当贴图塞进 Image
    // （非贴图 = 白块，别的贴图 = 色块）。现在取缓存前核对名字和类，不对就重新加载；加载结果也核对一遍
    static bool IsIconTexture(UObject* o, const std::string& path) {
        if (!o || !ue::Live(o)) return false;
        const size_t dot = path.rfind('.'), slash = path.rfind('/');
        const std::string want = dot != std::string::npos && (slash == std::string::npos || dot > slash) ? path.substr(dot + 1) : path.substr(slash == std::string::npos ? 0 : slash + 1);
        return ue::ObjName(o) == want && ue::ClassName(o).find("Texture") != std::string::npos;
    }
    UObject* IconTexture(const std::string& path) {
        if (path.empty()) return nullptr;
        if (auto it = iconTex_.find(path); it != iconTex_.end() && IsIconTexture(it->second, path)) return it->second;
        UObject* tex = towerwidgets::LoadAsset(path.c_str());
        if (!IsIconTexture(tex, path)) {
            if (tex) pbz::log::Info("影塔界面：图标 {} 加载到的是 {}（{}），不显示", path, ue::ObjName(tex), ue::ClassName(tex));
            tex = nullptr;
        }
        iconTex_[path] = tex;
        return tex;
    }
    void SetIcon(UObject* img, const std::string& path) {
        if (!img || !ue::Live(img)) return;
        UObject* tex = IconTexture(path);
        if (!tex) {  // Hidden：留位置，行不跳。3.4.3：没有图的格子也撑到同样大小（驿站里自制道具 / 随机位与有图的格子对齐）
            if (rows_ && rows_->iconAbove) if (auto* f = pe::FindFunction(ue::ClassOf(img), "SetDesiredSizeOverride")) {
                Args a(f); const double size[2] = {56.0, 56.0}; std::memcpy(a.At("DesiredSize"), size, sizeof size); pe::Call(img, f, a.buf.data());
            }
            SetByte(img, "SetVisibility", 2); return;
        }
        if (auto* f = pe::FindFunction(ue::ClassOf(img), "SetBrushFromTexture")) {
            Args a(f); std::memcpy(a.At("Texture"), &tex, 8); pe::Call(img, f, a.buf.data());
        }
        if (auto* f = pe::FindFunction(ue::ClassOf(img), "SetDesiredSizeOverride")) {
            Args a(f); const double size[2] = {rows_ && rows_->iconAbove ? 56.0 : 44.0, rows_ && rows_->iconAbove ? 56.0 : 44.0}; std::memcpy(a.At("DesiredSize"), size, sizeof size); pe::Call(img, f, a.buf.data());
        }
        SetByte(img, "SetVisibility", 4);  // SelfHitTestInvisible
    }
    std::optional<Rows> rows_;
    std::vector<UObject*> rowBtns_;                     // 前 rowData_ 个是内容行，后面是分类按钮
    int rowData_ = 0;
    std::vector<UObject*> rowCols_;                     // 多列时的各竖列
    std::array<UObject*, 3> rowCards_{};
    std::array<UObject*, 3> rowCardDetails_{};
    std::array<UObject*, 4> rowStats_{};               // 属性网格：左名 / 左值 / 右名 / 右值（同字体，名称列自适应、数值同一竖线）
    std::array<bool, 5> hexAligned_{};
    std::array<std::vector<UObject*>, 5> hexDone_{};  // 每个海克斯按钮里已处理的图
    std::array<double, 5> hexShift_{};               // 每个海克斯按钮的动效平移量                 // 海克斯按钮已按文字中心校正（原生按钮左侧有选中装饰，文字偏右）
    unsigned long long rowsInjectedAt_ = 0;
    UObject* rowSizer_ = nullptr;                      // 准备页内容总宽（SizeBox）
    int fitStep_ = 0;                                  // 0 量背景并定宽 → 1 平移居中 → 2 完成
    UObject* rowHolderSlot_ = nullptr;                  // 列表在正文下面的槽位：商品少时居中
    bool rowSingle_ = false;                            // 商品少：只用第一列，占满宽度（居中显示）
    int ItemOf(int button) const {                      // 第 button 个内容按钮显示第几件；-1 = 空着
        const int cols = std::max<int>(1, static_cast<int>(rowCols_.size()));
        if (rowSingle_ && !rows_->noCollapse) return button % cols == 0 ? rows_->first + button / cols : -1;
        return rows_->first + button;
    }
    void FillSlot(UObject* slot) {
        if (!slot) return;
        if (auto* f = pe::FindFunction(ue::ClassOf(slot), "SetSize")) { Args p(f); const float v = 1.0f; const uint8_t fill = 1; std::memcpy(p.buf.data(), &v, 4); std::memcpy(p.buf.data() + 4, &fill, 1); pe::Call(slot, f, p.buf.data()); }
    }
    bool rowsInjected_ = false, rowsBroken_ = false;
    std::string rowsHeader_, rowsBody_, rowsDrawn_;
    unsigned long long rowsDeadline_ = 0, rowsNext_ = 0;
    UObject* rowsDesc_ = nullptr;

    // 3.4.3（玩家：无尽 103 层战斗菜单里出现一排 {ButtonText} 空按钮、不能继续）：上一个对话框插进去的原生行按钮没拿掉，
    // 游戏在旧对话框未关完时复用同一个对话框显示新内容 → 残留的空按钮出现在新菜单里。清记录前先把插进去的控件全部收起
    void HideInjectedRows() {
        if (fourthButton_ && ue::Live(fourthButton_)) SetByte(fourthButton_, "SetVisibility", 1);  // 战斗菜单自建的第 4 个按钮同理
        if (!rowsInjected_) return;
        auto hide = [this](UObject* w) { if (w && ue::Live(w)) SetByte(w, "SetVisibility", 1); };  // Collapsed
        for (auto* w : rowBtns_) hide(w);
        for (auto* w : rowCols_) hide(w);
        for (auto* w : rowCards_) hide(w);
        for (auto* w : rowCardDetails_) hide(w);
        for (auto* w : rowStats_) hide(w);
    }
    void ClearRows() { HideInjectedRows(); rows_.reset(); rowBtns_.clear(); rowIcons_.clear(); rowLit_.clear(); rowInput_ = nullptr; rowData_ = 0; rowCols_.clear(); rowCards_ = {}; rowCardDetails_ = {}; rowStats_ = {}; hexAligned_ = {}; hexDone_ = {}; hexShift_ = {}; rowsInjectedAt_ = 0; rowSizer_ = nullptr; fitStep_ = 0; rowHolderSlot_ = nullptr; rowSingle_ = false; rowsInjected_ = false; rowsDrawn_.clear(); rowsDesc_ = nullptr; padLastFocus_ = nullptr; fourthButton_ = nullptr; fourthDeadline_ = 0; }
    int RowIndex(UObject* button) const {
        if (!rows_ || !rowsInjected_) return -1;
        for (size_t i = 0; i < rowBtns_.size(); ++i) if (rowBtns_[i] == button) return static_cast<int>(i);
        return -1;
    }
    bool CallBool(UObject* o, const char* fn) {
        auto* f = o && ue::Live(o) ? pe::FindFunction(ue::ClassOf(o), fn) : nullptr;
        if (!f) return false;
        Args a(f);
        pe::Call(o, f, a.buf.data());
        return ue::Get<uint8_t>(a.At("ReturnValue")) != 0;
    }
    int CallInt(UObject* o, const char* fn, UObject* arg = nullptr) {
        auto* f = o && ue::Live(o) ? pe::FindFunction(ue::ClassOf(o), fn) : nullptr;
        if (!f) return -1;
        Args a(f);
        if (arg) std::memcpy(a.buf.data(), &arg, 8);
        pe::Call(o, f, a.buf.data());
        return ue::Get<int32_t>(a.At("ReturnValue"));
    }
    void SetByte(UObject* o, const char* fn, uint8_t v) {
        auto* f = o && ue::Live(o) ? pe::FindFunction(ue::ClassOf(o), fn) : nullptr;
        if (!f) return;
        Args a(f);
        a.buf[0] = v;
        pe::Call(o, f, a.buf.data());
    }
    UObject* SpawnIn(UObject* outer, const char* cls) {
        auto* c = towerperf::ClassNamed(cls);
        auto* gs = towerperf::ClassNamed("GameplayStatics");
        auto* lib = gs ? towerwidgets::Default(gs) : nullptr;
        auto* f = gs ? pe::FindFunction(gs, "SpawnObject") : nullptr;
        if (!c || !lib || !f || !outer) return nullptr;
        Args a(f);
        std::memcpy(a.At("ObjectClass"), &c, 8);
        std::memcpy(a.At("Outer"), &outer, 8);
        pe::Call(lib, f, a.buf.data());
        return ue::Get<UObject*>(a.At("ReturnValue"));
    }
    UObject* CreateLike(UObject* cls, UObject* world) {
        if (!wblCls_ || !ue::Live(wblCls_)) { wblCls_ = towerperf::ClassNamed("WidgetBlueprintLibrary"); wblCdo_ = nullptr; }
        if (wblCdo_ && !ue::Live(wblCdo_)) wblCdo_ = nullptr;
        if (wblCls_ && !wblCdo_) wblCdo_ = Cdo(wblCls_);
        auto* create = wblCls_ ? pe::FindFunction(wblCls_, "Create") : nullptr;
        if (!wblCdo_ || !create) return nullptr;
        Args a(create);
        std::memcpy(a.At("WorldContextObject"), &world, 8);
        std::memcpy(a.At("WidgetType"), &cls, 8);
        if (auto* pc = towerwidgets::Controller()) std::memcpy(a.At("OwningPlayer"), &pc, 8);
        pe::Call(wblCdo_, create, a.buf.data());
        return ue::Get<UObject*>(a.At("ReturnValue"));
    }
    void ButtonText(UObject* b, const std::string& s) {
        if (!towerwidgets::SetText(b, s, "SetButtonText", "InText")) towerwidgets::SetText(b, s, "UpdateButtonText", "InText");
    }
    // 槽位设置（常见几种槽都有的字段）：读出来，换容器后原样设回去
    struct SlotCopy { std::array<uint8_t, 16> padding{}, size{}; uint8_t h = 0, v = 0; bool hasPad = false, hasSize = false, hasH = false, hasV = false; };
    SlotCopy ReadSlot(UObject* slot) {
        SlotCopy c;
        if (!slot) return c;
        UObject* cls = ue::ClassOf(slot);
        if (const int o = ue::PropOffset(cls, "Padding"); o >= 0) { ue::Read(slot + o, c.padding.data(), 16); c.hasPad = true; }
        if (const int o = ue::PropOffset(cls, "Size"); o >= 0) { ue::Read(slot + o, c.size.data(), 8); c.hasSize = true; }
        if (const int o = ue::PropOffset(cls, "HorizontalAlignment"); o >= 0) { c.h = ue::Get<uint8_t>(slot + o); c.hasH = true; }
        if (const int o = ue::PropOffset(cls, "VerticalAlignment"); o >= 0) { c.v = ue::Get<uint8_t>(slot + o); c.hasV = true; }
        return c;
    }
    void ApplySlot(UObject* slot, const SlotCopy& c) {
        if (!slot) return;
        auto call = [&](const char* fn, const void* data, size_t n) {
            auto* f = pe::FindFunction(ue::ClassOf(slot), fn);
            if (!f) return;
            Args a(f);
            std::memcpy(a.buf.data(), data, n);
            pe::Call(slot, f, a.buf.data());
        };
        if (c.hasPad) call("SetPadding", c.padding.data(), 16);
        if (c.hasSize) call("SetSize", c.size.data(), 8);
        if (c.hasH) call("SetHorizontalAlignment", &c.h, 1);
        if (c.hasV) call("SetVerticalAlignment", &c.v, 1);
    }
    UObject* SlotOf(UObject* w) {
        static int off = -2;
        if (off == -2) off = ue::PropOffset(towerperf::ClassNamed("Widget"), "Slot");
        return off >= 0 && w ? ue::Get<UObject*>(w + off) : nullptr;
    }
    // 用户 2026-11-11：准备页内容压出黑框。正文外层的原生 SizeBox 定了高度，属性 + 海克斯 + 入口三行放不下就画到框外。
    // 只在塔自己弹的这个对话框实例上撤掉高度 / 最大高度限制，让黑框随内容长高；原生对话框不受影响（每次新建）
    void ReleaseNativeHeight(UObject* box) {
        auto* sizeCls = towerperf::ClassNamed("SizeBox");
        if (!box || !sizeCls || !ue::IsA(box, sizeCls)) return;
        const int h = ue::PropOffset(sizeCls, "HeightOverride"), m = ue::PropOffset(sizeCls, "MaxDesiredHeight");
        pbz::log::Info("影塔界面：准备页正文框 {} 原高度 {:.0f} 最大 {:.0f}，撤掉限制", ue::ObjName(box), h >= 0 ? ue::Get<float>(box + h) : -1.0f, m >= 0 ? ue::Get<float>(box + m) : -1.0f);
        for (const char* fn : {"ClearHeightOverride", "ClearMaxDesiredHeight"}) if (auto* f = pe::FindFunction(sizeCls, fn)) { Args a(f); pe::Call(box, f, a.buf.data()); }
    }
    // 控件在屏幕上的左右边与「屏幕 / 本地」缩放；还没排版返回 false
    bool AbsSpan(UObject* w, double* left, double* right, double* scale) {
        static UObject* lib = nullptr;
        if (!lib || !ue::Live(lib)) if (auto* c = towerperf::ClassNamed("SlateBlueprintLibrary")) lib = towerwidgets::Default(c);
        auto* geomFn = w && ue::Live(w) ? pe::FindFunction(ue::ClassOf(w), "GetCachedGeometry") : nullptr;
        auto* sizeFn = lib ? pe::FindFunction(ue::ClassOf(lib), "GetLocalSize") : nullptr;
        auto* absFn = lib ? pe::FindFunction(ue::ClassOf(lib), "GetAbsoluteSize") : nullptr;
        auto* toAbs = lib ? pe::FindFunction(ue::ClassOf(lib), "LocalToAbsolute") : nullptr;
        if (!geomFn || !sizeFn || !absFn || !toAbs) return false;
        constexpr size_t kGeom = 0x38;
        Args g(geomFn); pe::Call(w, geomFn, g.buf.data());
        std::array<uint8_t, kGeom> geom{}; std::memcpy(geom.data(), g.At("ReturnValue"), kGeom);
        Args s(sizeFn); std::memcpy(s.At("Geometry"), geom.data(), kGeom); pe::Call(lib, sizeFn, s.buf.data());
        Args a(absFn); std::memcpy(a.At("Geometry"), geom.data(), kGeom); pe::Call(lib, absFn, a.buf.data());
        const double lw = ue::Get<double>(s.At("ReturnValue")), aw = ue::Get<double>(a.At("ReturnValue"));
        if (!std::isfinite(lw) || !std::isfinite(aw) || lw < 2 || aw < 1) return false;
        Args t(toAbs); std::memcpy(t.At("Geometry"), geom.data(), kGeom);
        const double local[2] = {0, 0}; std::memcpy(t.At("LocalCoordinate"), local, sizeof local);
        pe::Call(lib, toAbs, t.buf.data());
        *left = ue::Get<double>(t.At("ReturnValue")); *right = *left + aw; *scale = aw / lw;
        return std::isfinite(*left);
    }
    bool AbsCenterX(UObject* w, double* cx, double* scale) {
        double l = 0, r = 0;
        if (!AbsSpan(w, &l, &r, scale)) return false;
        *cx = (l + r) / 2; return true;
    }
    // 用户 2026-11-11：准备页内容超出黑底（属性偏左、海克斯选中动效露在黑底外）。
    // 排版后量对话框里最宽的原生底图，把内容总宽收到「底图宽 - 两侧留白」，再整体平移到底图正中
    void FitToBackground() {
        if (!rows_ || !rows_->statsGrid || fitStep_ >= 2 || !rowSizer_ || !ue::Live(rowSizer_) || GetTickCount64() < rowsInjectedAt_ + 150) return;
        UObject* d = Dialog();
        if (!d) return;
        UObject* bg = nullptr; double bl = 0, br = 0, bs = 1;
        for (const char* name : {"Image_312", "Image_232", "Image_229", "Image_219", "Image_152", "Image_160", "SelectedBG"}) {
            const int o = ue::PropOffset(ue::ClassOf(d), name);
            UObject* img = o >= 0 ? ue::Get<UObject*>(d + o) : nullptr;
            double l = 0, r = 0, sc = 1;
            if (img && AbsSpan(img, &l, &r, &sc) && r - l > br - bl) { bg = img; bl = l; br = r; bs = sc; }
        }
        double cl = 0, cr = 0, cs = 1;
        if (!AbsSpan(rowSizer_, &cl, &cr, &cs)) return;
        constexpr double kMargin = 80;  // 每侧留白（本地单位），够放下按钮选中动效
        if (!bg) {
            if (fitStep_ == 0) pbz::log::Info("影塔界面：准备页没找到原生底图，保持宽度 1000");
            fitStep_ = 2; return;
        }
        if (fitStep_ == 0) {
            const double width = std::clamp((br - bl) / cs - 2 * kMargin, 600.0, 1000.0);
            pbz::log::Info("影塔界面：准备页底图 {} 宽 {:.0f}（左 {:.0f}），内容宽 {:.0f}（左 {:.0f}）→ 内容定宽 {:.0f}", ue::ObjName(bg), (br - bl) / bs, bl, (cr - cl) / cs, cl, width);
            if (auto* f = pe::FindFunction(ue::ClassOf(rowSizer_), "SetWidthOverride")) { Args p(f); const float w = static_cast<float>(width); std::memcpy(p.buf.data(), &w, 4); pe::Call(rowSizer_, f, p.buf.data()); }
            fitStep_ = 1; rowsInjectedAt_ = GetTickCount64();  // 等一次排版再量
            hexAligned_ = {}; hexDone_ = {}; hexShift_ = {};    // 宽度变了，海克斯按钮重新量
            return;
        }
        const double shift = ((bl + br) / 2 - (cl + cr) / 2) / cs;
        if (std::abs(shift) >= 1 && std::abs(shift) < 600)
            if (auto* f = pe::FindFunction(ue::ClassOf(rowSizer_), "SetRenderTranslation")) { Args p(f); const double v[2] = {shift, 0}; std::memcpy(p.buf.data(), v, sizeof v); pe::Call(rowSizer_, f, p.buf.data()); }
        pbz::log::Info("影塔界面：准备页内容平移 {:.0f} 到底图正中", shift);
        fitStep_ = 2;
    }
    // 用户 2026-11-11：海克斯一行偏右。原生菜单按钮左侧留了选中装饰，文字不在按钮正中；
    // 排版完成后量一次文字中心与按钮中心的差，把按钮整体平移回去（渲染平移，点击区域一起移动）
    // 按钮内部的全部子控件（UserWidget.WidgetTree.RootWidget 往下，面板 / 单内容控件都展开）
    void Descend(UObject* w, std::vector<UObject*>& out, int depth = 0) {
        if (!w || !ue::Live(w) || depth > 10 || out.size() > 64) return;
        out.push_back(w);
        if (const int n = CallInt(w, "GetChildrenCount"); n > 0) { for (int i = 0; i < n && i < 32; ++i) Descend(CallObj(w, "GetChildAt", nullptr, &i), out, depth + 1); return; }
        if (UObject* c = CallObj(w, "GetContent")) Descend(c, out, depth + 1);
        // 子 UserWidget：进它自己的控件树
        static int treeOff = -2, rootOff = -2;
        if (treeOff == -2) treeOff = ue::PropOffset(towerperf::ClassNamed("UserWidget"), "WidgetTree");
        if (rootOff == -2) rootOff = ue::PropOffset(towerperf::ClassNamed("WidgetTree"), "RootWidget");
        if (depth > 0 && treeOff >= 0 && rootOff >= 0) if (UObject* t = ue::Get<UObject*>(w + treeOff); t && ue::Live(t)) Descend(ue::Get<UObject*>(t + rootOff), out, depth + 1);
    }
    void AlignHexButtons() {
        if (!rows_ || !rows_->statsGrid || GetTickCount64() < rowsInjectedAt_ + 120) return;
        // 用户 2026-11-11 六修：原生选中动效是左右两半（Image_62 / Image_105，各宽约 110，两头自带渐隐），
        // 合起来的中心比文字中心偏左约 50。按钮保持原生大小、居中在格子里；选中时把两半「一起」平移同一段距离，
        // 让动效整体中心对准文字中心——保留原生的两头收尾，不再一刀切，也不越到相邻格子
        auto translate = [](UObject* w, double x) {
            if (auto* f = pe::FindFunction(ue::ClassOf(w), "SetRenderTranslation")) { Args p(f); const double v[2] = {x, 0}; std::memcpy(p.buf.data(), v, sizeof v); pe::Call(w, f, p.buf.data()); }
        };
        static int treeOff = -2, rootOff = -2;
        if (treeOff == -2) treeOff = ue::PropOffset(towerperf::ClassNamed("UserWidget"), "WidgetTree");
        if (rootOff == -2) rootOff = ue::PropOffset(towerperf::ClassNamed("WidgetTree"), "RootWidget");
        auto* imageCls = towerperf::ClassNamed("Image");
        if (treeOff < 0 || rootOff < 0 || !imageCls) return;
        for (size_t i = 0; i < hexAligned_.size() && i < rowBtns_.size(); ++i) {
            UObject* b = rowBtns_[i];
            if (!b || !ue::Live(b)) continue;
            const int to = ue::PropOffset(ue::ClassOf(b), "ButtonTextBlock");
            UObject* text = to >= 0 ? ue::Get<UObject*>(b + to) : nullptr;
            double bl = 0, br = 0, bs = 1, tc = 0, ts = 1;
            if (!text || !AbsSpan(b, &bl, &br, &bs) || !AbsCenterX(text, &tc, &ts)) continue;
            UObject* tree = ue::Get<UObject*>(b + treeOff);
            if (!tree || !ue::Live(tree)) continue;
            std::vector<UObject*> all; Descend(ue::Get<UObject*>(tree + rootOff), all);
            auto& done = hexDone_[i];
            // 本次能量到的、还没处理的动效图（排除和按钮一样宽的底图）
            struct Part { UObject* w; double l, r, sc; };
            std::vector<Part> parts;
            for (UObject* w : all) {
                if (!ue::IsA(w, imageCls) || std::find(done.begin(), done.end(), w) != done.end()) continue;
                double l = 0, r = 0, sc = 1;
                if (!AbsSpan(w, &l, &r, &sc)) continue;
                if (r - l >= (br - bl) * 0.9) { done.push_back(w); continue; }
                parts.push_back({w, l, r, sc});
            }
            if (parts.empty() || (!hexAligned_[i] && parts.size() < 2)) continue;  // 两半都显示出来再一起算
            if (!hexAligned_[i]) {  // 第一次量到：按这一组的整体中心算平移量，之后出现的图用同一个平移量
                double l = parts[0].l, r = parts[0].r;
                for (const auto& p : parts) { l = std::min(l, p.l); r = std::max(r, p.r); }
                hexShift_[i] = (tc - (l + r) / 2) / parts[0].sc;
                hexAligned_[i] = true;
                if (i == 0) pbz::log::Info("影塔界面：海克斯选中动效 {} 张，整体中心偏离文字 {:.1f}，一起平移 {:.1f}", parts.size(), ((l + r) / 2 - tc) / parts[0].sc, hexShift_[i]);
            }
            for (const auto& p : parts) {
                if (std::abs(hexShift_[i]) >= 1 && std::abs(hexShift_[i]) < 400) translate(p.w, hexShift_[i]);
                done.push_back(p.w);
            }
        }
    }
    // 把 anchor 就地换成 box（box 沿用 anchor 的槽位设置），再把 anchor 放进 box 第一格
    bool WrapInto(UObject* anchor, UObject* box) {
        UObject* parent = CallObj(anchor, "GetParent");
        if (!parent) return false;
        const SlotCopy own = ReadSlot(SlotOf(anchor));
        auto* content = towerperf::ClassNamed("ContentWidget");
        if (content && ue::IsA(parent, content)) {
            UObject* slot = CallObj(parent, "SetContent", box);  // 换掉原内容（anchor 从这里摘下）
            ApplySlot(slot, own);
        } else {
            const int idx = CallInt(parent, "GetChildIndex", anchor);
            const int count = CallInt(parent, "GetChildrenCount");
            if (idx < 0 || count <= idx) return false;
            struct Later { UObject* w; SlotCopy c; };
            std::vector<Later> later;
            for (int i = idx + 1; i < count && i < 32; ++i)
                if (UObject* w = CallObj(parent, "GetChildAt", nullptr, &i)) later.push_back({w, ReadSlot(SlotOf(w))});
            CallObj(parent, "RemoveChild", anchor);
            for (auto& l : later) CallObj(parent, "RemoveChild", l.w);
            ApplySlot(CallObj(parent, "AddChild", box), own);
            for (auto& l : later) ApplySlot(CallObj(parent, "AddChild", l.w), l.c);
        }
        if (!CallObj(box, "AddChild", anchor)) return false;
        pbz::log::Info("影塔界面：列表插在 {}（{}）里正文的位置", ue::ObjName(parent), ue::ObjName(ue::ClassOf(parent)));
        return true;
    }
    bool InjectRows(UObject* d) {
        static int descOff = -2, treeOff = -2;
        if (descOff == -2) descOff = ue::PropOffset(screenCls_, "RichText_Description");
        if (treeOff == -2) treeOff = ue::PropOffset(towerperf::ClassNamed("UserWidget"), "WidgetTree");
        UObject* desc = descOff >= 0 ? ue::Get<UObject*>(d + descOff) : nullptr;
        UObject* tree = treeOff >= 0 ? ue::Get<UObject*>(d + treeOff) : nullptr;
        UObject* box = d ? ue::Get<UObject*>(d + screenEntries_) : nullptr;
        if (!desc || !tree || !box) return false;
        Args a(entries_);
        pe::Call(box, entries_, a.buf.data());
        struct Arr { UObject** data; int32_t num, max; };
        const Arr list = ue::Get<Arr>(a.At("ReturnValue"));
        if (!list.data || list.num < 1) return false;  // 对话框还没生成自己的按钮：下一帧再试
        UObject* sample = ue::Get<UObject*>(list.data);
        UObject* cls = sample ? ue::ClassOf(sample) : nullptr;
        UObject* outer = SpawnIn(tree, rows_->sideBySide ? "HorizontalBox" : "VerticalBox");
        UObject* column = SpawnIn(tree, "VerticalBox");
        if (!cls || !outer || !column) return false;
        UObject* group = nullptr;
        if (const int g = ue::PropOffset(ue::ClassOf(d), "ButtonGroup"); g >= 0) group = ue::Get<UObject*>(d + g);
        std::vector<UObject*> btns;
        const int cols = std::clamp(rows_->columns, 1, 4)  /* 3.1.0 N8：交易页 4 列（原来封顶 3，交易页错位） */;
        std::vector<UObject*> colBoxes{column};
        UObject* holder = column;
        if (cols > 1) {  // 多列：横排几个竖列，等宽
            holder = SpawnIn(tree, "HorizontalBox");
            if (!holder) return false;
            colBoxes.clear();
            for (int c = 0; c < cols; ++c) {
                UObject* box = SpawnIn(tree, "VerticalBox");
                if (!box) return false;
                UObject* cs = CallObj(holder, "AddChild", box); FillSlot(cs);
                if (rows_->icon && (rows_->iconAbove || rows_->pagerPair) && cs) if (auto* fn = pe::FindFunction(ue::ClassOf(cs), "SetPadding")) {
                    Args p(fn); const float gap[4] = {c == rows_->splitAt && c > 0 ? 36.0f : 4.0f, 0, 4, 0}; std::memcpy(p.buf.data(), gap, sizeof gap); pe::Call(cs, fn, p.buf.data());
                }
                if (rows_->hexCards && cs) if (auto* fn = pe::FindFunction(ue::ClassOf(cs), "SetPadding")) {
                    Args p(fn); const float gap[4] = {8, 0, 8, 0}; std::memcpy(p.buf.data(), gap, sizeof gap); pe::Call(cs, fn, p.buf.data());
                }
                colBoxes.push_back(box);
            }
        }
        if (rows_->hexCards && cols == 3) {
            // Use the game's Chinese font, as the standalone shop panel does.
            std::array<uint8_t, 0x60> font{}; bool haveFont = false;
            auto* textClass = towerperf::ClassNamed("TextBlock");
            const int fontOff = textClass ? ue::PropOffset(textClass, "Font") : -1;
            if (fontOff >= 0) ue::ForEach([&](int, UObject* o) {
                if (!o || !ue::Live(o) || ue::IsDefault(o) || !ue::IsA(o, textClass)) return true;
                auto* fontObject = ue::Get<UObject*>(o + fontOff);
                if (!fontObject || ue::ObjName(fontObject) != "ZH_FZKaiTi_Font") return true;
                haveFont = ue::Read(o + fontOff, font.data(), font.size()); return !haveFont;
            });
            for (int c = 0; c < 3; ++c) {
                for (int part = 0; part < 2; ++part) {
                    auto* text = SpawnIn(tree, "TextBlock");
                    auto* size = SpawnIn(tree, "SizeBox");
                    if (!text || !size || !CallObj(size, "SetContent", text)) return false;
                    if (haveFont) if (auto* fn = pe::FindFunction(ue::ClassOf(text), "SetFont")) {
                        Args p(fn); const float points = part == 0 ? 17.0f : 13.0f;
                        std::memcpy(font.data() + 0x48, &points, 4); std::memcpy(p.buf.data(), font.data(), font.size()); pe::Call(text, fn, p.buf.data());
                    }
                    SetByte(text, "SetAutoWrapText", 1);
                    SetByte(text, "SetJustification", 1);
                    if (auto* fn = pe::FindFunction(ue::ClassOf(size), "SetHeightOverride")) { Args p(fn); const float height = part == 0 ? 38.0f : 175.0f; std::memcpy(p.buf.data(), &height, 4); pe::Call(size, fn, p.buf.data()); }
                    if (!CallObj(colBoxes[c], "AddChild", size)) return false;
                    if (part == 0) rowCards_[c] = text; else rowCardDetails_[c] = text;
                }
            }
        }
        UObject* languageButton = nullptr; UObject* shopButton = nullptr;
        // 用户 2026-11-11：海克斯一行固定 5 格（前 100 层最多 5 个，含质变）；翻页（无尽模式才会超过一页）单独一行，
        // 语言 / 驿站再换一行，以后加的入口放这一行
        UObject* hexRow = nullptr; UObject* pageRow = nullptr; UObject* utilityRow = nullptr;
        if (rows_->statsGrid) {
            hexRow = SpawnIn(tree, "HorizontalBox"); pageRow = SpawnIn(tree, "HorizontalBox"); utilityRow = SpawnIn(tree, "HorizontalBox");
            if (!hexRow || !pageRow || !utilityRow) return false;
            for (UObject* r : {hexRow, pageRow, utilityRow}) {
                UObject* rs = CallObj(holder, "AddChild", r);
                if (!rs) return false;
                // 海克斯一行两侧内缩，选中动效留在黑底里；用户 2026-11-11：整体偏右（左空右挤），左 20 右 80 往左挪 30
                const float gap[4] = {r == hexRow ? 20.0f : 0.0f, r == hexRow ? 10.0f : 4.0f, r == hexRow ? 80.0f : 0.0f, 0};
                if (auto* fn = pe::FindFunction(ue::ClassOf(rs), "SetPadding")) { Args p(fn); std::memcpy(p.buf.data(), gap, 16); pe::Call(rs, fn, p.buf.data()); }
            }
        }
        const int rowLimit = rows_->statsGrid || rows_->pagerPair ? 9 : rows_->sideBySide ? 7 : rows_->forgeGrid ? 6 : kRowsPerPage;
        const int perCol = std::clamp(rows_->perColumn, 1, rowLimit);
        std::vector<UObject*> pagerLines(static_cast<size_t>(cols), nullptr);
        for (int i = 0; i < perCol * cols; ++i) {
            UObject* b = CreateLike(cls, d);
            if (!b) break;
            UObject* slot = nullptr;
            if (rows_->pagerPair && i / cols >= perCol - 2) {  // N9h：＜ ＞ 同一行、居中
                auto& line = pagerLines[static_cast<size_t>(i % cols)];
                if (!line) { line = SpawnIn(tree, "HorizontalBox"); if (UObject* ls = line ? CallObj(colBoxes[static_cast<size_t>(i % cols)], "AddChild", line) : nullptr) SetByte(ls, "SetHorizontalAlignment", 2); }
                slot = line ? CallObj(line, "AddChild", b) : nullptr;
                if (slot) if (auto* f = pe::FindFunction(ue::ClassOf(slot), "SetPadding")) { Args p(f); const float lp[4] = {12, 0, 12, 0}; std::memcpy(p.buf.data(), lp, 16); pe::Call(slot, f, p.buf.data()); }
                if (rows_->icon) rowIcons_.push_back(nullptr);
                if (group && ue::Live(group)) CallObj(group, "AddWidget", b);
                btns.push_back(b);
                continue;
            } else if (rows_->statsGrid) {
                if (i < 5) {
                    // These are newly created native menu buttons, so their
                    // unselected style can be made more legible independently.
                    const int opacity = ue::PropOffset(ue::ClassOf(b), "UnselectedOpacity");
                    if (opacity >= 0) { const double v = .95; ue::Write(b + opacity, &v, sizeof v); }
                    // 用户 2026-11-11 五修：按钮不撑满格子（撑满后原生内部排版靠左，文字与选中动效错位、收尾被切）。
                    // 格子等宽，按钮按原生大小居中——和底部「下一层」一样是原生排版，不做任何平移
                    slot = CallObj(hexRow, "AddChild", b); FillSlot(slot); SetByte(slot, "SetHorizontalAlignment", 2);
                } else { slot = CallObj(i < 7 ? pageRow : utilityRow, "AddChild", b); FillSlot(slot); }
            } else if (rows_->icon && rows_->iconAbove) {  // N9g：一格 = 上图标（居中）+ 下按钮
                UObject* cell = SpawnIn(tree, "VerticalBox");
                UObject* img = SpawnIn(tree, "Image");
                UObject* cellSlot = cell ? CallObj(colBoxes[static_cast<size_t>(i % cols)], "AddChild", cell) : nullptr;
                if (img && cell) if (UObject* is = CallObj(cell, "AddChild", img)) SetByte(is, "SetHorizontalAlignment", 2);
                slot = cell ? CallObj(cell, "AddChild", b) : CallObj(colBoxes[static_cast<size_t>(i % cols)], "AddChild", b);
                if (slot) SetByte(slot, "SetHorizontalAlignment", 2);  // 按钮按原生大小居中（撑满时原生排版靠左，文字和图标错开）
                if (cellSlot) if (auto* f = pe::FindFunction(ue::ClassOf(cellSlot), "SetPadding")) { Args p(f); const float lp[4] = {0, 6, 0, 6}; std::memcpy(p.buf.data(), lp, 16); pe::Call(cellSlot, f, p.buf.data()); }
                rowIcons_.push_back(img);
            } else if (rows_->icon) {  // 3.1.0：图标 + 按钮横排一行
                UObject* line = SpawnIn(tree, "HorizontalBox");
                UObject* img = SpawnIn(tree, "Image");
                UObject* lineSlot = line ? CallObj(colBoxes[static_cast<size_t>(i % cols)], "AddChild", line) : nullptr;
                if (img && line) { if (UObject* is = CallObj(line, "AddChild", img)) { SetByte(is, "SetVerticalAlignment", 2); const float ip[4] = {0, 0, 8, 0}; if (auto* f = pe::FindFunction(ue::ClassOf(is), "SetPadding")) { Args p(f); std::memcpy(p.buf.data(), ip, 16); pe::Call(is, f, p.buf.data()); } } }
                slot = line ? CallObj(line, "AddChild", b) : CallObj(colBoxes[static_cast<size_t>(i % cols)], "AddChild", b);
                if (slot) { SetByte(slot, "SetVerticalAlignment", 2); if (line) FillSlot(slot); }  // 按钮占满格子剩余宽度，文字不溢到下一格
                if (lineSlot) if (auto* f = pe::FindFunction(ue::ClassOf(lineSlot), "SetPadding")) { Args p(f); const float lp[4] = {0, 3, 0, 3}; std::memcpy(p.buf.data(), lp, 16); pe::Call(lineSlot, f, p.buf.data()); }
                rowIcons_.push_back(img);
            } else slot = rows_->sideBySide && i >= 5 ? nullptr : CallObj(colBoxes[static_cast<size_t>(i % cols)], "AddChild", b);
            if (rows_->sideBySide && i == 5) languageButton = b;
            if (rows_->sideBySide && i == 6) shopButton = b;
            const float pad[4] = {0, 2, 0, 2};  // 用户 2026-11-05：驿站超出屏幕下沿，行距收紧
            if (slot) if (auto* f = pe::FindFunction(ue::ClassOf(slot), "SetPadding")) { Args p(f); std::memcpy(p.buf.data(), pad, 16); pe::Call(slot, f, p.buf.data()); }
            if (group && ue::Live(group)) CallObj(group, "AddWidget", b);
            btns.push_back(b);
        }
        if (btns.empty()) return false;
        if (rows_->statsGrid) {
            // A single rich-text widget with space padding is not a grid:
            // proportional glyphs and line wrapping displace the other column.
            // Use two native TextBlocks with the same font and equal slots.
            std::array<uint8_t, 0x60> font{}; bool haveFont = false;
            auto* textClass = towerperf::ClassNamed("TextBlock");
            const int fontOff = textClass ? ue::PropOffset(textClass, "Font") : -1;
            if (fontOff >= 0) ue::ForEach([&](int, UObject* o) {
                if (!o || !ue::Live(o) || ue::IsDefault(o) || !ue::IsA(o, textClass)) return true;
                auto* fontObject = ue::Get<UObject*>(o + fontOff);
                if (!fontObject || ue::ObjName(fontObject) != "ZH_FZKaiTi_Font") return true;
                haveFont = ue::Read(o + fontOff, font.data(), font.size()); return !haveFont;
            });
            // 用户 2026-11-11：属性没对齐、字号比原生正文大。每半边拆成「名称列 + 数值列」两个文本块，
            // 名称列按最宽的名称自适应，数值从同一竖线开始；字号 15，不再比原生正文大
            UObject* stats = SpawnIn(tree, "HorizontalBox");
            if (!haveFont || !stats) return false;
            const float points = 15.0f; std::memcpy(font.data() + 0x48, &points, 4);
            for (int half = 0; half < 2; ++half) {
                UObject* pair = SpawnIn(tree, "HorizontalBox");
                UObject* pairSlot = pair ? CallObj(stats, "AddChild", pair) : nullptr;
                if (!pairSlot) return false;
                FillSlot(pairSlot);
                SetByte(pairSlot, "SetHorizontalAlignment", 2);  // 用户 2026-11-11：左右两组各自在半边居中，不再整体偏左
                for (int part = 0; part < 2; ++part) {
                    UObject* text = SpawnIn(tree, "TextBlock");
                    auto* setFont = text ? pe::FindFunction(ue::ClassOf(text), "SetFont") : nullptr;
                    if (!setFont) return false;
                    Args p(setFont); std::memcpy(p.buf.data(), font.data(), font.size()); pe::Call(text, setFont, p.buf.data());
                    SetByte(text, "SetAutoWrapText", 0);
                    UObject* slot = CallObj(pair, "AddChild", text);
                    if (!slot) return false;
                    if (part == 1) {
                        FillSlot(slot);
                        if (auto* fn = pe::FindFunction(ue::ClassOf(slot), "SetPadding")) { Args q(fn); const float gap[4] = {20, 0, 0, 0}; std::memcpy(q.buf.data(), gap, 16); pe::Call(slot, fn, q.buf.data()); }
                    }
                    rowStats_[static_cast<size_t>(half * 2 + part)] = text;
                }
            }
            UObject* nativeBox = CallObj(desc, "GetParent");
            if (!WrapInto(desc, outer) || !CallObj(outer, "AddChild", stats)) return false;
            ReleaseNativeHeight(nativeBox);
        } else if (rows_->sideBySide) {
            UObject* left = SpawnIn(tree, "VerticalBox");
            if (!left || !WrapInto(desc, left) || !WrapInto(left, outer)) return false;
            FillSlot(SlotOf(left));
            if (languageButton && !CallObj(left, "AddChild", languageButton)) return false;
            if (shopButton && !CallObj(left, "AddChild", shopButton)) return false;
        } else if (!WrapInto(desc, outer)) return false;
        const int dataButtons = static_cast<int>(btns.size());
        if (!rows_->tabs.empty()) {
            // 不使用 Overlay：它不参与计算正文宽度，原生按钮又有最小宽度，六个并排会越界和重叠。
            // 三个一行放进竖向布局，让面板按真实内容测量，分类选中时文字变宽也不侵入邻列。
            UObject* tabs = nullptr;
            for (size_t i = 0; i < rows_->tabs.size(); ++i) {
                if (i % 3 == 0) {
                    tabs = SpawnIn(tree, "HorizontalBox");
                    if (!tabs || !CallObj(outer, "AddChild", tabs)) break;
                }
                UObject* b = CreateLike(cls, d);
                if (!b) break;
                if (UObject* ts = CallObj(tabs, "AddChild", b)) FillSlot(ts);
                if (group && ue::Live(group)) CallObj(group, "AddWidget", b);
                btns.push_back(b);
            }
            if (btns.size() != static_cast<size_t>(dataButtons) + rows_->tabs.size())
                pbz::log::Info("影塔界面：分类按钮未全部创建");
        }
        if (cols > 1 || rows_->sideBySide || rows_->statsGrid) {  // 用户 2026-11-08：原生按钮自带的宽度把内容撑出黑底：多列页面固定总宽
            UObject* sizer = SpawnIn(tree, "SizeBox");
            if (sizer && WrapInto(outer, sizer))
                if (auto* f = pe::FindFunction(ue::ClassOf(sizer), "SetWidthOverride")) { Args p(f); const float w = rows_->statsGrid ? 1000.0f : 880.0f; std::memcpy(p.buf.data(), &w, 4); pe::Call(sizer, f, p.buf.data()); }
            if (rows_->statsGrid) rowSizer_ = sizer;
        }
        rowInput_ = nullptr; rowInputSeen_.clear(); rowInputWant_.clear();
        if (rows_->numberInput) {  // 3.6.1：数量输入框（引擎 EditableText，游戏楷体、居中、无边框）；键盘直接打数字，手柄用下面的加减按钮
            // 用户 2026-11-15：输入框要融入原生界面——无底框；字用游戏楷体、暖白（同对话框正文）；固定宽度居中，下方一条半透明细线当下划线
            UObject* input = SpawnIn(tree, "EditableText");
            UObject* field = SpawnIn(tree, "VerticalBox");
            UObject* sizer = SpawnIn(tree, "SizeBox");
            UObject* line = SpawnIn(tree, "Image");
            bool placed = false;
            if (input && field && sizer && CallObj(field, "AddChild", input) && CallObj(sizer, "SetContent", field)) {
                if (auto* f = pe::FindFunction(ue::ClassOf(sizer), "SetWidthOverride")) { Args p(f); const float w = 260.0f; std::memcpy(p.buf.data(), &w, 4); pe::Call(sizer, f, p.buf.data()); }
                if (UObject* ss = CallObj(outer, "AddChild", sizer)) { SetByte(ss, "SetHorizontalAlignment", 2); placed = true; }
                if (line) if (UObject* ls = CallObj(field, "AddChild", line)) {
                    if (auto* f = pe::FindFunction(ue::ClassOf(line), "SetColorAndOpacity")) { Args p(f); const float c[4] = {0.92f, 0.89f, 0.82f, 0.35f}; std::memcpy(p.buf.data(), c, 16); pe::Call(line, f, p.buf.data()); }
                    if (auto* f = pe::FindFunction(ue::ClassOf(line), "SetDesiredSizeOverride")) { Args p(f); const double sz[2] = {260.0, 2.0}; std::memcpy(p.buf.data(), sz, 16); pe::Call(line, f, p.buf.data()); }
                    if (auto* f = pe::FindFunction(ue::ClassOf(ls), "SetPadding")) { Args p(f); const float pad[4] = {0, 2, 0, 0}; std::memcpy(p.buf.data(), pad, 16); pe::Call(ls, f, p.buf.data()); }
                }
                // 文字颜色：WidgetStyle.ColorAndOpacity（反射取偏移；取不到就保留引擎默认）
                const int styleOff = ue::PropOffset(ue::ClassOf(input), "WidgetStyle");
                UObject* styleStruct = towerperf::ClassNamed("EditableTextStyle");
                const int colorOff = styleStruct ? ue::PropOffset(styleStruct, "ColorAndOpacity") : -1;
                if (styleOff >= 0 && colorOff >= 0) {
                    const float c[4] = {0.92f, 0.89f, 0.82f, 1.0f}; const uint8_t specified = 0;
                    ue::Write(input + styleOff + colorOff, c, 16); ue::Write(input + styleOff + colorOff + 16, &specified, 1);
                }
                if (const int sel = ue::PropOffset(ue::ClassOf(input), "SelectAllTextWhenFocused"); sel >= 0) { const uint8_t on = 1; ue::Write(input + sel, &on, 1); }  // 点进去全选，直接重打
                if (auto* f = pe::FindFunction(ue::ClassOf(input), "SetMinimumDesiredWidth")) { Args p(f); const float w = 240.0f; std::memcpy(p.buf.data(), &w, 4); pe::Call(input, f, p.buf.data()); }
            }
            if (placed) {
                std::array<uint8_t, 0x60> font{}; bool haveFont = false;
                auto* textClass = towerperf::ClassNamed("TextBlock");
                const int fontOff = textClass ? ue::PropOffset(textClass, "Font") : -1;
                if (fontOff >= 0) ue::ForEach([&](int, UObject* o) {
                    if (!o || !ue::Live(o) || ue::IsDefault(o) || !ue::IsA(o, textClass)) return true;
                    auto* fontObject = ue::Get<UObject*>(o + fontOff);
                    if (!fontObject || ue::ObjName(fontObject) != "ZH_FZKaiTi_Font") return true;
                    haveFont = ue::Read(o + fontOff, font.data(), font.size()); return !haveFont;
                });
                if (haveFont) if (auto* fn = pe::FindFunction(ue::ClassOf(input), "SetFont")) {
                    Args p(fn); const float points = 28.0f; std::memcpy(font.data() + 0x48, &points, 4); std::memcpy(p.buf.data(), font.data(), font.size()); pe::Call(input, fn, p.buf.data());
                }
                SetByte(input, "SetJustification", 1);  // 居中
                if (UObject* is = SlotOf(input)) if (auto* f = pe::FindFunction(ue::ClassOf(is), "SetPadding")) { Args p(f); const float pad[4] = {0, 10, 0, 0}; std::memcpy(p.buf.data(), pad, 16); pe::Call(is, f, p.buf.data()); }
                const std::string want = rows_->inputText ? rows_->inputText() : std::string();
                towerwidgets::SetText(input, want, "SetText", "InText");
                towerwidgets::SetText(input, "输入数量", "SetHintText", "InHintText");
                rowInput_ = input; rowInputSeen_ = want; rowInputWant_ = want;
            } else pbz::log::Info("影塔界面：数量输入框创建失败，只用加减按钮");
        }
        if (UObject* slot = CallObj(outer, "AddChild", holder)) {
            rowHolderSlot_ = slot;
            if (rows_->sideBySide) FillSlot(slot);
            const float pad[4] = {rows_->sideBySide ? 16.0f : 0.0f, 6, 0, 0};
            if (auto* f = pe::FindFunction(ue::ClassOf(slot), "SetPadding")) { Args p(f); std::memcpy(p.buf.data(), pad, 16); pe::Call(slot, f, p.buf.data()); }
        }
        rowBtns_ = std::move(btns);
        rowData_ = dataButtons;
        if (cols > 1) rowCols_ = colBoxes;
        rowsDesc_ = desc;
        rowsInjected_ = true;
        rowsInjectedAt_ = GetTickCount64();
        pbz::log::Info("影塔界面：「{}」插入 {} 行原生按钮（{}，按钮组 {}）", rowsHeader_, rowBtns_.size(), ue::ObjName(cls), group ? "有" : "无");
        return true;
    }
    void CloseDialog() {
        if (UObject* d = Dialog()) {
            if (auto* f = pe::FindFunction(ue::ClassOf(d), "CloseWindowByCancel")) pe::Call(d, f, nullptr);
            else if (auto* k = pe::FindFunction(ue::ClassOf(d), "KillDialog")) pe::Call(d, k, nullptr);
        }
        open_ = false;
        shopConfirmFocusPending_ = false;
        gate_.Close();
        towerwidgets::OverlayBusy(false);
        ClearRows();
    }
    // 3.6.1 数量输入框：每 0.2 秒读一次。玩家打了字 → onInput（解析、夹到 1–最大）；按钮改了数量 → 把框里的数字改过来。
    // 框被清空时不立刻回填（让玩家能删了重打）；打了非法内容 / 超出范围就改成解析后的值
    void InputTick() {
        if (!rows_ || !rows_->numberInput || !rowInput_ || !ue::Live(rowInput_) || !rows_->inputText || !rows_->onInput) return;
        const auto now = GetTickCount64();
        if (now < rowInputAt_) return;
        rowInputAt_ = now + 200;
        std::string typed;
        if (!towerwidgets::GetText(rowInput_, &typed)) return;
        const std::string want = rows_->inputText();
        if (want != rowInputWant_) {  // 加减按钮改了数量
            rowInputWant_ = want;
            if (typed != want) { towerwidgets::SetText(rowInput_, want, "SetText", "InText"); typed = want; }
            rowInputSeen_ = typed; return;
        }
        if (typed == rowInputSeen_) return;
        rowInputSeen_ = typed;
        if (typed.empty()) return;
        rows_->onInput(typed);
        rowInputWant_ = rows_->inputText();
        if (rowInputWant_ != typed) { towerwidgets::SetText(rowInput_, rowInputWant_, "SetText", "InText"); rowInputSeen_ = rowInputWant_; }
        rowsDrawn_.clear();
    }
    void TickRows() {
        if (!open_ || !rows_) return;
        const auto now = GetTickCount64();
        if (!rowsInjected_) {
            if (UObject* d = Dialog(); d && InjectRows(d)) { rowsNext_ = 0; }
            else if (now >= rowsDeadline_) {
                rowsBroken_ = true;
                pbz::log::Info("影塔界面：「{}」原生按钮列插不进去，之后改用分页确认框", rowsHeader_);
                CloseDialog();
                pendingShow_ = now + 900;
                return;
            } else return;
        }
        InputTick();
        if (now < rowsNext_) return;
        rowsNext_ = now + 100;
        FitToBackground();
        if (fitStep_ >= 2) AlignHexButtons();
        if (rows_->alive && !rows_->alive()) { pbz::log::Info("影塔界面：「{}」条件已变，关闭", rowsHeader_); CloseDialog(); return; }
        const int count = rows_->count ? rows_->count() : 0;
        const std::string body = rows_->body ? rows_->body() : rowsBody_;
        const std::string detail;  // 用户 2026-11-05：不显示说明（悬停插说明会让列表跳动）
        std::string key = body + "\x1f" + detail;
        key += std::format("\x1ftab{}", rows_->tab);
        if (rows_->hexCards && rows_->card) for (int c = 0; c < 3; ++c) {
            key += "\x1f" + rows_->card(c);
            if (rows_->cardDetail) key += "\x1f" + rows_->cardDetail(c);
        }
        if (rowCols_.size() > 1) {  // 这一页一列放得下：收起其余列，第一列占满宽度
            const bool single = !rows_->noCollapse && std::max(0, count - rows_->first) <= rowData_ / static_cast<int>(rowCols_.size());
            if (single != rowSingle_) {
                rowSingle_ = single;
                for (size_t c = 1; c < rowCols_.size(); ++c) if (ue::Live(rowCols_[c])) SetByte(rowCols_[c], "SetVisibility", single ? 1 : 4);
                if (rowHolderSlot_ && ue::Live(rowHolderSlot_)) SetByte(rowHolderSlot_, "SetHorizontalAlignment", single ? 2 : 0);
            }
        }
        std::vector<std::pair<std::string, bool>> state(static_cast<size_t>(rowData_));
        for (size_t i = 0; i < state.size(); ++i) {
            const int idx = ItemOf(static_cast<int>(i));
            if (idx >= 0 && idx < count) state[i] = {rows_->label(idx), !rows_->enabled || rows_->enabled(idx)};
            key += std::format("\x1f{}{}", state[i].first, state[i].second ? 1 : 0);
            if (rows_->icon && idx >= 0 && idx < count) key += rows_->icon(idx);
        }
        if (rows_->lit) {  // 3.6.1：已选的格子每帧重新点亮（原生按钮组在光标移动时会把选中底图收回去）
            if (rowLit_.size() != rowBtns_.size()) rowLit_.assign(rowBtns_.size(), 0);
            for (size_t i = 0; i < state.size() && i < rowBtns_.size(); ++i) {
                const int idx = ItemOf(static_cast<int>(i));
                const bool want = idx >= 0 && idx < count && rows_->lit(idx);
                if (!want && !rowLit_[i]) continue;
                UObject* b = rowBtns_[i];
                if (!ue::Live(b)) continue;
                if (auto* fn = pe::FindFunction(ue::ClassOf(b), "DoSelectedAppearance")) {
                    Args a(fn);
                    int off = ue::PropOffset(fn, "IsSelected");
                    if (off < 0) off = ue::PropOffset(fn, "bIsSelected");
                    if (off < 0) off = 0;
                    if (off < static_cast<int>(a.buf.size())) a.buf[static_cast<size_t>(off)] = want ? 1 : 0;
                    pe::Call(b, fn, a.buf.data());
                }
                rowLit_[i] = want ? 1 : 0;
            }
        }
        if (key == rowsDrawn_) return;
        rowsDrawn_ = key;
        if (rows_->hexCards && rows_->card) for (int c = 0; c < 3; ++c)
        {
            if (rowCards_[c] && ue::Live(rowCards_[c])) towerwidgets::SetText(rowCards_[c], rows_->card(c));
            if (rows_->cardDetail && rowCardDetails_[c] && ue::Live(rowCardDetails_[c])) towerwidgets::SetText(rowCardDetails_[c], rows_->cardDetail(c));
        }
        for (size_t i = static_cast<size_t>(rowData_); i < rowBtns_.size(); ++i) {
            const int k = static_cast<int>(i) - rowData_;
            if (ue::Live(rowBtns_[i]) && k < static_cast<int>(rows_->tabs.size()))
                ButtonText(rowBtns_[i], k == rows_->tab ? "【" + rows_->tabs[k] + "】" : rows_->tabs[k]);
        }
        for (size_t i = 0; i < state.size(); ++i) {
            UObject* b = rowBtns_[i];
            if (!ue::Live(b)) continue;
            const bool on = !state[i].first.empty();
            // A used card refresh stays hidden in its slot so the select row
            // doesn't move upward when its neighbour is refreshed.
            const bool keepPlace = rows_->hexCards || (rows_->statsGrid && (i < 5 || i == 8));  // 海克斯 5 格、驿站位空着也占位
            SetByte(b, "SetVisibility", on ? 0 : (keepPlace || rows_->icon ? 2 : 1));  // Visible / Hidden / Collapsed
            if (rows_->icon && i < rowIcons_.size()) {
                const int idx = ItemOf(static_cast<int>(i));
                SetIcon(rowIcons_[i], on && idx >= 0 ? rows_->icon(idx) : std::string());
            }
            if (!on) continue;
            ButtonText(b, state[i].first);
            SetByte(b, "SetIsEnabled", state[i].second ? 1 : 0);
        }
        if (rows_->focusIndex >= 0) {
            if (lastPadAt_ > lastMouseAt_) for (size_t i = 0; i < state.size(); ++i)
                if (ItemOf(static_cast<int>(i)) == rows_->focusIndex && state[i].second && ue::Live(rowBtns_[i])) {
                    FocusOn(rowBtns_[i]); padFocused_ = true; break;
                }
            rows_->focusIndex = -1;
        }
        if (rowsDesc_ && ue::Live(rowsDesc_)) {
            if (rows_->statsGrid && std::all_of(rowStats_.begin(), rowStats_.end(), [](UObject* t) { return t && ue::Live(t); })) {
                std::vector<std::string> lines; std::string notes; std::istringstream in(body); std::string line;
                while (std::getline(in, line)) if (!line.empty()) {
                    if (tower::menutext::IsStatLine(line)) lines.push_back(line);
                    else notes += "\n" + line;  // 提示句放标题下方居中，不进两列表格
                }
                const auto text = tower::menutext::StatsGrid(lines);
                towerwidgets::SetText(rowsDesc_, tower::rewardui::Compact(rowsHeader_ + notes));
                SetByte(rowsDesc_, "SetJustification", 1);  // 标题居中，和下面左右两组属性对称
                for (size_t c = 0; c < text.size(); ++c) towerwidgets::SetText(rowStats_[c], text[c]);
            } else towerwidgets::SetText(rowsDesc_, tower::rewardui::Compact(rowsHeader_ + "\n\n" + body + (detail.empty() ? "" : "\n\n" + detail)));
        }
    }
    // 行按钮被点：就地执行（刷新文字），或先关对话框、关完再执行
    bool OnRowClicked(UObject* button) {
        const int row = RowIndex(button);
        if (row < 0) return false;
        if (row >= rowData_) {  // 分类按钮
            const int tab = row - rowData_;
            if (tab != rows_->tab && rows_->onTab) {
                rows_->tab = tab;
                rows_->first = 0;
                rows_->onTab(tab);
                rowsDrawn_.clear();
                rowsNext_ = 0;
            }
            return true;
        }
        const int idx = ItemOf(row);
        if (idx < 0 || idx >= (rows_->count ? rows_->count() : 0) || (rows_->enabled && !rows_->enabled(idx)) || (!rows_->act && !rows_->prepare)) return true;
        pbz::log::Info("影塔界面：「{}」第 {} 行「{}」", rowsHeader_, idx + 1, rows_->label(idx));
        if (rows_->closes && rows_->closes(idx)) {
            std::function<void()> action;
            if (rows_->prepare) action = rows_->prepare(idx);
            else { auto act = rows_->act; action = [act, idx] { act(idx); }; }
            if (!action) return true;
            CloseDialog();
            pendingAct_ = std::move(action);
            pendingActAt_ = GetTickCount64() + 700;
        } else {
            rows_->act(idx);
            rowsNext_ = GetTickCount64() + 150;  // 命令下一帧才执行：稍后刷新
            rowsDrawn_.clear();
        }
        return true;
    }

    void TickFourthButton() {
        if (!open_ || opts_.size() != 4 || fourthButton_ || !fourthDeadline_) return;
        UObject* d = Dialog();
        UObject* box = d ? ue::Get<UObject*>(d + screenEntries_) : nullptr;
        auto existing = DialogButtons();
        if (box && existing.size() == 3) {
            auto* fn = pe::FindFunction(ue::ClassOf(box), "BP_CreateEntryOfClass");
            UObject* cls = ue::ClassOf(existing[0]);
            const int input = fn ? ue::PropOffset(fn, "EntryClass") : -1;
            const int output = fn ? ue::PropOffset(fn, "ReturnValue") : -1;
            if (fn && cls && input >= 0 && input + 8 <= 0x400 && output >= 0 && output + 8 <= 0x400 && (input + 8 <= output || output + 8 <= input)) {
                Args a(fn); std::memcpy(a.buf.data() + input, &cls, 8); pe::Call(box, fn, a.buf.data());
                auto* b = ue::Get<UObject*>(a.buf.data() + output);
                if (b && ue::Live(b)) {
                    fourthButton_ = b;
                    ButtonText(b, opts_[3].label);
                    if (const int g = ue::PropOffset(ue::ClassOf(d), "ButtonGroup"); g >= 0)
                        if (auto* group = ue::Get<UObject*>(d + g); group && ue::Live(group)) CallObj(group, "AddWidget", b);
                    for (auto* entry : DialogButtons()) FillSlot(SlotOf(entry));
                    pbz::log::Info("影塔界面：战斗菜单创建第四个原生按钮「{}」", opts_[3].label);
                    return;
                }
            }
        }
        if (GetTickCount64() >= fourthDeadline_) {
            fourthDeadline_ = 0;
            const bool fighting = towerapi::Run().phase == tower::Phase::Fighting;
            pbz::log::Info("影塔界面：第四个按钮未创建，回退到分页菜单");
            CloseDialog();
            Then([this, fighting] {
                if (fighting) Show("影塔·百战 · 战斗中", "", {{"继续战斗", [] {}}, {"信息", [this] { ShowFightInfo(); }}, {"更多选项", [this] {
                    Show("影塔·百战 · 战斗中", "", {{"保存并退出", [this] { ShowSaveExitConfirm(); }}, {"本层问题", [this] { ShowSkipConfirm(); }}, {"返回", [this] { ShowMain(); }}});
                }}});
                else Show("影塔·百战", "", {{"下一层", [this] { SendNext(); }}, {"整备", [this] { ShowUpgrade(); }}, {"更多选项", [this] {
                    Show("影塔·百战", "", {{"保存并退出", [this] { ShowSaveExitConfirm(); }}, {"本层问题", [this] { ShowSkipConfirm(); }}, {"返回", [this] { ShowMain(); }}});
                }}});
            });
        }
    }

    void Show(const std::string& header, const std::string& body, std::vector<Option> opts, std::optional<Rows> rows = std::nullopt) {
        if (opts.empty()) return;
        shopConfirmFocusPending_ = false;
        ClearRows();
        if (opts.size() > kMaxButtons + 1) {
            pbz::log::Info("影塔界面：「{}」有 {} 个选项，对话框只显示 {} 个，截掉多余的", header, opts.size(), kMaxButtons);
            opts.resize(kMaxButtons);
        }
        UObject* textCdo = TextCdo();
        if (!asyncCdo_) asyncCdo_ = Cdo(asyncCls_);
        UObject* pawn = PlayerPawn();
        if (!textCdo || !asyncCdo_ || !pawn) { pbz::log::Info("影塔界面：对话框无法弹出（文本库 {} 异步 {} 玩家 {}）", !!textCdo, !!asyncCdo_, !!pawn); return; }
        Args a(yesNo_);
        std::memcpy(a.At("InWorldContextObject"), &pawn, 8);
        const auto message = header + "\n\n" + body;
        if (!MakeText(header, a.At("Title")) || !MakeText(tower::rewardui::Compact(message), a.At("Message"))) return;  // 先按原文压缩，再由 MakeText 在显示边界翻译
        pe::Call(asyncCdo_, yesNo_, a.buf.data());
        UObject* action = ue::Get<UObject*>(a.At("ReturnValue"));
        UObject* desc = action ? ue::Get<UObject*>(action + actDesc_) : nullptr;
        if (!desc) { pbz::log::Info("影塔界面：没拿到对话框描述"); return; }
        struct Arr { uint8_t* data; int32_t num, max; };
        Arr arr = ue::Get<Arr>(desc + descActions_);
        if (!arr.data || arr.num < 1) { pbz::log::Info("影塔界面：按钮数组为空"); return; }
        const int n = std::min<int>({static_cast<int>(opts.size()), arr.max, static_cast<int>(kMaxButtons)});
        for (int i = 0; i < n; ++i) {
            uint8_t* e = arr.data + i * kActionSize;
            const uint8_t result = static_cast<uint8_t>(i);  // 0 确认 1 拒绝 2 取消（3 = Killed 不画按钮）
            ue::Write(e, &result, 1);
            uint8_t text[kTextSize];
            if (!MakeText(opts[i].label, text)) return;
            ue::Write(e + 8, text, kTextSize);
        }
        if (n != arr.num) { int32_t num = n; ue::Write(desc + descActions_ + 8, &num, 4); }
        if (n < std::min<int>(static_cast<int>(opts.size()), static_cast<int>(kMaxButtons))) {
            pbz::log::Info("影塔界面：按钮数组余量只有 {}，少显示选项", arr.max);
            opts.resize(n);
        }
        desc_ = desc;
        opts_ = std::move(opts);
        open_ = true;
        towerwidgets::OverlayBusy(true);
        openedAt_ = GetTickCount64();
        if (opts_.size() == 4) fourthDeadline_ = openedAt_ + 2500;
        padFocused_ = false;
        gate_.Open(openedAt_, ConfirmInputs());  // 新输入门：打开前按住 / 连击的确认键不算选择
        if (rows) { rows_ = std::move(*rows); rowsHeader_ = header; rowsBody_ = body; rowsDeadline_ = openedAt_ + 2500; }
        pe::Call(action, activate_, nullptr);
        pbz::log::Info("影塔界面：弹出「{}」，{} 个按钮（数组余量 {}）{}", header, n, arr.max, rows_ ? "，带原生按钮列" : "");
    }

    // ---- 钟菜单入口 ----
    // 钟菜单蓝图类会随回标题 / 读档重新加载（新的类与函数对象）：观察挂在旧函数上就再也收不到打开事件
    // （13f7e98 实机：载入塔档后敲钟没有任何"钟菜单打开"日志）。每 5 秒核对一次，类或函数换了就摘掉旧观察重新挂
    bool BellHookStale() {
        if (observeBell_ <= 0) return false;
        if (!ue::Live(bellCls_) || !ue::Live(bellActivated_) || ue::ClassOf(bellActivated_) == nullptr) return true;
        return towerperf::ClassNamed("W_BellMenu_C") != bellCls_;
    }
    void UnhookBell() {
        if (observeBell_ > 0) pe::Unobserve(observeBell_);
        if (observeBell2_ > 0) pe::Unobserve(observeBell2_);
        observeBell_ = observeBell2_ = 0;
        bellCls_ = bellActivated_ = nullptr;
        bellButtons_.clear();
    }
    void HookBellMenu() {
        if (GetTickCount64() < nextBellTry_) return;
        nextBellTry_ = GetTickCount64() + 5000;  // 钟菜单类第一次打开钟时才加载：每 5 秒试一次
        if (observeBell_ && BellHookStale()) { pbz::log::Info("影塔界面：钟菜单类已重新加载，重新挂接"); UnhookBell(); }
        if (observeBell_) return;
        bellCls_ = towerperf::ClassNamed("W_BellMenu_C");
        if (!bellCls_) return;
        bellBox_ = ue::PropOffset(bellCls_, "MainBox");
        bellClose_ = ue::PropOffset(bellCls_, "CloseBtn");
        bellTeleport_ = ue::PropOffset(bellCls_, "TeleportBtn");
        auto* activated = pe::FindFunction(bellCls_, "BP_OnActivated");
        auto* activate = pe::FindFunction(bellCls_, "Activate");  // 蓝图自己的 Activate，打开时紧跟 BP_OnActivated（2026-10-30 调用记录）
        if (bellBox_ < 0 || bellClose_ < 0 || bellTeleport_ < 0 || !activated) { pbz::log::Info("影塔界面：钟菜单字段缺失，入口不可用"); observeBell_ = -1; return; }
        // 钟菜单每次打开都是新建的实例（调用记录里新旧两个实例）
        auto onOpen = [this](UObject* obj, void*) {
            if (!bellCls_ || !ue::IsA(obj, bellCls_)) return;
            if (!bellQueued_.Add(obj)) return;  // 同一次打开会来两个回调：只排一次
            pbz::gamethread::Post([this, obj] { bellQueued_.Done(obj); if (ue::Live(obj)) AddBellButton(obj); });  // 激活流程走完再加
        };
        observeBell_ = pe::Observe(activated, onOpen);
        if (activate) observeBell2_ = pe::Observe(activate, onOpen);
        bellActivated_ = activated;
        pbz::log::Info("影塔界面：已挂上钟菜单（{}{}）", ue::ObjName(ue::ClassOf(activated)), activate ? " + Activate" : "");
    }
    // 调一个只有一个对象 / 整数参数（偏移 0）的函数，返回对象返回值
    UObject* CallObj(UObject* obj, const char* fn, UObject* arg = nullptr, const int* intArg = nullptr) {
        auto* f = obj ? pe::FindFunction(ue::ClassOf(obj), fn) : nullptr;
        if (!f) return nullptr;
        Args a(f);
        if (arg) std::memcpy(a.buf.data(), &arg, 8);
        if (intArg) std::memcpy(a.buf.data(), intArg, 4);
        pe::Call(obj, f, a.buf.data());
        return ue::Get<UObject*>(a.At("ReturnValue"));
    }
    bool InBox(UObject* box, UObject* w) {
        auto* countFn = pe::FindFunction(ue::ClassOf(box), "GetChildrenCount");
        if (!countFn) return false;
        Args c(countFn);
        pe::Call(box, countFn, c.buf.data());
        const int count = ue::Get<int32_t>(c.At("ReturnValue"));
        for (int i = 0; i < count && i < 32; ++i)
            if (CallObj(box, "GetChildAt", nullptr, &i) == w) return true;
        return false;
    }
    // 钟菜单：保留原生内容，只加影塔自己的项。塔外：「影塔·百战」；塔内（独立塔档里）：「退出影塔」「铸魂」（用户 2026-10-31）
    struct BellEntry { UObject* button; int action; };  // action：0 主菜单 / 1 退出影塔 / 2 铸魂
    void AddBellButton(UObject* menu) {
        UObject* box = ue::Get<UObject*>(menu + bellBox_);
        UObject* closeBtn = ue::Get<UObject*>(menu + bellClose_);
        UObject* sample = ue::Get<UObject*>(menu + bellTeleport_);
        std::erase_if(bellButtons_, [](const auto& kv) { return !ue::Live(kv.first); });  // 关掉的旧钟菜单实例
        if (!bellButtons_.count(menu)) pbz::log::Info("影塔界面：钟菜单打开（{}）", ue::ObjName(menu));
        if (!box || !sample) { pbz::log::Info("影塔界面：钟菜单还没有按钮容器（MainBox {} 传送 {}）", !!box, !!sample); return; }
        if (auto it = bellButtons_.find(menu); it != bellButtons_.end() && !it->second.empty() && InBox(box, it->second[0].button)) return;  // 已经加过
        // Native PBZButtonGroup owns hover/selection transitions and controller navigation.
        // Adding only to MainBox leaves a freshly created button in its preview appearance.
        auto* getGroup = pe::FindFunction(ue::ClassOf(menu), "GetButtonGroupByID");
        if (!getGroup) return;
        Args groupArgs(getGroup);
        const auto groupId = towerwidgets::Name("BellMenu");
        std::memcpy(groupArgs.At("ID"), &groupId, 8);
        pe::Call(menu, getGroup, groupArgs.buf.data());
        auto* group = ue::Get<UObject*>(groupArgs.At("ReturnValue"));
        if (!group || !ue::Live(group) || !pe::FindFunction(ue::ClassOf(group), "AddWidget") || !pe::FindFunction(ue::ClassOf(group), "RemoveWidget")) {
            pbz::log::Info("影塔界面：原生钟按钮组不可用，不插入无导航的按钮"); return;
        }
        const bool inside = towerapi::TripPhase() == static_cast<int>(tower::trip::Phase::Inside);
        std::vector<std::pair<std::string, int>> want;
        if (inside) { want.push_back({"退出影塔", 1}); want.push_back({"铸魂", 2}); }
        else want.push_back({"影塔·百战", 0});
        std::vector<std::array<uint8_t, kTextSize>> texts(want.size());
        for (size_t i = 0; i < want.size(); ++i)
            if (!MakeText(want[i].first, texts[i].data())) return;  // 文本不可用：这次不加按钮（下次打开钟再试），不创建半截控件
        if (!wblCls_) wblCls_ = towerperf::ClassNamed("WidgetBlueprintLibrary");
        if (wblCdo_ && !ue::Live(wblCdo_)) wblCdo_ = nullptr;
        if (wblCls_ && !ue::Live(wblCls_)) { wblCls_ = towerperf::ClassNamed("WidgetBlueprintLibrary"); wblCdo_ = nullptr; }
        if (wblCls_ && !wblCdo_) wblCdo_ = Cdo(wblCls_);
        auto* create = wblCls_ ? pe::FindFunction(wblCls_, "Create") : nullptr;
        if (!wblCdo_ || !create) { pbz::log::Info("影塔界面：WidgetBlueprintLibrary.Create 不可用（{} {}）", !!wblCdo_, !!create); return; }
        // 放在"离开"之前：没有按位置插入的接口，先拿掉"离开"、加我们的、再把"离开"加回去
        bool moveClose = false;
        if (closeBtn && InBox(box, closeBtn)) {
            auto* rm = pe::FindFunction(ue::ClassOf(box), "RemoveChild");
            if (rm) { Args r(rm); std::memcpy(r.buf.data(), &closeBtn, 8); pe::Call(box, rm, r.buf.data()); moveClose = ue::Get<uint8_t>(r.At("ReturnValue")) != 0; }
        }
        auto addTo = [&](UObject* w) { return CallObj(box, "AddChildToVerticalBox", w) || CallObj(box, "AddChild", w); };  // 容器不一定是 VerticalBox
        if (moveClose) CallObj(group, "RemoveWidget", closeBtn);
        std::vector<BellEntry> added;
        for (size_t i = 0; i < want.size(); ++i) {
            Args a(create);
            UObject* cls = ue::ClassOf(sample);
            std::memcpy(a.At("WorldContextObject"), &menu, 8);
            std::memcpy(a.At("WidgetType"), &cls, 8);
            if (auto* pc = towerwidgets::Controller()) std::memcpy(a.At("OwningPlayer"), &pc, 8);
            pe::Call(wblCdo_, create, a.buf.data());
            UObject* btn = ue::Get<UObject*>(a.At("ReturnValue"));
            if (!btn) { pbz::log::Info("影塔界面：钟菜单按钮创建失败"); break; }
            // 文字：SetWidgetText(FlowChoiceData{ChoiceText, ChoiceIndex, Last})
            if (auto* set = pe::FindFunction(ue::ClassOf(btn), "SetWidgetText")) {
                Args t(set);
                uint8_t* data = t.At("InText");
                std::memcpy(data, texts[i].data(), kTextSize);
                const int32_t index = 99 + static_cast<int32_t>(i);
                std::memcpy(data + 0x10, &index, 4);
                pe::Call(btn, set, t.buf.data());
            } else pbz::log::Info("影塔界面：钟菜单按钮没有 SetWidgetText（{}）", ue::ObjName(ue::ClassOf(btn)));
            if (!addTo(btn)) { pbz::log::Info("影塔界面：钟菜单容器 {} 加按钮失败", ue::ObjName(ue::ClassOf(box))); continue; }
            CallObj(group, "AddWidget", btn);
            if (auto* appearance = pe::FindFunction(ue::ClassOf(btn), "DoSelectedAppearance")) {
                Args state(appearance); // false, exactly as original bell initializes unselected entries
                pe::Call(btn, appearance, state.buf.data());
            }
            added.push_back({btn, want[i].second});
        }
        if (moveClose) { addTo(closeBtn); CallObj(group, "AddWidget", closeBtn); }
        bellButtons_[menu] = std::move(added);
        pbz::log::Info("影塔界面：钟菜单加入{}{}", inside ? "「退出影塔」「铸魂」" : "「影塔·百战」", moveClose ? "（在离开之前）" : "");
    }

    // 描述对象和对话框都可能随关闭、切图、GC 被释放：每次取用都在对象表里核对身份，失效就当对话框已关闭。
    // FWeakObjectPtr 的序列号在这版引擎里的位置未核实，所以不比序列号，改为"槽位仍指向它 + 未销毁 + 类对"
    UObject* Dialog() {
        if (!desc_) return nullptr;
        if (!ue::Live(desc_)) { desc_ = nullptr; return nullptr; }
        const auto weak = ue::Get<uint64_t>(desc_ + descDialog_);  // FWeakObjectPtr{ObjectIndex, SerialNumber}
        const int index = static_cast<int32_t>(weak & 0xFFFFFFFF);
        UObject* d = index > 0 ? ue::Obj(index) : nullptr;
        return d && ue::Live(d) && ue::IsA(d, screenCls_) ? d : nullptr;
    }
    // Esc / 点空白关闭：对话框不再激活就当作关闭了，不算选择
    void CheckClosed() {
        if (!open_) return;
        Pad pad{}; const auto now = GetTickCount64();
        const bool b = Foreground() && ((ReadPad(&pad) && (pad.buttons & 0x2000)) ||
                         (padBackAt_ && now >= padBackAt_ && now - padBackAt_ < 1000));
        // Normally wait out the opening animation. A deliberate B press after
        // the input gate's minimum opening time may close it sooner.
        const auto age = now - openedAt_;
        if (age < 1500 && !(b && age >= tower::inputgate::Gate::kMinOpenMs)) return;
        UObject* d = Dialog();
        bool active = false;
        if (d) { uint8_t r[16] = {}; pe::Call(d, isActivated_, r); active = r[0] != 0; }
        if (!active) {
            // CommonUI may close on B before PadTick observes it. If no native
            // option ran, preserve the dialog's explicit Back action.
            std::function<void()> back;
            if (b && !pendingAct_) for (const auto& opt : opts_)
                if (opt.label == "返回" || opt.label == "知道了" || opt.label == "继续战斗" || opt.label == "取消") { back = opt.act; break; }
            open_ = false; gate_.Close(); ClearRows();
            if (back) { padBackAt_ = 0; pendingAct_ = std::move(back); pendingActAt_ = now + 700; pbz::log::Info("影塔界面：原生关闭后执行手柄返回"); }
            else pbz::log::Info("影塔界面：对话框已关闭（未选择）");
        }
    }

    int DialogIndex(UObject* button, int* count = nullptr) {
        UObject* d = Dialog();
        UObject* box = d ? ue::Get<UObject*>(d + screenEntries_) : nullptr;
        if (!box) return -1;
        Args a(entries_);
        pe::Call(box, entries_, a.buf.data());
        struct Arr { UObject** data; int32_t num, max; };
        const Arr list = ue::Get<Arr>(a.At("ReturnValue"));
        if (count) *count = list.num;
        for (int i = 0; list.data && i < list.num && i < 8; ++i)
            if (ue::Get<UObject*>(list.data + i) == button) return i;
        return -1;
    }
    void OnClicked(UObject* button) {
        for (auto& [menu, entries] : bellButtons_)
            for (auto& e : entries)
                if (e.button == button) {  // 钟菜单里我们加的项：关掉钟菜单，稍后弹对应的影塔菜单
                    pbz::log::Info("影塔界面：钟菜单「{}」被点击", e.action == 1 ? "退出影塔" : e.action == 2 ? "铸魂" : "影塔·百战");
                    UObject* m = menu;
                    const int action = e.action;
                    pbz::gamethread::Post([this, m, action] {
                        if (!ue::Live(m)) return;
                        if (auto* close = pe::FindFunction(ue::ClassOf(m), "ClickCloseButton")) pe::Call(m, close, nullptr);
                        if (action == 0) pendingShow_ = GetTickCount64() + 700;
                        else Then([this, action] { if (action == 1) ShowLeave(); else ShowSnap(0); });
                    });
                    return;
                }
        if (!open_) return;
        if (OnRowClicked(button)) return;
        int count = 0;
        const int index = DialogIndex(button, &count);
        if (index < 0) {  // 不是这个对话框的按钮（或列表没取到）
            pbz::log::Info("影塔界面：点击的按钮不在对话框按钮列表里（列表 {} 个）", count);
            return;
        }
        open_ = false;
        gate_.Close();
        towerwidgets::OverlayBusy(false);
        ClearRows();
        if (index >= static_cast<int>(opts_.size())) { pbz::log::Info("影塔界面：点了第 {} 个按钮，超出选项", index + 1); return; }
        pbz::log::Info("影塔界面：选择第 {} 项「{}」", index + 1, opts_[index].label);
        // 等这个对话框的关闭动画走完再执行：马上弹下一个会被一起关掉（2026-10-30 实测"背包"一闪就没）
        pendingAct_ = opts_[index].act;
        pendingActAt_ = GetTickCount64() + 700;
    }

    UObject *textLib_ = nullptr, *asyncCls_ = nullptr, *descCls_ = nullptr, *btnCls_ = nullptr, *screenCls_ = nullptr, *activatable_ = nullptr, *entryCls_ = nullptr;
    UObject *toText_ = nullptr, *yesNo_ = nullptr, *activate_ = nullptr, *clicked_ = nullptr, *entries_ = nullptr, *isActivated_ = nullptr;
    UObject *asyncCdo_ = nullptr, *desc_ = nullptr;
    UObject* fourthButton_ = nullptr;
    unsigned long long fourthDeadline_ = 0;
    tower::ui::Lazy<uint8_t> text_;      // 文本库默认对象
    tower::ui::Once bellQueued_;         // 已排队加按钮的钟菜单实例
    int actDesc_ = -1, descActions_ = -1, descDialog_ = -1, screenEntries_ = -1, observeBase_ = 0, observeOverride_ = 0;
    std::vector<Option> opts_;
    unsigned long long lastPadAt_ = 0, nextFocusFix_ = 0, padBackAt_ = 0;
    bool shopSeen_ = false;
    bool shopDismissed_ = false;
    bool shopConfirmFocusPending_ = false;
    bool padChord_ = false, padBack_ = false, padFocused_ = false, padStick_ = false;
    WORD padPrev_ = 0, padDir_ = 0;
    unsigned long long padConfirmAt_ = 0, padConfirmReleasedAt_ = 0, padNavAt_ = 0;
    unsigned uiClicks_ = 0, padClicksAtPress_ = 0;
    int padNavDir_ = 1, padAssistLogs_ = 0;
    UObject* padNavFrom_ = nullptr;
    UObject* padLastFocus_ = nullptr;
    unsigned long long padNextFocusSample_ = 0;
    UObject* padConfirmTarget_ = nullptr;
    POINT cursor_{};
    bool cursorValid_ = false;
    unsigned long long lastMouseAt_ = 0;
    int padFocusLogs_ = 0;
    bool open_ = false, insertDown_ = false, rewardChoosing_ = false;
    tower::hotkey::Watch hotkey_;  // 自定义打开键（tower_hotkey.h）
    tower::inputgate::Gate gate_;
    int gateLogs_ = 0;
    std::function<void()> pendingAct_;  // 点选之后延迟执行的动作
    unsigned long long pendingActAt_ = 0;
    unsigned long long openedAt_ = 0, pendingShow_ = 0, nextBellTry_ = 0, nextHousekeep_ = 0;
    UObject *bellCls_ = nullptr, *bellActivated_ = nullptr, *wblCls_ = nullptr, *wblCdo_ = nullptr;
    int bellBox_ = -1, bellClose_ = -1, bellTeleport_ = -1, observeBell_ = 0, observeBell2_ = 0;
    std::unordered_map<UObject*, std::vector<BellEntry>> bellButtons_;  // 钟菜单实例 → 我们加的按钮
    tower::Phase lastPhase_ = tower::Phase::Idle;
    int errSeen_ = 0; std::string pendingErr_; unsigned long long pendingErrAt_ = 0;
};

}  // namespace

namespace towerui {
void Register() { pbz::AddFeature(std::make_unique<TowerUi>()); }
}  // namespace towerui
