#include <set>
#include <cstdio>
#include <cstdlib>
#include "../src/tower_growth_run.h"
static void Check(bool ok, const char* why) { if (!ok) { std::printf("FAIL: %s\n", why); std::exit(1); } }
int main() {
    tower::growthrun::Runner r, loaded;
    r.profile = tower::growth::Profile::Kit();
    Check(tower::growthrun::Runner::Load(r.Save(), &loaded) && !loaded.hexes.enabled, "legacy run remains without augment system");
    r.hexes.Init(20261110); Check(r.hexes.Begin(1), "draw first milestone");
    const auto offered = r.hexes.offers;
    Check(tower::growthrun::Runner::Load(r.Save(), &loaded) && loaded.hexes.pending == 1 && loaded.hexes.offers == offered, "pending choice retained");
    Check(loaded.hexes.Pick(0), "choose once"); loaded.hexes.ClearFloor(1);
    tower::growthrun::Runner roundtrip;
    Check(tower::growthrun::Runner::Load(loaded.Save(), &roundtrip) && roundtrip.hexes.WasCleared(1) && roundtrip.hexes.completed == 1, "choice and floor persisted together");
    {   // 4.1.0 海克斯冷却随成长档保存；旧档没有这行 = 不在冷却
        int cooling = 0; for (auto c : roundtrip.hexes.cool) cooling += c > 0;
        Check(cooling >= 1 && roundtrip.hexes.cool == loaded.hexes.cool, "4.1.0 hex cooldowns persist");
        std::string old = loaded.Save(); const auto at = old.find("gr.hexcool="); Check(at != std::string::npos, "4.1.0 hexcool line written");
        old.erase(at, old.find('\n', at) - at + 1);
        tower::growthrun::Runner legacy; Check(tower::growthrun::Runner::Load(old, &legacy) && legacy.hexes.cool == decltype(legacy.hexes.cool){}, "4.1.0 old saves load with no cooldowns");
    }
    loaded.hexRewardSecond = true; loaded.hexBonusSecond = true;
    Check(tower::growthrun::Runner::Load(loaded.Save(), &roundtrip) && roundtrip.hexRewardSecond && roundtrip.hexBonusSecond, "second reward and bonus page state persist");
    tower::growthrun::Runner forge; forge.profile = tower::growth::Profile::Kit();
    forge.GrantHexChoice(tower::hex::Forge);
    Check(forge.profile.temper == 20 && forge.profile.temperEarned == 20 && forge.profile.soulTokens == 5, "forge grants twenty temper and five tokens");
    for (bool doubled : {false, true}) {
        tower::growthrun::Runner burst; burst.profile = tower::growth::Profile::Kit(); burst.hexes.Init(20261110);
        burst.hexes.picks[0] = tower::hex::AttributeStack; burst.hexes.pickedAt[0] = 1; burst.hexes.completed = 1;
        if (doubled) { burst.hexes.picks[1] = tower::hex::Thrower; burst.hexes.pickedAt[1] = tower::hex::SlotFloor(1); burst.hexes.completed |= 2; }
        std::array<int, tower::kItemCount> items{};
        burst.hexAttributes.Begin(20261110, doubled, burst.Ctx(1,50,items));
        const int total = doubled ? 8 : 4;
        for (int i = 0; i < total; ++i) {
            Check(burst.hexAttributes.remaining == total-i && burst.hexAttributes.offers.size() == 3, "attribute burst presents three options and counts down");
            auto before = burst.hexAttributes.offers;
            tower::growthrun::Runner resumed;
            Check(tower::growthrun::Runner::Load(burst.Save(), &resumed) && resumed.hexAttributes.offers == before && resumed.hexAttributes.claimed == i, "attribute burst preserves current offer on reload");
            burst = std::move(resumed);
            tower::rewards::Offer got;
            const auto previous = burst.attrs.pm;
            auto grant = [&](const tower::rewards::Offer& o) { if (o.kind == tower::rewards::Kind::Attr) return burst.attrs.Add(o.index,o.amount)==o.amount; return true; };
            Check(!burst.hexAttributes.Pick(0, i+1, burst.Ctx(1,50,items), grant, &got) && burst.attrs.pm == previous, "stale page cannot claim another reward");
            Check(burst.hexAttributes.Pick(0, i, burst.Ctx(1,50,items), grant, &got), "claim grants one reward exactly once");
        }
        Check(burst.hexAttributes.remaining == 0 && burst.hexAttributes.offers.empty(), "attribute burst ends after all choices");
    }
    {
        tower::growthrun::Runner journal; journal.profile = tower::growth::Profile::Kit();
        journal.hexForgePurchases = 17; journal.hexJournal.active = true; journal.hexJournal.jade = 1234; journal.hexJournal.items[0] = 3;
        tower::growthrun::Runner resumed;
        Check(tower::growthrun::Runner::Load(journal.Save(), &resumed) && resumed.hexJournal.active && resumed.hexJournal.jade == 1234 && resumed.hexJournal.items[0] == 3 && resumed.hexForgePurchases == 17, "purchase journal survives interrupted save and preserves purchase sequence");
    }
    {
        tower::growthrun::Runner shop; shop.profile = tower::growth::Profile::Kit(); shop.hexes.Init(42);
        shop.hexes.picks[0]=tower::hex::AttributeStack; shop.hexes.pickedAt[0]=1; shop.hexes.completed=1;
        std::array<int, tower::kItemCount> items{}; int jade=299;
        Check(!shop.BeginAttributeForge(42,jade,1,50,items) && jade==299 && !shop.hexJournal.active, "forge rejects insufficient funds without mutation");
        jade=3000;
        Check(shop.BeginAttributeForge(42,jade,1,50,items) && jade==2500 && shop.hexAttributes.remaining==1 && shop.hexJournal.active, "4.0.1 forge before floor 100 costs 500 and journals one pending reward");
        Check(!shop.BeginAttributeForge(42,jade,1,50,items) && jade==2500, "pending purchase cannot charge twice");
        Check(tower::hex::ForgePrice(50,50) == 500 && tower::hex::ForgePrice(51,50) == 1000 && tower::hex::ForgePrice(100,50) == 1000 && tower::hex::ForgePrice(101,50) == 1500 && tower::hex::ForgePrice(151,50) == 2000 && tower::hex::ForgePrice(201,50) == 2500, "4.0.1 forge price: 500 / 1000 / 1500 / 2000 / 2500, +500 every 50 floors");
        tower::growthrun::Runner recovered;
        Check(tower::growthrun::Runner::Load(shop.Save(), &recovered) && recovered.hexJournal.jade==2500 && recovered.hexAttributes.offers==shop.hexAttributes.offers, "interrupted forge retains debit and same options");
    }
    for (bool doubled : {false, true}) {
        tower::growthrun::Runner r; r.profile = tower::growth::Profile::Kit();
        std::array<int, tower::kItemCount> items{};
        r.hexAttributes.BeginPurchase(99, doubled, r.Ctx(1,50,items));
        for (int i=0; i<(doubled ? 2 : 1); ++i) {
            Check(r.hexAttributes.offers.size()==3, "forge offers three attributes");
            for (auto o : r.hexAttributes.offers) Check(o.kind == tower::rewards::Kind::Attr, "forge excludes equipment and items");
            auto grant=[&](const tower::rewards::Offer& o){return r.attrs.Add(o.index,o.amount)==o.amount;};
            tower::rewards::Offer got;
            Check(r.hexAttributes.Pick(0,i,r.Ctx(1,50,items),grant,&got), "forge reward grants attribute");
        }
    }
    for (bool doubled : {false, true}) {  // 3.1.0 速战速决 × 投掷狂人：属性页选两次，存读档保留，档位跟层数走
        tower::growthrun::Runner b; b.profile = tower::growth::Profile::Kit(); b.hexes.Init(77);
        b.hexes.picks[0] = tower::hex::Blitz; b.hexes.pickedAt[0] = 1; b.hexes.completed = 1;
        if (doubled) { b.hexes.picks[1] = tower::hex::Thrower; b.hexes.pickedAt[1] = tower::hex::SlotFloor(1); b.hexes.completed |= 2; }
        std::array<int, tower::kItemCount> items{};
        const int n = tower::hex::kBlitzAttrPicks * (b.hexes.Has(tower::hex::Thrower) ? 2 : 1);
        b.hexAttributes.BeginAttributes(5150, n, b.Ctx(3, 50, items));
        Check(b.hexAttributes.blitz && b.hexAttributes.remaining == (doubled ? 2 : 1), "blitz page: Thrower doubles the picks");
        for (int i = 0; i < n; ++i) {
            tower::growthrun::Runner re;
            Check(tower::growthrun::Runner::Load(b.Save(), &re) && re.hexAttributes.blitz && re.hexAttributes.remaining == n - i && re.hexAttributes.offers == b.hexAttributes.offers, "blitz page survives reload mid-way");
            for (auto o : b.hexAttributes.offers) Check(o.kind == tower::rewards::Kind::Attr, "blitz page offers attributes only");
            auto grant = [&](const tower::rewards::Offer& o) { return b.attrs.Add(o.index, o.amount) == o.amount; };
            tower::rewards::Offer got;
            Check(b.hexAttributes.Pick(0, i, b.Ctx(3, 50, items), grant, &got), "blitz pick grants an attribute");
        }
        Check(b.hexAttributes.remaining == 0, "blitz page closes after its picks");
    }
    {   // 速战速决档位跟层数走，属性叠属性用顶档
        tower::growthrun::Runner lo; lo.profile = tower::growth::Profile::Kit(); std::array<int, tower::kItemCount> items{};
        lo.hexAttributes.BeginAttributes(9, 1, lo.Ctx(1, 50, items));
        tower::growthrun::Runner top; top.profile = tower::growth::Profile::Kit();
        top.hexAttributes.BeginPurchase(9, false, top.Ctx(1, 50, items));
        // 3.3.3：锻造改为等概率抽全部属性，不能再比"总量"；改为同一属性逐项比较档位
        auto c50 = top.Ctx(1, 50, items); c50.attributeFloor = 50;
        const auto pool = tower::rewards::detail::Attrs(c50);
        bool lower = !lo.hexAttributes.offers.empty() && !top.hexAttributes.offers.empty();
        for (auto o : lo.hexAttributes.offers)
            lower = lower && std::any_of(pool.begin(), pool.end(), [&](const auto& c) { return c.offer.index == o.index && c.offer.amount >= o.amount; });
        Check(lower, "blitz attributes follow the current floor tier, not the top tier");
        std::set<int> kinds;  // 锻造覆盖全部面板属性：多抽几次，每种属性都能出现
        for (uint64_t sd = 1; sd < 400; ++sd) { tower::growthrun::Runner f; f.profile = tower::growth::Profile::Kit(); f.hexAttributes.BeginPurchase(sd, false, f.Ctx(1, 50, items)); for (auto o : f.hexAttributes.offers) kinds.insert(o.index); }
        Check(static_cast<int>(kinds.size()) == tower::rewards::kAttrCount, "3.3.3 forge can offer every panel attribute");
    }
    auto bad = r.Save(); const auto p = bad.find("gr.hex="); Check(p != std::string::npos, "hex line present");
    bad.replace(p, bad.find('\n', p) - p, "gr.hex=broken");
    Check(!tower::growthrun::Runner::Load(bad, &roundtrip), "malformed state refuses resume");
    {   // 3.6.1 批量锻造：一次买 n 个、扣 n 份钱、一次生成 n 组（投掷狂人 ×2），不设上限；存读档保留选择；确认后加到属性上，加满的组退钱
        tower::growthrun::Runner f; f.profile = tower::growth::Profile::Kit(); f.hexes.Init(4242);
        f.hexes.picks[0] = tower::hex::AttributeStack; f.hexes.pickedAt[0] = 1; f.hexes.completed = 1;
        std::array<int, tower::kItemCount> items{};
        int jade = tower::hex::kForgeStep * 120 + 50;
        const int got = f.BeginForgeBatch(7, jade, std::numeric_limits<int>::max(), 1, 50, items);
        Check(got == 120 && jade == 50 && f.forgeBatch.rows.size() == 120 && f.forgeBatch.Unpicked() == 120 && f.hexForgePurchases == 120, "3.6.1 forge max buys every affordable forge, no cap");
        Check(f.BeginForgeBatch(7, jade, 1, 1, 50, items) == 0 && !f.BeginAttributeForge(7, jade, 1, 50, items), "3.6.1 a pending batch blocks another purchase");
        bool attrs = true; for (const auto& row : f.forgeBatch.rows) { attrs = attrs && !row.empty() && row.size() <= 3; for (const auto& o : row) attrs = attrs && o.kind == tower::rewards::Kind::Attr; }
        Check(attrs, "3.6.1 every group offers 1-3 attributes");
        f.forgeBatch.pick[0] = 2; f.forgeBatch.pick[5] = 1;
        tower::growthrun::Runner back;
        Check(tower::growthrun::Runner::Load(f.Save(), &back) && back.forgeBatch.rows.size() == 120 && back.forgeBatch.pick[0] == 2 && back.forgeBatch.pick[5] == 1 &&
              back.forgeBatch.rows[3] == f.forgeBatch.rows[3] && back.hexJournal.active, "3.6.1 batch survives save / load (exit midway, nothing lost)");
        int wasted = 0, applied = 0;
        const auto snapshot = f.forgeBatch;
        std::vector<int> picks(120, 0); picks[0] = 2;
        std::vector<tower::rewards::Offer> sent;
        const int granted = f.forgeBatch.Confirm(picks, [&](const tower::rewards::Offer& o) { ++applied; sent.push_back(o); return f.attrs.Add(o.index, o.amount); }, &wasted);
        Check(!f.forgeBatch.Active() && granted == 120 && applied == 120 && sent[0] == snapshot.rows[0][2] && sent[1] == snapshot.rows[1][0], "3.6.1 confirm grants exactly the chosen option of every group (unpicked = first)");
        int w2 = 0; auto full = snapshot;
        const int all = full.Confirm({}, [](const tower::rewards::Offer&) { return 0; }, &w2);
        Check(all == 120 && w2 == 120, "3.6.1 capped options are consumed, overflow discarded, no refund (user 2026-11-15)");
        Check(snapshot.forges == 120 && snapshot.mul == 1 && snapshot.Extra() == 0, "3.6.1 no thrower: no extra groups");
        tower::growthrun::Runner t; t.profile = tower::growth::Profile::Kit(); t.hexes.Init(4243);
        t.hexes.picks[0] = tower::hex::AttributeStack; t.hexes.pickedAt[0] = 1; t.hexes.picks[1] = tower::hex::Thrower; t.hexes.pickedAt[1] = tower::hex::SlotFloor(1); t.hexes.completed = 3;
        int jt = tower::hex::kForgeStep * 10;
        Check(t.BeginForgeBatch(9, jt, 10, 1, 50, items) == 10 && t.forgeBatch.rows.size() == 20 && t.forgeBatch.forges == 10 && t.forgeBatch.Extra() == 10 && jt == 0, "3.6.1 thrower Lv1: 10 forges = 20 groups, 10 extra shown");
        tower::growthrun::Runner tb; Check(tower::growthrun::Runner::Load(t.Save(), &tb) && tb.forgeBatch.forges == 10 && tb.forgeBatch.mul == 2 && tb.forgeBatch.Extra() == 10, "3.6.1 thrower extra survives save / load");
        tower::growthrun::Runner old;
        Check(tower::growthrun::Runner::Load(r.Save(), &old) && !old.forgeBatch.Active(), "3.6.1 saves without a batch load as before");
    }
    std::puts("TOWER_HEX_PERSIST_OK");
}
