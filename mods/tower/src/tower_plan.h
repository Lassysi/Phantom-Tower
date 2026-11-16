// 开发审计计划（Codex 实测用，不改正式 50 层编排、不解禁任何内容）：逐层指定 Boss 组合与场地策略。
// 语法：层之间用 ';'，同层多只用 '+'，选项跟在 '@' 后用 ',' 分隔：
//   "AHua_GuiTouDao;IronMaskMan_Boss+XiaoXue;A+B+C@stagger"
//   每只写类路径，或在池里唯一的身份名；选项 here = 本层就地打（不传送），stagger / nostagger = 1v3 错峰入场（3 只时默认错峰），
//   rongshu = 在榕树内场打（固定场地，不用 Boss 原生场地），endless = 无尽混编层（写的 Boss 只定场地）
// 校验（任何一条不过整份计划拒绝，并给出原因）：1–10 层；每层 1–3 只；每只在池里、过准入（禁用表永远优先）；
// 同层身份不同。不足 3 只可靠候选时就别写 1v3 层——计划不会拿未准入的内容凑数。
#pragma once
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace tower::plan {

struct Floor { std::vector<int> bosses; bool stagger = false, here = false, endless = false; std::string venue; };  // endless：无尽混编层，bosses[0] 只定场地，波次按种子随机（tower_endless.h）  // venue：固定场地键（@rongshu），空 = 按 Boss 原生场地

struct PoolView {  // 引擎提供：池大小、类路径、身份、是否准入
    int size = 0;
    std::function<std::string_view(int)> path, identity;
    std::function<bool(int)> admitted;
};

inline std::vector<std::string_view> Split(std::string_view s, char c) {
    std::vector<std::string_view> v;
    size_t a = 0;
    while (a <= s.size()) {
        const size_t b = s.find(c, a);
        v.push_back(s.substr(a, b == std::string_view::npos ? std::string_view::npos : b - a));
        if (b == std::string_view::npos) break;
        a = b + 1;
    }
    return v;
}
inline std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// 找一只：类路径精确匹配；否则按身份，且身份在池里必须唯一（同身份多个变体时要求写类路径）
inline int Find(const PoolView& pool, std::string_view key, std::string* why) {
    for (int i = 0; i < pool.size; ++i) if (pool.path(i) == key) return i;
    int hit = -1, n = 0;
    for (int i = 0; i < pool.size; ++i) if (pool.identity(i) == key) { hit = i; ++n; }
    if (n == 1) return hit;
    *why = n ? std::string("身份不唯一（") + std::string(key) + "），请写类路径" : std::string("池里没有：") + std::string(key);
    return -1;
}

inline bool Parse(std::string_view text, const PoolView& pool, std::vector<Floor>* out, std::string* why) {
    out->clear();
    text = Trim(text);
    if (text.empty()) { *why = "计划为空"; return false; }
    const auto floors = Split(text, ';');
    if (floors.size() > 10) { *why = "审计计划最多 10 层"; return false; }
    for (size_t f = 0; f < floors.size(); ++f) {
        Floor fl;
        auto spec = Trim(floors[f]);
        if (const size_t at = spec.find('@'); at != std::string_view::npos) {
            bool staggerSet = false;
            for (auto o : Split(spec.substr(at + 1), ',')) {
                o = Trim(o);
                if (o == "here") fl.here = true;
                else if (o == "rongshu") fl.venue = "fixed:rongshu_zdy";
                else if (o == "endless") fl.endless = true;  // QA：无尽混编层（最多 3 波、每波最多 3 Boss + 少量小怪）  // 榕树内场（tower_fixed_venue.h），影塔自己刷
                else if (o == "stagger") { fl.stagger = true; staggerSet = true; }
                else if (o == "nostagger") { fl.stagger = false; staggerSet = true; }
                else if (!o.empty()) { *why = "第 " + std::to_string(f + 1) + " 层未知选项：" + std::string(o); return false; }
            }
            spec = Trim(spec.substr(0, at));
            fl.stagger = staggerSet ? fl.stagger : false;
            if (!staggerSet) fl.stagger = Split(spec, '+').size() >= 3;
        } else fl.stagger = Split(spec, '+').size() >= 3;
        for (auto k : Split(spec, '+')) {
            k = Trim(k);
            std::string w;
            const int b = Find(pool, k, &w);
            if (b < 0) { *why = "第 " + std::to_string(f + 1) + " 层：" + w; return false; }
            if (!pool.admitted(b)) { *why = "第 " + std::to_string(f + 1) + " 层：" + std::string(k) + " 被禁用或未准入"; return false; }
            for (int x : fl.bosses)
                if (pool.identity(x) == pool.identity(b)) { *why = "第 " + std::to_string(f + 1) + " 层：同一身份出现两次（" + std::string(pool.identity(b)) + "）"; return false; }
            fl.bosses.push_back(b);
        }
        if (fl.bosses.empty() || fl.bosses.size() > 3) { *why = "第 " + std::to_string(f + 1) + " 层要 1–3 只"; return false; }
        if (fl.bosses.size() != 1) { out->clear(); *why = "多人层需原生编组与协同验证，本批不接受独立 Boss 拼接；请使用不同场地的单 Boss 层"; return false; }
        out->push_back(std::move(fl));
    }
    return true;
}

}  // namespace tower::plan
