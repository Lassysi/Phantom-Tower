// 影塔新奖励模型离线测试：进度门槛、第一区全凡、三格过滤 / 去重 / 刷新、商店交易与存读档、准备道具、快捷栏（不碰游戏）
#include <cstdio>
#include <map>
#include <cstdlib>
#include <string>

#include "../src/tower_rewards.h"
#include "../src/tower_shop_panel.h"
#include "../src/tower_reward_text.h"
#include "../src/tower_menu_text.h"
#include "../src/boss_drops.generated.h"
#include "../src/tower_economy.h"

using namespace tower;
using namespace tower::rewards;

static int g_checks = 0;
static void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { std::printf("FAIL %s\n", what); std::exit(1); }
}

// 测试表：几把兵器 / 饰品按区开放；一把 Boss 关联
static Catalog TestCatalog(int* bad = nullptr) {
    std::string t = "# test\n";
    int n = 0;
    for (int i = 0; i < growth::kWeaponCount && n < 12; ++i, ++n)
        t += std::format("{} {} {} {} 1 1\n", growth::kWeapons[i].kind == 1 ? "shaqi" : "main", growth::kWeapons[i].id, n % kZones, n == 5 ? 1 : 0);
    n = 0;
    for (int i = 0; i < growth::kAccessoryCount && n < 10; ++i)
        if (growth::AccessoryInPool(i)) { t += std::format("acc {} {} 0 1 1\n", growth::kAccessories[i].id, n % 3); ++n; }
    t += "acc AC_Loot 0 0 1 1\nmain NoSuchWeapon 0 0 1 1\nbroken line\nmed Hulu 9 0 1 1\n";
    return ParseCatalog(t, Catalog::Default(), bad);
}
static int ZoneOfOffer(const Catalog& cat, const Offer& o) {
    const Cat c = o.kind == Kind::Medicine ? Cat::Medicine : o.kind == Kind::Weapon ? (growth::kWeapons[o.index].kind == 1 ? Cat::Shaqi : Cat::Main) : Cat::Accessory;
    const Gate* g = cat.Find(c, o.index);
    return g ? g->zone : -9;
}

int main() {
    // ---- 进度表
    {
        const auto def = Catalog::Default();
        bool weaponsPending = true;
        for (auto& g : def.gates) if (g.cat == Cat::Main || g.cat == Cat::Shaqi || g.cat == Cat::Accessory) weaponsPending &= g.zone == -1;
        Check(weaponsPending, "default table: weapons and accessories are pending (no guessing by name)");
        Check(def.Find(Cat::Medicine, growth::MedicineIndex(growth::kMedTier2[0]))->zone == 1, "tier-2 medicine opens in zone 2");
        int bad = 0;
        const auto cat = TestCatalog(&bad);
        Check(bad == 4, "bad lines / unknown ids / out-of-range zone counted and skipped");
        Check(!cat.Find(Cat::Accessory, growth::AccessoryIndex("AC_Loot")), "excluded (paid / entitlement) accessory never enters the pool");
        Context c; growth::Profile p = growth::Profile::Kit(); growth::Blessings b; c.profile = &p; c.blessings = &b; c.catalog = &def; c.floor = 3;
        Xs r{11};
        bool none = true;
        for (int k = 0; k < 300; ++k) for (auto& o : Draw(r, c)) none &= o.kind != Kind::Weapon && o.kind != Kind::Accessory;
        Check(none, "pending weapons / accessories never drawn");
    }

    const auto cat = TestCatalog();
    {
        const auto official = Contract(); growth::Profile p; growth::Blessings b;
        Context c; c.profile = &p; c.blessings = &b; c.catalog = &official; c.floor = 50; c.floors = 50;
        for (auto [id, kind] : {std::pair{"ZhuGuangBaoQi", 0}, {"MoQie", 0}, {"XueSeLingDang", 1}, {"BaWangLianQiu", 1}, {"MoShou", 1}}) {
            const int i = growth::WeaponIndex(id, kind);
            const Cat category = kind ? Cat::Shaqi : Cat::Main;
            const Offer o{Kind::Weapon, i, 1, 9};
            Check(i >= 0 && RetiredWeapon(i) && official.Find(category, i) && official.Find(category, i)->zone < 0, "prototype weapon is blocked in official catalog");
            Check(!StillValid(o, c), "prototype cannot bypass the block via starter or boss quality");
            auto forced = official; forced.Set({category, i, 0, false, true, true});
            auto forcedContext = c; forcedContext.catalog = &forced;
            const auto candidates = detail::Equipment(forcedContext, false, category, false);
            Check(std::none_of(candidates.begin(), candidates.end(), [&](const detail::Cand& x) { return x.offer.index == i; }), "prototype stays blocked even if a local catalog override enables it");
            Shop s; s.floor = 50; s.version = 1; s.shelf.push_back({o, 100, false});
            int jade = 1000, grants = 0;
            Check(shoppanel::Build(s, jade, 0).rows.empty() && Purchase(s, 0, s.version, jade, c, [&](const Offer&) { ++grants; return true; }) == Buy::Invalid && jade == 1000 && grants == 0, "old shelf neither displays nor sells prototype");
            Stock14(s, c, {o});
            Check(std::none_of(s.shelf.begin(), s.shelf.end(), [&](const Ware& w) { return w.offer.kind == Kind::Weapon && w.offer.index == i; }), "boss supplement cannot restock prototype");
        }
    }
    // 3.4.0（用户：从第 1 层查起，驿站每类按数量随机上架，买完才没有；Boss 兵器打完后进池，非 Boss 道具随层数进池，无尽层全部兵器都进池）
    {
        const auto official = Contract(); growth::Blessings blessings;
        auto bossFloor = [](int w) { return 1 + (w * 7) % 50; };  // 模拟：第 w 把 Boss 兵器在某层被打掉
        auto supplement = [&](int floor) { std::vector<Offer> v; for (int w = 0; w < growth::kWeaponCount; ++w) if (detail::GrantableWeapon(w) && bossFloor(w) <= floor) v.push_back({Kind::Weapon, w, 1, 9}); return v; };
        auto shelfCount = [](const Shop& s, Kind k) { int n = 0; for (const auto& w : s.shelf) if (w.offer.kind == k && w.offer.quality != kRareQuality && !(k == Kind::Accessory && ResistCharm(w.offer.index))) ++n; return n; };
        auto poolCount = [&](const Context& c, int floor, Kind k) { auto cc = c; cc.shopPerCategory = 999; Shop all; all.floor = floor; Stock14(all, cc, supplement(floor)); return shelfCount(all, k); };
        // 1) 不买：每类上架数 = min(定额, 池子)；池子随层数只增不减；无尽层每把可发放的兵器都在池里
        {
            growth::Profile p; Context c; c.profile = &p; c.blessings = &blessings; c.catalog = &official; c.floors = 50;
            int last[3] = {0, 0, 0}; bool ok = true, grow = true;
            for (int f = 1; f <= 120; ++f) {
                c.floor = f;
                Shop s; s.floor = f; s.seed = 77; Stock14(s, c, supplement(f));
                const Kind ks[3] = {Kind::Weapon, Kind::Medicine, Kind::Accessory};
                const int quota[3] = {balance::kShelfWeapon, balance::kShelfMedicine, balance::kShelfAccessory};
                for (int k = 0; k < 3; ++k) {
                    const int pool = poolCount(c, f, ks[k]);
                    ok = ok && shelfCount(s, ks[k]) == std::min(quota[k], pool);
                    grow = grow && pool >= last[k];
                    last[k] = pool;
                }
            }
            Check(ok, "every floor 1-120: each category shows min(quota, pool) random items");
            Check(grow, "pools only grow with floors (boss weapons after their boss, others by zone)");
            int grantable = 0; for (int w = 0; w < growth::kWeaponCount; ++w) grantable += detail::GrantableWeapon(w) || (official.Find(growth::kWeapons[w].kind == 1 ? Cat::Shaqi : Cat::Main, w) && official.Find(growth::kWeapons[w].kind == 1 ? Cat::Shaqi : Cat::Main, w)->zone >= 0 && !RetiredWeapon(w));
            c.floor = 51; Check(poolCount(c, 51, Kind::Weapon) >= grantable - 2, "endless: every grantable weapon is in the weapon pool");
        }
        // 2) 每次驿站把上架的兵器 / 药 / 饰品全买下：只有池子真的空了（全拿到）才不再上架
        {
            growth::Profile p; Context c; c.profile = &p; c.blessings = &blessings; c.catalog = &official; c.floors = 50;
            bool fair = true;
            for (int f = 1; f <= 200; ++f) {
                c.floor = f;
                Shop s; s.floor = f; s.seed = 99; Stock14(s, c, supplement(f));
                for (Kind k : {Kind::Weapon, Kind::Medicine, Kind::Accessory}) fair = fair && (shelfCount(s, k) > 0) == (poolCount(c, f, k) > 0);
                for (const auto& w : s.shelf) {
                    if (w.offer.kind == Kind::Weapon) p.AddWeapon(w.offer.index);
                    else if (w.offer.kind == Kind::Medicine) p.AddMedicine(w.offer.index);
                    else if (w.offer.kind == Kind::Accessory) p.AddAccessory(w.offer.index);
                }
            }
            Check(fair, "a category is empty only when its pool is fully owned");
            c.floor = 200;
            Check(poolCount(c, 200, Kind::Weapon) == 0 && poolCount(c, 200, Kind::Medicine) == 0, "buying everything eventually empties the pools");
        }
    }
    // 正式目录中的非 Boss 武器：奖励和驿站共用候选，获得后同时从两处消失。
    {
        const auto official = Contract();
        growth::Blessings blessings;
        int checked = 0;
        for (const auto& gate : official.gates) {
            if ((gate.cat != Cat::Main && gate.cat != Cat::Shaqi) || gate.bossLinked ||
                gate.zone < 0 || !gate.reward || !gate.shop) continue;
            growth::Profile p;
            Context c; c.profile = &p; c.blessings = &blessings; c.catalog = &official; c.floor = 50; c.floors = 50;
            c.shopPerCategory = 999;  // 这里验池子成员：整池上架
            const auto contains = [&](const auto& candidates) {
                return std::any_of(candidates.begin(), candidates.end(), [&](const auto& x) {
                    return x.offer.kind == Kind::Weapon && x.offer.index == gate.index;
                });
            };
            Check(contains(detail::Equipment(c, false, Cat::Main, true)), "unowned non-Boss weapon enters normal and bonus reward candidates");
            Shop shop; shop.floor = 50; Stock14(shop, c, {});
            Check(contains(shop.shelf), "unowned non-Boss weapon enters shop shelf");
            Check(p.AddWeapon(gate.index), "weapon acquisition succeeds");
            Check(!contains(detail::Equipment(c, false, Cat::Main, true)), "acquired weapon disappears from both reward pools");
            Stock14(shop, c, {});
            Check(!contains(shop.shelf), "acquired weapon disappears from shop shelf");
            ++checked;
        }
        Check(checked > 0, "official non-Boss weapons were checked");
    }
    // 任务来源的正式饰品 / 药瓶随区开放，持有后不再作为新商品。
    {
        const auto official = Contract(); growth::Profile p; growth::Blessings blessings;
        Context c; c.profile = &p; c.blessings = &blessings; c.catalog = &official; c.floors = 50;
        c.shopPerCategory = 999;  // 验池子成员：整池上架
        int checked = 0;
        for (const auto& gate : official.gates) {
            if ((gate.cat != Cat::Accessory && gate.cat != Cat::Medicine) || gate.zone < 0 || !gate.shop || gate.bossLinked) continue;
            if (gate.cat == Cat::Accessory && ResistCharm(gate.index)) continue;  // 3.2.0 抗异常饰品常驻，下面单独验
            c.floor = gate.zone * 10 + 1;
            Shop shop; shop.floor = c.floor; Stock14(shop, c, {});
            const Kind kind = gate.cat == Cat::Accessory ? Kind::Accessory : Kind::Medicine;
            auto onShelf = [&] { return std::any_of(shop.shelf.begin(), shop.shelf.end(), [&](const Ware& w) { return w.offer.kind == kind && w.offer.index == gate.index && w.offer.quality != kRareQuality; }); };  // 珍品位不按进度，另测
            Check(onShelf(), "task accessory or medicine available at its progression zone");
            if (gate.zone > 0) {
                c.floor = gate.zone * 10; Stock14(shop, c, {});
                Check(!onShelf(), "task accessory or medicine not offered before its progression zone");
            }
            if (gate.cat == Cat::Accessory) p.AddAccessory(gate.index); else p.AddMedicine(gate.index);
            c.floor = gate.zone * 10 + 1; Stock14(shop, c, {});
            Check(!onShelf(), "owned accessory or medicine removed from shop");
            ++checked;
        }
        Check(checked > 0, "official accessories and medicines were checked");
    }
    // ---- Boss 必掉的兵器 / 影之武装只走固定掉落，三选一里永远不出（用户 2026-11-05：三选一出了巨凶斧）
    {
        growth::Profile p = growth::Profile::Kit(); growth::Blessings b;
        bool noBoss = true; int weapons = 0;
        for (int floor = 1; floor <= 50; ++floor)
            for (uint64_t seed = 1; seed < 60; ++seed) {
                Context c; c.profile = &p; c.blessings = &b; c.catalog = &cat; c.floor = floor; c.floors = 50;
                Xs r{seed * 104729 + floor};
                for (auto& o : Draw(r, c)) {
                    if (o.kind != Kind::Weapon) continue;
                    ++weapons;
                    noBoss &= !cat.Find(growth::kWeapons[o.index].kind == 1 ? Cat::Shaqi : Cat::Main, o.index)->bossLinked;
                }
            }
        Check(noBoss && weapons > 0, "three-choice never offers a Boss-linked weapon (fixed Boss drops only); non-Boss weapons still offered");
    }
    // ---- 第一区（1–10 层，含第 10 层与刷新）全凡；不越级；不重复；已持有过滤；领取时仍合法
    {
        growth::Profile p = growth::Profile::Kit(); growth::Blessings b;
        bool allMortal = true, inZone = true, unique = true, valid = true, ownedFiltered = true;
        for (int floor = 1; floor <= 10; ++floor)
            for (uint64_t seed = 1; seed < 120; ++seed) {
                Context c; c.profile = &p; c.blessings = &b; c.catalog = &cat; c.floor = floor; c.floors = 50;
                Xs r{seed * 7919 + floor};
                auto g = Draw(r, c);
                c.avoid.assign(g.begin(), g.end());
                auto again = Draw(r, c);  // 刷新
                for (auto* grp : {&g, &again}) {
                    for (int i = 0; i < 3; ++i) {
                        const auto& o = (*grp)[i];
                        allMortal &= o.quality == 1;
                        if (o.kind == Kind::Weapon || o.kind == Kind::Accessory || o.kind == Kind::Medicine) inZone &= ZoneOfOffer(cat, o) == 0;
                        if (o.kind == Kind::Weapon) ownedFiltered &= p.Find(o.index) < 0;
                        if (o.kind == Kind::Medicine) ownedFiltered &= !p.HasMedicine(o.index);
                        valid &= StillValid(o, c);
                        for (int j = 0; j < i; ++j) unique &= !((*grp)[j] == o);
                    }
                }
            }
        Check(allMortal, "floors 1-10 (incl. 10 and refresh) only mortal quality");
        Check(inZone, "zone 1 never shows later-zone equipment");
        Check(unique, "no duplicate item / effect inside one group");
        Check(ownedFiltered, "owned weapons / medicines are not offered as new");
        Check(valid, "every shown option is actually claimable");
    }
    // ---- 刷新避开刚显示的三项（候选够时）
    {
        growth::Profile p = growth::Profile::Kit(); growth::Blessings b;
        int overlaps = 0, total = 0;
        for (uint64_t seed = 1; seed < 200; ++seed) {
            Context c; c.profile = &p; c.blessings = &b; c.catalog = &cat; c.floor = 25; c.floors = 50;
            Xs r{seed};
            auto g = Draw(r, c);
            c.avoid.assign(g.begin(), g.end());
            auto h = Draw(r, c);
            for (auto& o : h) { ++total; for (auto& x : g) overlaps += o == x && o.kind != Kind::Jade; }
        }
        Check(overlaps * 20 < total, "refresh mostly avoids the previous three (legal pool permitting)");
    }
    // ---- 后段：36 层起无点数包；武器仍有来源；品质可高于凡
    {
        growth::Profile p = growth::Profile::Kit(); growth::Blessings b;
        bool noPoints = true; int weapons = 0, high = 0;
        for (uint64_t seed = 1; seed < 400; ++seed) {
            Context c; c.profile = &p; c.blessings = &b; c.catalog = &cat; c.floor = 41; c.floors = 50;
            Xs r{seed};
            for (auto& o : Draw(r, c)) {
                noPoints &= o.kind != Kind::TemperPack && o.kind != Kind::CultivatePack;
                weapons += o.kind == Kind::Weapon;
                high += o.quality > 1;
            }
        }
        Check(noPoints, "no point packs after floor 35");
        Check(weapons > 20, "late floors still offer weapons");
        Check(high > 0, "later zones can roll above mortal");
    }
    // ---- 祝福：只给有实际收益的，品质 = 实际层数
    {
        growth::Profile p = growth::Profile::Kit(); growth::Blessings b;
        for (int i = 0; i < growth::kBlessCount; ++i) if (growth::kBlessings[i].ready) b.stacks[i] = growth::kBlessings[i].maxStack;
        bool none = true;
        for (uint64_t seed = 1; seed < 200; ++seed) {
            Context c; c.profile = &p; c.blessings = &b; c.catalog = &cat; c.floor = 22; c.floors = 50;
            Xs r{seed};
            for (auto& o : Draw(r, c)) none &= o.kind != Kind::Blessing;
        }
        Check(none, "capped blessings filtered");
        growth::Blessings one; one.stacks[0] = growth::kBlessings[0].maxStack - 1;
        bool honest = true;
        for (uint64_t seed = 1; seed < 400; ++seed) {
            Context c; c.profile = &p; c.blessings = &one; c.catalog = &cat; c.floor = 45; c.floors = 50;
            Xs r{seed};
            for (auto& o : Draw(r, c)) if (o.kind == Kind::Blessing) honest &= o.quality == o.amount && o.amount <= one.Room(o.index) && (o.index != 0 || o.amount == 1);
        }
        Check(honest, "blessing quality equals the stacks it can really add (no truncated 'xuan')");
    }
    // ---- 饰品：满精炼过滤，未满给下一档
    {
        growth::Profile p = growth::Profile::Kit(); growth::Blessings b;
        int acc = -1;
        for (auto& g : cat.gates) if (g.cat == Cat::Accessory && g.zone == 0 && growth::kAccessories[g.index].refine >= 2) { acc = g.index; break; }
        Check(acc >= 0, "test table has a refinable zone-1 accessory");
        p.accessories.push_back({acc, 1});
        bool next = true, sawRefine = false;
        for (uint64_t seed = 1; seed < 300; ++seed) {
            Context c; c.profile = &p; c.blessings = &b; c.catalog = &cat; c.floor = 4; c.floors = 50;
            Xs r{seed};
            for (auto& o : Draw(r, c)) {
                if (o.index != acc || (o.kind != Kind::Refine && o.kind != Kind::Accessory)) continue;  // 下标只在同类里有意义
                next &= false;  // 3.1.0：已持有的饰品不再以任何形式出现（不再出精炼）
                sawRefine |= o.kind == Kind::Refine;
            }
        }
        Check(next && !sawRefine, "3.1.0: owned accessories never show up again (no refine offers)");
        p.accessories.back().refine = growth::kAccessories[acc].refine;
        bool gone = true;
        for (uint64_t seed = 1; seed < 300; ++seed) {
            Context c; c.profile = &p; c.blessings = &b; c.catalog = &cat; c.floor = 4; c.floors = 50;
            Xs r{seed};
            for (auto& o : Draw(r, c)) gone &= o.index != acc || (o.kind != Kind::Refine && o.kind != Kind::Accessory);
        }
        Check(gone, "fully refined accessory filtered");
    }
    // ---- 末层：不出"下一层 / 下一次三选一"才有用的道具；库存满的道具不出
    {
        growth::Profile p = growth::Profile::Kit(); growth::Blessings b;
        bool ok = true, full = true;
        for (uint64_t seed = 1; seed < 300; ++seed) {
            Context c; c.profile = &p; c.blessings = &b; c.catalog = &cat; c.floor = 50; c.floors = 50;
            Xs r{seed};
            for (auto& o : Draw(r, c)) if (o.kind == Kind::Item) ok &= ItemTiming(kItems[o.index].item) == Timing::Passive;
            Context d = c; d.floor = 12; d.items.fill(kItemStackCap);
            for (auto& o : Draw(r, d)) full &= o.kind != Kind::Item;
        }
        Check(ok, "last floor offers no next-floor / instant items");
        Check(full, "full item stacks are not offered");
        bool retired = true;
        for (int i = 0; i < kItemCount; ++i) if (kItems[i].item == Item::Heal || kItems[i].item == Item::Intel || kItems[i].item == Item::Borrow) retired &= ItemTiming(kItems[i].item) == Timing::Retired;
        Check(retired, "rejected / unimplemented items are retired");
    }

    // ---- 商店
    {
        growth::Profile p = growth::Profile::Kit(); growth::Blessings b;
        Context c; c.profile = &p; c.blessings = &b; c.catalog = &cat; c.floor = 15; c.floors = 50;
        Shop s; OpenShop(s, c, 99);
        Check(s.Open() && s.shelf.size() == 6, "six shelf slots");
        bool legal = true;
        for (auto& w : s.shelf) {
            if (w.offer.index < 0) { legal &= w.sold; continue; }
            if (w.offer.kind == Kind::Weapon || w.offer.kind == Kind::Accessory || w.offer.kind == Kind::Medicine) {
                legal &= ZoneOfOffer(cat, w.offer) <= 1;
                if (w.offer.kind == Kind::Weapon) { const Cat k = growth::kWeapons[w.offer.index].kind == 1 ? Cat::Shaqi : Cat::Main; legal &= !cat.Find(k, w.offer.index)->bossLinked; }
            }
            legal &= StillValid(w.offer, c) && w.price > 0;
        }
        Check(legal, "shelf: in zone, not boss-linked, claimable, priced");
        int slot = -1;
        for (int i = 0; i < 6; ++i) if (!s.shelf[i].sold) { slot = i; break; }
        Check(slot >= 0, "something to buy");
        const uint32_t v = s.version;
        int jade = 0;
        int grants = 0;
        auto ok = [&](const Offer&) { ++grants; return true; };
        Check(Purchase(s, slot, v, jade, c, ok) == Buy::NoJade && grants == 0, "not enough jade: nothing granted");
        jade = 10000;
        Check(Purchase(s, slot, v + 1, jade, c, ok) == Buy::Stale && jade == 10000, "stale shelf version refused");
        Check(Purchase(s, slot, v, jade, c, [](const Offer&) { return false; }) == Buy::GrantFailed && jade == 10000 && !s.shelf[slot].sold, "grant failure keeps jade and stock");
        const int price = s.shelf[slot].price;
        Check(Purchase(s, slot, v, jade, c, ok) == Buy::Ok && jade == 10000 - price && s.shelf[slot].sold && grants == 1, "purchase charges once after grant");
        Check(Purchase(s, slot, v, jade, c, ok) == Buy::Stale && grants == 1, "repeated click from the old screen does not buy again");
        Check(Purchase(s, slot, s.version, jade, c, ok) == Buy::Sold, "sold slot");
        // 存读档：货架 / 已购 / 刷新次数 / 随机状态都回来，读档不换货
        Prep prep; QuickBar quick;
        const auto text = Save(s, prep, quick);
        Shop s2; Prep p2; QuickBar q2;
        Check(Load(text, &s2, &p2, &q2), "load shop");
        bool same = s2.version == s.version && s2.seed == s.seed && s2.shelf.size() == s.shelf.size() && s2.bought.size() == s.bought.size();
        for (size_t i = 0; same && i < s.shelf.size(); ++i) same = s2.shelf[i].offer == s.shelf[i].offer && s2.shelf[i].price == s.shelf[i].price && s2.shelf[i].sold == s.shelf[i].sold;
        Check(same, "save/load keeps shelf, sold state and rng");
        Check(!Load("garbage", &s2, &p2, &q2) && !Load("shop 1 2 3 4 99", &s2, &p2, &q2), "bad save text refused");
        // 刷新：魂玉，逐次上涨；已买的不再上架
        const Offer boughtOffer = s.bought.back();
        const int before = jade;
        Check(Refresh(s, s.version, jade, c) == Buy::Ok && jade == before - RefreshCost(0, s.floor) && s.refreshes == 1, "refresh costs jade");
        bool notAgain = true;
        for (auto& w : s.shelf) notAgain &= !(w.offer == boughtOffer);
        Check(notAgain, "bought item not restocked this visit");
        const int before2 = jade;
        Refresh(s, s.version, jade, c);
        Check(before2 - jade == RefreshCost(1, s.floor) && RefreshCost(1, s.floor) > RefreshCost(0, s.floor), "refresh cost escalates");
        int poor = 0;
        Check(Refresh(s, s.version, poor, c) == Buy::NoJade, "refresh needs jade");
    }

    // ---- 准备道具（用户 2026-11-01：没有战斗中即时道具；每层最多预装 3 种不同的，不叠加，开战才扣）
    {
        std::array<int, kItemCount> have{};
        std::vector<int> preps;
        int revive = -1, reroll = -1;
        for (int i = 0; i < kItemCount; ++i) {
            const auto t = ItemTiming(kItems[i].item);
            if (t == Timing::Prep) preps.push_back(i);
            if (t == Timing::Passive) revive = i;
            if (t == Timing::Settlement) reroll = i;
            Check(t != Timing::Instant, "no item is used mid-fight any more");
        }
        Check(preps.size() == 11 && revive >= 0 && reroll >= 0, "eleven pre-floor items (3.1.0: +狂暴药 / 定魂香 / 悬赏令), revive stays automatic, reroll stays at the reward pick");
        { const int l = static_cast<int>(Item::Leech); Prep charm; have[l] = 1;
          Check(charm.Arm(l, have) && have[l] == 1, "lifesteal charm is prepared without consumption");
          Check(charm.Fire(have).size() == 1 && have[l] == 0 && charm.Active(l), "lifesteal charm consumes once on entry");
          charm.FloorEnded(); Check(!charm.Active(l), "lifesteal charm expires at floor end"); }
        Prep p;
        Check(!p.Arm(preps[0], have) && p.Why(preps[0], have) == "没有库存", "cannot arm without stock");
        for (int i : preps) have[i] = 2;
        have[revive] = 1;
        Check(!p.Arm(revive, have), "automatic items cannot be armed");
        Check(p.Arm(preps[0], have) && have[preps[0]] == 2, "arming does not consume");
        Check(!p.Arm(preps[0], have) && p.Why(preps[0], have) == "已经预装了", "the same item does not stack on one floor");
        Check(p.Arm(preps[1], have) && p.armed.size() == 2, "3.6.0: different MOD battle items can be used together (user 2026-11-15)");
        Check(p.Disarm(preps[0]) && p.armed.size() == 1 && p.Armed(preps[1]), "cancel one before the fight, nothing consumed");
        // 取消入场 / 生成失败：不调用 Fire，仍待发动
        Check(p.armed.size() == 1 && have[preps[0]] == 2 && have[preps[1]] == 2, "cancelled entry keeps the armed item unspent");
        const auto fired = p.Fire(have);
        Check(fired.size() == 1 && have[preps[1]] == 1 && have[preps[0]] == 2 && p.armed.empty(), "release consumes the armed item once");
        {
            auto retry = p; auto stock = have;
            Check(retry.RearmFired(stock) && retry.active.empty() && retry.Armed(preps[1]) && stock[preps[1]] == 2, "failed wave restores its consumed preparation for retry");
            Check(retry.RearmFired(stock) && stock[preps[1]] == 2, "repeated recovery cannot duplicate an item");
            Check(retry.Fire(stock).size() == 1 && stock[preps[1]] == 1, "retry consumes restored preparation exactly once");
            retry.armed = {preps[0]}; const auto saved = stock;
            Check(!retry.RearmFired(stock) && stock == saved && retry.Active(preps[1]), "conflicting preparation prevents recovery without mutating inventory");
        }
        Check(p.Active(preps[1]) && p.Fire(have).empty() && have[preps[1]] == 1, "fires only once per arm");
        p.FloorEnded();
        Check(p.active.empty(), "effects belong to that floor only");
        have[preps[4]] = 0;
        p.Arm(preps[4], have);
        Check(p.armed.empty(), "no stock: not armed");
        // 存读档：新格式多件；旧格式单件照读
        Shop s0; QuickBar q0; Prep p0;
        p0.Arm(preps[0], have); p0.Arm(preps[2], have);
        Shop s1; Prep p1; QuickBar q1;
        Check(p0.armed.size() == 2 && Load(Save(s0, p0, q0), &s1, &p1, &q1) && p1.armed == p0.armed, "prep survives save/load (several kinds)");
        Check(Load(std::format("shop -1 0 0 0 0 0 prep {} -1 quick -1 -1 -1 0", preps[0]), &s1, &p1, &q1) && p1.armed.size() == 1 && p1.armed[0] == preps[0], "old single-prep saves still load");
        Check(Load(std::format("shop -1 0 0 0 0 0 prep {} -1 quick -1 -1 -1 0", revive), &s1, &p1, &q1) && p1.armed.empty(), "an old save arming a non-prep item drops it");
    }
    // ---- 驿站大面板模型（32ffb7d：空货位隐藏重排、6 行一页、真实 slot、连点保护、结果文字）
    {
        namespace sp = tower::shoppanel;
        Shop s; s.floor = 5; s.version = 7;
        for (int i = 0; i < 8; ++i) s.shelf.push_back({Offer{Kind::Item, i % 2 ? -1 : 0, 1, 1}, 40 + i, false});  // 奇数位是空货位
        s.shelf.push_back({Offer{Kind::Jade, -1, 30, 1}, 10, true});  // 已售的魂玉袋（不是空位）
        const auto v = sp::Build(s, 45, 0);
        Check(v.total == 5 && v.pages == 1 && v.rows.size() == 5, "shop panel: empty slots hidden and counted out, 5 real goods on one page");
        Check(v.rows[1].slot == 2 && v.rows[4].slot == 8 && v.rows[4].sold, "shop panel: rows keep the real back-end slot after re-numbering");
        Check(v.rows[0].affordable && !v.rows[3].affordable && v.rows[0].name.find("（空）") == std::string::npos, "shop panel: affordability per row, no empty-slot text");
        for (int i = 0; i < 10; ++i) s.shelf.push_back({Offer{Kind::Item, 0, 1, 1}, 40, false});
        const auto p2 = sp::Build(s, 999, 9);
        Check(p2.total == 15 && p2.pages == 2 && p2.page == 1 && p2.rows.size() == 5, "shop panel: 10 rows per page, paging only beyond capacity, page clamped");
        Shop empty; empty.floor = 5; empty.shelf.push_back({Offer{Kind::Weapon, -1, 0, 1}, 0, true});
        Check(sp::Build(empty, 0, 0).total == 0 && sp::Build(empty, 0, 0).pages == 1, "shop panel: a shop of only empty slots shows one empty state");
        sp::Guard g;
        g.Arm(7, 45, 1000);
        Check(g.Busy(7, 45, 1500) && !g.Busy(8, 5, 1600), "shop panel: a second click is ignored until the shelf version changes");
        g.Arm(8, 5, 2000);
        Check(g.Busy(8, 5, 3100) && !g.Busy(8, 5, 3300), "shop panel: a refused trade releases after 1.2 s");
        Check(sp::Result("破甲符", 40, 45, 5, false).find("【成功】") == 0 && sp::Result("破甲符", 40, 30, 30, false).find("魂玉不足") != std::string::npos &&
              sp::Result("破甲符", 40, 45, 45, false).find("货架已变") != std::string::npos, "shop panel: success / not enough jade / shelf changed results");
    }

    // Confirming must buy precisely the item, amount, price and visit that the
    // player saw, even if another action changes the shelf while the modal is open.
    {
        namespace sp = tower::shoppanel;
        Shop s; s.floor = 5; s.seed = 33; s.version = 7;
        s.shelf.push_back({Offer{Kind::Item, static_cast<int>(Item::Sunder), 1, 1}, 40, false});
        const auto q = sp::Capture(s, "run-a", 0);
        Check(q && sp::Check(*q, s, "run-a", 40) == sp::QuoteStatus::Ready, "quote matches original item and sufficient balance");
        Check(sp::Check(*q, s, "run-a", 39) == sp::QuoteStatus::NoJade && sp::Check(*q, s, "run-b", 99) == sp::QuoteStatus::Stale, "quote rejects insufficient funds and another run");
        auto changed = s; changed.shelf[0].offer.amount = 2;
        Check(sp::Check(*q, changed, "run-a", 99) == sp::QuoteStatus::Stale, "same item with different amount is stale");
        changed = s; changed.shelf[0].offer.quality = 2;
        Check(sp::Check(*q, changed, "run-a", 99) == sp::QuoteStatus::Stale, "same item with different quality is stale");
        changed = s; changed.shelf[0].price = 41;
        Check(sp::Check(*q, changed, "run-a", 99) == sp::QuoteStatus::Stale, "changed price is never silently accepted");
        changed = s;
        Context prices; prices.floor = s.floor;
        std::array<int, kItemCount> purchaseCounts{};
        purchaseCounts[static_cast<int>(Item::Sunder)] = 1;
        prices.itemBuys = &purchaseCounts;
        Check(RepriceShelf(changed, prices) && changed.version == s.version + 1 && changed.seed == s.seed &&
              changed.shelf[0].offer == s.shelf[0].offer && changed.shelf[0].price == ShopPrice(s.shelf[0].offer, prices) &&
              sp::Check(*q, changed, "run-a", 9999) == sp::QuoteStatus::Stale && !RepriceShelf(changed, prices),
              "resuming a saved shelf updates the price and expires old quotes without rerolling merchandise");
        changed = s; ++changed.version;
        Check(sp::Check(*q, changed, "run-a", 99) == sp::QuoteStatus::Stale, "old shelf version is stale");
        changed = s; ++changed.seed;
        Check(sp::Check(*q, changed, "run-a", 99) == sp::QuoteStatus::Stale, "another shop visit is stale");
        changed = s; changed.shelf[0].sold = true;
        Check(sp::Check(*q, changed, "run-a", 99) == sp::QuoteStatus::Sold && !sp::Capture(changed, "run-a", 0), "sold merchandise cannot be confirmed again");
        Check(!sp::Capture(s, "", 0) && !sp::Capture(s, "run-a", 1), "invalid run and slot are rejected");
        changed = s; changed.bought.push_back(q->offer);
        Check(!sp::Recorded(*q, changed, 0, 100, 60), "purchase record without version change is not confirmed");
        ++changed.version;
        Check(sp::Recorded(*q, changed, 0, 100, 60), "matching new record, version and charge confirm purchase");
        Check(!sp::Recorded(*q, changed, 1, 100, 60) && !sp::Recorded(*q, changed, 0, 100, 61), "old record or wrong charge cannot confirm purchase");
        changed.bought[0].amount++;
        Check(!sp::Recorded(*q, changed, 0, 100, 60), "different offer cannot confirm purchase");
        const std::string longText = std::string(growth::kAccessories[growth::AccessoryIndex("AC_RandomDrink")].text);
        const auto pages = sp::EffectPages(longText, 35, 2);
        std::string joined; for (const auto& page : pages) joined += page;
        Check(pages.size() > 1 && joined == longText, "long multilingual effect pages preserve every original byte");
        Check(Effect(Offer{Kind::Item, static_cast<int>(Item::Rebirth), 1, 1}).find("层间准备") == std::string::npos &&
              Effect(Offer{Kind::Item, static_cast<int>(Item::Sunder), 1, 1}).find("层间准备") != std::string::npos,
              "automatic rebirth item is not mislabeled as a preparation item");
        int medicines = 0, accessories = 0;
        const auto catalog = Contract();
        for (const auto& gate : catalog.gates) {
            if (gate.zone < 0 || !gate.shop || gate.bossLinked) continue;
            if (gate.cat == Cat::Medicine) {
                ++medicines;
                Check(!growth::kMedicines[gate.index].text.empty() && Effect({Kind::Medicine, gate.index, 1, 1}).find(growth::kMedicines[gate.index].text) != std::string::npos,
                      "every sold medicine retains its native effect text");
            } else if (gate.cat == Cat::Accessory) {
                ++accessories;
                Check(!growth::kAccessories[gate.index].text.empty() && Effect({Kind::Accessory, gate.index, 1, 1}).find(growth::kAccessories[gate.index].text) != std::string::npos,
                      "every sold accessory retains its native effect text");
            }
        }
        Check(medicines > 0 && accessories > 0, "the effect audit covered both actual shop categories");
    }

    // ---- 奖励排版（313987b：长说明塞进三按钮横排，第三项出屏）：按钮只放短名称，效果另起一行
    {
        const Offer bless{Kind::Blessing, 0, 1, 1}, item{Kind::Item, 0, 1, 1}, med{Kind::Medicine, 0, 0, 1};
        Check(Name(bless).find("攻击") == std::string::npos && Effect(bless).find("本局一直生效") != std::string::npos && Effect(bless).find("攻击") != std::string::npos,
              "layout: a blessing button is the short name; scope and value go to the effect line");
        Check(Name(item) == std::string(tower::kItems[0].name) && Effect(item).find(std::string(tower::kItems[0].text)) != std::string::npos, "layout: item name short, item text in the effect line");
        Check(Name(med).size() < 40 && !Effect(med).empty(), "layout: medicine name short, effect present");
        Check(Entry(2, "厚生（凡）", "全局").find('\n') != std::string::npos, "layout: entry puts the effect on its own line");
    }

    // 30324ba 现场回归：累计面板和兵器长说明把第三项及按钮挤出屏。
    {
        const std::string old = "第 1 层通过 · 三选一\n\n本层收获：魂玉 +27\n兵器：残钢刃（开层前在背包里换上）"
            "\n\n本局加成\n攻击/杀气攻击 +0%\n生命上限 +0%\n祝福：铁骨×1"
            "\n\n三选一：\n1. 铁骨（凡）\n　　全局 · 本局一直生效：受到伤害 -8%（每层）；本次 1 层，最多 3 层"
            "\n2. 长形黑伤\n　　主兵器，1 级进塔档背包；Esc → 角色装备后生效，只在塔内"
            "\n3. 济生神药\n　　新药：回复30%生命值，低生命额外回复30%；装进药槽后战斗中使用";
        const auto text = tower::rewardui::Compact(old);
        Check(text.find("本局加成") == text.npos && text.find("攻击/杀气攻击") == text.npos, "reward overflow: cumulative statistics removed");
        Check(text.find("兵器：残钢刃\n") != text.npos && text.find("2. 长形黑伤\n3.") != text.npos, "reward overflow: both weapon descriptions removed; names retained");
        Check(text.find("受到伤害 -8%") != text.npos && text.find("最多 3 层") != text.npos && text.find("低生命额外回复30%") != text.npos, "reward overflow: blessing cap and medicine effect retained");
        Check(text.size() < old.size() * 3 / 4 && tower::rewardui::Compact(text) == text, "reward overflow: compact and idempotent");
        const std::string info = "影塔·百战 · 信息\n本局加成\n攻击 +15%\n主兵器，1 级进塔档背包；Esc → 角色装备后生效，只在塔内";
        Check(tower::rewardui::Compact(info) == info, "reward overflow: information/native/non-reward pages unchanged");
    }

    {
        const std::string old="影塔·百战 · 第 10 层\n\n【上次开层没有成功】白赤练异常消失，没有死亡记录，本层作废\n\n第 10/50 层\n魂玉 498\n生命 935/935\n杀气 800/800\n攻击 266\n杀气攻击 266\n本局加成\n　攻击/杀气攻击 +16%\n　生命上限 +10%\n　受伤 -8%\n　削杀气 +0%\n　攻速 +0%\n淬炼点 22\n修为点 24\n铸魂令 0\n祝福：淬锋×2、铁骨×1、厚生×1\n道具：缓时符×1";
        const auto small=tower::menutext::CompactReady(old);
        Check(small.find("上次开层") == small.npos && small.find("死亡记录")==small.npos,"ready overflow: internal diagnostic removed from player menu");
        Check(small.find("生命 935/935　杀气 800/800")!=small.npos && small.find("攻击 266　杀气攻击 266")!=small.npos,"ready overflow: live values retained in paired rows");
        Check(small.find("+16%")!=small.npos && small.find("-8%")!=small.npos && small.find("+0%")==small.npos,"ready overflow: current non-zero bonuses retained, zero rows omitted");
        Check(tower::menutext::Lines(small).size()<=9 && tower::menutext::CompactReady(small)==small,"ready overflow: bounded rows and idempotent");
        const std::string native="原生菜单\n第 10/50 层\n魂玉 498\n本局加成";
        Check(tower::menutext::CompactReady(native)==native,"ready overflow: native dialogs unchanged");
    }
    {
        const std::string body="结果：止步第 21 层（已完成 20 层，共 50 层）\n失败 Boss：火流星：磷火意识\n场地：原生场地\n生命 1/935\n主兵器：Tang 1级\n魂玉 502";
        std::string longResult="影塔·百战 · 止步第 21 层\n\n"+body+"\n\n逐层用时\n";
        for(int i=1;i<=50;++i)longResult+="第 "+std::to_string(i)+" 层 01:23.4\n";
        const auto compact=tower::menutext::CompactSettlement(longResult);
        Check(tower::menutext::Lines(compact).size()<=8 && compact.size()<600,"result overflow: 50-floor history cannot push exit below screen");
        Check(compact.find("失败 Boss：火流星：磷火意识")!=compact.npos && compact.find("主兵器")==compact.npos && compact.find("逐层用时")==compact.npos && compact.find("场地：")==compact.npos,"result overflow: user-removed fields omitted, identity retained");
        Check(tower::menutext::CompactSettlement(compact)==compact,"result overflow: idempotent live repair");
        Check(tower::menutext::CompactSettlement(body).starts_with("结果：止步第 21 层"),"result overflow: source body without dialog header supported");
        const std::string native="原生结算\n结果：其他游戏流程\n生命 1/935";
        Check(tower::menutext::CompactSettlement(native)==native,"result overflow: unrelated native dialogs untouched");
        {
            const auto g=tower::menutext::StatsGrid({"魂玉 2908","攻击力加成 +265.8%","伤害减免 90%","攻击速度 +50%（含测试）","持续回血 1% / 秒","无空格"});
            Check(g[0]=="魂玉\n攻击力加成\n伤害减免" && g[1]=="2908\n+265.8%\n90%","stats grid: left names / values split at first space");
            Check(g[2]=="攻击速度\n持续回血\n无空格" && g[3]=="+50%（含测试）\n1% / 秒\n","stats grid: right column keeps rows aligned");
            const auto odd=tower::menutext::StatsGrid({"a 1","b 2","c 3"});
            Check(tower::menutext::IsStatLine("攻击力加成 +354.2%") && tower::menutext::IsStatLine("魂玉 18214") && !tower::menutext::IsStatLine("第 16 层已经领过奖励，这次只有魂玉 +46") && !tower::menutext::IsStatLine("无空格"), "notice lines stay out of the stats grid");
            Check(odd[0]=="a\nb" && odd[2]=="c" && odd[3]=="3","stats grid: odd count puts the extra row left");
        }
    }
    {   // Boss 原生道具掉落（用户 2026-11-04：39/40 层灭法之法 Boss 打完没掉落）
        auto has = [](std::string_view stem, std::string_view item) {
            for (const auto& d : tower::kBossItemDrops) if (d.stem == stem && d.item == item) return true;
            return false;
        };
        Check(has("BP_Monster_QiShu_Bing", "K_16001") && has("BP_Monster_QiShu_Huo", "K_05003") && has("BP_Monster_QiShu_Du", "K_08001") && has("BP_Monster_QiShu_Lei", "K_12001"),
              "boss drops: the four MieFa masks come from their death flows");
        bool clean = true;
        for (const auto& d : tower::kBossItemDrops)
            clean = clean && d.count > 0 && d.item.substr(0, 2) != "F_" && d.item.substr(0, 10) != "Item_Piece" && d.item != "Item_001" && d.item != "Item_002";
        Check(clean, "boss drops: no permanent stat items (memory flowers / pieces) bypass tower growth");
        Check(!has("BP_Monster_FemaleVessel_YuDing", "K_07001"), "boss drops: statically unreachable grant nodes stay out");
        int yz = -1;
        for (int i = 0; i < tower::kWeaponDropCount; ++i) if (tower::kWeaponDrops[i].stem == "BP_Monster_FemaleVessel_YuZan") yz = i;
        Check(yz == tower::kWeaponDropCount - 2 && tower::kWeaponDrops[yz].shaqi && tower::kWeaponDrops[yz].verified, "boss drops: YuZan pipa appended without moving saved weapon indices");
        const auto bro = tower::WeaponForPath("/PBZCombatAsset/ActorData/Mobs/035_HuaShan/BP_Monster_HuaShan_Blue.BP_Monster_HuaShan_Blue_C");
        Check(bro >= 0 && tower::kWeaponDrops[bro].weapon == "JianKeZhaoJiLing" && tower::kWeaponDrops[bro].shaqi && tower::kWeaponDrops[bro].verified &&
              has("BP_Monster_HuaShan_Blue", "K_03004"), "boss drops: Big Brother grants the formation summon token and K_03004 (BPF_JianZhenDeath)");
        // 用户 2026-11-06：第 50 层左殇发紫冥剑 + 破碎龙体；炽刃不是 Boss 掉落
        const auto zs = tower::WeaponForPath("/PBZCombatAsset/ActorData/Mobs/076_ZuoShang_P2/BP_Monster_ZuoShang_P2.BP_Monster_ZuoShang_P2_C");
        Check(zs >= 0 && tower::kWeaponDrops[zs].weapon == "ZiMingJian" && tower::kWeaponDrops[zs].verified && tower::EarnedBossWeapon("ZiMingJian", 0) &&
              has("BP_Monster_ZuoShang_P2", "TU_18008"), "boss drops: floor 50 ZuoShang grants ZiMingJian and its soul-forge material");
        const auto hd = tower::WeaponForPath("/PBZCombatAsset/ActorData/Mobs/008_GreatBladeMan/BP_Monster_HuoDaoDuiZhang.BP_Monster_HuoDaoDuiZhang_C");
        Check(hd >= 0 && !tower::kWeaponDrops[hd].verified && tower::NotBossWeapon(tower::kWeaponDrops[hd].weapon) && has("BP_Monster_HuoDaoDuiZhang", "TU_150012"),
              "boss drops: HuoDao captain only drops the flame piece, XieHuo is a classic shop weapon");
    }
    // ---- v14 普通奖励（用户 2026-11-05）：类型权重 50/30/20、刷新令第四位约 30%、属性整轮封顶、每层重抽、相邻层不整组相同
    {
        const auto cat = TestCatalog();
        growth::Profile prof = growth::Profile::Kit();
        growth::Blessings bless;
        AttrTotals attrs;
        Context c;
        c.floor = 12; c.floors = 50; c.profile = &prof; c.blessings = &bless; c.catalog = &cat; c.attrs = &attrs;
        int byType[5] = {}, refresh = 0, groups = 0, dup = 0, illegal = 0, jadeBad = 0;
        const int rewardJade = RewardJade(c);
        for (uint64_t seed = 1; seed <= 3000; ++seed) {
            growth::Xs r{seed * 2654435761ull | 1};
            const auto g = Draw14(r, c);
            ++groups; refresh += g.refresh;
            for (int i = 0; i < 3; ++i) {
                const auto& o = g.normal[i];
                if (o.kind == Kind::Attr) ++byType[0];
                else if (o.kind == Kind::Item) { ++byType[1]; illegal += ItemTiming(kItems[o.index].item) != Timing::Prep && ItemTiming(kItems[o.index].item) != Timing::Use; }
                else if (o.kind == Kind::Weapon || o.kind == Kind::Accessory || o.kind == Kind::Medicine || o.kind == Kind::Refine) ++byType[2];
                else if (o.kind == Kind::TemperPack || o.kind == Kind::CultivatePack) ++byType[3];
                else if (o.kind == Kind::Jade && o.index == kRewardJadeIndex) { ++byType[4]; jadeBad += o.amount != rewardJade; }
                for (int j = 0; j < i; ++j) dup += g.normal[j] == o;
                if (o.kind == Kind::Weapon) illegal += ZoneOfOffer(cat, o) > tower::rewards::ZoneOf(c.floor, c.floors);
            }
        }
        const double total = byType[0] + byType[1] + byType[2] + byType[3] + byType[4];
        Check(dup == 0 && illegal == 0, "v14 draw: no duplicates in a group, MOD slots are battle items, native goods respect the progression zone");
        std::printf("info: v15 draw attr %.3f mod %.3f native %.3f points %.3f jade %.3f refresh %.3f\n", byType[0] / total, byType[1] / total, byType[2] / total, byType[3] / total, byType[4] / total, refresh / double(groups));
        Check(std::abs(byType[0] / total - 0.625) < 0.05 && std::abs(byType[1] / total - 0.10) < 0.04 && byType[2] == 0 && std::abs(byType[3] / total - 0.24) < 0.05,
              "v3 draw: attribute / MOD item / native goods / points about 65 / 10 / 0 / 25 (of 104)");
        Check(byType[4] > 0 && std::abs(byType[4] / total - 4.0 / 104) < 0.015 && jadeBad == 0, "3.6.0 reward page jade option: weight 4, below attributes");
        Check(rewardJade == std::min(ShopPrice(Offer{Kind::TemperPack, 0, 3, 1}, c), ShopPrice(Offer{Kind::CultivatePack, 0, 1, 1}, c)) && rewardJade > 0,
              "3.6.0 reward jade = lower of the shop temper / cultivate price");
        Check(std::abs(refresh / double(groups) - 0.05) < 0.02, "v15 draw: refresh token in about 5% of draws, never in a normal slot");
        {   // 铸魂令：整轮 15 层，层上固定一位
            // 用户 2026-11-12：铸魂令改为每层约 7%（前 50 层与无尽层同一规则），给铸魂令相关海克斯让路
            int soul = 0, many = 0; for (int f = 1; f <= 50; ++f) soul += SoulFloor(12345, f, 50);
            for (uint64_t s = 1; s <= 200; ++s) for (int f = 1; f <= 100; ++f) many += SoulFloor(s, f, 50);
            Check(soul <= 10 && SoulFloor(12345, 7, 50) == SoulFloor(12345, 7, 50) && std::abs(many / 20000.0 - kSoulRewardPct / 100.0) < 0.015 && !SoulFloor(12345, 7, 3),
                  "soul tokens: about 7% of reward pages (seed-fixed), also past floor 50; none in short runs");
            Context cs = c; cs.soulFloor = true; growth::Xs rs{99};
            const auto gs = Draw14(rs, cs);
            Check(std::count_if(gs.normal.begin(), gs.normal.end(), [](const Offer& o) { return o.kind == Kind::SoulToken; }) == 1, "v15 soul tokens: a soul floor offers exactly one token");
        }
        // 属性：档位按区，整轮封顶，到顶不再出现；读档不能突破上限
        AttrTotals t;
        // 用户 2026-11-13：除伤害减免外不封顶（攻击原来 +200% 封顶）
        Check(t.Add(0, 1500) == 1500 && t.Add(0, 800) == 800 && t.Room(0) > 100000, "attack is uncapped");
        Check(std::abs(t.Mul(AttrKind::Attack) - 3.30) < 1e-9, "multiplier reads the full total");
        Check(t.Add(4, 900) == 800 && t.Room(4) == 0, "damage reduction attribute stops at 80%");
        c.attrs = &t;
        bool noGuard = true;
        for (uint64_t seed = 1; seed <= 500; ++seed) {
            growth::Xs r{seed | 1};
            for (auto& o : Draw14(r, c).normal) noGuard = noGuard && !(o.kind == Kind::Attr && o.index == 4);
        }
        Check(noGuard, "v14 attrs: a capped attribute (damage reduction) never shows up again");
        AttrTotals full;
        for (int i = 0; i < kAttrCount; ++i) full.Add(i, 100000);
        c.attrs = &full;
        bool none = true;
        for (uint64_t seed = 1; seed <= 200; ++seed) { growth::Xs r{seed | 1}; for (auto& o : Draw14(r, c).normal) none = none && !(o.kind == Kind::Attr && kAttrs[o.index].kind != AttrKind::Life); }
        Check(!none, "uncapped attributes keep showing up after huge totals");
        bool cappedGone = true;
        for (uint64_t seed = 1; seed <= 200; ++seed) { growth::Xs r{seed | 1}; for (auto& o : Draw14(r, c).normal)
            cappedGone = cappedGone && !(o.kind == Kind::Attr && (kAttrs[o.index].kind == AttrKind::Defense || kAttrs[o.index].cap == 450)); }
        Check(cappedGone, "only damage reduction and resistances run out");
        {   // 1.0.1：生命少见档（×2.5）只占生命奖励出现次数的约 10%
            int lifeSeen = 0, lifeRare = 0;
            for (uint64_t seed = 1; seed <= 20000; ++seed) {
                growth::Xs r{seed * 2654435761ull | 1};
                for (auto& o : Draw14(r, c).normal)
                    if (o.kind == Kind::Attr && kAttrs[o.index].kind == AttrKind::Life) { ++lifeSeen; lifeRare += o.quality == 2; }  // 少见档 quality 2
            }
            const double share = lifeSeen ? double(lifeRare) / lifeSeen : 0;
            Check(lifeSeen > 1000 && share > 0.06 && share < 0.14, "1.0.1 attrs: rare life tier is about 10% of life offers");
        }
        Check(kAttrs[0].cap == kNoCap && kAttrs[1].cap == kNoCap && kAttrs[3].cap == kNoCap && kAttrs[4].cap == 800 && kAttrs[2].cap == kNoCap && kAttrs[5].cap == kNoCap,
              "caps: only damage reduction (80%) and resistances stay capped");
        // 3.0.0：无尽层每 30 层一档，攻击 +50% / 破防 +25% / 杀气 +25% 上限，减伤不抬；档位值 +25% / 档
        Check(EndlessTier(50) == 0 && EndlessTier(51) == 1 && EndlessTier(80) == 1 && EndlessTier(81) == 2 && EndlessTier(111) == 3, "endless tier: display 100-129 / 130-159 / 160-189");
        Check(AttrCapAt(0, 2) == kNoCap && AttrCapAt(4, 5) == 800, "endless tiers do not cap uncapped stats; damage reduction stays 80%");
        {
            AttrTotals full;
            Context t = c; t.attrs = &full; t.floor = 81; t.floors = 50;
            int attackStep = 0;
            for (auto& cand : detail::Attrs(t)) if (cand.offer.index == 0) attackStep = cand.offer.amount;
            const int base = c.difficulty == 1 ? kAttrs[0].step[4] : kAttrs[0].hard[4];
            Check(attackStep == base * 150 / 100, "endless tier 2 attack reward is 150% of the last zone step");
        }
        Check(kAttrs[5].cap == kNoCap && kAttrs[5].step[4] == 10 && kAttrs[5].hard[4] == 12 && kAttrs[5].weight == 1 && kAttrs[4].weight == 5, "v3: lifesteal 1% per pick cap 5%; damage reduction rare (weight 5)");
        for (int difficulty = 0; difficulty <= 1; ++difficulty) for (int zone = 0; zone < 5; ++zone) {
            AttrTotals fresh; Context t = c; t.attrs = &fresh; t.difficulty = difficulty; t.floor = zone * 10 + 1; t.floors = 50;
            const auto candidates = detail::Attrs(t);
            int seen[6]{}; int rare = 0;
            for (const auto& x : candidates) {
                const auto& o = x.offer;
                const auto& def = kAttrs[o.index];
                const int expected = difficulty == 1 ? def.step[zone] : def.hard[zone];
                Check(o.amount == (def.kind == AttrKind::Life && o.quality == 2 ? expected * 3 / 2 : expected), "difficulty and zone offer amount");
                ++seen[o.index]; if (def.kind == AttrKind::Life && o.quality == 2) ++rare;
            }
            for (int i = 0; i < 6; ++i) Check(seen[i] == (i == 2 ? 2 : i == 5 && zone < 3 ? 0 : 1), "eligible attribute options present");
            Check(rare == 1, "rare life option present");
        }
        { AttrTotals fresh; Context t = c; t.attrs = &fresh; t.floors = 50;
          for (int f : {30, 31}) { t.floor = f; bool found = false; for (auto& x : detail::Attrs(t)) found |= x.offer.index == 5;
              Check(found == (f == 31), "v3: lifesteal starts at displayed floor 61"); }
          t.floors = 3; t.floor = 3; bool found = false; for (auto& x : detail::Attrs(t)) found |= x.offer.index == 5;
          Check(!found, "short trials cannot offer late-game lifesteal");
        }
        { AttrTotals nearCap; nearCap.pm[4] = kAttrs[4].cap - 7; Context t = c; t.attrs = &nearCap;
          for (const auto& x : detail::Attrs(t)) if (x.offer.index == 4) {
              Check(x.offer.amount == 7 && StillValid(x.offer, t), "displayed amount is capped to actual remaining room");
              nearCap.Add(4, 1); Check(!StillValid(x.offer, t), "stale offer cannot credit less than displayed");
          }
        }
        Check(detail::Points(c)[0].offer.kind == Kind::TemperPack && detail::Points(c)[0].offer.amount == 2, "points: tempering pack +2");
        // 相邻层：上一层整组不重现；刷新：避开刷新前那组
        c.attrs = &attrs;
        growth::Xs r0{77};
        const auto first = Draw14(r0, c);
        c.lastFloor.assign(first.normal.begin(), first.normal.end());
        int same = 0;
        for (uint64_t seed = 1; seed <= 300; ++seed) {
            growth::Xs r{seed | 1};
            const auto g = Draw14(r, c);
            std::vector<Offer> v(g.normal.begin(), g.normal.end());
            same += detail::SameSet(v, c.lastFloor);
        }
        Check(same == 0, "v14 draw: the previous floor's exact group is never repeated when the pool allows");
        Check(Label(Offer{Kind::Attr, 2, 50, 1}) == "生命上限 +5%" && Label(Offer{Kind::Attr, 0, 5, 1}) == "攻击力 +0.5%" && Effect(Offer{Kind::Attr, 1, 5, 1}).find("破防能力") != std::string::npos, "v15 attrs: percent wording on weapons");
        // 驿站：MOD 道具常备 + 进度内原生货一直在架；补充渠道武器
        Shop sh; sh.floor = 15;
        c.floor = 15;
        int spare = 0;
        while (spare < growth::kWeaponCount && prof.Find(spare) >= 0) ++spare;
        std::vector<Offer> sup{{Kind::Weapon, spare, 1, 9}};
        Stock14(sh, c, sup);
        int mods = 0, natives = 0, supply = 0;
        for (auto& w : sh.shelf) { mods += w.offer.kind == Kind::Item; natives += w.offer.kind != Kind::Item && w.offer.quality != 9; supply += w.offer.quality == 9; }
        Check(mods == 2 && supply >= 1, "2.0.0 shop: two random MOD items plus boss weapons (3.6.0: refill may add more)");
        {   // 第一个 Boss 随机为阿花：未击败不开放，击败后下次驿站可买她的兵器。
            Context early = c; growth::Profile starter = growth::Profile::Kit(); early.profile = &starter; early.floor = 1;
            Shop before; before.floor = 1; before.seed = 42; Stock14(before, early, BossDropOffers({}));
            const int drop = WeaponForPath("/PBZCombatAsset/ActorData/Mobs/042_AHua/BP_Monster_AHua_GuiTouDao.BP_Monster_AHua_GuiTouDao_C");
            const int weapon = growth::WeaponIndex("GuiTouBlade", 0);
            Shop after; after.floor = 1; after.seed = 42; Stock14(after, early, BossDropOffers({drop, drop}));
            const auto present = [weapon](const Shop& s) { return std::count_if(s.shelf.begin(), s.shelf.end(), [weapon](const auto& w) { return w.offer.kind == Kind::Weapon && w.offer.index == weapon; }); };
            bool priced = false; for (const auto& w : after.shelf) if (w.offer.kind == Kind::Weapon && w.offer.index == weapon) priced = w.price == tower::balance::Price(tower::balance::Buy::Weapon, 1, 0);
            Check(drop >= 0 && weapon >= 0 && present(before) == 0 && present(after) == 1 && priced,
                  "random first boss: Ahua's weapon appears once at the v3 weapon price only after she is defeated");
        }
        {
            Context ctx = c; ctx.floor = 15; ctx.floors = 50;
            const int idx = static_cast<int>(Item::Foresight);
            bool found = false, priced = false, unchanged = false, absentNext = false;
            for (int seed = 1; seed <= 1000 && !found; ++seed) {
                Shop shop; shop.floor = 15; shop.seed = seed;
                Stock14(shop, ctx, {});
                for (int slot = 0; slot < static_cast<int>(shop.shelf.size()); ++slot) {
                    const auto ware = shop.shelf[slot];
                    if (ware.offer.kind != Kind::Item || ware.offer.index != idx) continue;
                    found = true;
                    priced = ware.price > 2 * tower::balance::Price(tower::balance::Buy::Mod, tower::balance::ShownOfBoss(ctx.floor), 0);  // v3：透视眼 ×2.4
                    Offer other{}; for (const auto& w : shop.shelf) if (w.offer.kind == Kind::Item && w.offer.index != idx) other = w.offer;
                    int jade = ware.price;
                    const auto result = Purchase(shop, slot, shop.version, jade, ctx, [&](const Offer&) { ctx.items[idx] = 1; return true; });
                    Stock14(shop, ctx, {});
                    unchanged = result == Buy::Ok && jade == 0 && std::any_of(shop.shelf.begin(), shop.shelf.end(), [&](const Ware& w) { return w.offer == other; }) &&
                        std::none_of(shop.shelf.begin(), shop.shelf.end(), [&](const Ware& w) { return w.offer.kind == Kind::Item && w.offer.index == idx; });
                    Shop next; next.floor = 16; next.seed = seed; Stock14(next, ctx, {});
                    absentNext = std::none_of(next.shelf.begin(), next.shelf.end(), [&](const Ware& w) { return w.offer.kind == Kind::Item && w.offer.index == idx; });
                    break;
                }
            }
            Check(found && priced && unchanged && absentNext, "foresight purchase preserves other shelf item and removes future copies");
            Check(!StillValid({Kind::Item, idx, 1, 1}, ctx), "foresight cannot be bought twice");
        }
        {   // 用户 2026-11-11：六项抗性进属性池，权重低、每次小幅、单项上限 60%
            AttrTotals at; Context rc = c; rc.attrs = &at; rc.floor = 30; rc.floors = 50;
            const auto pool = detail::Attrs(rc);
            int resistWeight = 0, lifeWeight = 0, resistKinds = 0;
            for (const auto& x : pool) {
                const auto k = kAttrs[x.offer.index].kind;
                if (k >= AttrKind::ResistFire) { resistWeight += x.weight; ++resistKinds; Check(x.offer.amount <= 30, "resistance step is small"); }
                if (k == AttrKind::Life) lifeWeight += x.weight;
            }
            Check(resistKinds == 6 && resistWeight > 0 && resistWeight < lifeWeight, "six resistances in the attribute pool with low weight");
            const int fire = static_cast<int>(AttrKind::ResistFire);
            for (int i = 0; i < 100; ++i) at.Add(fire, 30);
            Check(at.pm[fire] == 450 && at.Room(fire) == 0, "v3: resistance capped at 45%");
            Check(std::size(kResists) == 6 && kResists[4].acc[1] != nullptr, "bleed resistance covers bleed and blood burst buildup");
        }
        {   // 用户 2026-11-11：武器 / 药 / 饰品三个池每次各上架 4 个；武库扩建武器 +2；钱生钱每类 +2、MOD 道具 3 个、两种点数同时上架
            const auto official = Contract(); growth::Profile none; growth::Blessings bl;
            Context base; base.profile = &none; base.blessings = &bl; base.catalog = &official; base.floor = 50; base.floors = 50;
            auto count = [](const Shop& sh) {
                std::array<int, 6> n{};  // 武器 药 饰品 MOD 淬炼 修为
                for (const auto& w : sh.shelf) {
                    if (w.offer.quality == kRareQuality) continue;  // 3.2.1 珍品位另算
                    if (w.offer.kind == Kind::Weapon) ++n[0]; else if (w.offer.kind == Kind::Medicine) ++n[1]; else if (w.offer.kind == Kind::Accessory && !ResistCharm(w.offer.index)) ++n[2];  // 3.2.0 常驻抗异常饰品另算
                    else if (w.offer.kind == Kind::Item) ++n[3]; else if (w.offer.kind == Kind::TemperPack) ++n[4]; else if (w.offer.kind == Kind::CultivatePack) ++n[5];
                }
                return n;
            };
            Context full = base; full.shopPerCategory = 999; Shop all; all.floor = 50; all.seed = 3; Stock14(all, full, {});
            const auto avail = count(all);
            Check(avail[0] > 6 && avail[1] > 0 && avail[2] > 0, "official pools are large enough to test the cap");
            Shop sh; sh.floor = 50; sh.seed = 3; Stock14(sh, base, {});
            auto n = count(sh);
            Check(n[0] == 4 && n[1] == std::min(2, avail[1]) && n[2] == std::min(3, avail[2]), "3.2.1 shelves: 4 weapons, 2 medicines, 3 accessories");
            Check(n[3] == 2 && n[4] + n[5] == 1, "two MOD items and exactly one point pack by default");
            bool sawTemper = false, sawCultivate = false;
            for (int f = 1; f <= 40; ++f) { Shop t; t.floor = f; t.seed = 3; Stock14(t, base, {}); auto m = count(t); sawTemper |= m[4] == 1; sawCultivate |= m[5] == 1; }
            Check(sawTemper && sawCultivate, "the single point pack alternates between visits");
            Context arsenal = base; arsenal.shopExtraWeapons = 2; arsenal.weaponPricePct = 70;
            Shop a; a.floor = 50; a.seed = 3; Stock14(a, arsenal, {});
            n = count(a);
            Check(n[0] == 6 && n[1] == std::min(2, avail[1]), "arsenal expansion: two more weapons only");
            bool cheaper = true;
            for (const auto& w : a.shelf) if (w.offer.kind == Kind::Weapon && w.offer.quality != kRareQuality)
                for (const auto& f : all.shelf) if (f.offer == w.offer) cheaper = cheaper && w.price == f.price * 70 / 100;
            Check(cheaper, "arsenal expansion: weapons discounted");
            Context rich = base; rich.shopExtraAll = tower::balance::kInvestorExtra; rich.shopBothPoints = true; rich.shopModItems = 3; rich.modPurchaseLimit = 3;
            Shop r; r.floor = 50; r.seed = 3; Stock14(r, rich, {});
            n = count(r);
            Check(n[0] == 5 && n[1] == std::min(3, avail[1]) && n[2] == std::min(4, avail[2]) && n[3] == 3 && n[4] == 1 && n[5] == 1, "v3 money makes money: +1 per pool, three MOD items, both point packs");
            // 同一次驿站：买下一件后其余不变、不补新货
            std::vector<Offer> before; for (const auto& w : sh.shelf) if (w.offer.kind == Kind::Weapon) before.push_back(w.offer);
            sh.bought.push_back(before[0]);
            growth::Profile owns = none; owns.AddWeapon(before[0].index);
            Context after = base; after.profile = &owns;
            Stock14(sh, after, {});
            std::vector<Offer> left; for (const auto& w : sh.shelf) if (w.offer.kind == Kind::Weapon) left.push_back(w.offer);
            Check(left.size() + 1 == before.size() && std::equal(left.begin(), left.end(), before.begin() + 1), "buying one weapon keeps the others and adds nothing");
            // 不同层的驿站换一批
            Shop other; other.floor = 49; other.seed = 3; Stock14(other, base, {});
            std::vector<Offer> o2; for (const auto& w : other.shelf) if (w.offer.kind == Kind::Weapon) o2.push_back(w.offer);
            Check(o2 != before, "a different visit draws a different weapon set");
        }
        {   // 2.0.0：每次驿站 2 个 MOD 道具，重生符最少、双倍价，复活符不卖；货架重排不换货，买下后本次没有了
            const int rb = static_cast<int>(Item::Rebirth), rv = static_cast<int>(Item::Revive), leech = static_cast<int>(Item::Leech);
            std::map<int, int> seen;
            bool doubled = true, stable = true;
            for (int f = 0; f < 4000; ++f) {
                Shop t; t.floor = f; t.seed = 7;
                Stock14(t, c, {});
                std::vector<Offer> first;
                for (auto& w : t.shelf) if (w.offer.kind == Kind::Item) {
                    first.push_back(w.offer); ++seen[w.offer.index];
                    if (w.offer.index == rb) doubled = doubled && w.price == tower::balance::Price(tower::balance::Buy::Mod, tower::balance::ShownOfBoss(c.floor), 0) * 3;
                    if (w.offer.index == leech) doubled = doubled && w.price == tower::balance::Price(tower::balance::Buy::Mod, tower::balance::ShownOfBoss(c.floor), 0) * 27 / 10;
                }
                stable = stable && first.size() == 2 && !(first[0] == first[1]);
                Stock14(t, c, {});
                size_t k = 0;
                for (auto& w : t.shelf) if (w.offer.kind == Kind::Item) stable = stable && k < first.size() && w.offer == first[k++];
                t.bought.push_back(first[0]);
                Stock14(t, c, {});
                int left = 0;
                for (auto& w : t.shelf) if (w.offer.kind == Kind::Item) { ++left; stable = stable && w.offer == first[1]; }
                stable = stable && left == 1;
            }
            int lowest = 1 << 30;
            for (auto& [k, n] : seen) if (k != rb) lowest = std::min(lowest, n);
            Check(seen.count(rb) && seen[rb] < lowest, "2.0.0 shop: rebirth charm is the rarest MOD item");
            Check(!seen.count(rv), "2.0.0 shop: revive charm is never sold");
            Check(seen.count(leech) && kItems[rb].weight < kItems[leech].weight && kItems[leech].weight < kItems[static_cast<int>(Item::Foresight)].weight, "charm is shop-only, second rarest");
            Check(doubled && stable, "shop prices and restocking preserve the same MOD item");
        }
        {   // Purchasing the ninth talisman must not reshuffle the other item.
            const int leech = static_cast<int>(Item::Leech);
            Context ctx = c; ctx.items[leech] = kItemStackCap - 1;
            bool found = false;
            for (uint64_t seed = 1; seed <= 2000 && !found; ++seed) {
                Shop shop; shop.floor = 15; shop.seed = seed; Stock14(shop, ctx, {});
                int slot = -1; Offer other{};
                for (int i = 0; i < static_cast<int>(shop.shelf.size()); ++i) if (shop.shelf[i].offer.kind == Kind::Item) {
                    if (shop.shelf[i].offer.index == leech) slot = i; else other = shop.shelf[i].offer;
                }
                if (slot < 0) continue;
                found = true; int jade = shop.shelf[slot].price;
                Check(Purchase(shop, slot, shop.version, jade, ctx, [&](const Offer&) { ++ctx.items[leech]; return true; }) == Buy::Ok,
                      "ninth lifesteal talisman purchased");
                Stock14(shop, ctx, {});
                int count = 0;
                for (auto& w : shop.shelf) if (w.offer.kind == Kind::Item) { ++count; Check(w.offer == other, "other item stays unchanged at cap"); }
                Check(count == 1 && ctx.items[leech] == kItemStackCap, "purchased charm stays off this shelf");
            }
            Check(found, "lifesteal shop case found");
        }
        {   // 1.0.1：额外挑战三选一：三项互不相同，池里有复活符 / 重生符
            bool sawRevive = false, sawRebirth = false, distinct = true, sized = true;
            for (uint64_t seed = 1; seed <= 300; ++seed) {
                growth::Xs xr{seed * 0x9E3779B97F4A7C15ull | 1};
                const auto b = DrawBonus3(xr, c);
                sized = sized && b.size() == 3;
                for (size_t i = 0; i < b.size(); ++i) {
                    for (size_t j = i + 1; j < b.size(); ++j) distinct = distinct && !(b[i].kind == b[j].kind && b[i].index == b[j].index);
                    sawRevive = sawRevive || (b[i].kind == Kind::Item && b[i].index == static_cast<int>(Item::Revive));
                    sawRebirth = sawRebirth || (b[i].kind == Kind::Item && b[i].index == static_cast<int>(Item::Rebirth));
                }
            }
            Check(sized && distinct, "1.0.1 bonus: three distinct extra rewards");
            Check(sawRevive && sawRebirth, "1.0.1 bonus: revive and rebirth charms are in the extra pool");
        }
        {   // v3（用户 2026-11-12）：额外奖励按类别抽——魂玉包 35、点数 25、MOD / 符 25、装备 15
            int jadeN = 0, points = 0, items = 0, equip = 0, total = 0;
            for (uint64_t seed = 1; seed <= 4000; ++seed) {
                growth::Xs xr{seed * 0x9E3779B97F4A7C15ull | 1};
                const auto b = DrawBonus3(xr, c);
                if (b.empty()) continue;
                const auto& o = b[0];  // 第一格不受组内去重影响，比例最接近类别权重
                ++total; jadeN += o.kind == Kind::Jade; points += o.kind == Kind::TemperPack || o.kind == Kind::CultivatePack; items += o.kind == Kind::Item;
                equip += o.kind == Kind::Weapon || o.kind == Kind::Accessory || o.kind == Kind::Medicine || o.kind == Kind::Refine;
            }
            std::printf("info: v3 bonus first slot jade %.3f points %.3f items %.3f equip %.3f\n", jadeN / double(total), points / double(total), items / double(total), equip / double(total));
            Check(total > 0 && std::abs(jadeN / double(total) - 0.35) < 0.05 && std::abs(points / double(total) - 0.25) < 0.05 && std::abs(items / double(total) - 0.25) < 0.05 && std::abs(equip / double(total) - 0.15) < 0.05,
                  "v3 bonus: jade 35 / points 25 / MOD and charms 25 / equipment 15");
        }
        const size_t before = sh.shelf.size();
        Stock14(sh, c, sup);
        Check(sh.shelf.size() == before, "v14 shop: restocking keeps unbought goods (they do not vanish when unaffordable)");
        {   // 2.0.0：买下 MOD 道具后本次驿站不再有；下一次到驿站又有
            int slot = -1;
            for (int i = 0; i < static_cast<int>(sh.shelf.size()); ++i) if (sh.shelf[i].offer.kind == Kind::Item) slot = i;
            int jade = 100000;
            Check(slot >= 0 && Purchase(sh, slot, sh.version, jade, c, [](const Offer&) { return true; }) == Buy::Ok, "2.0.0 shop: the MOD item can be bought");
            Stock14(sh, c, sup);
            Check(std::count_if(sh.shelf.begin(), sh.shelf.end(), [](const Ware& w) { return w.offer.kind == Kind::Item; }) == 1, "2.0.0 shop: the bought MOD item is gone, the other one stays");
            Shop next; next.floor = 25; next.seed = sh.seed;
            Stock14(next, c, sup);
            Check(std::any_of(next.shelf.begin(), next.shelf.end(), [](const Ware& w) { return w.offer.kind == Kind::Item; }), "2.0.0 shop: the next visit has one again");
        }
        {
            Context repeat = c; repeat.modPurchaseLimit = 3;
            Shop s; s.floor = 25; s.seed = 123; Stock14(s, repeat, sup);
            const auto it = std::find_if(s.shelf.begin(), s.shelf.end(), [](const Ware& w) { return w.offer.kind == Kind::Item; });
            Check(it != s.shelf.end(), "investor: MOD item available");
            if (it != s.shelf.end()) {
                const Offer chosen = it->offer; int jade = 100000;
                for (int n = 0; n < 3; ++n) {
                    const auto pos = std::find_if(s.shelf.begin(), s.shelf.end(), [&](const Ware& w) { return w.offer == chosen; });
                    Check(pos != s.shelf.end(), "investor: item remains until third purchase");
                    if (pos == s.shelf.end()) break;
                    Check(Purchase(s, static_cast<int>(pos - s.shelf.begin()), s.version, jade, repeat, [](const Offer&) { return true; }) == Buy::Ok, "investor: purchase succeeds");
                    Stock14(s, repeat, sup);
                    Shop restored; Prep prep; QuickBar quick;
                    Check(Load(Save(s, prep, quick), &restored, &prep, &quick) && std::count(restored.bought.begin(), restored.bought.end(), chosen) == n + 1, "investor: purchase count survives save");
                    s = restored;
                }
                Check(std::none_of(s.shelf.begin(), s.shelf.end(), [&](const Ware& w) { return w.offer == chosen; }), "investor: item removed after third purchase");
            }
        }
    }
    {   // 3.1.0 N5：魂玉袋 / 经验丹放 MOD 背包点用、驿站低概率便宜上架；狂暴药 +150% / +25%
        const int bag = static_cast<int>(Item::JadeBag), pill = static_cast<int>(Item::ExpPill), rage = static_cast<int>(Item::Rage);
        Check(ItemTiming(Item::JadeBag) == Timing::Use && ItemTiming(Item::ExpPill) == Timing::Use && ItemTiming(Item::Rage) == Timing::Prep, "JadeBag / ExpPill are use-from-bag items");
        Check(kItems[bag].weight < kItems[rage].weight && kItems[pill].weight < kItems[rage].weight && kItems[bag].weight > 0, "JadeBag / ExpPill are rarer than battle items");
        Check(std::string_view(kItems[rage].text) == "本层攻击力 +150%，受到的伤害 +25%", "Rage text matches 2.5x / 1.25x");
        for (int f : {1, 10, 30, 60}) {
            Context c; c.floor = f; c.floors = 50;
            bool bagIn = false, pillIn = false;
            for (auto& cand : detail::ModItems(c)) { bagIn = bagIn || cand.offer.index == bag; pillIn = pillIn || cand.offer.index == pill; }
            Check(bagIn && pillIn, "JadeBag / ExpPill can be drawn for the shop MOD slots");
            const int weapon = balance::Price(balance::Buy::Weapon, balance::ShownOfBoss(f), 0);
            const int price = ShopPrice(Offer{Kind::Item, bag, 1, 1}, c);
            Check(price == std::max(1, weapon * 15 / 100) && price < std::max(1, weapon * 3 / 10), "JadeBag costs 15% of the floor's weapon price, pays 30% when used");
            Check(ShopPrice(Offer{Kind::Item, pill, 1, 1}, c) <= ShopPrice(Offer{Kind::Item, rage, 1, 1}, c), "ExpPill is discounted");
            const int later = std::max(1, balance::Price(balance::Buy::Weapon, balance::ShownOfBoss(f + 20), 0) * 3 / 10);
            Check(later > price, "saving a JadeBag for a later shop pays more than it cost");
        }
        Prep p; std::array<int, kItemCount> have{}; have[bag] = 1;
        Check(!p.Why(bag, have).empty(), "JadeBag cannot be armed as a battle prep item");
        Context last; last.floor = 50; last.floors = 50; bool lastBag = false;
        for (auto& cand : detail::Items(last)) lastBag = lastBag || cand.offer.index == bag;
        Check(!lastBag, "no JadeBag on the very last floor reward");
    }
    Check(RarePrice(300) == 500 && RarePrice(500) == 500 && RarePrice(501) == 701 && RarePrice(1000) == 1400, "3.3.1 rare price curve");
    {   // 3.2.1 珍品位：武器 / 药 / 饰品各 1，全图鉴随机（不按进度），价格 max(1000, 正常价 ×1.4)，同店固定、买下不补
        const auto official = Contract(); growth::Profile p; growth::Blessings blessings;
        Context c; c.profile = &p; c.blessings = &blessings; c.catalog = &official; c.floors = 50; c.floor = 1;
        auto rares = [](const Shop& sh, Kind k) { std::vector<Ware> v; for (auto& w : sh.shelf) if (w.offer.quality == kRareQuality && w.offer.kind == k) v.push_back(w); return v; };
        Shop a; a.floor = 1; a.seed = 9; Stock14(a, c, {});
        Check(rares(a, Kind::Weapon).size() == 1 && rares(a, Kind::Medicine).size() == 1 && rares(a, Kind::Accessory).size() == 1, "one rare weapon, medicine and accessory per shop");
        for (Kind k : {Kind::Weapon, Kind::Medicine, Kind::Accessory}) {
            const auto r = rares(a, k)[0];
            Check(r.price == 500, "rare price starts at 500 on early floors (3.3.1)");
            Check(Name(r.offer).rfind("珍·随机", 0) == 0 && Effect(r.offer).find("随机获得") != std::string::npos && Label(r.offer).find("随机") == std::string::npos,
                  "rare slot is a lucky draw: shelf shows only the category, the real item is revealed after buying");
            Check(StillValid(r.offer, c) && WhyNot(r.offer, c).empty(), "rare item is buyable on floor 1 regardless of progression");
            if (k == Kind::Accessory) Check(!ResistCharm(r.offer.index), "rare accessory slot never repeats a resistance charm");
        }
        bool late = false;  // 前期能抽到后期才开放的东西
        for (uint64_t seed = 1; seed <= 300 && !late; ++seed) {
            Shop e; e.floor = 1; e.seed = seed; Stock14(e, c, {});
            for (auto& w : e.shelf) if (w.offer.quality == kRareQuality) {
                const Cat cat = w.offer.kind == Kind::Medicine ? Cat::Medicine : w.offer.kind == Kind::Accessory ? Cat::Accessory : growth::kWeapons[w.offer.index].kind == 1 ? Cat::Shaqi : Cat::Main;
                if (const Gate* g = official.Find(cat, w.offer.index); g && g->zone >= 3) late = true;
            }
        }
        Check(late, "rare slot can offer late-game items on floor 1");
        bool allOk = true;  // 只放能正常发放的：区号 ≥ 0 的已核验物品；Boss 关联兵器要有已核验的掉落记录；不出退役兵器
        for (uint64_t seed = 1; seed <= 500; ++seed) {
            Shop e; e.floor = 1; e.seed = seed; Stock14(e, c, {});
            for (auto& w : e.shelf) if (w.offer.quality == kRareQuality) {
                const Cat cat = w.offer.kind == Kind::Medicine ? Cat::Medicine : w.offer.kind == Kind::Accessory ? Cat::Accessory : growth::kWeapons[w.offer.index].kind == 1 ? Cat::Shaqi : Cat::Main;
                const Gate* g = official.Find(cat, w.offer.index);
                bool ok = g && g->zone >= 0 && (g->reward || g->shop) && !RetiredOffer(w.offer);
                if (ok && w.offer.kind == Kind::Weapon && g->bossLinked && !g->shop) {
                    const auto id = growth::kWeapons[w.offer.index].id;
                    ok = std::any_of(std::begin(tower::kWeaponDrops), std::end(tower::kWeaponDrops), [&](const auto& d) { return d.weapon == id && d.verified; }) && !tower::NotBossWeapon(id);
                }
                allOk = allOk && ok;
            }
        }
        Check(allOk, "rare slot only offers verified, grantable items (no scrapped / unreleased ones)");
        Shop b; b.floor = 1; b.seed = 9; Stock14(b, c, {});
        Check(rares(b, Kind::Weapon)[0].offer == rares(a, Kind::Weapon)[0].offer, "same shop visit = same rare items");
        for (int f : {60, 100, 150}) {
            Context pc = c; pc.floor = f; pc.floors = 50;
            const Offer n{Kind::Weapon, rares(a, Kind::Weapon)[0].offer.index, 1, 1}, r{Kind::Weapon, n.index, 1, kRareQuality};
            Check(ShopPrice(r, pc) == RarePrice(ShopPrice(n, pc)), "rare price = 500 until the normal price passes 500, then normal x1.4");
        }
        const auto bought = rares(a, Kind::Medicine)[0].offer;
        a.bought.push_back(bought); p.AddMedicine(bought.index); Stock14(a, c, {});
        Check(rares(a, Kind::Medicine).empty(), "a bought rare item is not replaced in the same visit");
    }
    {   // 3.2.0 抗异常饰品常驻驿站：不占随机饰品格、不按进度、买下后下架、价格随层数涨
        const auto official = Contract(); growth::Profile p; growth::Blessings blessings;
        Context c; c.profile = &p; c.blessings = &blessings; c.catalog = &official; c.floors = 50; c.floor = 1;
        int charms = 0; for (int i = 0; i < growth::kAccessoryCount; ++i) charms += ResistCharm(i);
        Check(charms == 5, "five resistance charms resolve by id");
        auto count = [&](const Shop& sh, bool resist) { int n = 0; for (auto& w : sh.shelf) if (w.offer.kind == Kind::Accessory && w.offer.quality != kRareQuality && ResistCharm(w.offer.index) == resist) ++n; return n; };
        Shop a; a.floor = 1; Stock14(a, c, {});
        Check(count(a, true) == 5, "all five charms are on the very first shelf");
        for (auto& w : a.shelf) if (w.offer.kind == Kind::Accessory && ResistCharm(w.offer.index))
            Check(StillValid(w.offer, c) && WhyNot(w.offer, c).empty(), "charms are buyable on floor 1 (not locked by progression)");
        Check(count(a, false) <= balance::kShelfAccessory, "charms do not take the random accessory slots");
        int early = 0; for (auto& w : a.shelf) if (w.offer.kind == Kind::Accessory && ResistCharm(w.offer.index)) early = w.price;
        c.floor = 40; Shop b; b.floor = 40; Stock14(b, c, {});
        int late = 0; for (auto& w : b.shelf) if (w.offer.kind == Kind::Accessory && ResistCharm(w.offer.index)) late = w.price;
        Check(early > 0 && late > early, "charm price rises with floors");
        for (int f : {1, 20, 40, 60}) {
            Context pc = c; pc.floor = f; int other = -1, charm = -1;
            for (int i = 0; i < growth::kAccessoryCount; ++i) { if (other < 0 && !ResistCharm(i)) other = i; if (charm < 0 && ResistCharm(i)) charm = i; }
            const int po = ShopPrice(Offer{Kind::Accessory, other, 1, 1}, pc), pr = ShopPrice(Offer{Kind::Accessory, charm, 1, 1}, pc);
            Check(pr == std::max(1, po * 6 / 5), "charm costs 20% more than a normal accessory on the same floor");
        }
        int ice = -1; for (int i = 0; i < growth::kAccessoryCount; ++i) if (growth::kAccessories[i].id == "AC_ResistIce") ice = i;
        p.AddAccessory(ice); Shop d; d.floor = 40; Stock14(d, c, {});
        Check(count(d, true) == 4 && std::none_of(d.shelf.begin(), d.shelf.end(), [&](const Ware& w) { return w.offer.kind == Kind::Accessory && w.offer.index == ice; }), "a bought charm leaves the shelf");
        for (int seed = 1; seed <= 200; ++seed) { Shop e; e.floor = 20; e.seed = static_cast<uint64_t>(seed); c.floor = 20; Stock14(e, c, {}); Check(count(e, false) <= balance::kShelfAccessory, "random accessory slots unchanged"); }
    }
    {   // 3.6.0 驿站武器补货：非 Boss 武器都持有了（或拿了拾荒者）时，补本区及之前区的 Boss 关联武器
        const auto official = Contract(); growth::Profile p; growth::Blessings blessings;
        Context c; c.profile = &p; c.blessings = &blessings; c.catalog = &official; c.floors = 50; c.floor = 17;
        const int zone = tower::rewards::ZoneOf(c.floor, c.floors);
        for (const auto& g : official.gates) if ((g.cat == Cat::Main || g.cat == Cat::Shaqi) && !g.bossLinked && g.zone >= 0) {
            p.AddWeapon(g.index);
        }
        Shop s; s.floor = 17; s.seed = 77; Stock14(s, c, {});
        int main = 0, shadow = 0; bool zoneOk = true, linkedOnly = true;
        for (const auto& w : s.shelf) if (w.offer.kind == Kind::Weapon && w.offer.quality != kRareQuality) {
            (growth::kWeapons[w.offer.index].kind == 1 ? shadow : main)++;
            for (const auto& g : official.gates) if (g.index == w.offer.index && (g.cat == Cat::Main || g.cat == Cat::Shaqi)) { zoneOk = zoneOk && g.zone <= zone; linkedOnly = linkedOnly && g.bossLinked; }
        }
        std::printf("info: 3.6.0 shop fill main %d shadow %d\n", main, shadow);
        Check(main + shadow > 0 && zoneOk && linkedOnly, "3.6.0 shop weapons refill from boss-linked weapons of this / earlier zones when non-boss ones are all owned");
    }
    {   // 3.6.0 悬赏令：×3、权重 3、价格跟层走（不超过 Boss 最低基础魂玉 × 0.8，用了一定赚）
        int bi = -1; for (int i = 0; i < kItemCount; ++i) if (kItems[i].item == Item::Bounty) bi = i;
        Check(bi >= 0 && kItems[bi].weight == 3 && tower::kBountyMul == 3.0 && kItems[bi].text.find("×3") != std::string_view::npos, "3.6.0 bounty x3, weight 3");
        bool ok = true;
        for (int f : {1, 5, 20, 40, 50, 51, 80, 150}) {
            Context c; c.floor = f; c.floors = 50;
            const int price = ShopPrice(Offer{Kind::Item, bi, 1, 1}, c);
            const double minBase = balance::kBossJade * balance::J(balance::ShownOfBoss(f)) * (1.0 - balance::kBossJadeSpread);
            ok = ok && price > 0 && price <= tower::BountyPriceCap(f) && price <= minBase * 0.8 + 1e-9 && 3 * minBase - minBase - price >= minBase * 1.2 - 1e-9;
        }
        Check(ok, "3.6.0 bounty price <= 0.8 x lowest boss base jade; net gain >= 1.2 x base");
    }
    std::printf("TOWER_REWARDS_OK %d checks (no game process)\n", g_checks);
    return 0;
}
