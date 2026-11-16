// 3.1.0 驿站【交易】离线测试：回收 / 以旧换新 / 熔铸的规则、所得、武器池移除、存读档
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include "../src/tower_trade.h"
#include "../src/tower_growth_run.h"
#include "../src/tower_rewards.h"
static int g_checks = 0;
static void Check(bool ok, const char* why) { ++g_checks; if (!ok) { std::printf("FAIL %s\n", why); std::exit(1); } }
using namespace tower;
int main() {
    namespace td = trade;
    auto W = [](const char* id, int kind) { return growth::WeaponIndex(id, kind); };
    growth::Profile p = growth::Profile::Kit();
    const int tang = W("Tang", 0), zhanma = W("ZhanMaBlade", 0), bow = W("UnnamedBow", 1), axe = W("BrutalAxe", 1);
    Check(tang >= 0 && zhanma >= 0 && bow >= 0 && axe >= 0, "kit weapons resolve");
    int third = -1; for (auto& w : p.weapons) if (growth::kWeapons[w.index].kind == 0 && w.index != tang && w.index != zhanma) third = w.index;
    Check(third >= 0, "kit has a third main weapon");
    p.weapons[p.Find(third)].level = 6; p.weapons[p.Find(third)].snapped = true; p.weapons[p.Find(third)].spent = 5;
    const std::vector<int> equipped{tang, zhanma, bow, axe};
    // 规则
    Check(td::GiveBlocked(p, tang, equipped).empty() || td::KindCount(p, 0) <= 1, "N7: equipped weapons can be traded too");
    Check(td::GiveBlocked(p, third, equipped).empty(), "a spare weapon can be traded");
    Check(!td::GiveBlocked(p, bow, {}).empty() || td::KindCount(p, 1) > 1, "last weapon of a kind is protected");
    // 回收：等级 6 → 返还 5 + 1；魂玉 80%；铸过魂返还铸魂令
    { auto q = p; const int t0 = q.temper, s0 = q.soulTokens;
      const auto g = td::ApplyRecycle(q, third, 1000);
      Check(g.temper == 3 && g.jade == 800 && g.soul == 1 && q.temper == t0 + 3 && q.soulTokens == s0 + 1 && q.Find(third) < 0, "3.4.0 recycle refunds level/2, 80% jade, soul token");
      Check(!q.Banned(third) && q.AddWeapon(third), "recycled weapons return to the pool"); }
    {   // 3.4.0（用户）：淬炼大师 5 点升到 11 级 → 回收退 2 + 1；游戏背包认领、带等级进塔（没花点）→ 只有 1；旧档没记花费按等级倒推再减半
        auto q = p; const int L = W("Long", 0);
        q.weapons.push_back({L, 11, false, 5});
        Check(td::RecycleGain(q, L, 1000).temper == 5, "master: 5 points to Lv11 recycles for 5 (no loss)");
        q.weapons[q.Find(L)].spent = 10; Check(td::RecycleGain(q, L, 1000).temper == 5, "no master: 10 points to Lv11 recycles for 5 (half)");
        q.weapons[q.Find(L)].spent = 7; Check(td::RecycleGain(q, L, 1000).temper == 5, "master picked midway: still (level-1)/2");
        q.weapons[q.Find(L)].spent = 0; Check(td::RecycleGain(q, L, 1000).temper == 1, "adopted Lv11 with nothing spent counts as Lv1");
        q.weapons[q.Find(L)].level = 60; q.weapons[q.Find(L)].spent = 30; Check(td::RecycleGain(q, L, 1000).temper == 30, "forge master Lv60 recycles for 30");
        q.weapons[q.Find(L)].level = 30; q.weapons[q.Find(L)].spent = 29; Check(td::RecycleGain(q, L, 1000).temper == 15, "Lv30 recycles for 15");
        q.weapons[q.Find(L)].level = 1; q.weapons[q.Find(L)].spent = 0; Check(td::RecycleGain(q, L, 1000).temper == 1, "Lv1 still gives 1");
        { auto m = q; const int G = W("GuiTouBlade", 0); m.weapons[m.Find(L)].level = 3; m.weapons[m.Find(L)].spent = 2; m.weapons.push_back({G, 3, false, 2});
          Check(td::RecycleManyGain(m, {L, G}, 1000).temper == 3 && td::RecycleGain(m, L, 1000).temper == 1, "batch: levels summed first, then halved"); }
        { auto m = q; m.weapons[m.Find(L)].level = 60; m.weapons[m.Find(L)].spent = 30; std::vector<int> ws{L};
          for (const char* id : {"GuiTouBlade", "ChiZheCanJian", "SnakeDual"}) { const int x = W(id, 0); if (m.Find(x) < 0) m.weapons.push_back({x, 1, false, 0}); ws.push_back(x); }
          Check(td::RecycleManyGain(m, ws, 1000).temper == 30 + 3, "batch: Lv60 + three Lv1 = 30 + 1 each"); }
        q.weapons[q.Find(L)].level = 11; q.weapons[q.Find(L)].spent = -1; q.temperMaster = true; Check(td::RecycleGain(q, L, 1000).temper == 5, "old save without spent record: (level-1)/2");
        growth::Profile r = growth::Profile::Kit(); r.temper = 5; r.temperMaster = true; const int t = third;
        for (int i = 0; i < 5; ++i) r.Upgrade(t, 60);
        Check(r.weapons[r.Find(t)].level == 11 && r.weapons[r.Find(t)].spent == 5, "upgrade records points actually spent");
        Check(r.Downgrade(t, 6) == growth::Err::Ok && r.temper == 2 && r.weapons[r.Find(t)].spent == 3, "downgrade refunds spent points proportionally");
        growth::Profile back; Check(growth::Profile::Load(r.Save(), &back, 60) && back.weapons[back.Find(t)].spent == 3, "spent survives save/load"); }
    // N9i：药 / 饰品也能回收，按驿站同类价 80%，饰品退打磨点
    { auto q = p; const td::Prices pr{1000, 500, 600};
      if (!q.medicines.empty()) {
        const int m = td::kMedCode + q.medicines[0];
        Check(td::RecycleMixBlocked(q, {m}, {}).empty() && td::RecycleMixGain(q, {m}, pr).jade == 400, "N9i: medicine recycles for 80% of its price");
        const int j0 = q.temper; const auto g = td::ApplyRecycleMix(q, {m}, pr);
        Check(g.jade == 400 && q.temper == j0 && !td::Owns(q, m), "N9i: medicine removed after recycle");
        Check(!td::RecycleMixBlocked(q, {m}, {}).empty(), "N9i: can't recycle a medicine twice");
      }
      q.accessories.push_back({0, 3});
      const int a = td::kAccCode;
      const auto g = td::RecycleMixGain(q, {a, third}, pr);
      Check(g.jade == 480 + 800 && g.temper == 1 + 3, "N9i: accessory 80% + 1 temper whatever its level, mixed with weapon");
      td::ApplyRecycleMix(q, {a, third}, pr);
      Check(!td::Owns(q, a) && !td::Owns(q, third), "N9i: mixed recycle removes both"); }
    { growthrun::Runner r; r.profile = p; r.backpackSynced = true; growthrun::Runner back;
      Check(growthrun::Runner::Load(r.Save(), &back) && back.backpackSynced, "3.3.1: backpack sync flag survives save/load");
      growthrun::Runner fresh; fresh.profile = p; growthrun::Runner back2;
      Check(growthrun::Runner::Load(fresh.Save(), &back2) && !back2.backpackSynced, "3.3.1: old saves start unsynced (one chance)"); }
    // N9i：回收的东西流回下一次驿站（存读档保留）
    { growthrun::Runner r; r.profile = p; r.recycled = {third, td::kMedCode + 1};
      growthrun::Runner back; Check(growthrun::Runner::Load(r.Save(), &back) && back.recycled == r.recycled, "N9i: recycled list survives save/load");
      Check(growthrun::Runner::CodeOffer(td::kMedCode + 1).kind == rewards::Kind::Medicine && growthrun::Runner::CodeOffer(third).index == third, "N9i: codes map to shop offers"); }
    // 熔铸：牺牲品移除且进 banned；强化 +15% 一次，最多 10 次；15 把送铸魂令
    { auto q = p; const auto g = td::ApplyMelt(q, tang, third);
      Check(q.Find(third) < 0 && q.Banned(third) && !q.AddWeapon(third), "melted weapon is removed from the pool for good");
      Check(q.MeltCount(tang) == 1 && std::abs(td::MeltMul(1) - 1.15) < 1e-9 && g.temper == 5, "melt: +15% once, levels refunded");
      { auto r = q; r.AddWeapon(third == tang ? bow : third);  // N9g：熔铸过的也能交出（同类还有别的），交出后熔铸记录清掉
        if (td::KindCount(r, growth::kWeapons[tang].kind) > 1) { Check(td::GiveBlocked(r, tang, {}).empty(), "N9g: a melt-strengthened weapon can be traded"); td::Remove(r, tang); Check(r.MeltCount(tang) == 0, "N9g: removing clears melt record"); } }
      rewards::Context c; c.profile = &q;
      Check(rewards::detail::Owned(q, rewards::Cat::Main, third), "banned weapon counts as owned for every offer pool");
      // 存读档
      growth::Profile back; Check(growth::Profile::Load(q.Save(), &back) && back.Banned(third) && back.MeltCount(tang) == 1 && back.meltTotal == 1, "melt record survives save/load");
      for (int i = 0; i < 20; ++i) for (auto& o : q.melt) if (o.first == tang) o.second = std::min(td::kMeltMax, o.second + 1);
      Check(!td::MeltTargetBlocked(q, tang).empty() && td::MeltMul(q.MeltCount(tang)) == 1.0 + 0.15 * 10, "melt caps at 10 times (+150%)");
      q.meltTotal = 14; q.weapons.push_back({W("Long", 0), 1, false, 0});
      const int soul = q.soulTokens; td::ApplyMelt(q, zhanma, W("Long", 0));
      Check(q.meltTotal == 15 && q.soulTokens == soul + 1, "every 15 melts give a soul token"); }
    // 以旧换新
    { auto q = p; q.weapons.push_back({W("Long", 0), 3, false, 2});
      const int want = W("GuiTouBlade", 0);
      const auto g = td::ApplyExchange(q, third, W("Long", 0), want);
      Check(q.Find(third) < 0 && q.Find(W("Long", 0)) < 0 && q.Find(want) >= 0 && g.temper == 5 + 2 && g.soul == 1, "exchange: two out, one in, levels refunded"); }
    // 每次驿站各限 1 次
    { td::Visit v; v.Mark(7, td::Recycle);
      Check(v.Used(7, td::Recycle) && !v.Used(7, td::Melt) && !v.Used(8, td::Recycle), "one trade of each kind per shop visit");
      growthrun::Runner r; r.on = true; r.tradeVisit = v; growthrun::Runner back;
      Check(growthrun::Runner::Load(r.Save(), &back) && back.tradeVisit.Used(7, td::Recycle), "per-visit trade use survives save/load"); }
    {   // N8 批量回收 / 批量熔铸
        auto q = p; const int L = W("Long", 0), G = W("GuiTouBlade", 0);
        q.weapons.push_back({L, 3, false, 2}); q.weapons.push_back({G, 1, false, 0});
        const std::vector<int> three{third, L, G};
        Check(td::GiveManyBlocked(q, three, equipped).empty(), "recycle many: spare weapons can all go");
        const auto pre = td::RecycleManyGain(q, three, 1000);
        Check(pre.temper == (6 + 3) / 2 + 1 && pre.jade == 2400 && pre.soul == 1, "recycle many: refunds and jade add up");
        const auto got = td::ApplyRecycleMany(q, three, 1000);
        Check(got.temper == pre.temper && got.jade == pre.jade && q.Find(third) < 0 && q.Find(L) < 0 && q.Find(G) < 0, "recycle many: applied equals preview");
        int mains = td::KindCount(q, 0); std::vector<int> all; for (auto& w : q.weapons) if (growth::kWeapons[w.index].kind == 0) all.push_back(w.index);
        Check(mains >= 1 && !td::GiveManyBlocked(q, all, {}).empty(), "recycle many: can't hand in every weapon of a kind");
        Check(!td::GiveManyBlocked(q, {all[0], all[0]}, {}).empty() || all.size() < 2, "recycle many: no duplicates");
        auto m = p; m.weapons.push_back({L, 2, false, 1}); m.weapons.push_back({G, 1, false, 0});
        Check(td::MeltManyBlocked(m, tang, {third, L, G}, {}).empty(), "melt many: three sacrifices into one");
        const auto mg = td::MeltManyGain(m, tang, {third, L, G});
        td::ApplyMeltMany(m, tang, {third, L, G});
        Check(m.MeltCount(tang) == 3 && m.Banned(third) && m.Banned(L) && m.Banned(G) && m.meltTotal == 3 && mg.temper == 5 + 1, "melt many: +3 melts, all banned, refunds add up");
        for (auto& o : m.melt) if (o.first == tang) o.second = 9;
        m.weapons.push_back({W("Long", 0) == L ? W("ZhanMaBlade", 0) : L, 1, false, 0});
        Check(td::MeltRoom(m, tang) == 1 && !td::MeltManyBlocked(m, tang, {zhanma, bow}, {}).empty(), "melt many: can't pass 10 melts");
        Check(!td::MeltManyBlocked(m, tang, {tang}, {}).empty(), "melt many: can't melt into itself");
    }
    std::printf("TOWER_TRADE_OK %d checks\n", g_checks);
}
