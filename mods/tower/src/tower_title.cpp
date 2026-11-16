// 自动切档的游戏端：原生"返回标题"与"继续"（证据等级：静态，来自游戏 JS 源码与 SDK，待实机）。
//   返回标题 = TsHelper.returnStart（死亡菜单 / 结局页同一入口）：PBZGameInstance.ChangeGameType(StartMenu)，
//     PBZBlueprintFunctionLibrary.GetGameDataPolicy().StartMenuPolicy.GetCorrectStartMenuMapName，GameplayStatics.OpenLevel(图名)。
//   继续     = WB_StartMenu_V2 的 ContinueGame：PBZGameInstance.LoadGame(GetContinueSlotName) 成功后
//     SetDemoVersionLoadingScreen(false)，GameplayStatics.OpenLevel(LevelName, true, GameModeOptions)。
// 没有走开始菜单的 JS 动画 / 目的地 loading 图（DestinationLoading 是 JS），加载屏用 GameInstance 默认的那张。
// 世界上下文一律用 GameInstance（跨世界存活），子系统 / 控件每次重查。
#include <windows.h>

#include <array>
#include <cstring>
#include <format>
#include <string>

#include "../../../src/core/host/log.h"
#include "../../../src/core/sdk/pbz_module.h"
#include "tower_title.h"
#include "tower_widgets.h"
#include "tower_perf.h"

namespace towertitle {
namespace {
namespace ue = pbz::ue;
namespace pe = pbz::pe;
using ue::UObject;

struct FStr { wchar_t* data; int32_t num, max; };
struct Args {
    UObject* fn; std::array<uint8_t, 0x300> bytes{};
    uint8_t* At(const char* name) { const int o = fn ? ue::PropOffset(fn, name) : -1; return o >= 0 && o < 0x280 ? bytes.data() + o : nullptr; }
};
FStr Str(std::wstring& w) { return w.empty() ? FStr{nullptr, 0, 0} : FStr{w.data(), static_cast<int32_t>(w.size() + 1), static_cast<int32_t>(w.size() + 1)}; }
std::wstring ReadStr(const uint8_t* p) {
    FStr s; std::memcpy(&s, p, sizeof s);
    if (!s.data || s.num <= 1 || s.num > 4096) return {};
    std::wstring out(static_cast<size_t>(s.num - 1), L'\0');
    return ue::Read(s.data, out.data(), out.size() * sizeof(wchar_t)) ? out : std::wstring();
}
std::string Narrow(const std::wstring& w) {  // 关卡名 / 槽名是 ASCII（只用于日志）
    std::string s;
    for (wchar_t c : w) s += c < 0x80 ? static_cast<char>(c) : '?';
    return s;
}

UObject* Live(const char* cls) {
    auto* c = towerperf::ClassNamed(cls);
    UObject* found = nullptr;
    if (c) ue::ForEach([&](int, UObject* o) { if (!ue::IsDefault(o) && ue::IsA(o, c) && ue::Live(o)) { found = o; return false; } return true; });
    return found;
}
UObject* GameInstance() { return Live("PBZGameInstance"); }
UObject* Fn(UObject* o, const char* f) { return o && ue::Live(o) ? pe::FindFunction(ue::ClassOf(o), f) : nullptr; }
uint64_t ToName(std::wstring w) {
    auto* lib = towerwidgets::Default(towerperf::ClassNamed("KismetStringLibrary"));
    auto* f = Fn(lib, "Conv_StringToName");
    if (!f) return 0;
    Args a{f};
    if (!a.At("InString") || !a.At("ReturnValue")) return 0;
    const FStr s = Str(w);
    std::memcpy(a.At("InString"), &s, sizeof s);
    pe::Call(lib, f, a.bytes.data());
    return ue::Get<uint64_t>(a.At("ReturnValue"));
}
bool OpenLevel(UObject* ctx, uint64_t level, std::wstring options, std::string* why) {
    auto* lib = towerwidgets::Default(towerperf::ClassNamed("GameplayStatics"));
    auto* f = Fn(lib, "OpenLevel");
    Args a{f};
    if (!f || !a.At("WorldContextObject") || !a.At("LevelName") || !a.At("bAbsolute") || !a.At("Options")) { *why = "OpenLevel 接口不可用"; return false; }
    std::memcpy(a.At("WorldContextObject"), &ctx, 8);
    std::memcpy(a.At("LevelName"), &level, 8);
    *a.At("bAbsolute") = 1;
    const FStr s = Str(options);
    std::memcpy(a.At("Options"), &s, sizeof s);
    pe::Call(lib, f, a.bytes.data());
    return true;
}
bool ReadBool(UObject* o, const char* name, bool* value) {
    auto* f = Fn(o, name); if (!f) return false;
    Args a{f}; if (!a.At("ReturnValue")) return false;
    pe::Call(o, f, a.bytes.data()); *value = *a.At("ReturnValue") != 0; return true;
}
// 活动中的 WB_TempLoadingScreen（同入场画面门）
bool ScreenUp(bool* known) {
    *known = true;
    auto* c = towerperf::ClassNamed("WB_TempLoadingScreen_C");
    if (!c) return false;
    bool up = false;
    ue::ForEach([&](int, UObject* o) {
        if (ue::IsDefault(o) || !ue::IsA(o, c) || !ue::Live(o)) return true;
        bool active = true;
        if (!ReadBool(o, "IsActivated", &active)) { *known = false; return false; }
        if (active) { up = true; return false; }
        return true;
    });
    return up;
}
uint64_t g_onlyLevel = 0;
}  // namespace

bool Quiet() {
    auto* gi = GameInstance();
    if (!gi) return false;
    bool known = true;
    if (ScreenUp(&known) || !known) { g_onlyLevel = 0; return false; }
    const int off = ue::PropOffset(ue::ClassOf(gi), "IsLoadingLevel");
    if (off < 0) return false;
    if (!ue::Get<uint8_t>(gi + off)) { g_onlyLevel = 0; return true; }
    // 同入场画面门：只剩 IsLoadingLevel 卡住超过 15 秒时忽略它（直接读的字段，可能是位域）
    const uint64_t now = GetTickCount64();
    if (!g_onlyLevel) g_onlyLevel = now;
    return now - g_onlyLevel >= 15000;
}

bool MenuReady() {
    auto* gi = GameInstance();
    bool warning = true;
    if (!gi || !ReadBool(gi, "IsStartupWarningActive", &warning) || warning) return false;
    return Live("WB_StartMenu_V2_C") != nullptr;
}

bool ToTitle(std::string* why) {
    auto* gi = GameInstance();
    if (!gi) { *why = "GameInstance 未就绪"; return false; }
    auto* change = Fn(gi, "ChangeGameType");
    Args c{change};
    if (!change || !c.At("GameType")) { *why = "ChangeGameType 接口不可用"; return false; }
    auto* lib = towerwidgets::Default(towerperf::ClassNamed("PBZBlueprintFunctionLibrary"));
    auto* getPolicy = Fn(lib, "GetGameDataPolicy");
    Args g{getPolicy};
    if (!getPolicy || !g.At("ReturnValue")) { *why = "GetGameDataPolicy 接口不可用"; return false; }
    pe::Call(lib, getPolicy, g.bytes.data());
    auto* policy = ue::Get<UObject*>(g.At("ReturnValue"));
    const int off = policy && ue::Live(policy) ? ue::PropOffset(ue::ClassOf(policy), "StartMenuPolicy") : -1;
    auto* start = off >= 0 ? ue::Get<UObject*>(policy + off) : nullptr;
    auto* getMap = Fn(start, "GetCorrectStartMenuMapName");
    Args m{getMap};
    if (!getMap || !m.At("MapName")) { *why = "开始菜单策略不可用"; return false; }
    pe::Call(start, getMap, m.bytes.data());
    const uint64_t map = ue::Get<uint64_t>(m.At("MapName"));
    if (!map) { *why = "开始菜单图名为空"; return false; }
    *c.At("GameType") = 0;  // EStartGameType::StartMenu
    pe::Call(gi, change, c.bytes.data());
    if (!OpenLevel(gi, map, L"", why)) return false;
    pbz::log::Info("影塔自动切档：已请求返回标题（原生 returnStart 路径）");
    return true;
}

bool Continue(std::string* why) {
    auto* gi = GameInstance();
    auto* sub = Live("PBZSaveLoadSubsystem");
    auto* slotFn = Fn(sub, "GetContinueSlotName");
    auto* load = Fn(gi, "LoadGame");
    Args s{slotFn}, l{load};
    if (!gi || !slotFn || !load || !s.At("ReturnValue") || !l.At("SlotName") || !l.At("LoadGameResultParam")) { *why = "继续游戏接口不可用"; return false; }
    pe::Call(sub, slotFn, s.bytes.data());
    std::wstring slot = ReadStr(s.At("ReturnValue"));
    if (slot.empty()) { *why = "没有续玩槽"; return false; }
    const FStr fs = Str(slot);
    std::memcpy(l.At("SlotName"), &fs, sizeof fs);
    pe::Call(gi, load, l.bytes.data());  // 结果里引擎分配的字符串 / JSON 对象不释放（每次切档一次）
    const uint8_t* r = l.At("LoadGameResultParam");
    if (r[0] != 0) { *why = "续玩槽载入失败（" + Narrow(slot) + "）"; return false; }
    const std::wstring level = ReadStr(r + 0x08), options = ReadStr(r + 0x18);
    if (level.empty()) { *why = "续玩槽没有关卡名"; return false; }
    if (auto* screen = Fn(gi, "SetDemoVersionLoadingScreen")) {
        Args d{screen};
        if (d.At("bInTutorial")) { *d.At("bInTutorial") = 0; pe::Call(gi, screen, d.bytes.data()); }
    }
    const uint64_t name = ToName(level);
    if (!name) { *why = "关卡名转换失败"; return false; }
    if (!OpenLevel(gi, name, options, why)) return false;
    pbz::log::Info("影塔自动切档：已请求继续（槽 {}，关卡 {}）", Narrow(slot), Narrow(level));
    return true;
}

}  // namespace towertitle
