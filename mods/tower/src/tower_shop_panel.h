// 驿站大面板的纯逻辑（32ffb7d 用户：逐行商品 + 右侧对应购买；常规一页 6 行，超出才分页；空货位隐藏、按真实商品计数；
// 刷新 / 离开固定独立按钮；成功 / 缺钱 / 货架变化有醒目结果）。界面（tower_shop_ui.cpp）只按这里的 View 画，
// 点击按行号换回后台真实 slot + 货架版本，重排不会买错；发出后到状态变化（或 1.2 秒）之前全部按钮不再受理，连点不会重复成交。
#pragma once
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "tower_rewards.h"

namespace tower::shoppanel {

inline constexpr int kRows = 10;  // v14：一页约 10 件（用户 2026-11-05）

struct Row {
    int slot = -1;          // 后台真实货位
    std::string name, effect;
    int price = 0;
    bool sold = false, affordable = false;
};
struct View {
    std::vector<Row> rows;  // 本页（最多 kRows）
    int page = 0, pages = 0, total = 0;
    int jade = 0, refreshCost = 0;
    bool canRefresh = false;
    uint32_t version = 0;
};

// A confirmation must refer to the exact displayed item and shelf, including
// amount and quality (Offer::operator== intentionally compares identity only).
struct Quote {
    std::string run;
    int floor = -1, slot = -1, price = 0;
    uint64_t seed = 0;
    uint32_t version = 0;
    rewards::Offer offer;
};
inline std::optional<Quote> Capture(const rewards::Shop& s, std::string_view run, int slot) {
    if (!s.Open() || run.empty() || slot < 0 || slot >= static_cast<int>(s.shelf.size())) return std::nullopt;
    const auto& w = s.shelf[slot];
    if (w.sold || w.price < 0 || !rewards::Sane(w.offer) || rewards::Empty(w.offer)) return std::nullopt;
    return Quote{std::string(run), s.floor, slot, w.price, s.seed, s.version, w.offer};
}
enum class QuoteStatus { Ready, Stale, Sold, NoJade };
inline QuoteStatus Check(const Quote& q, const rewards::Shop& s, std::string_view run, int jade) {
    if (!s.Open() || run != q.run || s.floor != q.floor || s.seed != q.seed || s.version != q.version ||
        q.slot < 0 || q.slot >= static_cast<int>(s.shelf.size())) return QuoteStatus::Stale;
    const auto& w = s.shelf[q.slot];
    if (w.sold) return QuoteStatus::Sold;
    if (w.price != q.price || w.offer.kind != q.offer.kind || w.offer.index != q.offer.index ||
        w.offer.amount != q.offer.amount || w.offer.quality != q.offer.quality || !rewards::Sane(w.offer) || rewards::Empty(w.offer)) return QuoteStatus::Stale;
    return jade < q.price ? QuoteStatus::NoJade : QuoteStatus::Ready;
}

// A balance change alone can come from another action, so require the matching
// newly recorded purchase and a shelf version change as well.
inline bool Recorded(const Quote& q, const rewards::Shop& s, size_t beforeCount, int before, int after) {
    if (s.floor != q.floor || s.seed != q.seed || s.version == q.version ||
        s.bought.size() <= beforeCount || static_cast<long long>(before) - after != q.price) return false;
    for (size_t i = beforeCount; i < s.bought.size(); ++i) {
        const auto& o = s.bought[i];
        if (o.kind == q.offer.kind && o.index == q.offer.index &&
            o.amount == q.offer.amount && o.quality == q.offer.quality) return true;
    }
    return false;
}

// Split at UTF-8 codepoint boundaries; do not truncate effect descriptions.
// Explicit line breaks count against the page budget as they consume height.
inline std::vector<std::string> EffectPages(std::string_view text, size_t maxChars = 100, size_t maxBreaks = 3) {
    std::vector<std::string> out(1);
    size_t chars = 0, breaks = 0;
    maxChars = std::max<size_t>(1, maxChars);
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
        n = std::min(n, text.size() - i);
        if (!out.back().empty() && (chars >= maxChars || (c == '\n' && breaks >= maxBreaks))) {
            out.emplace_back(); chars = breaks = 0;
        }
        out.back().append(text.substr(i, n));
        ++chars; if (c == '\n') ++breaks;
        i += n;
    }
    return out;
}

inline View Build(const rewards::Shop& s, int jade, int page) {
    View v;
    v.jade = jade;
    v.version = s.version;
    v.refreshCost = rewards::RefreshCost(s.refreshes, s.floor);
    v.canRefresh = s.Open() && jade >= v.refreshCost;
    std::vector<int> goods;
    for (int i = 0; i < static_cast<int>(s.shelf.size()); ++i)
        if (!rewards::Empty(s.shelf[i].offer) && !rewards::RetiredOffer(s.shelf[i].offer)) goods.push_back(i);
    v.total = static_cast<int>(goods.size());
    v.pages = v.total ? (v.total + kRows - 1) / kRows : 1;
    v.page = std::clamp(page, 0, v.pages - 1);
    for (int k = v.page * kRows; k < v.total && k < (v.page + 1) * kRows; ++k) {
        const auto& w = s.shelf[goods[k]];
        v.rows.push_back({goods[k], rewards::Name(w.offer), rewards::Effect(w.offer), w.price, w.sold, !w.sold && jade >= w.price});  // 3.2.1 珍品位标「珍·」
    }
    return v;
}

// 交易结果：按发出前后的魂玉与货架版本判断（扣了 = 成功；没扣 = 缺钱 / 货架已变）
inline std::string Result(const std::string& what, int price, int before, int after, bool refresh) {
    if (after == before - price) return std::format("【成功】{}，花费 {} 魂玉，余 {}", what, price, after);
    if (before < price) return std::format("【未成交】魂玉不足：{} 需要 {}，现有 {}", what, price, before);
    return std::format("【未成交】{}：{}，没有扣魂玉", what, refresh ? "刷新没有生效" : "货架已变或已售出");
}

// 连点保护：发出一笔后，到货架版本或魂玉变化（成交 / 被拒都会让状态刷新）或超时之前不再受理
struct Guard {
    bool pending = false;
    uint32_t version = 0;
    int jade = 0;
    uint64_t until = 0;
    bool Busy(uint32_t ver, int nowJade, uint64_t now) {
        if (pending && (ver != version || nowJade != jade || now >= until)) pending = false;
        return pending;
    }
    // 被拒不改状态：1.2 秒后按未成交结算
    void Arm(uint32_t ver, int nowJade, uint64_t now) { pending = true; version = ver; jade = nowJade; until = now + 1200; }
};

}  // namespace tower::shoppanel
