#pragma once
#include "tower_rewards.h"

namespace tower::hex {
// A one-shot, saved reward sequence. It has its own RNG and never overwrites
// a pending floor reward, challenge reward, refresh token, or shop stock.
struct AttributeBurst {
    int remaining = 0, claimed = 0;
    uint64_t seed = 0;
    bool attributeOnly = false;
    bool blitz = false;  // 速战速决给的（界面标题用）；存档里 attributeOnly 字段写 2
    std::vector<rewards::Offer> offers;

    void Draw(rewards::Context context) {
        // 属性叠属性：后期档位的属性值（attributeFloor 50）；3.1.0 速战速决改为按当前层的档位（用户：不要顶配属性）
        // 3.4.5（用户）：锻造不再固定给最后一区的数值，改为「当前区数值 × 1.5」（锻造价按层分段，前期只要 300）
        auto pool = rewards::detail::Attrs(context);
        if (attributeOnly && !blitz && context.attrs) {
            // 锻造覆盖全部面板属性（3.3.3）：生命吸取平时 61 层后才进候选，锻造里一直有（各区数值相同）
            const int leech = static_cast<int>(rewards::AttrKind::LifeSteal);
            if (std::none_of(pool.begin(), pool.end(), [&](const auto& c) { return c.offer.kind == rewards::Kind::Attr && c.offer.index == leech; })) {
                auto late = context; late.attributeFloor = std::max(31, context.attributeFloor > 0 ? context.attributeFloor : context.floor);
                for (const auto& c : rewards::detail::Attrs(late)) if (c.offer.kind == rewards::Kind::Attr && c.offer.index == leech) pool.push_back(c);
            }
            for (auto& c : pool) if (c.offer.kind == rewards::Kind::Attr) c.offer.amount = std::max(1, std::min(context.attrs->Room(c.offer.index), c.offer.amount * 3 / 2));
        }
        // 3.3.3（用户：锻造要覆盖所有面板属性）：锻造按属性等概率抽（生命的少见档仍占生命的 1/10），速战速决照原权重
        // 3.4.2（用户：锻造几乎全是抗性）：基础属性为主、抗性偶尔出现——攻击 20、生命 18 + 高档 2、破防 15、杀气 12、减伤 8、吸血 5、抗性各 2
        if (attributeOnly && !blitz) for (auto& c : pool) {
            if (c.offer.kind != rewards::Kind::Attr) continue;
            using K = rewards::AttrKind;
            switch (static_cast<K>(c.offer.index)) {
                // 4.0.1（用户 2026-11-16）：杀气 / 攻击 / 生命 / 破防为主，其次减伤，再次抗性，吸血几乎最低
                case K::Attack: c.weight = 20; break;
                case K::Life: c.weight = c.offer.quality == 2 ? 2 : 18; break;
                case K::Break: c.weight = 18; break;
                case K::Shachi: c.weight = 18; break;
                case K::Defense: c.weight = 6; break;
                case K::LifeSteal: c.weight = 1; break;
                default: c.weight = 2; break;  // 六项抗性
            }
        }
        growth::Xs rng{(seed ^ (0x9E3779B97F4A7C15ull * static_cast<uint64_t>(claimed + 1))) | 1};
        offers.clear();
        if (!attributeOnly) {
            const auto group = rewards::Draw14(rng, context);
            offers.assign(group.normal.begin(), group.normal.end());
            return;
        }
        for (int i = 0; i < 3; ++i) {
            rewards::Offer offer;
            if (!rewards::detail::Pick(rng, pool, offers, {}, &offer)) break;
            offers.push_back(offer);
        }
        // Near the normal caps there may be fewer than three useful attributes.
        // Supply distinct, useful life tiers rather than a capped/empty choice.
        const int life = static_cast<int>(rewards::AttrKind::Life);
        const int room = context.attrs ? context.attrs->Room(life) : 0;
        const int step = context.difficulty == 1 ? rewards::kAttrs[life].step[4] : rewards::kAttrs[life].hard[4];
        for (int scale : {2, 3, 4}) {
            if (offers.size() >= 3) break;
            const int amount = std::min(room, step * scale / 2);
            if (amount <= 0 || std::any_of(offers.begin(), offers.end(), [&](const rewards::Offer& o) {
                return o.kind == rewards::Kind::Attr && o.index == life && o.amount == amount;
            })) continue;
            offers.push_back({rewards::Kind::Attr, life, amount, 1});
        }
        if (offers.empty()) remaining = 0;
    }
    void Begin(uint64_t runSeed, bool thrower, const rewards::Context& context) { BeginMul(runSeed, thrower ? 2 : 1, context); }
    // 3.6.0：投掷狂人 Lv2 ×3（mul = 1 / 2 / 3）
    void BeginMul(uint64_t runSeed, int mul, const rewards::Context& context) {
        *this = {}; seed = runSeed ^ 0xA77B1B17E6A99123ull;
        remaining = 4 * std::clamp(mul, 1, 3); Draw(context);
    }
    // 速战速决（F 批）：n 次只出属性的三选一
    void BeginAttributes(uint64_t burstSeed, int count, const rewards::Context& context) {
        *this = {}; seed = burstSeed ^ 0x5EEDB11751ull;
        attributeOnly = true; blitz = true; remaining = std::max(0, count); Draw(context);
    }
    void BeginPurchase(uint64_t purchaseSeed, bool thrower, const rewards::Context& context) { BeginPurchaseMul(purchaseSeed, thrower ? 2 : 1, context); }
    void BeginPurchaseMul(uint64_t purchaseSeed, int mul, const rewards::Context& context) {
        *this = {}; seed = purchaseSeed ^ 0xF046E45A8B93ull;
        attributeOnly = true; remaining = std::clamp(mul, 1, 3); Draw(context);
    }
    template <class Grant>
    bool Pick(int slot, int expectedClaimed, rewards::Context context, Grant grant, rewards::Offer* got) {
        if (!remaining || expectedClaimed != claimed || slot < 0 || slot >= static_cast<int>(offers.size())) return false;
        const auto offer = offers[slot];
        if (!blitz) context.attributeFloor = 50;
        if ((attributeOnly && offer.kind != rewards::Kind::Attr) || !rewards::StillValid(offer, context) || !grant(offer)) return false;
        if (got) *got = offer;
        --remaining; ++claimed;
        offers.clear();
        if (remaining) Draw(context);
        return true;
    }
    std::string Save() const {
        std::string result = std::format("2 {} {} {} {} {}", remaining, claimed, seed, blitz ? 2 : attributeOnly ? 1 : 0, offers.size());
        for (const auto& o : offers) result += " " + rewards::SaveOffer(o);
        return result;
    }
    static bool Load(std::string_view text, AttributeBurst* output) {
        AttributeBurst burst; int version = 0, count = -1;
        std::istringstream in{std::string(text)};
        int only = 1;
        if (!(in >> version >> burst.remaining >> burst.claimed >> burst.seed) || (version != 1 && version != 2)) return false;
        if (version == 2 && (!(in >> only) || only < 0 || only > 2)) return false;
        burst.attributeOnly = only != 0; burst.blitz = only == 2;
        if (!(in >> count) ||
            burst.remaining < 0 || burst.claimed < 0 || burst.remaining + burst.claimed > 8 || count < 0 || count > 3 ||
            (burst.remaining > 0 && count == 0) || (burst.remaining == 0 && count != 0)) return false;
        for (int i = 0; i < count; ++i) {
            rewards::Offer offer;
            if (!rewards::LoadOffer(in, &offer) || !rewards::Sane(offer) || (burst.attributeOnly && offer.kind != rewards::Kind::Attr)) return false;
            if (std::any_of(burst.offers.begin(), burst.offers.end(), [&](const rewards::Offer& o) {
                return o.kind == offer.kind && o.index == offer.index && o.amount == offer.amount;
            })) return false;
            burst.offers.push_back(offer);
        }
        in >> std::ws; if (!in.eof()) return false;
        *output = std::move(burst); return true;
    }
};

// 3.6.1（用户 2026-11-15：后期几十万魂玉，锻造器一次只能买一个、要点几百次）：批量锻造。
// 一次买 n 个（×1 / ×5 / ×10 / 最大，最多 kForgeBatchMax 个），买的瞬间扣魂玉，n 组（投掷狂人 ×mul）属性三选一一次生成并存档；
// 界面一页显示多组、每组选一个，最后「确认」统一发放。中途退出不丢：下次打开接着选。
// 买了就是这一批，不退款（用户 2026-11-15：否则可以退出重买刷属性）。确认时每组照玩家选的发：有上限的属性（减伤 80%、抗性各 45%）
// 超出上限的部分作废，玩家自己算（同一批前面的组可能已经把上限占满）
inline constexpr int kForgeBatchMax = 100000;  // 用户 2026-11-15：不设上限，买多少展示多少（只防溢出）
struct ForgeBatch {
    uint64_t seed = 0;
    int forges = 0, mul = 1;                           // 买了几个锻造器；每个几组（投掷狂人：Lv1 2 组、Lv2 3 组）
    std::vector<std::vector<rewards::Offer>> rows;     // 每组 1–3 个选项
    std::vector<int> pick;                             // 每组选了第几个（-1 = 没选）
    bool Active() const { return !rows.empty(); }
    int Unpicked() const { return static_cast<int>(std::count(pick.begin(), pick.end(), -1)); }
    int Extra() const { return std::max(0, static_cast<int>(rows.size()) - forges); }  // 投掷狂人额外给的组数
    // forges 个锻造器、每个 mul 组；第 k 组的随机数只由 (seed, k) 决定，权重与单个锻造器相同（相当于独立买了 forges 个）
    void Begin(uint64_t purchaseSeed, int count, int perForge, int /*unitPrice*/, const rewards::Context& context) {
        *this = {};
        seed = purchaseSeed ^ 0xF046E45A8B93ull;
        mul = std::clamp(perForge, 1, 3);
        forges = std::clamp(count, 0, kForgeBatchMax);
        const int groups = forges * mul;
        for (int k = 0; k < groups; ++k) {
            AttributeBurst b;
            b.seed = seed ^ (0xD6E8FEB86659FD93ull * static_cast<uint64_t>(k + 1));
            b.attributeOnly = true; b.remaining = 1;
            b.Draw(context);
            if (b.offers.empty()) continue;
            rows.push_back(b.offers);
        }
        pick.assign(rows.size(), -1);
    }
    // 确认：picks[i] = 第 i 组选的序号（-1 / 越界按第 0 个）。grant 发放一项、返回实际加上去的量（超出上限的部分作废）；
    // 返回发放的组数，wasted = 有部分或全部超出上限作废的组数。不退款、不替换
    template <class Grant>
    int Confirm(const std::vector<int>& picks, Grant grant, int* wasted = nullptr) {
        int ok = 0, lost = 0;
        for (size_t i = 0; i < rows.size(); ++i) {
            const auto& row = rows[i];
            int k = i < picks.size() ? picks[i] : -1;
            if (k < 0 || k >= static_cast<int>(row.size())) k = 0;
            const auto& o = row[static_cast<size_t>(k)];
            if (grant(o) < o.amount) ++lost;
            ++ok;
        }
        if (wasted) *wasted = lost;
        *this = {};
        return ok;
    }
    std::string Save() const {
        std::string s = std::format("2 {} {} {} {}", seed, forges, mul, rows.size());
        for (size_t i = 0; i < rows.size(); ++i) {
            s += std::format(" {} {}", i < pick.size() ? pick[i] : -1, rows[i].size());
            for (const auto& o : rows[i]) s += " " + rewards::SaveOffer(o);
        }
        return s;
    }
    static bool Load(std::string_view text, ForgeBatch* out) {
        ForgeBatch b; int version = 0; long long n = -1;
        std::istringstream in{std::string(text)};
        if (!(in >> version >> b.seed) || (version != 1 && version != 2)) return false;
        if (version == 1) { int unused = 0; if (!(in >> unused >> n)) return false; b.forges = static_cast<int>(std::clamp(n, 0LL, static_cast<long long>(kForgeBatchMax))); b.mul = 1; }  // 3.6.1 测试包早期格式
        else if (!(in >> b.forges >> b.mul >> n) || b.forges < 0 || b.mul < 1 || b.mul > 3) return false;
        if (n < 0 || n > kForgeBatchMax * 3LL) return false;
        for (long long i = 0; i < n; ++i) {
            int p = -1, count = 0;
            if (!(in >> p >> count) || count < 1 || count > 3 || p < -1 || p >= count) return false;
            std::vector<rewards::Offer> row;
            for (int k = 0; k < count; ++k) {
                rewards::Offer o;
                if (!rewards::LoadOffer(in, &o) || !rewards::Sane(o) || o.kind != rewards::Kind::Attr) return false;
                row.push_back(o);
            }
            b.rows.push_back(std::move(row)); b.pick.push_back(p);
        }
        in >> std::ws; if (!in.eof()) return false;
        *out = std::move(b); return true;
    }
};
}
