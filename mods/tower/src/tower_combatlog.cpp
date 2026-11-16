#include "tower_combatlog.h"
#include "tower_verbose.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <cmath>
#include <deque>
#include <format>
#include <map>
#include <unordered_map>
#include <vector>

#include "../../../src/core/host/log.h"
#include "../../../src/core/sdk/pbz_module.h"
#include "tower_perf.h"
#include "tower_vm_gate.h"

namespace towercombat {
namespace {
namespace ue = pbz::ue;
namespace pe = pbz::pe;
using ue::UObject;

struct Hit { double t; float amount; std::string tags, cause; };
struct BossLog { std::string name; std::map<std::string, int> tags; int events = 0; float total = 0; float lastHp = -1; double lastEvent = -1; unsigned adjustments=0; };

bool g_on = false;
int g_floor = 0;
UObject* g_player = nullptr;
double g_now = 0, g_lastPlayerHit = -1, g_lastIdleLog = -1, g_lastPlayerEvent = -1;
float g_playerHp = -1;
int g_nxTake = 0, g_peTake = 0;
UObject* g_takeNx = nullptr;
std::deque<Hit> g_hits;  // 玩家最近受伤
std::unordered_map<UObject*, BossLog> g_bosses;
std::vector<int> g_tokens;
std::vector<UObject*> g_hooked;
std::vector<UObject*> g_vmHooked;
struct Cause{std::string text;uint64_t wall=0;};
std::unordered_map<UObject*,Cause> g_causes;
int g_onDamagePe=0,g_onDamageVm=0,g_hpDrops=0,g_playerEvents=0;
UObject* TrackedOwner(UObject* who){
    for(int depth=0;who && ue::Live(who) && depth<4;++depth,who=ue::Get<UObject*>(who+ue::Layout::ObjOuter))
        if(who==g_player || g_bosses.count(who))return who;
    return nullptr;
}
void RememberCause(UObject* component,void* raw,int off){
    auto* owner=TrackedOwner(component);
    if(!owner || !raw || off<0)return;
    g_causes[owner]={ue::Name(ue::Get<uint32_t>(static_cast<uint8_t*>(raw)+off)),GetTickCount64()};
}
std::string TakeCause(UObject* who){
    const auto i=g_causes.find(who);if(i==g_causes.end())return {};
    const auto c=i->second;g_causes.erase(i);
    return GetTickCount64()-c.wall<=100?c.text:std::string();
}

std::string Tags(const uint8_t* container) {
    struct Arr { const uint64_t* data; int32_t num, max; };
    const Arr a = ue::Get<Arr>(const_cast<uint8_t*>(container));
    std::string out;
    for (int i = 0; a.data && i < a.num && i < 6; ++i) out += (out.empty() ? "" : "|") + ue::Name(static_cast<uint32_t>(ue::Get<uint64_t>(reinterpret_cast<const uint8_t*>(a.data + i))));
    return out.empty() ? "-" : out;
}

void HookFn(UObject* fn, pe::Observer cb) {
    if (!fn || std::find(g_hooked.begin(), g_hooked.end(), fn) != g_hooked.end()) return;
    g_hooked.push_back(fn);
    g_tokens.push_back(pe::Observe(fn, std::move(cb)));
}

void HookDamage(UObject* cls) {
    UObject* f = cls ? pe::FindFunction(cls, "OnDamage") : nullptr;
    if (!f) return;
    const int amount = ue::PropOffset(f, "DamageAmount"), tags = ue::PropOffset(f, "DamageTags");
    auto observe=[amount,tags,f](UObject* who,void* raw,const char* source){
        if(!g_on || !raw || (who!=g_player && !g_bosses.count(who)))return;
        auto* p = static_cast<uint8_t*>(raw);
        const float dmg = amount >= 0 ? ue::Get<float>(p + amount) : 0;
        if(!std::isfinite(dmg))return;
        const std::string t = tags >= 0 ? Tags(p + tags) : "-";
        if(std::string_view(source)=="ProcessEvent"){
            ++g_onDamagePe;
            // A reflected call to a Blueprint event proceeds through the VM too.
            // Once that exact function is observed at the VM boundary, use the
            // VM as the single damage event; PE remains a route counter only.
            if(towervm::Ready() && std::find(g_vmHooked.begin(),g_vmHooked.end(),f)!=g_vmHooked.end())return;
        }else ++g_onDamageVm;
        if (who == g_player) {
            g_hits.push_back({g_now, dmg, std::string(source)+":"+t, TakeCause(who)});
            if (g_hits.size() > 8) g_hits.pop_front();
            g_lastPlayerHit = g_lastPlayerEvent = g_now;
            ++g_playerEvents;
        } else if (auto it = g_bosses.find(who); it != g_bosses.end()) {
            ++it->second.events; it->second.total += dmg; ++it->second.tags[std::string(source)+":"+t]; it->second.lastEvent = g_now;
        }
    };
    HookFn(f,[observe](UObject* who,void* raw){observe(who,raw,"ProcessEvent");});
    if(towervm::Ready() && std::find(g_vmHooked.begin(),g_vmHooked.end(),f)==g_vmHooked.end()){
        std::string why;
        if(towervm::Add(f,[observe](UObject* who,UObject*,void* raw){observe(who,raw,"蓝图执行入口");return towervm::Action::Pass;},&why))g_vmHooked.push_back(f);
        else pbz::log::Info("影塔战斗取证：OnDamage 蓝图观察未挂（{}）；不把空回调记成无伤",why);
    }
}

}  // namespace

void Begin(int floor, void* player) {
    End("restart");
    g_on = true; g_floor = floor; g_player = static_cast<UObject*>(player);
    g_now = 0; g_lastPlayerHit = -1; g_lastIdleLog = -1;
    if (g_player && ue::Live(g_player)) HookDamage(ue::ClassOf(g_player));
    if (auto* attr = towerperf::ClassNamed("AttributeComponent"))
        if (auto* f = pe::FindFunction(attr, "TakeDamage")) {
            const int cause = ue::PropOffset(f, "DamageCause");
            HookFn(f, [cause](UObject* who, void* raw) {
                if (g_on && raw && cause >= 0) { RememberCause(who,raw,cause); ++g_peTake; }
            });
            // TakeDamage 不再挂原生执行入口（2026-11-05 ui3 第 7 层：槽位空出来后第一次挂上，Boss 只掉杀气不掉血；
            // 之前一直因槽位用完没挂上、伤害正常）。只留 ProcessEvent 观察
        }
    pbz::log::Info("影塔战斗取证：第 {} 层开始（观察 OnDamage {} 个、TakeDamage {}）", floor, g_hooked.size(), towerperf::ClassNamed("AttributeComponent") ? "可用" : "不可用");
}

void Watch(void* bossV, const std::string& name) {
    auto* boss = static_cast<UObject*>(bossV);
    if (!g_on || !boss || !ue::Live(boss)) return;
    g_bosses[boss].name = name;
    HookDamage(ue::ClassOf(boss));
}

void Tick(double fightSeconds) {
    if (!g_on || fightSeconds < 0) return;
    g_now = fightSeconds;
    // 开打 30 秒没让玩家受过伤：记一次（6 清道夫不出招的取证），之后每 60 秒最多再记一次
    const double quiet = g_lastPlayerHit < 0 ? fightSeconds : fightSeconds - g_lastPlayerHit;
    if (quiet >= 30 && (g_lastIdleLog < 0 || fightSeconds - g_lastIdleLog >= 60)) {
        g_lastIdleLog = fightSeconds;
        const int ctrl = ue::PropOffset(towerperf::ClassNamed("Pawn"), "Controller");
        std::string who;
        for (auto& [b, l] : g_bosses) {
            UObject* c = ctrl >= 0 && ue::Live(b) ? ue::Get<UObject*>(b + ctrl) : nullptr;
            who += std::format("{}{}（AI {}）", who.empty() ? "" : "、", l.name, c && ue::Live(c) ? ue::ClassName(c) : "无控制器");
        }
        pbz::log::Info("影塔战斗取证：第 {} 层开打 {:.0f} 秒，玩家已 {:.0f} 秒没受伤（Boss 可能不出招）：{}", g_floor, fightSeconds, quiet, who.empty() ? "-" : who);
    }
}

void Sample(void* whoV, float hp) {
    auto* who = static_cast<UObject*>(whoV);
    if (!g_on || !who || !std::isfinite(hp)) return;
    auto drop = [&](float& last, double lastEvent) -> float {
        const float d = last >= 0 && hp < last - 0.5f ? last - hp : 0;
        last = hp;
        return lastEvent >= 0 && g_now - lastEvent < 0.1 ? 0 : d;  // 观察点刚记过的同一下不重复计
    };
    if (who == g_player) {
        if (const float d = drop(g_playerHp, g_lastPlayerEvent); d > 0) {
            ++g_hpDrops;g_hits.push_back({g_now, d, "血量下降（观察点没收到）", TakeCause(who)});
            if (g_hits.size() > 8) g_hits.pop_front();
            g_lastPlayerHit = g_now;
        }
    } else if (auto it = g_bosses.find(who); it != g_bosses.end()) {
        if (const float d = drop(it->second.lastHp, it->second.lastEvent); d > 0) { ++g_hpDrops;++it->second.events; it->second.total += d; ++it->second.tags["血量下降"]; }
    }
}
void Adjusted(void* whoV,float hp,const char* reason){
    auto* who=static_cast<UObject*>(whoV);
    if(!g_on || !who || !std::isfinite(hp))return;
    if(who==g_player)g_playerHp=hp;
    else if(auto it=g_bosses.find(who);it!=g_bosses.end()){
        it->second.lastHp=hp;
        if(++it->second.adjustments>3)return;
    }
    else if(who!=g_player && !towerdev::kPerfLog)return;  // 3.1.0：小怪的塔内血量调整（火上浇油每秒一次）正式版不写日志，避免长时间战斗逐行写盘
    pbz::log::Info("影塔战斗取证：塔自身血量调整 actor={} hp={:.0f} reason={}（不计受伤/致死来源）",ue::ObjName(who),hp,reason);
}

bool PlayerDamageRoute() { return g_on && g_playerEvents > 0; }
double SincePlayerDamage() { return g_lastPlayerEvent < 0 ? 1e9 : g_now - g_lastPlayerEvent; }

std::string LastHits(int n) {
    std::string out;
    const int from = std::max(0, static_cast<int>(g_hits.size()) - n);
    for (int i = from; i < static_cast<int>(g_hits.size()); ++i) {
        const auto& h = g_hits[i];
        out += std::format("{}[{:.1f}s] {:.0f} 标签 {}{}", out.empty() ? "" : "；", h.t, h.amount, h.tags, h.cause.empty() ? "" : " 原因 " + h.cause);
    }
    return out.empty() ? "没有记录到受伤事件（伤害可能不经 OnDamage）" : out;
}

void End(const char* why) {
    if (g_on && !g_bosses.empty()) {
        for (auto& [b, l] : g_bosses) {
            std::string t;
            for (auto& [k, v] : l.tags) t += std::format("{}{}×{}", t.empty() ? "" : "，", k, v);
            if (std::string_view(why) != "mob_wave") pbz::log::Info("影塔战斗取证：第 {} 层 {}（{}）受伤 {} 次、合计 {:.0f}；按来源 / 标签：{}；TakeDamage 经 ProcessEvent {} 次、经原生执行入口 {} 次", g_floor, l.name, why,
                           l.events, l.total, t.empty() ? "无" : t, g_peTake, g_nxTake);
        }
    }
    if(g_on && std::string_view(why) != "mob_wave")pbz::log::Info("影塔战斗取证：第 {} 层来源汇总 OnDamage ProcessEvent={} 蓝图执行入口={} 血量下降={} TakeDamage ProcessEvent={} 原生执行入口={}（空观察不等于无伤；塔调血不计）",g_floor,g_onDamagePe,g_onDamageVm,g_hpDrops,g_peTake,g_nxTake);
    g_on = false;
    if (g_takeNx) { pbz::nx::Unhook(g_takeNx); g_takeNx = nullptr; }
    g_playerHp = -1; g_lastPlayerEvent = -1; g_nxTake = g_peTake = 0;
    for (int t : g_tokens) pe::Unobserve(t);
    for(auto* fn:g_vmHooked)towervm::Remove(fn);
    g_tokens.clear(); g_hooked.clear();g_vmHooked.clear(); g_bosses.clear(); g_hits.clear(); g_player = nullptr; g_causes.clear();
    g_onDamagePe=g_onDamageVm=g_hpDrops=g_playerEvents=0;
}

}  // namespace towercombat
