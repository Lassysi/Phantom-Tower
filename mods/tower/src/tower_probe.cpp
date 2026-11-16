// 影塔实验工具（开发用，证据等级：未验证）：按名字调用游戏函数，验证塔内成长要用的原生接口；外加成就拦截。
//   {"op":"tower","cmd":"call","cls":"PBZBackPackSubsystem","fn":"GetWeaponLevel","on":"inst",
//    "a1n":"WeaponID","a1t":"name","a1v":"BrutalAxe","a2n":"bIsNormalWeapon","a2t":"bool","a2v":"false"}
//   on：inst（该类第一个非默认实例，默认）/ cdo（函数库）/ player（玩家角色）
//   参数最多 6 个：aNn 参数名，aNt 类型 name|int|bool|float|str|names（逗号分隔 → TArray<FName>）|obj，aNv 值；
//   obj 的值：player（玩家角色）/ asc（玩家 AbilitySystemComponent）/ inst:<类名>（该类第一个实例）
//   回 {"op":"tower","event":"call","ok":…,"ret":"<ReturnValue 前 16 字节十六进制>","int":…,"buf":"<参数缓冲前 0x60 字节>"}
//   {"op":"tower","cmd":"ach","value":"on|off|state"}  塔内屏蔽成就：成就系统 StopListening，并吞掉解锁 / 进度调用
#include <windows.h>

#include <atomic>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "../../../src/core/host/log.h"
#include "../../../src/core/ipc/pipe.h"
#include "../../../src/core/sdk/pbz_module.h"
#include "tower_perf.h"

namespace {

using pbz::ue::UObject;
namespace ue = pbz::ue;
namespace pe = pbz::pe;
namespace attr = pbz::attr;

struct FArr { void* data; int32_t num, max; };

UObject* FirstOf(const std::string& clsName, bool wantDefault) {
    UObject* cls = towerperf::ClassNamed(clsName);
    if (!cls) return nullptr;
    UObject* found = nullptr;
    ue::ForEach([&](int, UObject* o) {
        // 跳过蓝图组件模板（…_GEN_VARIABLE，2026-10-30 能力树探针取到的就是它）
        if (ue::IsA(o, cls) && ue::IsDefault(o) == wantDefault && ue::ObjName(o).find("GEN_VARIABLE") == std::string::npos) { found = o; return false; }
        return true;
    });
    return found;
}
UObject* CallRetObj(UObject* obj, const char* fnName) {
    auto* fn = obj ? pe::FindFunction(ue::ClassOf(obj), fnName) : nullptr;
    if (!fn) return nullptr;
    std::vector<uint8_t> buf(0x40, 0);
    pe::Call(obj, fn, buf.data());
    const int o = ue::PropOffset(fn, "ReturnValue");
    return o >= 0 ? ue::Get<UObject*>(buf.data() + o) : nullptr;
}
UObject* Player();
// 该类里属于玩家角色或玩家控制器的那个组件
UObject* OwnedByPlayer(const std::string& clsName) {
    UObject* cls = towerperf::ClassNamed(clsName);
    UObject* pawn = Player();
    if (!cls || !pawn) return nullptr;
    UObject* pc = CallRetObj(pawn, "GetController");
    UObject* found = nullptr;
    ue::ForEach([&](int, UObject* o) {
        if (!ue::IsA(o, cls) || ue::IsDefault(o)) return true;
        UObject* owner = CallRetObj(o, "GetOwner");
        if (owner && (owner == pawn || owner == pc)) { found = o; return false; }
        return true;
    });
    return found;
}
UObject* Player() {
    for (auto& t : attr::Targets())
        if (t.who == attr::Who::Player && attr::Alive(t)) return t.obj;
    return nullptr;
}
UObject* PlayerAsc() {  // 角色上的 GetAbilitySystemComponent 不是 UFunction：走 AbilitySystemBlueprintLibrary
    UObject* p = Player();
    UObject* lib = towerperf::ClassNamed("AbilitySystemBlueprintLibrary");
    UObject* cdo = lib ? FirstOf("AbilitySystemBlueprintLibrary", true) : nullptr;
    auto* fn = lib ? pe::FindFunction(lib, "GetAbilitySystemComponent") : nullptr;
    if (!p || !cdo || !fn) return nullptr;
    std::vector<uint8_t> buf(0x40, 0);
    const int a = ue::PropOffset(fn, "Actor");
    if (a < 0) return nullptr;
    std::memcpy(buf.data() + a, &p, 8);
    pe::Call(cdo, fn, buf.data());
    const int o = ue::PropOffset(fn, "ReturnValue");
    return o >= 0 ? ue::Get<UObject*>(buf.data() + o) : nullptr;
}
UObject* ObjArg(const std::string& v) {
    if (v == "player") return Player();
    if (v == "asc") return PlayerAsc();
    if (v.rfind("inst:", 0) == 0) return FirstOf(v.substr(5), false);
    if (v.rfind("owned:", 0) == 0) return OwnedByPlayer(v.substr(6));
    return nullptr;
}

UObject *g_strLib = nullptr, *g_strCdo = nullptr, *g_toName = nullptr;
uint64_t ToName(const std::string& s) {
    if (!g_toName) {
        g_strLib = towerperf::ClassNamed("KismetStringLibrary");
        g_strCdo = g_strLib ? FirstOf("KismetStringLibrary", true) : nullptr;
        g_toName = g_strLib ? pe::FindFunction(g_strLib, "Conv_StringToName") : nullptr;
        if (!g_toName || !g_strCdo) return 0;
    }
    std::wstring w(s.begin(), s.end());
    std::vector<uint8_t> buf(0x80, 0);
    FArr str{const_cast<wchar_t*>(w.c_str()), static_cast<int32_t>(w.size() + 1), static_cast<int32_t>(w.size() + 1)};
    std::memcpy(buf.data() + ue::PropOffset(g_toName, "InString"), &str, sizeof str);
    pe::Call(g_strCdo, g_toName, buf.data());
    return ue::Get<uint64_t>(buf.data() + ue::PropOffset(g_toName, "ReturnValue"));
}
std::string Hex(const uint8_t* p, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) s += std::format("{:02x}", p[i]);
    return s;
}

// ---- 成就拦截 ----
std::atomic<bool> g_achBlock{false};
std::atomic<long> g_achSwallowed{0};
std::vector<UObject*> g_achFns;
void AchSet(bool on) {
    if (on && g_achFns.empty()) {
        for (auto [cls, fn] : {std::pair{"PBZAchievementAdapter", "UnlockAchievement"}, {"PBZAchievementAdapter", "SetProgress"},
                               {"PBZAchievementAdapter", "AddProgress"}, {"PBZAchievementAdapter", "MarkElementComplete"},
                               {"PBZOnlineManager", "UnlockAchievement"}, {"PBZOnlineManager", "SetAchievementProgress"}}) {
            UObject* c = towerperf::ClassNamed(cls);
            UObject* f = c ? pe::FindFunction(c, fn) : nullptr;
            if (!f) { pbz::log::Info("影塔成就拦截：找不到 {}.{}（类{}）", cls, fn, c ? "在" : "不在"); continue; }
            g_achFns.push_back(f);
            const std::string label = std::string(cls) + "." + fn;
            pe::Intercept(f, [label](UObject*, void*) {
                if (!g_achBlock) return false;
                ++g_achSwallowed;
                pbz::log::Info("影塔成就拦截：吞掉 {}", label);
                return true;
            });
        }
    }
    g_achBlock = on;
    if (UObject* adapter = FirstOf("PBZAchievementAdapter", false))  // 成就系统停止 / 恢复监听游戏事件
        if (auto* f = pe::FindFunction(ue::ClassOf(adapter), on ? "StopListening" : "StartListening")) { uint8_t b[16] = {}; pe::Call(adapter, f, b); }
    pbz::log::Info("影塔成就拦截：{}（拦截 {} 个函数）", on ? "开" : "关", g_achFns.size());
}

}  // namespace

namespace towerprobe {

// 返回事件 JSON
std::string Call(const std::string& line) {
    auto F = [&](const std::string& k) { return pbz::ipc::Field(line, k.c_str()).value_or(""); };
    const std::string clsName = F("cls"), fnName = F("fn"), on = F("on").empty() ? "inst" : F("on");
    auto fail = [&](const std::string& why) {
        pbz::log::Info("影塔实验调用 {}.{} 失败：{}", clsName, fnName, why);
        return std::format(R"({{"op":"tower","event":"call","ok":false,"fn":"{}","msg":"{}"}})", pbz::ipc::Esc(fnName), pbz::ipc::Esc(why));
    };
    UObject* target = on == "player" ? Player() : on == "owned" ? OwnedByPlayer(clsName) : FirstOf(clsName, on == "cdo");
    if (!target) return fail("找不到对象");
    UObject* fn = pe::FindFunction(ue::ClassOf(target), fnName);
    if (!fn) return fail("找不到函数");
    std::vector<uint8_t> buf(0x400, 0);
    std::vector<std::vector<uint64_t>> arrays;  // names 参数的存储，调用期间有效
    std::vector<std::wstring> strs;
    arrays.reserve(6); strs.reserve(6);
    for (int i = 1; i <= 6; ++i) {
        const std::string n = F(std::format("a{}n", i)), t = F(std::format("a{}t", i)), v = F(std::format("a{}v", i));
        if (n.empty()) continue;
        const int o = ue::PropOffset(fn, n);
        if (o < 0 || o + 16 > static_cast<int>(buf.size())) return fail("参数不存在：" + n);
        uint8_t* p = buf.data() + o;
        if (t == "name") { const uint64_t x = ToName(v); std::memcpy(p, &x, 8); }
        else if (t == "int") { const int32_t x = std::stoi(v); std::memcpy(p, &x, 4); }
        else if (t == "bool") *p = v == "true" || v == "1";
        else if (t == "float") { const float x = std::stof(v); std::memcpy(p, &x, 4); }
        else if (t == "obj") { UObject* x = ObjArg(v); if (!x) return fail("对象参数找不到：" + v); std::memcpy(p, &x, 8); }
        else if (t == "str") { strs.emplace_back(v.begin(), v.end()); FArr s{strs.back().data(), static_cast<int32_t>(strs.back().size() + 1), static_cast<int32_t>(strs.back().size() + 1)}; std::memcpy(p, &s, sizeof s); }
        else if (t == "names") {
            arrays.emplace_back();
            size_t a = 0;
            while (a <= v.size()) { const size_t b = v.find(',', a); const auto part = v.substr(a, b == std::string::npos ? std::string::npos : b - a); if (!part.empty()) arrays.back().push_back(ToName(part)); if (b == std::string::npos) break; a = b + 1; }
            FArr arr{arrays.back().data(), static_cast<int32_t>(arrays.back().size()), static_cast<int32_t>(arrays.back().size())};
            std::memcpy(p, &arr, sizeof arr);
        } else return fail("未知类型：" + t);
    }
    pe::Call(target, fn, buf.data());
    const int r = ue::PropOffset(fn, "ReturnValue");
    const std::string ret = r >= 0 ? Hex(buf.data() + r, 16) : "";
    const int32_t asInt = r >= 0 ? ue::Get<int32_t>(buf.data() + r) : 0;
    pbz::log::Info("影塔实验调用 {}.{}（{}）完成，返回 {}", clsName, fnName, ue::ObjName(target), ret.empty() ? "无" : ret);
    return std::format(R"({{"op":"tower","event":"call","ok":true,"fn":"{}","obj":"{}","ret":"{}","int":{},"buf":"{}"}})",
                       pbz::ipc::Esc(fnName), pbz::ipc::Esc(ue::ObjName(target)), ret, asInt, Hex(buf.data(), 0x60));
}

// 观察：{"cmd":"obs","cls":…,"fn":…,"pn":"<FString 参数名，可空>"}，每次调用记一行日志
std::string Obs(const std::string& line) {
    const std::string cls = pbz::ipc::Field(line, "cls").value_or(""), fn = pbz::ipc::Field(line, "fn").value_or(""), pn = pbz::ipc::Field(line, "pn").value_or("");
    UObject* c = towerperf::ClassNamed(cls);
    UObject* f = c ? pe::FindFunction(c, fn) : nullptr;
    if (!f) return std::format(R"({{"op":"tower","event":"obs","ok":false,"fn":"{}"}})", pbz::ipc::Esc(fn));
    const int off = pn.empty() ? -1 : ue::PropOffset(f, pn);
    const std::string label = cls + "." + fn;
    pe::Observe(f, [label, off](UObject* obj, void* params) {
        std::string arg;
        if (off >= 0 && params) {
            const auto s = ue::Get<FArr>(static_cast<uint8_t*>(params) + off);
            if (s.data && s.num > 0 && s.num < 512) {
                std::wstring w(static_cast<wchar_t*>(s.data), s.num - 1);
                arg = std::string(w.begin(), w.end());  // 地图 / 存档名都是 ASCII
            }
        }
        pbz::log::Info("影塔观察：{}（{}）{}", label, ue::ObjName(obj), arg.empty() ? "" : " 参数 " + arg);
    });
    return std::format(R"({{"op":"tower","event":"obs","ok":true,"fn":"{}","param":{}}})", pbz::ipc::Esc(label), off);
}

std::string Ach(const std::string& v) {
    if (v == "on" || v == "off") AchSet(v == "on");
    return std::format(R"({{"op":"tower","event":"ach","block":{},"swallowed":{},"fns":{}}})", g_achBlock.load(), g_achSwallowed.load(), g_achFns.size());
}

}  // namespace towerprobe
