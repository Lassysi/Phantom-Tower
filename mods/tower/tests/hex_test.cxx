#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include "../src/tower_hex.h"
#include "../src/tower_balance.h"
#include "../src/tower_servants.h"
using namespace tower::hex;
static void Check(bool ok, const char* why) { if (!ok) { std::printf("FAIL: %s\n", why); std::exit(1); } }
int main() {
    {
        State s; s.Init(20261112);
        constexpr Mask pool = (1ull << ServantMaster) | (1ull << Tank) | (1ull << Swift) | (1ull << Bell);
        for (const int floor : {1, 30, 60}) {
            Check(s.Begin(floor, pool & ~(1ull << Bell)), "servant repeat floor offers");
            const auto it = std::find(s.offers.begin(), s.offers.end(), ServantMaster);
            Check(it != s.offers.end(), "servant remains eligible after an earlier pick");
            const int slot = static_cast<int>(it - s.offers.begin());
            State pending;
            Check(State::Load(s.Save(), &pending) && pending.Pick(slot), "repeat offer survives reload");
            Check(State::Load(pending.Save(), &s), "repeated pick survives reload");
        }
        Check(s.PickCount(ServantMaster) == 3 && !s.CanPick(ServantMaster, 90), "servant stops at three");
        Check(s.Begin(90, pool) && std::find(s.offers.begin(), s.offers.end(), ServantMaster) == s.offers.end(), "fourth servant cannot be drawn");
        Check(!s.Refresh(0, pool), "fourth servant cannot be rerolled in");
        s.offers[0] = ServantMaster;
        Check(!s.Pick(0), "fourth servant cannot be selected");
        State loaded;
        Check(!State::Load(s.Save(), &loaded), "reject over-cap pending servant offer");
        s.pending = 0; s.offers = {-1, -1, -1}; s.picks[3] = ServantMaster; s.pickedAt[3] = 90; s.completed |= 8;
        Check(!State::Load(s.Save(), &loaded), "reject four servants in a save");

        using namespace tower::servants;
        const auto accept = [](int) { return true; };
        const auto order = Order(20261112, 30, accept);
        Check(order.size() >= 45 && order == Order(20261112, 30, accept) && order != Order(20261112, 31, accept), "servants have stable independent per-floor draws");
        for (size_t i = 0; i < order.size(); ++i) {
            Check(tower::kBossPool[order[i]].name.find("暂译") == std::string_view::npos, "placeholder-named entries are not summoned");
            for (size_t j = i + 1; j < order.size(); ++j) Check(tower::kBossPool[order[i]].identity != tower::kBossPool[order[j]].identity, "one entry per identity per floor (equal odds per boss)");
        }
        bool hookFirst = false, hookSecond = false;
        std::vector<int> seen;
        for (int f = 1; f <= 40; ++f) for (int n : Order(20261112, f, accept)) seen.push_back(n);  // 变体跨层轮到
        for (int n : seen) {
            const auto& b = tower::kBossPool[n];
            Check(b.identity != "LionDancer" && b.identity != "HuaShan_Blue" && b.identity != "LengTu" && b.identity != "MengHua", "excluded bosses cannot be summoned");
            Check(tower::servants::DirectSpawnOk(n) || (!tower::floorplay::SpawnerFirst(n) && tower::floorplay::NativeOnly(n).empty()), "native-only arena bosses cannot be summoned (3.7.0: except ZuoShang P2, user-verified direct spawn)");
            if (b.identity == "HookMan") (b.path.find("HookMan_P2") != std::string_view::npos ? hookSecond : hookFirst) = true;
        }
        Check(hookFirst && hookSecond, "standalone phases have separate entries");
        for (size_t i = 0; i < order.size(); ++i) for (size_t j = i + 1; j < order.size(); ++j)
            Check(ActualPath(order[i]) != ActualPath(order[j]), "one entry per actually spawned path (core final-phase swap)");
        Check(kPrefix.substr(0, 6) != "tower-", "servants use the trainer standalone spawn path, not the tower path");
        int swapped = 0, plain = 0;
        for (int i = 0; i < static_cast<int>(std::size(tower::kBossPool)); ++i) {
            if (ActualPath(i) != tower::kBossPool[i].path) { ++swapped; Check(!PlainPath(i), "phase-swapped entries never use the tower fallback path"); }
            else if (PlainPath(i)) ++plain;
        }
        Check(swapped > 0 && plain > 50, "fallback path covers ordinary bosses only");
        Check(PoolValue(40, 100, 500, true, true) == 500, "spawn at full health");
        Check(PoolValue(250, 500, 800, false, true) == 400, "keep damaged health fraction when cap grows");
        Check(PoolValue(100, 200, 100, false, false) == 50, "keep spent shachi fraction when cap shrinks");
        Check(PoolValue(0, 500, 800, false, true) == 0, "a cap change cannot revive a dead servant");
    }
    for (uint64_t seed = 0; seed < 10000; ++seed) {
        State s; s.Init(seed);
        for (int floor : {1, 30, 60, 90}) {
            Check(s.Begin(floor), "begin milestone");
            const auto first = s.offers;
            Check(first[0] != first[1] && first[0] != first[2] && first[1] != first[2], "unique offers");
            for (int id : first) Check(defs[id].from <= floor, "offer gate");
            State loaded; Check(State::Load(s.Save(), &loaded), "load pending");
            Check(loaded.offers == first && loaded.rng == s.rng, "preserve pending");
            for (int slot = 0; slot < 3; ++slot) {
                const auto before = loaded.offers;
                Check(loaded.Refresh(slot), "each slot refreshes once");
                Check(loaded.offers[slot] != before[slot], "refreshed option changes");
                for (int j = 0; j < 3; ++j) if (j != slot) Check(loaded.offers[j] == before[j], "other cards remain unchanged");
                Check(!loaded.Refresh(slot), "same card cannot refresh twice");
                State resumed; Check(State::Load(loaded.Save(), &resumed), "per-card refresh survives resume"); loaded = resumed;
            }
            Check(loaded.offers[0] != loaded.offers[1] && loaded.offers[0] != loaded.offers[2] && loaded.offers[1] != loaded.offers[2], "refresh keeps cards distinct");
            s = loaded; Check(s.Pick(0), "pick"); Check(!s.Pick(0), "no repeated pick");
            Check(!s.Begin(floor), "milestone completed");
            State reloaded; Check(State::Load(s.Save(), &reloaded), "load selection"); s = reloaded;
        }
    }
    State s; s.Init(9); s.picks[0] = Tank; s.pickedAt[0] = 1; s.completed = 1;
    s.ClearFloor(1); s.ClearFloor(1); Check(s.Levels(Tank) == 1 && std::abs(s.Health() - kTankLife) < 1e-9, "replayed floor counts once");
    s.ClearFloor(64); s.ClearFloor(65); s.ClearFloor(100); Check(s.Levels(Tank) == 4, "all displayed floors tracked");
    State restored; Check(State::Load(s.Save(), &restored) && restored.Levels(Tank) == 4, "growth preserved");
    Check(!State::Load("1 1 1 broken", &restored), "reject corrupt save");
    s.picks[1] = Leech; s.pickedAt[1] = 30; s.completed |= 2; Check(s.LifeSteal() == 50, "five percentage points");
    Check(std::abs(1000 * Scale(1.048, .8) - 1886.4) < 1e-8, "normal reward and augment multiply");
    Check(std::abs((Scale(1.4, 1.2 + .8) - 1) * 100 - 320) < 1e-8, "panel combines health rewards and augment bonuses without native equipment");
    Check(std::abs((Scale(1.12, 1.0) - 1) * 100 - 124) < 1e-8, "panel combines shachi rewards and scientist health form");
    Check(std::abs(DamageFactor(200, 300) - .50) < 1e-9, "3.6.0 damage reduction sources add");
    Check(kAsceticStep == .10, "3.6.0 ascetic +10% per floor");
    Check(std::abs(Reduction(300, 300, true) - .80) < 1e-9 && ReductionCapped(500, 300) && !ReductionCapped(400, 300) && ReductionCapped(750, 150, kMinTakenIcathia) && !ReductionCapped(700, 150, kMinTakenIcathia),
          "3.6.0 guard charm adds 20%; cap flag at 80% / 90%");
    Check(std::abs(InterestOn(1000) - 100) == 0 && InterestOn(1000000) == 100000, "4.0.0 hedge fund 10%, uncapped");
    Check(std::abs(DamageFactor(500, 1200) - .2) < 1e-9, "tower damage reduction capped at eighty percent");
    State e; e.Init(7); e.picks[0] = Escape; e.pickedAt[0] = 1; e.completed = 1;
    e.UpdateEscape(true, 201, 1000); Check(!e.escapeActive, "escape does not activate above threshold");
    e.UpdateEscape(true, 200, 1000); Check(e.escapeActive && std::abs(e.Speed() - .8) < 1e-9, "escape activates at twenty percent");
    e.UpdateEscape(true, 800, 1000); Check(e.escapeActive, "escape remains active through healing");
    State next; Check(State::Load(e.Save(), &next) && !next.escapeActive, "escape does not persist across resume");
    e.UpdateEscape(false, 100, 1000); Check(!e.escapeActive && e.Speed() == 0, "escape resets outside the floor");
    State g; g.Init(8); g.picks[0] = Grit; g.pickedAt[0] = 1; g.completed = 1;
    g.UpdateGrit(true, 1000, 1.0); g.UpdateGrit(true, 900, 1.1);
    Check(g.gritStacks == 1 && g.Defense() == 10, "health decrease grants one percent defense");
    g.UpdateGrit(true, 950, 2.0); g.UpdateGrit(true, 940, 2.1);
    Check(g.gritStacks == 2 && g.Defense() == 20, "healing then self damage counts another decrease");
    Check(State::Load(g.Save(), &next) && next.gritStacks == 0, "grit stacks do not persist across resume");
    g.UpdateGrit(false, 940, 2.2); Check(g.gritStacks == 0 && g.gritLastHealth < 0 && g.gritLastDrop < 0, "grit resets between floors");
    {   // 持续掉血（每帧 -1，持续 5 秒）只算一次；停 0.5 秒后再掉血才算下一次
        State d; d.Init(8); d.picks[0] = Grit; d.pickedAt[0] = 1; d.completed = 1;
        double hp = 1000, t = 0;
        d.UpdateGrit(true, hp, t);
        for (int i = 0; i < 300; ++i) { t += 1.0 / 60; hp -= 1; d.UpdateGrit(true, hp, t); }
        Check(d.gritStacks == 1, "continuous drain is one decrease, not one stack per frame");
        t += 0.3; hp -= 50; d.UpdateGrit(true, hp, t);
        Check(d.gritStacks == 1, "a hit inside the quiet window merges with the previous decrease");
        t += 0.6; hp -= 50; d.UpdateGrit(true, hp, t);
        Check(d.gritStacks == 2, "a hit after the quiet window counts again");
        for (int i = 0; i < 100; ++i) { t += 1.0; hp -= 1; d.UpdateGrit(true, hp, t); }
        Check(d.gritStacks == State::kGritCap, "stacks stop at the cap");
    }
    State k; k.Init(3); k.picks[0] = KillingIntent; k.pickedAt[0] = 1; k.completed = 1;
    Check(!k.CanRevive(false, 80, 100) && k.CanRevive(false, 81, 100), "revival requires strictly over eighty percent");
    Check(!k.CanRevive(true, 100, 100) && !k.CanRevive(false, 100, 0), "revival limited once and requires valid maximum");
    State science; science.Init(10); science.picks[0] = Scientist; science.pickedAt[0] = 1; science.completed = 1;
    const int branch = science.ScientistForm(1, 20261110);
    Check((branch == 1 || branch == 2) && science.ScientistForm(1, 20261110) == branch && science.ScientistForm(0, 20261110) == 0 && science.ScientistForm(2, 20261110) > 0, "scientist applies from selected floor onward with stable per-floor branch");
    State scienceSaved; Check(State::Load(science.Save(), &scienceSaved) && scienceSaved.ScientistForm(1, 20261110) == branch, "scientist branch survives reload");
    const auto boost = science.ScientistBoost(1, 20261110);
    Check(boost.shachiCap == (branch == 1 ? 1 : 0) && science.ScientistBoost(0, 20261110).shachiCap == 0, "scientist shachi cap applies only to health form");
    Check((branch == 1 && boost.health == 1 && boost.attack == .4 && boost.armorBreak == .1 && boost.move == 0 && boost.attackSpeed == 0) ||
          (branch == 2 && boost.health == 0 && boost.attack == 0 && boost.armorBreak == 0 && boost.move == .6 && boost.attackSpeed == .6), "scientist branch values (speed form +60% / +60%)");
    {   // v3 数值参数
        namespace b = tower::balance;
        Check(b::J(10) == 60 && b::J(100) == 330, "v3 income J(s) = 30 + 3s");
        Check(b::PriceCurve(b::Buy::Temper, 1) == 0.50 && b::PriceCurve(b::Buy::Weapon, 5) == 0.65 && b::PriceCurve(b::Buy::Mod, 20) == 1.0 &&
              b::PriceCurve(b::Buy::Weapon, 50) == 1.0 && std::abs(b::PriceCurve(b::Buy::Weapon, 100) - 1.5) < 1e-9 && b::PriceCurve(b::Buy::Weapon, 300) == 2.0 &&
              b::Price(b::Buy::Temper, 1, 0) * 2 <= b::Price(b::Buy::Temper, 20, 0) / 2, "3.0.1 price curve: cheap first shops, x1 from 20, x1.5 at 100, x2 cap");
        Check(b::Price(b::Buy::Weapon, 49, 0) == 1274 && std::abs(b::Price(b::Buy::Weapon, 49, 1) - 1274 * 1.05) <= 1.0 &&
              b::Price(b::Buy::Weapon, 49, 10) == 1682, "weapon costs 7.2 base-income floors, rises 5% per buy (3.3.3), caps at 9.5 floors");
        Check(std::abs(b::kPriceRise[static_cast<int>(b::Buy::Mod)] - 0.20 / 3) < 1e-12 && std::abs(b::kPriceRise[static_cast<int>(b::Buy::Accessory)] - 0.04) < 1e-12, "3.3.3 price rises: 1/3 of the original, linear");
        Check(std::abs(b::Price(b::Buy::Temper, 49, 200) - 2.0 * b::Price(b::Buy::Temper, 49, 0)) <= 2.0 && b::Price(b::Buy::Temper, 49, 20) < 2 * b::Price(b::Buy::Temper, 49, 0),
              "3.3.3 purchase rises stop at x2");
        Check(b::Price(b::Buy::Temper, 49, 0) == 566 && b::Price(b::Buy::Cultivate, 49, 0) == 478,
              "growth point bundles cost multiple floors of base income");
        Check(std::abs(b::SoftSpeed(0.5, b::kMoveKnee) - 1.5) < 1e-9 && std::abs(b::SoftSpeed(1.2, b::kMoveKnee) - 2.2) < 1e-9 && std::abs(b::SoftSpeed(9, b::kMoveKnee) - 10.0) < 1e-9, "speed: uncapped (user 2026-11-13)");
        Check(std::abs(b::E(1) - 1.15) < 1e-9 && std::abs(b::E(30) - 1.15) < 1e-9 && b::E(60) > 2.07 && b::E(60) < 2.09 && b::E(100) > 6.7 && b::E(100) < 6.9, "difficulty growth E(s): 1.15 / 2.08 / 6.8");
        Check(b::JadeTotal(100, 0.5, 2.0, 1.0) == 600 && b::JadeTotal(100, 0, 1, 0) == 100 && b::JadeTotal(100, 0.5, 2.0 * 2.0, 0.2) == 720,
              "3.1.0 jade: (1 + additive) x multipliers x (1 + snowball)");
        Check(InterestOn(5000, b::InterestCap(99)) == 500 && InterestOn(50000, b::InterestCap(1)) == 5000, "hedge fund: 10% of held jade, no cap (4.0.0)");
    }
    {   // v3 分级抽取：三档都有卡；第 90 层传说比第 1 层多；仆从大师第 2、3 次权重递减
        int counts[4] = {}; for (int id = 0; id < Count; ++id) if (Playable(id)) ++counts[TierOf(id)];
        Check(counts[1] == 22 && counts[2] == 29 && counts[3] == 19 && !Playable(Encore) && MultiJump == 71 && TierOf(MultiJump) == 1 && MaxRank(MultiJump) == 2 && MultiJumpAirJumps(1) == 1 && MultiJumpAirJumps(2) == 3 && MultiJumpAirJumps(0) == 0 && (kPlayableMask & Bit(MultiJump)) && TierOf(MedSpark) == 3 && TierOf(Riposte) == 2 && TierOf(Afterimage) == 1 && TierOf(ShadowClone) == 3 && Riposte == 64 && !(kPlayableMask & Bit(Riposte)), "hex tiers incl. 3.1.0 / 3.3.3 / 3.3.4 / 3.6.0 additions (all rare); 3.7.0 playstyle ids from 64, kept out of the pool until wired");
        Check(TierOf(Tank) == 3 && TierOf(Swift) == 3 && TierOf(Icathia) == 3 && TierOf(BreakGrowth) == 3,
              "the four moved augments use legendary draw odds");
        auto legendShare = [](int floor) { int t3 = 0; for (uint64_t seed = 1; seed <= 3000; ++seed) { State s; s.Init(seed); if (s.Draw(floor, false)) for (int id : s.offers) t3 += TierOf(id) == 3; } return t3; };
        std::printf("info: legendary cards on 3000 pages floor 1 %d, floor 90 %d\n", legendShare(1), legendShare(90));
        Check(legendShare(90) > legendShare(1) * 3, "legendary odds rise with the hex floor");
        State sv; sv.Init(5); Check(sv.WeightOf(ServantMaster) == 20, "servant first pick full weight (3.6.0 base 20)");
        sv.picks[0] = ServantMaster; Check(sv.WeightOf(ServantMaster) == 10, "servant second pick half weight");
        sv.picks[1] = ServantMaster; Check(sv.WeightOf(ServantMaster) == 5, "servant third pick quarter weight");
        sv.picks[2] = Tank;
        Check(sv.WeightOf(Swift) == sv.WeightOf(ServantMaster), "tank lowers swift to third servant weight");
        sv.picks[2] = Swift;
        Check(sv.WeightOf(Tank) == sv.WeightOf(ServantMaster), "swift lowers tank to third servant weight");
        auto refreshedSwift = [](bool hasTank) {
            int count = 0;
            for (uint64_t seed = 1; seed <= 3000; ++seed) {
                State s; s.Init(seed); s.pending = 60; s.offers = {Leech, Sharp, Bell};
                if (hasTank) s.picks[0] = Tank;
                if (s.Refresh(0, (1ull << Swift) | (1ull << Scientist)) && s.offers[0] == Swift) ++count;
            }
            return count;
        };
        Check(refreshedSwift(true) * 2 < refreshedSwift(false), "the engine pairing penalty also applies to individual refreshes");
    }
    bool sawHealth = false, sawSpeed = false;
    for (uint64_t seed = 0; seed < 100; ++seed) {
        const int form = science.ScientistForm(1, seed); const auto b = science.ScientistBoost(1, seed);
        Check(b.shachiCap == (form == 1 ? 1 : 0), "only scientist health form boosts shachi cap");
        if (form == 2) Check(b.move == .60 && b.attackSpeed == .60, "scientist speed form boosts both speeds by 60%");
        sawHealth |= form == 1; sawSpeed |= form == 2;
    }
    Check(sawHealth && sawSpeed, "both scientist forms exercised");
    State later; later.Init(11); later.picks[1] = Scientist; later.pickedAt[1] = 30; later.completed = 2;
    Check(later.ScientistForm(29, 20261110) == 0 && later.ScientistForm(30, 20261110) && later.ScientistForm(90, 20261110), "scientist affects no prior floor and all later floors");
    State heavy; heavy.Init(13); heavy.picks[0]=Heavy; heavy.pickedAt[0]=1; heavy.completed=1;
    Check(State::Load(heavy.Save(), &restored) && restored.Has(Heavy) && restored.CanPick(Heavy,30) && restored.Rank(Heavy) == 1, "heavy selection persists; 3.6.0 it can be upgraded");
    State scavenger; scavenger.Init(14); scavenger.picks[1]=Scavenger; scavenger.pickedAt[1]=30; scavenger.completed=2;
    Check(State::Load(scavenger.Save(), &restored) && restored.PickedFloor(Scavenger)==30, "scavenger acquisition floor survives reload");
    Check(ScavengerShopAllowed(0, 35, false) && ScavengerShopAllowed(30, 29, false) &&
          !ScavengerShopAllowed(30, 30, false) && !ScavengerShopAllowed(30, 31, false) &&
          ScavengerShopAllowed(30, 31, true), "scavenger preserves earlier unlocks and routes later first unlocks away from shop");
    {   // E 批数值（2026-11-11）
        auto with = [](std::initializer_list<Id> ids) { State s; s.Init(7); int i = 0; for (Id id : ids) { s.picks[i] = id; s.pickedAt[i] = 1; ++i; } return s; };
        auto near = [](double a, double b) { return std::abs(a - b) < 1e-9; };
        State g = with({GlassCannon});
        Check(near(g.Attack(), 1.60) && near(g.Health(), -.30), "glass cannon: attack +160%, life -30%");
        State bp = with({BloodPact});
        Check(bp.LifeSteal() == 50 && near(bp.Health(), -.10), "blood pact: leech +5%, life -10%");
        State both = with({GlassCannon, BloodPact, Leech});
        Check(both.LifeSteal() == 100 && near(both.Health(), -.40), "glass + pact stack additively inside hex source");
        Check(Scale(1.0, -.50) > 0, "life multiplier stays positive");
        State d = with({Decapitate, Sweep});
        Check(near(d.EnemyTaken(false), 1.2) && near(d.EnemyTaken(true), 1.5), "decapitate boss x1.2, sweep mobs x1.5");
        Check(near(State{}.EnemyTaken(false), 1.0) && near(State{}.EnemyTaken(true), 1.0), "no enemy multiplier without hexes");
        State f = with({Fury});
        f.UpdateFury(true, 299, 1000); Check(f.furyActive && near(f.Attack(), 1.0) && near(f.Break(), .3), "fury below 30%: attack +100%, break +30%");
        f.UpdateFury(true, 300, 1000); Check(!f.furyActive && near(f.Attack(), 0), "fury ends at 30% (not latched)");
        f.UpdateFury(false, 100, 1000); Check(!f.furyActive, "fury off outside combat");
        State o = with({Opening});
        o.UpdateOpening(true, 0); Check(o.openingActive && near(o.Attack(), .8), "opening active at fight start");
        o.UpdateOpening(true, 7.9); Check(o.openingActive, "opening active until 8s");
        o.UpdateOpening(true, 8.0); Check(!o.openingActive, "opening ends at 8s");
        o.UpdateOpening(false, 1); Check(!o.openingActive, "opening off outside combat");
        Check(InterestOn(0) == 0 && InterestOn(2908) == 290 && InterestOn(20000) == 2000 && InterestOn(-5) == 0, "hedge fund 10%, uncapped (4.0.0)");
        State c = with({Charmsmith});
        Check(c.CharmDue(kCharmEvery, 0) && !c.CharmDue(kCharmEvery, 1) && !c.CharmDue(kCharmEvery + 1, 0), "charm on multiples, max one held");
        c.ClearFloor(kCharmEvery); Check(!c.CharmDue(kCharmEvery, 0), "charm not repeated on a replayed floor");
        Check(!State{}.CharmDue(kCharmEvery, 0), "charm needs the hex");
        State saved = with({GlassCannon, BloodPact, Decapitate, Sweep}); saved.completed = 0b1111;
        for (int i = 0; i < 4; ++i) saved.pickedAt[i] = SlotFloor(i);
        State back; Check(State::Load(saved.Save(), &back) && back.Has(Sweep) && back.Has(GlassCannon), "E-batch ids persist");
    }
    {   // F 批数值（2026-11-11）
        auto with = [](std::initializer_list<Id> ids) { State s; s.Init(9); int i = 0; for (Id id : ids) { s.picks[i] = id; s.pickedAt[i] = 1; ++i; } return s; };
        auto near = [](double a, double b) { return std::abs(a - b) < 1e-9; };
        State sb = with({SnowBall});
        Check(near(sb.SnowBallBonus(), 0), "snowball starts at zero");
        for (int f = 1; f <= 10; ++f) sb.ClearFloor(f);
        Check(near(sb.SnowBallBonus(), .50), "snowball +5% per cleared floor (user 2026-11-13)");
        for (int f = 11; f <= 100; ++f) sb.ClearFloor(f);
        Check(near(sb.SnowBallBonus(), 5.0), "snowball keeps growing past +100% (3.1.0)");
        sb.endlessCleared = 400; Check(near(sb.SnowBallBonus(), 25.0), "snowball has no cap (user 2026-11-13): 100 + 400 floors = +2500%");
        Check(near(State{}.SnowBallBonus(), 0), "snowball needs the hex");
        State as = with({Ascetic, Flawless}); as.asceticStacks = 5; as.flawlessStacks = 4;
        Check(near(as.Health(), .50) && near(as.Attack(), .40), "v3: ascetic +10% life (3.6.0), flawless +10% attack per stack");
        State no = with({}); no.asceticStacks = 5; no.flawlessStacks = 4;
        Check(near(no.Health(), 0) && near(no.Attack(), 0), "stacks do nothing without the hexes");
        State e = with({Echo}); e.echoActive = true; e.echoStacks = 1;
        Check(near(e.Attack(), .5), "echo +50% attack while active");
        e.echoStacks = 3; Check(near(e.Attack(), 1.5) && kEchoSeconds == 10, "echo stacks +50% each, 10 s window");
    }
    {   // G 批（2026-11-11）
        auto with = [](std::initializer_list<Id> ids) { State s; s.Init(11); int i = 0; for (Id id : ids) { s.picks[i] = id; s.pickedAt[i] = SlotFloor(i); s.completed |= 1u << i; ++i; } return s; };
        auto near = [](double a, double b) { return std::abs(a - b) < 1e-9; };
        State b = with({BackToBasics});
        Check(near(b.Attack(), 1.0) && near(b.Break(), .80) && b.Defense() == 100 && b.LifeSteal() == 50,
              "back to basics grants +100% attack, +80% break, +10% mitigation and +5% leech");
        Check(near(DamageFactor(0, b.Defense()), .90) && near(DamageFactor(200, b.Defense()), .70),
              "back to basics uses the existing damage reduction stacking rule");
        State retired = with({Invincible, BackToBasics}), resumed;
        Check(State::Load(retired.Save(), &resumed) && resumed.picks[0] == Invincible && !resumed.Has(Invincible) &&
              resumed.Has(BackToBasics) && !resumed.Begin(1), "retired hex preserves the used milestone and later IDs when loaded");
        State offer; offer.Init(11); offer.pending = 1; offer.offers = {Invincible, Tank, Swift}; offer.rerolled = 1;
        offer.seen = (1ull << Invincible) | (1ull << Tank) | (1ull << Swift);
        Check(State::Load(offer.Save(), &resumed) && !resumed.Pick(0), "legacy offer loads without allowing selection of the retired hex");
        Check(resumed.ReplaceRetiredOffer(1ull << Bell) && resumed.offers == std::array<int, 3>{Bell, Tank, Swift} &&
              resumed.rerolled == 1 && resumed.Pick(0), "retired offer is replaced in place without spending a refresh");
        Check(State{}.GrantRandom(2, 1ull << Invincible).empty(), "transmutation cannot grant the retired hex even with a stale pool mask");
        State t = with({Transmutation});
        const auto got = t.GrantRandom(kTransmuteCount, kAllMask);
        Check(got.size() == 2 && got[0] != got[1] && got[0] != Transmutation && got[1] != Transmutation && t.Has(static_cast<Id>(got[0])) && t.Has(static_cast<Id>(got[1])), "transmutation grants two distinct new hexes");
        State back; Check(State::Load(t.Save(), &back) && back.Has(static_cast<Id>(got[1])), "transmuted hexes persist");
        State full = with({Tank, Swift, Icathia, Leech});
        Check(full.GrantRandom(2, kAllMask).size() == 2 && full.extra.size() == 2 && full.extraAt[0] < 0, "3.0.1: transmutation grants go to extra slots even when fixed slots are full");
        { State back; Check(State::Load(full.Save(), &back) && back.extra.size() == 2, "transmutation extra grants survive save/load"); }
        {   // 3.4.0（用户）：每 10 层一个；旧档按「应得 − 已选」补发
            Check(!ExtraHexFloor(1) && ExtraHexFloor(5) && ExtraHexFloor(10) && ExtraHexFloor(15) && !ExtraHexFloor(30) && ExtraHexFloor(55) && !ExtraHexFloor(12) && ExtraHexFloor(100) && ExtraHexFloor(105) && ExtraHexFloor(175) && !ExtraHexFloor(176),
                  "3.6.0 hex floors: 1 and every 5 (1/30/60/90 fixed slots, others extra)");
            Check(HexFloor(1) && HexFloor(5) && !HexFloor(6) && HexFloor(30), "3.6.0 HexFloor every 5");
            Check(State{}.DueHexes(61) == 13 && State{}.DueHexes(2) == 1 && State{}.DueHexes(111) == 23, "due count: floor 1 + every 5");
            // 3.3.x 旧档：到 60 层，选过 1 / 15 / 30 / 45 层（4 个）→ 开 61 层前补 3 个（应得 1/10/20/30/40/50/60 = 7）
            State old; old.Init(4); old.enabled = true;
            old.picks[0] = Tank; old.pickedAt[0] = 1; old.picks[1] = Swift; old.pickedAt[1] = 30; old.completed = 0b11;
            old.extra = {Leech, Echo}; old.extraAt = {15, 45}; old.extraBase = {0, 0};
            State loaded; Check(State::Load(old.Save(), &loaded) && loaded.HadHexes() == 4, "old save loads with its 4 picks");
            int backfills = 0;
            for (int guard = 0; guard < 10; ++guard) {
                const int m = loaded.MissedExtra(61); if (!m) break;
                Check(loaded.Begin(m) && loaded.pending == m && m % 5 == 0, "backfill opens on an unused 5-floor");
                int slot = -1; for (int k = 0; k < 3; ++k) if (loaded.offers[k] >= 0) { slot = k; break; }
                Check(slot >= 0 && loaded.Pick(slot), "backfill pick works"); ++backfills;
            }
            Check(backfills == 8 && loaded.HadHexes() == 12 && loaded.MissedExtra(61) == 0, "3.6.0: old run at floor 60 with 4 picks backfills all 8 unused 5-floors (5/10/20/25/35/40/50/55; fixed 60 opens by its own path)");
            // 质变给的（extraAt < 0）不占补发名额；新局正常走不补发
            State q; q.Init(9); q.enabled = true; q.extra = {Leech}; q.extraAt = {-30}; q.extraBase = {0};
            Check(q.MissedExtra(2) == 0 && q.DueHexes(2) == 1, "fresh run before floor 1 pick: nothing to backfill (floor 1 opens normally)");
            State fresh; fresh.Init(5); fresh.enabled = true; fresh.picks[0] = Tank; fresh.pickedAt[0] = 1; fresh.completed = 1;
            fresh.extra = {Leech}; fresh.extraAt = {5}; fresh.extraBase = {0};
            Check(fresh.MissedExtra(10) == 0 && fresh.Begin(10) && fresh.pending == 10, "floor 10 opens a normal pick, no backfill");
            State skipped5; skipped5.Init(5); skipped5.enabled = true; skipped5.picks[0] = Tank; skipped5.pickedAt[0] = 1; skipped5.completed = 1;
            Check(skipped5.MissedExtra(10) == 5, "3.4.x run already past floor 5 without its pick: backfill floor 5 before floor 10");
            // 3.6.0（用户）：无尽层老档只多补 5 个；按新规则打上来的无尽局不补
            {
                State en; en.Init(8); en.enabled = true;
                en.picks[0] = Tank; en.pickedAt[0] = 1; en.picks[1] = Swift; en.pickedAt[1] = 30; en.picks[2] = Leech; en.pickedAt[2] = 60; en.picks[3] = Sharp; en.pickedAt[3] = 90; en.completed = 0b1111;
                const Id more[] = {Bell, Battle, BreakGrowth, Desperate, Sturdy, Keen, Pursuit, Bulwark, Escape, Grit, KillingIntent, Scientist};
                int k = 0;
                for (int f = 10; f <= 150; f += 10) if (f != 30 && f != 60 && f != 90) { en.extra.push_back(more[k++]); en.extraAt.push_back(f); en.extraBase.push_back(0); }
                Check(en.HadHexes() == 16 && en.OwedHexes(151) == 21, "3.6.0 endless old save at 150: owed = old rule 16 + 5");
                int got = 0;
                for (int guard = 0; guard < 30; ++guard) {
                    const int m = en.MissedExtra(151); if (!m) break;
                    if (!en.Begin(m) || !en.Pick(0)) break;
                    ++got;
                }
                Check(got == 5 && en.MissedExtra(151) == 0, "3.6.0 endless old save gets exactly 5 backfills");
                State fresh100; fresh100.Init(9); fresh100.enabled = true;
                for (int f = 5; f <= 150; f += 5) if (f != 30 && f != 60 && f != 90) { fresh100.extra.push_back(Tank); fresh100.extraAt.push_back(f); fresh100.extraBase.push_back(0); }
                Check(fresh100.MissedExtra(151) == 0, "3.6.0 endless run that followed the 5-floor rule: no backfill");
                { State at100; at100.Init(11); at100.enabled = true; at100.picks[0] = Tank; at100.pickedAt[0] = 1; at100.completed = 1;
                  for (int f = 10; f <= 90; f += 10) if (f != 30 && f != 60 && f != 90) { at100.extra.push_back(Leech + f / 10); at100.extraAt.push_back(f); at100.extraBase.push_back(0); }
                  Check(at100.OwedHexes(100) == at100.OwedHexes(99) - 0 * 0 + (at100.OwedHexes(100) - at100.OwedHexes(99)) && at100.OwedHexes(100) == 15, "3.6.0 save at the endless door (next shown 100) is owed old-rule 10 + 5, not the full 20"); }
                State climb; climb.Init(10); climb.enabled = true; climb.picks[0] = Tank; climb.pickedAt[0] = 1; climb.completed = 1;
                climb.extra = {Leech, Echo}; climb.extraAt = {10, 20}; climb.extraBase = {0, 0};
                Check(climb.OwedHexes(41) == 9 && climb.MissedExtra(41) == 5, "3.6.0 climbing save at floor 40: owed by the 5-floor rule (9), backfill starts at floor 5");
            }
            // 旧版存档停在第 15 层的海克斯选择页上（旧规则的额外层）：新版必须照常读进来、能选
            State mid; mid.Init(6); mid.enabled = true; mid.picks[0] = Tank; mid.pickedAt[0] = 1; mid.completed = 1;
            mid.pending = 15; mid.offers = {Swift, Leech, Echo}; mid.seen |= (1ull << Swift) | (1ull << Leech) | (1ull << Echo) | (1ull << Tank);
            State midBack; Check(State::Load(mid.Save(), &midBack) && midBack.pending == 15, "3.3.x save paused on a floor-15 offer still loads");
            Check(midBack.Pick(0) && midBack.EndlessDone(15) && midBack.HadHexes() == 2, "legacy floor-15 offer can be picked and counts toward the total");
        }
        Check(kArcaneChargeMul == 6.0f && defs[Arcane].text == "绝技充能效率更快；武器切换无冷却",
              "arcane keeps the native 6x charge multiplier and uses the approved short description");
    }
    {  // 无尽模式（用户 2026-11-12）：第 120 / 150 … 层各选一个；如有神助只在有回归基本功后可选；池子见底给 2 / 1 张；存读档带尾段
        Check(EndlessMilestone(120) && EndlessMilestone(150) && EndlessMilestone(100) && EndlessMilestone(130) && EndlessMilestone(125) && !EndlessMilestone(126), "3.6.0: endless hex floors every 5");
        State s; s.Init(77);
        Check(!s.CanPick(DivineAid, 1), "DivineAid needs BackToBasics first");
        s.picks[0] = BackToBasics; s.pickedAt[0] = SlotFloor(0); s.completed = 1;
        Check(s.CanPick(DivineAid, 1), "DivineAid becomes pickable once BackToBasics is held");
        Check(TierOf(DivineAid) == 3, "DivineAid is legendary");
        Check(s.Begin(120) && s.pending == 120, "endless milestone opens an offer");
        int slot = -1; for (int i = 0; i < 3; ++i) if (s.offers[i] >= 0 && s.CanPick(static_cast<Id>(s.offers[i]), 120)) { slot = i; break; }
        const int got = s.offers[slot];
        Check(slot >= 0 && s.Pick(slot) && s.Has(static_cast<Id>(got)) && s.EndlessDone(120) && !s.Begin(120), "endless pick is stored once and not offered again");
        s.ClearFloor(121); s.ClearFloor(122);
        State back; Check(State::Load(s.Save(), &back) && back.Has(static_cast<Id>(got)) && back.endlessCleared == 2 && back.EndlessDone(120), "endless picks survive save/load");
        { State old; old.Init(9); old.extra = {Tank}; old.extraAt = {180}; old.extraBase = {0}; old.endlessCleared = 3; State ob;
          Check(State::Load(old.Save(), &ob) && ob.Has(Tank), "3.0.0 endless pick at floor 180 still loads"); }
        State legacy; legacy.Init(5); State l2; Check(State::Load(legacy.Save(), &l2) && l2.extra.empty(), "old saves without the endless tail still load");
        // 拿到只剩 2 个可选：只给 2 张，第三格空
        State few; few.Init(3); int filled = 0;
        for (int id = 0; id < Count; ++id) {
            if (!Playable(id) || id == ServantMaster) continue;
            few.extra.push_back(id); few.extraAt.push_back(170 + 20 * filled); few.extraBase.push_back(0); ++filled;
        }
        while (!few.extra.empty() && (few.extra.back() == DivineAid || few.extra.back() == Transmutation)) { few.extra.pop_back(); few.extraAt.pop_back(); few.extraBase.pop_back(); }
        int left = 0; for (int id = 0; id < Count; ++id) if (Playable(id) && few.CanPick(static_cast<Id>(id), 3000) && Unlocked(id, 3000)) ++left;
        const int at = 170 + 20 * static_cast<int>(few.extra.size());
        const bool opened = few.Begin(at);
        int shown = 0; for (int x : few.offers) shown += x >= 0;
        Check(!opened || shown == std::min(3, left), "pool nearly empty: offer shows only the remaining hexes");
    }
    {   // 3.1.0 N5/N6 数值类海克斯与道具（用户：数值类后台测好，不要有 bug）
        Check(BackWaterOn(1.0) == 0 && BackWaterOn(0.95) == 0 && std::abs(BackWaterOn(0.9) - .15) < 1e-9 && std::abs(BackWaterOn(0.85) - .15) < 1e-9 &&
              std::abs(BackWaterOn(0.5) - .75) < 1e-9 && std::abs(BackWaterOn(0.05) - 1.35) < 1e-9 && std::abs(BackWaterOn(0.0) - 1.45) < 1e-9 && BackWaterOn(-1) == 0 && BackWaterOn(1.3) == 0,
              "BackWater: +15% per 10% HP lost, capped at +145%");
        State w; w.Init(3); w.extra = {WarFeed}; w.extraAt = {-1}; w.extraBase = {0};
        w.warStacks = 50; Check(std::abs(w.Attack() - 7.50) < 1e-9, "WarFeed: no cap, 50 kills = +750% (15% each, user 3.2.1)");
        w.warStacks = 0; Check(w.Attack() == 0, "WarFeed: resets to 0 on a new floor (engine writes 0)");
        State b; b.Init(4); b.extra = {BackWater}; b.extraAt = {-1}; b.extraBase = {0}; b.backWaterBonus = BackWaterOn(0.3);
        Check(std::abs(b.Attack() - 1.05) < 1e-9, "BackWater feeds Attack()");
        Check((kPlayableMask & (1ull << FlyingKick)) && TierOf(FlyingKick) == 1 && defs[FlyingKick].name == std::string_view("飞身踢"), "3.6.0 flying kick: common tier, reuses the retired Pouch id");
        Check(KickLine(4999, 100000) && !KickLine(5000, 100000) && !KickLine(0, 100000) && !KickLine(100, 0) && KickLine(9999, 100000, .10), "3.6.0 flying kick: boss HP below 5% of its max (10% / 15% when upgraded)");
        Check(KickLeave(0) == 1.0f && KickLeave(1) == 1.0f && KickLeave(5000) == 5001.0f && KickLeave(-3) == 1.0f, "3.6.2 flying kick never writes 0: leaves 1 HP (or 1 above the native lock line) so the game resolves death / phase");
        Check(!(kPlayableMask & (1ull << Wealth)) && !(kPlayableMask & (1ull << Spoils)), "3.3.4 wealth / spoils retired from the pool");
        for (int s = 0; s < 400; ++s) { State d; d.Init(2000 + s); if (d.Begin(95)) for (int x : d.offers) Check(x != Wealth && x != Spoils, "wealth / spoils never offered"); }
        Check(defs[WarFeed].text.find("60") == std::string_view::npos && defs[BackWater].text.find("15%") != std::string_view::npos, "descriptions match the numbers");
    }
    {   // 3.2.1 Boss 助战：100 层前没有；100–150 约 1/3 层 1 个 20%；150 层上约 2/5 层 1–2 个 30%；同种子同层结果固定
        using tower::servants::AssistFor;
        int none = 0; for (int s = 1; s < 100; ++s) none += AssistFor(77, s).count; Check(none == 0, "no assist before floor 100");
        int mid = 0, midN = 0, hi = 0, hiN = 0, twos = 0; bool shareOk = true, maxOk = true;
        for (uint64_t seed = 1; seed <= 200; ++seed) {
            for (int s = 100; s <= 150; ++s) { const auto a = AssistFor(seed, s); ++midN; mid += a.count > 0; shareOk = shareOk && (a.count == 0 || (a.count == 1 && a.share == 0.20f)); }
            for (int s = 151; s <= 400; ++s) { const auto a = AssistFor(seed, s); ++hiN; hi += a.count > 0; twos += a.count == 2; shareOk = shareOk && (a.count == 0 || a.share == 0.30f); maxOk = maxOk && a.count <= 2; }
        }
        Check(shareOk && maxOk, "assist share 20% (100-150) / 30% (150+), at most 2");
        Check(mid * 100 / midN >= 28 && mid * 100 / midN <= 38, "100-150: about a third of floors have one assist");
        Check(hi * 100 / hiN >= 35 && hi * 100 / hiN <= 45 && twos > hi / 4 && twos < hi * 3 / 4, "150+: about 40% of floors, one or two assists");
        Check(AssistFor(12345, 177).count == AssistFor(12345, 177).count && AssistFor(12345, 177).share == AssistFor(12345, 177).share, "assist is fixed by seed and floor");
        {  // 3.7.0：海克斯编号扩到 128
            Check(Bit(3).lo == 8 && !Bit(3).hi && Bit(70).hi == 64 && !Bit(70).lo && !Bit(-1) && !Bit(128) && (Bit(70) | Bit(3)) == Mask{8, 64}, "3.7.0 128-bit hex mask: ids 64-127 go to the high word");
            State a; a.Init(11); a.seen = Bit(Tank) | Bit(Swift);
            State b; Check(State::Load(a.Save(), &b) && b.seen == a.seen && a.Save().find(" h ") == std::string::npos, "3.7.0: saves without high ids stay in the old format");
            State c; c.Init(11); c.seen = Bit(Tank) | Mask{0, 1};
            State d; Check(State::Load(c.Save(), &d) == (Count > 64) && (Count <= 64 || d.seen == c.seen), "3.7.0: high seen word round-trips (rejected only while no id >= 64 exists)");
        }
        {
            using tower::servants::KeepFrac;
            Check(KeepFrac(0.4f, 1.0f) == 0.4f && KeepFrac(0.4f, 0.2f) == 0.2f && KeepFrac(0.4f, 0.43f) == 0.43f && KeepFrac(-1, 0.7f) == 0.7f && KeepFrac(0.3f, -1) == 0.3f,
                  "3.6.3 servant HP carries within a floor: a jump back to full (enemy phase / relay) is reverted, damage and small regen pass");
        }
        Check(tower::servants::kShare == 0.8f && kMaxServants == 3, "servant master keeps its own 3 slots at 80% (assists are extra)");
    }
    {   // 3.2.0 种子一致：同种子、同池、同选择 → 每一层的海克斯候选完全一样；池子一变（旧做法按本机接口）序列就会乱
        auto run = [](uint64_t seed, Mask mask) {
            State s; s.Init(seed); std::vector<int> seenOffers;
            for (int f : {1, 15, 30, 45, 60, 75, 90, 105, 120, 135}) {
                if (!s.Begin(f, mask)) continue;
                for (int x : s.offers) seenOffers.push_back(x);
                int slot = 0; while (slot < 3 && (s.offers[slot] < 0 || !s.CanPick(static_cast<Id>(s.offers[slot]), f))) ++slot;
                if (slot < 3) s.Pick(slot);
                s.ClearFloor(f);
            }
            return seenOffers;
        };
        for (uint64_t seed : {7ull, 20261113ull, 0xABCDEFull}) {
            Check(run(seed, kPlayableMask) == run(seed, kPlayableMask), "same seed + same picks = same hex offers on every hex floor");
        }
        int differs = 0;
        for (uint64_t seed = 1; seed <= 50; ++seed) differs += run(seed, kPlayableMask) != run(seed, kPlayableMask & ~(1ull << Scientist));
        Check(differs > 0, "a machine-dependent pool would change the offers (why draws use the fixed pool)");
    }
    {   // 3.2.0：海克斯拿到层号不合现行规则（测试包第 2 层 / 旧版无尽 120）照读、海克斯保留；重复层号仍拒
        State s; s.Init(41); s.extra = {Grit, Tank}; s.extraAt = {2, 120}; s.extraBase = {0, 0};
        State b; Check(State::Load(s.Save(), &b) && b.Has(Grit) && b.Has(Tank), "old rule hex floors still load");
        State d; d.Init(42); d.extra = {Grit, Tank}; d.extraAt = {15, 15}; d.extraBase = {0, 0};
        State e; Check(!State::Load(d.Save(), &e), "duplicate hex floor is still rejected");
    }
    {   // 3.2.0 存档过渡：积少成多旧档已叠的层按 +2% 保留，之后每层 +5%
        State s; s.Init(31); s.extra = {SnowBall}; s.extraAt = {-1}; s.extraBase = {0};
        for (int f = 1; f <= 10; ++f) s.ClearFloor(f);
        const int L0 = s.Levels(SnowBall);
        std::string old = s.Save(); const auto cut = old.rfind(" s "); Check(cut != std::string::npos, "3.2.0 saves carry the SnowBall tail");
        old.resize(cut);  // 模拟 3.2.0 之前的档（没有尾段）
        State back; Check(State::Load(old, &back) && back.snowLegacy == L0 && std::abs(back.SnowBallBonus() - 0.02 * L0) < 1e-9, "old save: existing SnowBall layers keep +2% each");
        for (int f = 11; f <= 14; ++f) back.ClearFloor(f);
        const int L1 = back.Levels(SnowBall);
        Check(L1 > L0 && std::abs(back.SnowBallBonus() - (0.02 * L0 + 0.05 * (L1 - L0))) < 1e-9, "after update: each new layer adds +5%");
        State again; Check(State::Load(back.Save(), &again) && again.snowLegacy == L0 && std::abs(again.SnowBallBonus() - back.SnowBallBonus()) < 1e-9, "legacy count survives later saves");
        State fresh; fresh.Init(32); fresh.extra = {SnowBall}; fresh.extraAt = {-1}; fresh.extraBase = {0}; for (int f = 1; f <= 6; ++f) fresh.ClearFloor(f);
        Check(fresh.snowLegacy == 0 && std::abs(fresh.SnowBallBonus() - 0.05 * fresh.Levels(SnowBall)) < 1e-9, "new pick: +5% per layer from the start");
        State none; none.Init(33); std::string o2 = none.Save(); o2.resize(o2.rfind(" s ")); State nb;
        Check(State::Load(o2, &nb) && nb.snowLegacy == 0, "old save without SnowBall: nothing carried");
    }
    {   // 3.6.0 反震
        Check(TierOf(Thorns) == 2 && TierOf(Heavy) == 2 && defs[Thorns].name == "反震" && Playable(Thorns) && (kPlayableMask & (1ull << Thorns)), "3.6.0 thorns: rare tier with Heavy, playable");
        auto near = [](double a, double b) { return std::abs(a - b) < 1e-9; };
        // 掉 1000 原始伤害、玩家上限 10000、目标上限 1e6：0.03 × 0.1 = 0.3% → 3000
        Check(near(ThornsDamage(1000, 10000, 1e6), 18000) && near(ThornsDamage(10000, 10000, 1e6), 180000) && near(ThornsDamage(1e9, 10000, 1e6), 1e6),
              "3.6.0 thorns: full bar = 18% max HP, no single-hit cap (user 2026-11-15)");
        Check(ThornsDamage(0, 10000, 1e6) == 0 && ThornsDamage(500, 0, 1e6) == 0 && ThornsDamage(500, 10000, 0) == 0 && ThornsDamage(-5, 10000, 1e6) == 0, "3.6.0 thorns: no damage on bad input");
    }
    {   // 3.6.0 海克斯升级（notes/upgrade.txt）：张数 = 等级，换数值不叠加
        auto near = [](double a, double b) { return std::abs(a - b) < 1e-9; };
        auto lv = [](Id id, int n, int firstAt = 1) { State s; s.Init(40); s.enabled = true;
            for (int k = 0; k < n; ++k) { s.extra.push_back(id); s.extraAt.push_back(firstAt + 5 * k); s.extraBase.push_back(0); } return s; };
        Check(MaxRank(Tank) == 3 && MaxRank(SnowBall) == 2 && MaxRank(Icathia) == 1 && !Upgradable(Icathia) && Upgradable(ServantMaster) && Upgradable(FlyingKick) && MaxRank(Thrower) == 2,
              "3.6.0 upgrade: max ranks follow upgrade.txt");
        Check(NameAt(Tank, 2) == "坦克引擎 Lv2" && NameAt(Icathia, 1) == "艾卡西亚的陷落" && NameAt(ServantMaster, 2) == "仆从大师 Lv2" && TextAt(ServantMaster, 3) == "随机召唤 3 名 Boss 助战" && NameAt(SnowBall, 5) == "积少成多 Lv2",
              "3.6.0 upgrade: names show LvN only for upgradable hexes, clamped to max");
        Check(TextAt(Tank, 2).find("12%") != std::string_view::npos && TextAt(Tank, 3).find("16%") != std::string_view::npos && TextAt(Icathia, 2) == defs[Icathia].text,
              "3.6.0 upgrade: per-level description");
        // 坦克引擎：Lv1 每层 8%，选到 Lv2 变成每层 12% 按全部层数重算（不是 8% + 12%）
        State t1 = lv(Tank, 1); for (int f = 1; f <= 10; ++f) t1.ClearFloor(f);
        State t2 = lv(Tank, 2); for (int f = 1; f <= 10; ++f) t2.ClearFloor(f);
        State t3 = lv(Tank, 3); for (int f = 1; f <= 10; ++f) t3.ClearFloor(f);
        Check(t1.Rank(Tank) == 1 && t2.Rank(Tank) == 2 && t3.Rank(Tank) == 3 && near(t1.Health(), .80) && near(t2.Health(), 1.20) && near(t3.Health(), 1.60),
              "3.6.0 upgrade: tank 10 floors = +80% / +120% / +160% (value replaced, layers counted from first pick)");
        Check(t1.CanPick(Tank, 20) && t2.CanPick(Tank, 20) && !t3.CanPick(Tank, 20) && !lv(SnowBall, 2).CanPick(SnowBall, 20) && !lv(Icathia, 1).CanPick(Icathia, 20),
              "3.6.0 upgrade: can re-pick until max rank; non-upgradable stays unique");
        Check(State{}.OfferLevel(Tank) == 1 && t1.OfferLevel(Tank) == 2 && t2.OfferLevel(Tank) == 3, "3.6.0 upgrade: offer level = current + 1");
        State w; w.Init(41);
        Check(w.WeightOf(Tank) == 20 && t1.WeightOf(Tank) == 14 && t2.WeightOf(Tank) == 10 && lv(Blitz, 1).WeightOf(Blitz) == 2 && t1.WeightOf(Swift) == 5,
              "3.6.0 upgrade weights: base 20, Lv2 x0.7, Lv3 x0.5, blitz Lv2 very rare, engine rival 5");
        // 已见过的已持有海克斯仍能作为升级卡出现；选中后升级
        State up = lv(Tank, 1); up.seen |= 1ull << Tank;
        Check(up.Begin(5 + 5, 1ull << Tank) && up.offers[0] == Tank && up.Pick(0) && up.Rank(Tank) == 2, "3.6.0 upgrade: owned hex offered again and picking it raises the level");
        // 存档：固定栏位里重复（升级）可以读，超过最高级拒绝
        State fixed; fixed.Init(42); fixed.enabled = true;
        fixed.picks[0] = Tank; fixed.pickedAt[0] = 1; fixed.picks[1] = Tank; fixed.pickedAt[1] = 30; fixed.completed = 0b11;
        State fb; Check(State::Load(fixed.Save(), &fb) && fb.Rank(Tank) == 2, "3.6.0 upgrade: duplicate in fixed slots loads as Lv2");
        fixed.extra = {Tank, Tank}; fixed.extraAt = {5, 10}; fixed.extraBase = {0, 0};
        Check(!State::Load(fixed.Save(), &fb), "3.6.0 upgrade: more copies than max rank is rejected as corrupt");
        State icx; icx.Init(43); icx.enabled = true; icx.picks[0] = Icathia; icx.pickedAt[0] = 1; icx.picks[1] = Icathia; icx.pickedAt[1] = 30; icx.completed = 0b11;
        Check(!State::Load(icx.Save(), &fb), "3.6.0 upgrade: non-upgradable duplicate still rejected");
        // 升级卡记在后面的层，叠层从最早那张算
        State late; late.Init(44); late.enabled = true; late.extra = {Battle}; late.extraAt = {5}; late.extraBase = {0};
        late.picks[1] = Battle; late.pickedAt[1] = 30; late.completed = 0b10;
        for (int f = 5; f <= 34; ++f) late.ClearFloor(f);
        Check(late.Levels(Battle) == 30 && late.PickedFloor(Battle) == 5 && near(late.Attack(), .04 * 30), "3.6.0 upgrade: layers from the earliest copy, Lv2 step 4%");
        // 其它分级数值
        Check(near(lv(Investor, 2).InvestorMul(), 2.5) && near(lv(Investor, 1).InvestorMul(), 2.0) && near(State{}.InvestorMul(), 1.0), "3.6.0 upgrade: investor x2 / x2.5");
        Check(near(lv(Interest, 2).InterestRate(), .15) && InterestOn(1000, 0, lv(Interest, 2).InterestRate()) == 150, "4.0.0 upgrade: hedge fund 15%");
        Check(MaxRank(Leech) == 1 && !Upgradable(Leech) && MaxRank(BloodPact) == 2 && lv(BloodPact, 2).LifeSteal() == 100 && lv(BloodPact, 1).LifeSteal() == 50 && lv(Leech, 3).LifeSteal() == 50,
              "4.0.0 leech single level +5%; blood pact Lv1 +5% / Lv2 +10% (old Lv3 copies capped)");
        Check(near(lv(FlyingKick, 1).KickRate(), .05) && near(lv(FlyingKick, 2).KickRate(), .10) && near(lv(FlyingKick, 3).KickRate(), .15), "3.6.0 upgrade: flying kick 5 / 10 / 15%");
        Check(near(lv(Thorns, 2).ThornsShare(), .22) && near(lv(Thorns, 3).ThornsShare(), .24) && near(ThornsDamage(10000, 10000, 1e6, .24), 240000), "3.6.0 upgrade: thorns 18 / 22 / 24%");
        Check(near(lv(Blitz, 2).BlitzSeconds(), 120) && lv(Blitz, 2).BlitzPicks() == 2 && lv(Blitz, 1).BlitzPicks() == 1, "3.6.0 upgrade: blitz Lv2 120 s, 2 picks");
        Check(lv(Grit, 2).GritCap() == 40 && lv(Grit, 1).GritCap() == 30 && near(lv(KillingIntent, 3).ReviveLine(), .60), "3.6.0 upgrade: grit cap 40, killing intent 60%");
        Check(lv(Leech, 2).LifeSteal() == 50 && lv(Bulwark, 2).Defense() == 350 && lv(Bell, 2).Defense() == 250, "bulwark / bell upgrades; 4.0.0 leech no longer upgrades");
        Check(near(lv(Sturdy, 3).Health(), 1.60) && near(lv(GlassCannon, 2).Attack(), 2.20) && near(lv(GlassCannon, 2).Health(), -.30) && near(lv(Desperate, 3).Break(), .50), "3.6.0 upgrade: sturdy / glass / desperate");
        Check(near(lv(Keen, 2).Attack(), .45) && near(lv(Keen, 2).Speed(), .30) && near(lv(Sharp, 2).Attack(), .80) && near(lv(Pursuit, 3).Break(), .80), "3.6.0 upgrade: keen / sharp / pursuit");
        State as2 = lv(Ascetic, 2); as2.asceticStacks = 5; State fl3 = lv(Flawless, 3); fl3.flawlessStacks = 4;
        Check(near(as2.Health(), .75) && near(fl3.Attack(), 1.20), "3.6.0 upgrade: ascetic / flawless stacks re-valued by the current level");
        State d2 = lv(Decapitate, 2); d2.extra.push_back(Sweep); d2.extraAt.push_back(50); d2.extraBase.push_back(0); d2.extra.push_back(Sweep); d2.extraAt.push_back(55); d2.extraBase.push_back(0);
        Check(near(d2.EnemyTaken(false), 1.5) && near(d2.EnemyTaken(true), 2.0), "3.6.0 upgrade: decapitate +50%, sweep +100%");
#ifndef PBZ_HEX_TEST
        Check(lv(Charmsmith, 2).CharmEvery() == 10 && lv(Charmsmith, 1).CharmEvery() == 15 && lv(Charmsmith, 2).CharmDue(20, 0), "3.6.0 upgrade: charmsmith every 10 floors");
#endif
        Check(lv(Prepared, 2).PreparedItems() == 2 && lv(MedSpark, 2).MedSparkPoints() == 2 && lv(SparkFlow, 3).SparkFlowPct() == 75, "3.6.0 upgrade: prepared / spark");
        Check(near(BackWaterOn(0.0, lv(BackWater, 2).BackWaterStep(), lv(BackWater, 2).BackWaterCap()), 2.45) && near(BackWaterOn(0.5), .75), "3.6.0 upgrade: back water +25% per 10%, max 245%");
        State sc = lv(Scientist, 2); bool saw = false;
        for (uint64_t seed = 0; seed < 50; ++seed) if (sc.ScientistForm(10, seed) == 2) { saw = true; Check(near(sc.ScientistBoost(10, seed).move, .75), "3.6.0 upgrade: scientist x1.25"); }
        Check(saw, "3.6.0 upgrade: scientist form exercised");
        State fu = lv(Fury, 2); fu.UpdateFury(true, 100, 1000); Check(near(fu.Attack(), 1.80) && near(fu.Break(), .50), "3.6.0 upgrade: fury Lv2");
        Check(State{}.ThrowerMul() == 1 && lv(Thrower, 1).ThrowerMul() == 2 && lv(Thrower, 2).ThrowerMul() == 3 && lv(Thrower, 2).ThrowerExtraPages() == 2 && lv(Thrower, 1).WeightOf(Thrower) == 2,
              "3.6.0 thrower Lv2: three picks per page, very rare upgrade card");
        Check(lv(ServantMaster, 1).CanPick(ServantMaster, 10) && !lv(ServantMaster, 3).CanPick(ServantMaster, 10) && lv(ServantMaster, 2).OfferLevel(ServantMaster) == 3, "3.6.0 servant master Lv1-3 series");
        State op = lv(Opening, 3); op.UpdateOpening(true, 15.9); Check(op.openingActive && near(op.OpeningSeconds(), 16), "3.6.0 upgrade: opening 16 s at Lv3");
    }
    {   // 3.6.2（用户：选了 Lv1 后老刷 Lv2；刷掉的卡再刷别的格子又刷回来）
        const Id owned[] = {Tank, Battle, Leech, Sharp, Bell, Sturdy, Keen, Pursuit, Bulwark, Desperate};
        auto make = [&](uint64_t seed) {
            State s; s.Init(seed);
            for (Id id : owned) { s.extra.push_back(id); s.extraAt.push_back(-1); s.extraBase.push_back(0); }
            return s;
        };
        auto upgrades = [](const State& s) { int n = 0; for (int id : s.offers) if (id >= 0 && s.Has(static_cast<Id>(id))) ++n; return n; };
        int pagesWith = 0, pages = 0, later = 0, laterPages = 0;
        for (uint64_t seed = 1; seed <= 400; ++seed) {
            State s = make(seed * 7919);
            if (!s.Begin(5)) continue;
            ++pages; pagesWith += upgrades(s) > 0;
            Check(s.Pick(0), "3.6.2 pick works");
            for (int f : {10, 15, 20}) { if (!s.Begin(f)) break; ++laterPages; later += upgrades(s) > 0; s.Pick(0); }
        }
        std::printf("info: 3.6.2 upgrade cards on first page %d/%d, later pages %d/%d\n", pagesWith, pages, later, laterPages);
        Check(pages > 300 && pagesWith < pages * 6 / 10 && later < laterPages * 6 / 10, "3.6.2 owned Lv1 hexes do not flood every page with their upgrades");
        int bad = 0, tried = 0;
        for (uint64_t seed = 1; seed <= 400; ++seed) {
            State s = make(seed * 104729);
            if (!s.Begin(5)) continue;
            std::vector<int> gone;
            for (int slot = 0; slot < 3; ++slot) {
                const int before = s.offers[slot];
                if (!s.Refresh(slot)) continue;
                ++tried; gone.push_back(before);
                for (int g : gone) for (int x : s.offers) bad += x == g;
            }
        }
        Check(tried > 1000 && bad == 0, "3.6.2 a card refreshed away never comes back on the same page");
        State tiny; tiny.Init(5);  // 池子只剩几张：刷掉的才允许回来（不卡死）
        const Mask only = (1ull << Tank) | (1ull << Swift) | (1ull << Leech) | (1ull << Ocean);
        if (tiny.Begin(5, only)) { int ok = 0; for (int slot = 0; slot < 3; ++slot) ok += tiny.Refresh(slot, only); Check(ok >= 1, "3.6.2 tiny pool: refresh still works by reusing refreshed cards"); }
        // 4.1.0（用户 2026-11-16）：卡池每类一张、按权重抽——拿了 Lv1 后它的 Lv2 只按较低权重留在池里，不刷屏
        int floods = 0, follow = 0, nextPage = 0;
        for (uint64_t seed = 1; seed <= 2000; ++seed) {
            State s; s.Init(seed * 31337);
            if (!s.Begin(5)) continue;
            const int took = s.offers[0];
            if (!s.Pick(0) || !Upgradable(took)) continue;
            int shown = 0, page = 0; ++follow;
            for (int f : {10, 15, 20, 25}) {
                if (!s.Begin(f)) break;
                bool here = false; for (int x : s.offers) here = here || x == took;
                shown += here; nextPage += here && page == 0; ++page;
                s.Pick(s.offers[0] == took && s.offers[1] >= 0 ? 1 : 0);
            }
            floods += shown >= 3;
        }
        std::printf("info: 4.1.0 after Lv1, its Lv2 on the next page %d/%d, in >=3 of next 4 pages %d\n", nextPage, follow, floods);
        Check(follow > 1000 && nextPage <= follow * 15 / 100 && floods <= follow / 100, "4.1.0 after taking Lv1, its Lv2 stays a low-weight card (no flooding)");
        {   // 4.1.0：整局（每 5 层一页，随机刷新一格、随机选）升级卡随拥有数慢慢增加，100 层时平均每页 ≤ 0.7 张、2 张以上的页 ≤ 12%
            int pages = 0, ups = 0, twoUp = 0;
            for (uint64_t seed = 1; seed <= 1000; ++seed) {
                State s; s.Init(seed * 6364136223846793005ull + 1);
                uint64_t r = seed * 0x9E3779B97F4A7C15ull;
                auto rnd = [&]() { r ^= r << 13; r ^= r >> 7; r ^= r << 17; return r; };
                for (int f = 1; f <= 100; ++f) {
                    if (!HexFloor(f) || !s.Begin(f)) continue;
                    if (rnd() % 2) s.Refresh(static_cast<int>(rnd() % 3));
                    int up = 0; for (int id : s.offers) up += id >= 0 && s.Has(static_cast<Id>(id));
                    if (f == 100) { ++pages; ups += up; twoUp += up >= 2; }
                    int slot = static_cast<int>(rnd() % 3); if (s.offers[slot] < 0) slot = 0;
                    if (!s.Pick(slot)) for (int k = 0; k < 3 && !s.Pick(k); ++k) {}
                }
            }
            std::printf("info: 4.1.0 floor-100 page: upgrade cards %.2f per page, >=2 on %.1f%% of pages\n", double(ups) / pages, 100.0 * twoUp / pages);
            Check(pages > 900 && ups <= pages * 7 / 10 && twoUp <= pages * 12 / 100, "4.1.0 late pages are not flooded with upgrade cards");
        }
        {   // 4.1.0（用户 2026-11-16）：出现过没选的卡后 3 次选择不出现；刷掉的卡后 4 次不出现；冷却过后还会回来
            int shownBack = 0, rerollBack = 0, rounds = 0, returned = 0;
            for (uint64_t seed = 1; seed <= 1500; ++seed) {
                State s; s.Init(seed * 2654435761ull + 7);
                if (!s.Begin(5)) continue;
                const int r0 = s.offers[1];
                if (!s.Refresh(1)) continue;
                std::vector<int> unpicked{s.offers[1], s.offers[2]};
                if (!s.Pick(0)) continue;
                ++rounds;
                int page = 0; bool back = false;
                for (int f : {10, 15, 20, 25, 30, 35, 40, 45, 50, 55}) {
                    if (!s.Begin(f)) break;
                    ++page;
                    for (int x : s.offers) {
                        if (page <= 3) for (int u : unpicked) shownBack += x == u;
                        if (page <= 4) rerollBack += x == r0;
                        back = back || (page > 4 && x == r0);
                    }
                    int k = 0; while (k < 3 && (s.offers[k] == r0 || std::find(unpicked.begin(), unpicked.end(), s.offers[k]) != unpicked.end())) ++k;
                    s.Pick(k < 3 ? k : 0);
                }
                returned += back;
            }
            std::printf("info: 4.1.0 cooldown: %d rounds, unpicked back within 3 picks %d, rerolled back within 4 picks %d, rerolled card returned later %d\n", rounds, shownBack, rerollBack, returned);
            Check(rounds > 1000 && shownBack == 0 && rerollBack == 0, "4.1.0 shown cards cool down 3 picks, refreshed-away cards 4 picks");
            Check(returned > rounds / 20, "4.1.0 cooled cards flow back into the pool");
            {   // 权重重构：拿走 / 冷却的卡不把份额让给同档剩下的卡——某档只剩 1 张时它的出现率不暴涨
                State a; a.Init(1);
                int legend = 0; for (int id = 0; id < Count; ++id) if ((kPlayableMask & Bit(id)) && TierOf(id) == 3) ++legend;
                const int lone = Tank;  // 传说档
                for (int id = 0; id < Count; ++id) if ((kPlayableMask & Bit(id)) && TierOf(id) == 3 && id != lone && id != Swift) a.cool[static_cast<size_t>(id)] = 3;
                int hits = 0, drawn = 0;
                for (int k = 0; k < 4000; ++k) { State t = a; t.rng = (k * 0x9E3779B97F4A7C15ull) | 1ull; if (!t.Begin(5)) continue; ++drawn; for (int x : t.offers) hits += x == lone; }
                std::printf("info: 4.1.0 lone legendary (%d in tier) shown on %.1f%% of pages\n", legend, 100.0 * hits / drawn);
                Check(drawn > 3000 && hits < drawn * 3 / 100, "4.1.0 a lone card left in a tier is not boosted to near-certain");
            }
            State tiny; tiny.Init(9);  // 池子太小：冷却中的卡仍可补位，不出空页
            const Mask only = (1ull << Tank) | (1ull << Swift) | (1ull << Leech) | (1ull << Ocean);
            int full = 0; for (int f : {5, 10, 15}) { if (!tiny.Begin(f, only)) break; full += tiny.offers[2] >= 0; tiny.Pick(0); }
            Check(full == 3, "4.1.0 tiny pool: cooling cards still fill the page");
        }
        bool tracked = false;  // 3.7.0：种子相关（加了连跳后 77 号种子那页没有升级卡）→ 找一页有升级卡的
        for (uint64_t k = 70; k < 400 && !tracked; ++k) {
            State saved = make(k); if (!saved.Begin(5)) continue; saved.Refresh(0);
            tracked = saved.away != Mask{} && saved.seenUp != Mask{};
        }
        Check(tracked, "3.6.2 page tracks shown upgrades and refreshed-away cards");
    }
    std::puts("TOWER_HEX_OK");
}
