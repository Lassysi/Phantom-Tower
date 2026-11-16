// 影塔存档守卫（游戏线程；配合 tower_sandbox.cpp 的文件沙盒，三层）：
// 1. 原生存档锁：PBZSaveLoadSubsystem.EnterSaveVeto("PBZTower", ModeGate)。塔内游戏自己就不存档；锁是游戏里的状态，
//    运行库被卸掉、钩子没了以后它仍然在，退出游戏时也挡着（模块停止时只要内存还是塔内状态就不解锁）。
// 2. 进塔基准：开沙盒时 SaveJsonObjectToString 记下真进度（内存 + %LOCALAPPDATA%\PBZTower\baseline.json）。
// 3. 出塔还原：ReplaceSaveString 把基准放回游戏内存。2026-10-31 Codex 实测：同进程回标题后 LoadGame / JsonLoadFromDisk
//    读的是内存缓存，只换磁盘文件读回来的仍是塔内状态；所以必须换内存里的那份。
// 证据等级：静态（函数签名来自 SDK；ReplaceSaveString 是否同时写盘、锁是否挡住退出保存，需实机验证）。
#include <windows.h>

#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../../../src/core/host/log.h"
#include "../../../src/core/sdk/pbz_module.h"
#include "tower_perf.h"

namespace towersb { void SetDirty(bool); bool Dirty(); bool Active(); int Session(); bool Tainted(); uint64_t Activity(); }

namespace {

using pbz::ue::UObject;
namespace ue = pbz::ue;
namespace pe = pbz::pe;

struct FStr { wchar_t* data; int32_t num, max; };  // FString（TArray<TCHAR>）

UObject *g_cls = nullptr, *g_sub = nullptr, *g_strCdo = nullptr, *g_toName = nullptr;
bool g_veto = false;
std::wstring g_base;  // 进塔基准（游戏自己的存档 JSON 文本）
int g_baseSession = -1;  // 基准属于哪一轮沙盒
bool g_foreign = false;  // 内存里放过塔内存档、还没核验回到原档：这时内存不是真进度，不能拿来当基准
std::vector<int> g_saveObs;  // 原生保存入口的观察令牌

std::vector<uint8_t> Buf() { return std::vector<uint8_t>(0x400, 0); }
uint8_t* Arg(UObject* fn, std::vector<uint8_t>& b, std::string_view name) {
    const int o = ue::PropOffset(fn, name);
    static uint8_t sink[0x100];
    return o >= 0 && o + 0x100 <= static_cast<int>(b.size()) ? b.data() + o : sink;
}

UObject* Sub(std::string* why) {
    if (!g_cls) g_cls = towerperf::ClassNamed("PBZSaveLoadSubsystem");
    if (!g_cls) { *why = "存档子系统类未加载"; return nullptr; }
    if (g_sub && ue::ClassOf(g_sub) == g_cls) return g_sub;
    g_sub = nullptr;
    ue::ForEach([&](int, UObject* o) { if (ue::ClassOf(o) == g_cls && !ue::IsDefault(o)) { g_sub = o; return false; } return true; });
    if (!g_sub) *why = "存档子系统实例未找到";
    return g_sub;
}
UObject* Fn(const char* name, std::string* why) {
    auto* f = g_cls ? pe::FindFunction(g_cls, name) : nullptr;
    if (!f) *why = std::string("存档接口缺失：") + name;
    return f;
}
uint64_t ToName(const std::wstring& w) {
    if (!g_toName) {
        auto* lib = towerperf::ClassNamed("KismetStringLibrary");
        g_toName = lib ? pe::FindFunction(lib, "Conv_StringToName") : nullptr;
        if (lib) ue::ForEach([&](int, UObject* o) { if (ue::ClassOf(o) == lib && ue::IsDefault(o)) { g_strCdo = o; return false; } return true; });
    }
    if (!g_toName || !g_strCdo) return 0;
    auto b = Buf();
    FStr s{const_cast<wchar_t*>(w.c_str()), static_cast<int32_t>(w.size() + 1), static_cast<int32_t>(w.size() + 1)};
    std::memcpy(Arg(g_toName, b, "InString"), &s, sizeof s);
    pe::Call(g_strCdo, g_toName, b.data());
    return ue::Get<uint64_t>(Arg(g_toName, b, "ReturnValue"));
}
std::wstring ReadFStr(const uint8_t* p) {
    FStr s;
    std::memcpy(&s, p, sizeof s);
    if (!s.data || s.num <= 1 || s.num > (1 << 28)) return {};
    std::wstring out(static_cast<size_t>(s.num - 1), L'\0');
    if (!ue::Read(s.data, out.data(), out.size() * sizeof(wchar_t))) return {};
    return out;  // 引擎分配的那份不释放（模块没有引擎分配器；每局只取一次）
}

std::wstring BaseFile() {
    wchar_t base[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return {};
    std::wstring dir = std::wstring(base) + L"\\PBZTower";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\baseline.json";
}
void WriteBase() {
    const auto path = BaseFile();
    if (path.empty()) return;
    const int n = WideCharToMultiByte(CP_UTF8, 0, g_base.data(), static_cast<int>(g_base.size()), nullptr, 0, nullptr, nullptr);
    std::string u(static_cast<size_t>(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, g_base.data(), static_cast<int>(g_base.size()), u.data(), n, nullptr, nullptr);
    std::ofstream(path, std::ios::binary | std::ios::trunc) << u;
}

bool CallVeto(bool on, std::string* why) {
    auto* sub = Sub(why);
    auto* fn = sub ? Fn(on ? "EnterSaveVeto" : "ExitSaveVeto", why) : nullptr;
    if (!fn) return false;
    const uint64_t id = ToName(L"PBZTower");
    if (!id) { *why = "名字转换失败"; return false; }
    auto b = Buf();
    std::memcpy(Arg(fn, b, "SourceID"), &id, 8);
    if (on) *Arg(fn, b, "Reason") = 2;  // EPBZSaveVetoReason::ModeGate
    pe::Call(sub, fn, b.data());
    g_veto = on;
    return true;
}

}  // namespace

namespace towergate {

// 原生保存入口观察（锁开着时）：SDK 里 ForceSave 的结果枚举没有"被锁拒绝"，TrySave 才有 Suspended，
// 所以 ModeGate 锁很可能只挡 TrySave。这里只记录谁在锁开着时调了哪个入口，供实机核对；不拦，写盘仍由文件沙盒兜住
void ObserveSaves(bool on) {
    for (int t : g_saveObs) pe::Unobserve(t);
    g_saveObs.clear();
    if (!on || !g_cls) return;
    for (const char* name : {"TrySave", "ForceSave", "JsonSaveToDisk", "SaveAdvancedSaveGameNow", "SaveAdvancedObjectDataAndSync", "ReplaceSaveString"})
        if (auto* f = pe::FindFunction(g_cls, name))
            g_saveObs.push_back(pe::Observe(f, [name](UObject*, void*) {
                pbz::log::Info("影塔存档锁：保护期间游戏调用了 {}（存档锁 {}，沙盒 {}）", name, g_veto ? "开" : "已放开", towersb::Active() ? "开，写盘进影子档" : "关");
            }));
}

bool Veto(bool on, std::string* why) {
    if (on == g_veto) return true;
    if (!CallVeto(on, why)) return false;
    ObserveSaves(on);
    pbz::log::Info("影塔存档锁：{}", on ? "加上（塔内游戏不存档）" : "放开");
    return true;
}

// 出塔收尾第一步：只放开存档锁，文件沙盒保持开着（fa164d1 反例：同时放开时，锁里排队的 Suspended 存档 1.28 秒后写进了真档）。
// 观察保留到关沙盒为止，记录收尾期间原生调了哪个保存入口
bool Release(std::string* why) {
    if (!g_veto) return true;
    if (!CallVeto(false, why)) return false;
    pbz::log::Info("影塔存档锁：放开（文件沙盒仍开；锁里排队 / 空闲触发的原生存档会落进沙盒，等静下来再关沙盒）");
    return true;
}
void StopWatching() { ObserveSaves(false); }
// 原生存档没有未落盘的改动（HasDirtySaveGame / HasDirtyAdvancedSaveGame 都为假）。接口缺失按"不干净"处理
bool SaveClean(std::string* why) {
    auto* sub = Sub(why);
    if (!sub) return false;
    for (const char* name : {"HasDirtySaveGame", "HasDirtyAdvancedSaveGame"}) {
        auto* fn = Fn(name, why);
        if (!fn) return false;
        auto b = Buf();
        pe::Call(sub, fn, b.data());
        if (*Arg(fn, b, "ReturnValue")) { *why = std::string("原生存档仍有未落盘的改动（") + name + "）"; return false; }
    }
    return true;
}

// 记下进塔基准：同一轮沙盒里再开不覆盖（第一份才是真进度）；新一轮沙盒（关过再开）重新取。
// 上次强制关沙盒时内存还是塔内状态：此后内存里的不是真进度，拒绝取基准
bool Snapshot(std::string* why) {
    if (!g_base.empty() && g_baseSession == towersb::Session()) return true;
    if (towersb::Tainted()) { *why = "上次强制关沙盒时游戏内存仍是塔内状态：请退出并重启游戏后再开沙盒取基准"; return false; }
    if (g_foreign) { *why = "游戏内存里还是塔内存档（出塔未核验完成）：不把它当真进度记基准"; return false; }
    g_base.clear();
    auto* sub = Sub(why);
    auto* fn = sub ? Fn("SaveJsonObjectToString", why) : nullptr;
    if (!fn) return false;
    auto b = Buf();
    pe::Call(sub, fn, b.data());
    g_base = ReadFStr(Arg(fn, b, "ReturnValue"));
    if (g_base.size() < 64) { g_base.clear(); *why = "读不到当前进度（存档还没载入？进档后再开）"; return false; }
    g_baseSession = towersb::Session();
    WriteBase();
    towersb::SetDirty(true);  // 从这一刻起内存可能被塔改动；放回基准前不许关沙盒
    pbz::log::Info("影塔进塔基准：已记下 {} 字符（{}）", g_base.size(), "baseline.json");
    return true;
}

// 把一份存档 JSON 放进游戏内存（ReplaceSaveString；回标题 → 继续时载入它）。存档锁可能挡住替换：
// 失败时短暂放开锁再试一次，然后重新加锁（沙盒仍开着，写盘也只进沙盒）。clean：放的是进塔基准（内存回到真进度）
bool Put(const std::wstring& text, bool clean, std::string* why) {
    if (text.size() < 64) { *why = "存档内容为空"; return false; }
    auto* sub = Sub(why);
    auto* fn = sub ? Fn("ReplaceSaveString", why) : nullptr;
    auto* slotFn = sub ? Fn("GetContinueSlotName", why) : nullptr;
    if (!fn || !slotFn) return false;
    if (!towersb::Active()) { *why = "沙盒没开：替换会直接写真档，拒绝"; return false; }
    auto sb = Buf();
    pe::Call(sub, slotFn, sb.data());
    std::wstring slot = ReadFStr(Arg(slotFn, sb, "ReturnValue"));
    std::wstring body = text;  // 参数缓冲引用这份内存，调用期间保持存活
    auto attempt = [&]() {
        auto b = Buf();
        uint8_t* param = Arg(fn, b, "SaveGameParam");
        FStr s{slot.empty() ? nullptr : slot.data(), slot.empty() ? 0 : static_cast<int32_t>(slot.size() + 1), slot.empty() ? 0 : static_cast<int32_t>(slot.size() + 1)};
        std::memcpy(param + 0x78, &s, sizeof s);  // SaveSlotName
        param[0xA9] = 2;                          // IoContext = Restore
        FStr str{body.data(), static_cast<int32_t>(body.size() + 1), static_cast<int32_t>(body.size() + 1)};
        std::memcpy(Arg(fn, b, "String"), &str, sizeof str);
        pe::Call(sub, fn, b.data());
        return *Arg(fn, b, "ReturnValue") == 0;  // ESaveLoadResultType::Success
    };
    bool ok = attempt();
    if (!ok && g_veto) {
        std::string w;
        CallVeto(false, &w);
        ok = attempt();
        CallVeto(true, &w);
    }
    if (!ok) { *why = "ReplaceSaveString 返回失败"; return false; }
    towersb::SetDirty(!clean);
    if (!clean) g_foreign = true;
    pbz::log::Info("影塔存档：{}已放进游戏内存（槽 {}，{} 字符）；回标题 / 继续时载入", clean ? "进塔基准" : "塔内存档", std::string(slot.begin(), slot.end()), text.size());
    return true;
}

// 把基准放回游戏内存（同进程出塔；之后回标题 / 继续游戏读到的是真进度）
bool Restore(std::string* why) {
    if (g_base.empty()) { *why = "没有进塔基准"; return false; }
    return Put(g_base, true, why);
}

// 进塔基准文本：内存里有就用；没有（运行库热换后的新模块）读落盘的 baseline.json
bool BaselineText(std::wstring* out) {
    if (!g_base.empty()) { *out = g_base; return true; }
    std::ifstream f(BaseFile(), std::ios::binary);
    if (!f) return false;
    std::string u((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (u.size() < 64) return false;
    const int n = MultiByteToWideChar(CP_UTF8, 0, u.data(), static_cast<int>(u.size()), nullptr, 0);
    out->assign(static_cast<size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, u.data(), static_cast<int>(u.size()), out->data(), n);
    return n > 0;
}

// 已核验游戏里是原档（出塔完成）
void ConfirmOriginal() { g_foreign = false; }
// 塔档会话没走完（热换后的新模块）：同样不许记基准
void MarkForeign() { g_foreign = true; }

std::string State() {
    return std::format(R"("veto":{},"baseline_chars":{},"baseline_session":{})", g_veto ? "true" : "false", g_base.size(), g_baseSession);
}

// 塔内又有改动（开局 / 开层 / 换兵器）：基准已放回过也重新算脏，出塔前必须再还原一次
void Touch() {
    if (towersb::Active() && !g_base.empty() && g_baseSession == towersb::Session()) towersb::SetDirty(true);
}

// 模块停止（运行库卸载 / 热换）：内存仍是塔内状态时不解锁——钩子马上就没了，锁是最后一道
void OnStop() {
    if (g_veto && towersb::Dirty()) pbz::log::Info("影塔存档锁：运行库停止但内存仍是塔内状态，保留原生存档锁（游戏不会再存档，直到重启游戏）");
    else if (g_veto) { std::string why; Veto(false, &why); }
}

}  // namespace towergate
