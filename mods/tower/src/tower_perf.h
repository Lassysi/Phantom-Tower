// 影塔游戏线程开销（32ffb7d 用户实测：每次加载场景和读秒严重卡顿，开打后恢复）。
// 静态排查到的高可信热点：运行库 FindClass 找不到类时每次都整表扫描、且不缓存"没找到"——
// 入场画面门每帧查一次未加载的 WB_TempLoadingScreen_C、开层补满逐个查 28 个药效类（28 次整表扫描，上批实测约 780ms）。
// 这里给模块用的两样：
//   ClassNamed：找不到的类 2 秒内不再整表扫描（找到的走运行库缓存）；
//   Section：分段计时，每秒汇总一次，单次 > 0.5ms 或窗口分段累计 > 30ms 才写日志。嵌套分段不可相加解释帧耗时。
#pragma once
#include "tower_verbose.h"
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../../../src/core/host/log.h"
#include "../../../src/core/sdk/pbz_module.h"

namespace towerperf {

inline uint64_t Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return static_cast<uint64_t>(t.QuadPart); }
inline double Ms(uint64_t ticks) { static const double f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return 1000.0 / static_cast<double>(q.QuadPart); }(); return ticks * f; }

// 按名字找类。Core FindClass 找不到时整表遍历（约 15 万对象、逐个拼类名，几十到上百 ms）。
// 用户 2026-11-05：原版不卡、加 Mod 后"几乎固定几秒一顿"——当前地图没加载的类每 2 秒重扫一次就是这个节奏。
// 找不到的名字按指数退避（2 s 起翻倍，最长 30 s），找到即复位；对象表比上次找时多出 5000 个以上（换图 / 新内容加载），
// 2 s 后即可再找，避免新加载的类被退避耽误。单次超过 10 ms 记一行（同名每分钟一行）
inline pbz::ue::UObject* ClassNamed(std::string_view name) {
    struct Miss { uint64_t next = 0, interval = 0, logged = 0, tried = 0; int num = 0; };
    static std::unordered_map<std::string, Miss> misses;
    const std::string key(name);
    const uint64_t now = GetTickCount64();
    auto it = misses.find(key);
    // 3.0.0（自动爬塔实测：战斗中对象数一直涨，"多出 5000 个就 2 秒后重找"让缺席的 GA_SummonPassive_C 每 2 秒整表扫一次，
    // 一次 23–47 ms = 每 2 秒一顿）：对象表大幅增长（2 万+）也要隔 10 秒以上才提前重找，其余照指数退避
    if (it != misses.end() && now < it->second.next &&
        !(now - it->second.tried >= 30000 && pbz::ue::Num() - it->second.num > 20000)) return nullptr;  // 3.2.1：10 → 30 秒（战斗中对象数一直涨，缺席类整表扫 15–59 ms）
    const uint64_t t0 = Qpc();
    auto* c = pbz::ue::FindClass(name);
    const double ms = Ms(Qpc() - t0);
    if (c) { misses.erase(key); return c; }
    auto& m = misses[key];
    m.interval = m.interval ? std::min<uint64_t>(m.interval * 2, 30000) : 2000;
    m.next = now + m.interval;
    m.tried = now;
    m.num = pbz::ue::Num();
    if (ms > 10 && now - m.logged > 60000) {
        m.logged = now;
        pbz::log::Info("影塔找类：{} 不在对象表（整表扫描 {:.0f} ms），下次 {} 秒后再找", key, ms, m.interval / 1000);
    }
    return c;
}

struct Stat { double total = 0, peak = 0; int calls = 0; };
inline std::unordered_map<std::string, Stat>& Stats() { static std::unordered_map<std::string, Stat> s; return s; }
// 每秒汇总一次（由引擎 Tick 调）；phase 写当时所处阶段（加载 / 等待 / 读秒 / 战斗 …）
inline void Flush(std::string_view phase) {
    static uint64_t last = GetTickCount64();
    const uint64_t now = GetTickCount64();
    if (now - last < 1000) return;
    const auto window=now-last;
    last = now;
    auto& s = Stats();
    double sum = 0, peak = 0;
    for (auto& [k, v] : s) { sum += v.total; peak = std::max(peak, v.peak); }
    if (towerdev::kPerfLog && (sum > 30 || peak > 0.5)) {
        std::vector<std::pair<std::string, Stat>> rows(s.begin(), s.end());
        std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.second.total > b.second.total; });
        std::string line;
        for (size_t i = 0; i < rows.size(); ++i)
            line += std::format("{}{} {:.1f}ms/{}次 峰{:.1f}", i ? "；" : "", rows[i].first, rows[i].second.total, rows[i].second.calls, rows[i].second.peak);
        pbz::log::Info("影塔帧耗时（{}，窗口 {}ms，分段累计 {:.1f}ms（含嵌套，不可相加解释帧耗时），单次峰值 {:.1f}ms）：{}", phase,window,sum,peak,line);
    }
    s.clear();
}
// 4.0.0 卡顿优化（实测：准备 / 战斗中"玩家属性"每 2 秒一次 20–35 ms 尖峰 = 血条兜底查找整表 ForEach 约 31 万个对象）：
// 按类收集实例的分摊扫描。每帧只看 kIndexPerFrame 个对象（约 2.5 秒一轮），一轮扫完才替换结果；按类缓存 IsA 判定。
// 只在最近 5 秒有人查过时才扫（没人用不占时间）。结果可能落后一轮（≤ 3 秒），调用方用前自己核 Live
inline constexpr int kIndexPerFrame = 2048;
struct ClassIndex {
    std::vector<std::string> names;                                        // 登记的基类名（按 IsA 收）
    std::vector<pbz::ue::UObject*> bases;                                 // 本轮解析到的基类（下标同 names，没加载为空）
    std::vector<std::vector<pbz::ue::UObject*>> building, ready;          // 下标同 names
    std::unordered_map<pbz::ue::UObject*, uint32_t> kind;                 // 类 → 命中的登记位
    int cursor = 0, pass = 0; uint64_t lastUse = 0;
    ClassIndex() { names.reserve(32); bases.reserve(32); building.reserve(32); ready.reserve(32); }  // 最多 32 类：登记新类不搬家，Instances 返回的引用一直有效
};
inline ClassIndex& Index() { static ClassIndex x; return x; }
inline int IndexSlot(std::string_view name) {
    auto& x = Index();
    for (size_t i = 0; i < x.names.size(); ++i) if (x.names[i] == name) return static_cast<int>(i);
    if (x.names.size() >= 32) return -1;
    x.names.emplace_back(name); x.bases.push_back(nullptr); x.building.emplace_back(); x.ready.emplace_back();
    return static_cast<int>(x.names.size() - 1);
}
// 上一轮完整扫描里 IsA(name) 的非默认对象（可能已失效）；第一次查时登记，结果要等一轮后才有
inline const std::vector<pbz::ue::UObject*>& Instances(std::string_view name) {
    static const std::vector<pbz::ue::UObject*> none;
    auto& x = Index();
    x.lastUse = GetTickCount64();
    const int i = IndexSlot(name);
    return i < 0 ? none : x.ready[static_cast<size_t>(i)];
}
inline int IndexPass() { return Index().pass; }
// 每帧调一次（游戏线程）
inline void IndexStep() {
    auto& x = Index();
    if (x.names.empty() || GetTickCount64() - x.lastUse > 5000) return;
    namespace ue = pbz::ue;
    if (x.cursor == 0) for (size_t i = 0; i < x.names.size(); ++i) x.bases[i] = ClassNamed(x.names[i]);
    static ue::UObject* batch[kIndexPerFrame];
    const int n = ue::Num();
    const int got = ue::ObjBatch(x.cursor, kIndexPerFrame, batch);
    for (int k = 0; k < got; ++k) {
        auto* o = batch[k];
        if (!o) continue;
        auto* cls = ue::ClassOf(o);
        if (!cls) continue;
        auto [it, fresh] = x.kind.try_emplace(cls, 0u);
        if (fresh) {
            int depth = 0;
            for (auto* c = cls; c && depth < 64; c = ue::Get<ue::UObject*>(c + ue::Layout::StructSuper), ++depth)
                for (size_t i = 0; i < x.bases.size(); ++i) if (x.bases[i] && c == x.bases[i]) it->second |= 1u << i;
        }
        if (!it->second || ue::IsDefault(o)) continue;
        for (size_t i = 0; i < x.names.size(); ++i) if (it->second & (1u << i)) x.building[i].push_back(o);
    }
    x.cursor = std::min(n, x.cursor + kIndexPerFrame);
    if (x.cursor < n && got > 0) return;
    x.cursor = 0; ++x.pass;
    for (size_t i = 0; i < x.names.size(); ++i) { x.ready[i].swap(x.building[i]); x.building[i].clear(); }
    x.kind.clear();  // 类可能卸载后地址复用：每轮重新判断
}

// 用法：{ towerperf::Section _("入场门"); … }
struct Section {
    const char* name; uint64_t t0;
    explicit Section(const char* n) : name(n), t0(Qpc()) {}
    ~Section() { const double ms = Ms(Qpc() - t0); auto& st = Stats()[name]; st.total += ms; st.peak = std::max(st.peak, ms); ++st.calls; }
};

}  // namespace towerperf
