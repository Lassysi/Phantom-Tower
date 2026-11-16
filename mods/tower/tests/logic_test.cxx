// 影塔纯逻辑离线测试：不碰游戏。run.cmd 编译运行，全部通过打印 TOWER_LOGIC_OK。
#include <cstdio>
#include <numeric>
#include <set>

#include "../src/boss_pool.generated.h"
#include "../src/tower_logic.h"
#include "../src/tower_ui_logic.h"

static int g_checks = 0, g_failed = 0;
static void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_failed; std::printf("FAIL %s\n", what); }
}

int main() {
    using namespace tower;
    // 随机数：同种子同序列，不同种子不同序列
    { Rng a(42), b(42), c(43); bool same = true, diff = false;
      for (int i = 0; i < 100; ++i) { auto x = a.Next(), y = b.Next(), z = c.Next(); same &= x == y; diff |= x != z; }
      Check(same && diff, "rng is deterministic per seed"); }
    // 3 选 1：三张互不相同，且都是有效卡
    { Rng r(7); bool ok = true;
      for (int i = 0; i < 2000; ++i) { auto o = Draw(r); ok &= o[0] >= 0 && o[1] >= 0 && o[2] >= 0 && o[0] != o[1] && o[1] != o[2] && o[0] != o[2] && o[2] < kCardCount; }
      Check(ok, "draw gives three distinct cards"); }
    // 权重：凡卡远多于天卡
    { Rng r(815); bool allowed = true;
      for (int i = 0; i < 5000; ++i) {
        for (int c : Draw(r)) allowed &= CardInRewardPool(c);
        for (int z = 0; z < 5; ++z) for (int min = Fan; min <= Tian; ++min)
          allowed &= CardInRewardPool(DrawCard(r, z, min));
        const int item = DrawItem(r);
        allowed &= ItemInRewardPool(item);
      }
      Check(allowed, "all reward draws exclude floor-heal and unsupported mode items");
      Check(!CardInRewardPool(7) && kCards[7].id == "heal_1" && kCards[8].id == "shachi_1", "retired card preserves legacy save indexes");
      Check(!ItemInRewardPool(static_cast<int>(Item::Intel)) && !ItemInRewardPool(static_cast<int>(Item::Borrow)) && ItemInRewardPool(static_cast<int>(Item::Revive)), "unusable items excluded without shifting saved bag indexes"); }
    Check(!ItemInRewardPool(static_cast<int>(Item::Heal)), "instant heal consumable is not a clear reward");
    { Rng r(9); int fan = 0, tian = 0;
      for (int i = 0; i < 5000; ++i) for (int k : Draw(r)) (kCards[k].rarity == Fan ? fan : kCards[k].rarity == Tian ? tian : fan) += kCards[k].rarity == Fan || kCards[k].rarity == Tian;
      Check(fan > tian * 8, "common cards far outnumber heavenly ones"); }
    // 加成叠算与上下限
    { Mods m; for (int i = 0; i < 30; ++i) m.Add(kCards[3]); Check(m.TakenMul() == 0.2, "damage taken floors at 0.2");
      Mods d; d.Add(kCards[0]); d.Add(kCards[1]); Check(std::abs(d.DealtMul() - 1.40) < 1e-9, "same-type bonuses add");
      Mods s; for (int i = 0; i < 20; ++i) s.Add(kCards[10]); Check(s.SpeedMul() == 1.6, "attack speed caps at 1.6"); }
    // 战力目标
    { Tuning t; Check(TargetHp(t, 1) == t.hp0, "floor 1 target is hp0");
      Check(TargetHp(t, 5) > TargetHp(t, 4), "target hp grows per floor");
      Check(std::abs(HpScale(10000, 25000) - 2.5) < 1e-9, "scale is target over native");
      Check(HpScale(0, 25000) == 1.0, "unknown native hp is not scaled");
      Check(BossDmgMul(t, 1) == 1.0 && std::abs(BossDmgMul(t, 11) - 1.3) < 1e-9, "boss damage grows 3% per floor (50-floor plan, acceptance defaults)"); }
    // Boss 池与选择
    { const int n = static_cast<int>(std::size(kBossPool));
      Check(n >= 30, "boss pool has enough bosses");
      bool sorted = true; for (int i = 1; i < n; ++i) sorted &= kBossPool[i - 1].hp <= kBossPool[i].hp;
      Check(sorted, "pool sorted by base hp");
      Rng r(5); std::vector<int> used; std::set<int> seen; bool ok = true;
      for (int f = 1; f <= 50; ++f) { int b = PickBoss(r, f, 50, n, used); ok &= b >= 0 && b < n; if (f <= n) ok &= !seen.count(b); seen.insert(b); used.push_back(b); }
      Check(ok, "50 floors pick valid bosses without repeats while pool lasts");
      Rng r2(5); std::vector<int> none; int early = 0, late = 0;
      for (int i = 0; i < 200; ++i) { early += PickBoss(r2, 1, 10, n, none); late += PickBoss(r2, 10, 10, n, none); }
      Check(late > early, "later floors draw from the stronger end"); }
    // 状态机
    { Run run; run.Start(1, 3);
      Check(run.phase == Phase::Ready && run.floor == 0, "start waits for first floor");
      Check(!run.Pick(0), "cannot pick before a reward");
      int b = run.NextFloor(10); Check(b == 1 && run.bosses.size() == 1 && run.phase == Phase::Fighting && run.floor == 1, "next floor starts a fight");
      Check(run.NextFloor(10) == 0, "cannot open a floor during a fight");
      run.FloorCleared({}); Check(run.phase == Phase::Reward && run.offer[0].kind == OfferKind::Card && run.offer[0].index >= 0 && run.offer[1].kind == OfferKind::Item && run.offer[2].kind == OfferKind::Jade && run.offer[2].amount > 0, "clear offers a card, an item and a jade bag");
      Check(run.Pick(0) && run.picked.size() == 1 && run.phase == Phase::Ready, "pick applies card and waits");
      run.NextFloor(10); run.FloorCleared({}); run.Pick(0); run.NextFloor(10); run.FloorCleared({});
      Check(run.phase == Phase::Won && run.floor == 3, "last floor cleared wins");
      { Run duo; duo.Start(7, 3); duo.NextFloor(40); duo.FloorCleared({}); duo.Pick(0);  // 第 2 层 1v2：陪打满足条件
        Check(duo.NextFloor(40, [](int, int c) { return c % 2 == 0; }) == 2 && duo.bosses[1] % 2 == 0, "companion filter applies to the second boss");
        Run none; none.Start(7, 3); none.NextFloor(40); none.FloorCleared({}); none.Pick(0);
        Check(none.NextFloor(40, [](int, int) { return false; }) == 2 && none.bosses.size() == 2, "impossible companion filter falls back instead of failing"); }
      { Run solo; solo.Start(9, 3);  // 主 Boss 条件：不满足的不抽
        bool all = true;
        for (int f = 0; f < 3 && solo.phase != Phase::Won; ++f) {
          solo.NextFloor(40, {}, [](int c) { return c >= 20; });
          for (int b : solo.bosses) all &= b >= 20;
          solo.FloorCleared({}); solo.Pick(0);
        }
        Check(all, "lead filter excludes bosses (e.g. scene bosses without a marked native site)"); }
      Run lose; lose.Start(2, 3); lose.NextFloor(10); lose.Died(); Check(lose.phase == Phase::Lost, "death during a fight loses");
      Run same1, same2; same1.Start(99, 5); same2.Start(99, 5); same1.NextFloor(40); same2.NextFloor(40);
      Check(same1.bosses == same2.bosses, "same seed same first boss");
      Run undo; undo.Start(3, 5); undo.NextFloor(40); undo.UndoFloor();
      Check(undo.phase == Phase::Ready && undo.floor == 0 && undo.usedBosses.empty(), "failed spawn rolls the floor back"); }
    // 多 Boss 层
    { int duo = 0, trio = 0;
      for (int f = 1; f <= 50; ++f) { int c = BossCount(f, 50); duo += c == 2; trio += c == 3; }
      Check(duo == 4 && trio == 2, "50 floors have four 1v2 and two 1v3");
      Check(BossCount(43, 50) == 3 && BossCount(49, 50) == 3 && BossCount(33, 50) == 2 && BossCount(43, 40) != 3, "1v3 only on floors 43 and 49 of a full run");
      Check(ShareHp(3) * 3 < ShareHp(2) * 2 && ShareDmg(3) < ShareDmg(2), "1v3 has less total hp than 1v2 and weaker hits");
      Check(EntryDelay(3, 0) == 0 && EntryDelay(3, 2) > EntryDelay(3, 1) && EntryDelay(2, 1) == 0, "1v3 bosses enter staggered");
      Check(BossCount(2, 3) == 2 && BossCount(1, 3) == 1 && BossCount(3, 3) == 1, "short run shows one 1v2 on floor 2");
      Check(ShareHp(2) * 2 < 1.5 && ShareHp(3) * 3 < 1.6 && ShareDmg(3) < ShareDmg(2), "multi-boss floors stay easy");
      Run r; r.Start(11, 3); r.NextFloor(40); r.FloorCleared({}); r.Pick(0); r.NextFloor(40);
      Check(r.bosses.size() == 2 && r.bosses[0] != r.bosses[1], "1v2 floor spawns two different bosses");
      Check(EasierFloor(13, 3) < 13 && EasierFloor(13, 1) == 13, "multi-boss floors draw weaker bosses"); }
    // 经济：魂玉、道具、兵器必掉、神通极稀有、存档
    { Check(JadeFor(1, false, 0.5) == 50 && JadeFor(10, true, 0.5) == 261 && JadeFor(1, false, 0.0) == 42, "3.3.3 jade: boss 1.5 J(s) +-15%, zone boss x2");
      Run random; random.Start(42, 50);
      const std::string_view ahua = "/PBZCombatAsset/ActorData/Mobs/042_AHua/BP_Monster_AHua_GuiTouDao.BP_Monster_AHua_GuiTouDao_C";
      random.RecordShopDrops({ahua, ahua});
      random.floor = 3; random.RecordClearedRoute({ahua});
      Run restored; Check(random.shuffleBosses && random.shopDrops.size() == 1 && kWeaponDrops[random.shopDrops[0]].weapon == "GuiTouBlade" &&
                          restored.Load(random.Save()) && restored.shuffleBosses && restored.shopDrops == random.shopDrops,
                          "random shop: only recorded defeated boss drops, deduplicated and persisted");
      random.RecordClearedRoute({"/Game/another.Boss_C"});
      Check(restored.clearedRoute[3].size() == 1 && restored.clearedRoute[3][0] == ahua && random.clearedRoute[3][0] == ahua,
            "previously defeated actual boss survives save and cannot be overwritten by a later retry");
      Check(ZoneOf(1, 50) == 0 && ZoneOf(10, 50) == 0 && ZoneOf(11, 50) == 1 && ZoneOf(50, 50) == 4 && ZoneOf(3, 3) == 0, "zones are ten floors each");
      Run r; r.Start(5, 3); r.NextFloor(40);
      r.FloorCleared({"/Game/RenewalContent/Actors/Mobs/Normal/BP_Monster_PengMeng_Axe.BP_Monster_PengMeng_Axe_C", "/Game/x/BP_Monster_Nobody.BP_Monster_Nobody_C"});
      Check(r.last.weapons.size() == 1 && kWeaponDrops[r.weapons[0]].weapon == "BrutalAxe", "a boss with a weapon always drops it");
      Check(r.jade == r.last.jade && r.jade > 0, "clearing a floor gives jade");
      r.Pick(2); Check(r.jade > r.last.jade, "jade bag adds jade");
      r.NextFloor(40); r.FloorCleared({"/a/BP_Monster_LeiDang.BP_Monster_LeiDang_C", "/a/BP_Monster_LeiDang_P2.BP_Monster_LeiDang_P2_C"});
      Check(r.weapons.size() == 2, "phase two of the same boss does not drop the weapon twice");
      Run it; it.Start(6, 5); it.NextFloor(40); it.FloorCleared({}); it.Pick(1);
      Check(std::accumulate(it.items.begin(), it.items.end(), 0) == 1, "item pick goes to the bag");
      it.items[static_cast<int>(Item::Reroll)] = 1; it.NextFloor(40); it.FloorCleared({});
      Check(it.Reroll() && !it.Reroll(), "reroll consumes one token");
      it.items[static_cast<int>(Item::Revive)] = 1; it.Pick(0); it.NextFloor(40);
      Check(it.TryRevive() && !it.TryRevive(), "revive charm works once");
      // 神通极稀有：灵犀只在 30/40/50 层、每局最多 1 个
      Check(ShentongChance(Tier::Lingxi, 29, 50) == 0 && ShentongChance(Tier::Lingxi, 20, 50) == 0 && ShentongChance(Tier::Lingxi, 30, 50) == 0.01, "lingxi only on floors 30/40/50 at 1%");
      Check(ShentongChance(Tier::Normal, 7, 50) == 0.005 && ShentongChance(Tier::Rare, 7, 50) == 0.001, "normal and rare shentong are rare");
      int lingxi = 0, any = 0, runs = 2000;
      for (int k = 1; k <= runs; ++k) {
          Run x; x.Start(1000 + k, 50); int got = 0;
          for (int f = 1; f <= 50; ++f) { x.NextFloor(86); x.FloorCleared({}); if (x.phase == Phase::Reward) x.Pick(2); }
          for (int s2 : x.shentong) { ++any; got += kShentong[s2].tier == Tier::Lingxi; }
          Check(got <= 1, "at most one lingxi per run");
          lingxi += got;
      }
      Check(lingxi * 100 < runs * 6, "lingxi appears in well under 6% of full runs");
      Check(any * 2 < runs, "on average under half a shentong per full run");
      // 存档往返
      Run a; a.Start(77, 50); a.id = "abc123"; a.NextFloor(86); a.FloorCleared({"/a/BP_Monster_Yuu.BP_Monster_Yuu_C"}); a.items[0] = 2; a.shentong = {1};
      Run b; Check(b.Load(a.Save()), "saved run loads");
      Check(b.id == "abc123" && b.seed == a.seed && b.floor == a.floor && b.jade == a.jade && b.items == a.items && b.shentong == a.shentong && b.weapons == a.weapons
            && b.phase == Phase::Reward && b.offer[0].index == a.offer[0].index && b.rng.s == a.rng.s, "save keeps the whole run");
      Run c; Check(!c.Load("garbage") && !c.Load("v=1\nseed=0\n"), "bad save is rejected"); }
    { Run old; old.Start(77, 50); old.NextFloor(86); old.FloorCleared({});
      old.offer = {{{OfferKind::Card, 7, 0}, {OfferKind::Item, static_cast<int>(Item::Borrow), 1}, {OfferKind::Jade, -1, 51}}};
      old.items[static_cast<int>(Item::Intel)] = 2; old.items[static_cast<int>(Item::Revive)] = 1;
      Run migrated; Check(migrated.Load(old.Save()), "old pending reward still loads");
      Check(CardInRewardPool(migrated.offer[0].index) && ItemInRewardPool(migrated.offer[1].index) && migrated.offer[2].amount == 51 && migrated.items == old.items, "retired pending slots replaced without changing jade or bag ownership"); }
    // 平衡：加成补偿、自适应
    { Mods none; Check(BuffHpComp(none) == 1.0 && BuffDmgComp(none) == 1.0, "no bonus, no compensation");
      Mods dps; dps.Add(kCards[0]); dps.Add(kCards[1]);  // 伤害 +40%
      Check(BuffHpComp(dps) > 1.2 && BuffHpComp(dps) < dps.DealtMul(), "damage bonus raises boss hp but not fully");
      Mods tank; tank.Add(kCards[3]); tank.Add(kCards[5]);  // 减伤 15%、生命 +20%
      Check(BuffDmgComp(tank) > 1.2 && BuffDmgComp(tank) < tank.MaxHpMul() / tank.TakenMul(), "defense bonus raises boss damage but not fully");
      Check(NextAdapt(1.0, {30, 0.5}) > 1.0 && NextAdapt(1.0, {90, 0.1}) > 1.0, "easy floor makes the next harder");
      Check(NextAdapt(1.0, {200, 0.5}) < 1.0 && NextAdapt(1.0, {90, 0.9}) < 1.0, "close call makes the next easier");
      Check(NextAdapt(1.0, {90, 0.5}) == 1.0, "a hard-won floor keeps the difficulty");
      double a = 1.0; for (int i = 0; i < 50; ++i) a = NextAdapt(a, {10, 0.0}); Check(a == 2.0, "adapt caps at 2");
      a = 1.0; for (int i = 0; i < 50; ++i) a = NextAdapt(a, {300, 0.95}); Check(a == 0.6, "adapt floors at 0.6");
      Run r; r.Start(3, 50); r.NextFloor(86); r.mods = dps; r.adapt = 1.3;
      Check(std::abs(r.BossHpTarget(1) - TargetHp(r.tuning, 1) * 1.3 * BuffHpComp(dps)) < 1e-6, "boss hp target combines curve, adapt and compensation");
      Run s; Check(s.Load(r.Save()) && std::abs(s.adapt - 1.3) < 1e-6, "adapt survives the run save");
      Check(s.norm[0] == 0 && s.norm[1] == 0 && s.norm[2] == 0, "fresh run has no start normalization yet");
      r.norm[0] = 850.0 / 3200.0; r.norm[1] = 1; r.norm[2] = 0.5;
      Run n; Check(n.Load(r.Save()) && std::abs(n.norm[0] - 850.0 / 3200.0) < 1e-5 && n.norm[1] == 1 && std::abs(n.norm[2] - 0.5) < 1e-6, "start normalization survives the run save");
      Run fresh; fresh.Start(9, 50); Check(fresh.norm[0] == 0, "new run resets start normalization"); }
    // 统一起点
    { Standard st; Check(st.attack == 140 && st.shachiAttack == 140 && st.maxHp == 900 && st.shachiMax == 440 && st.infliction == 1050, "standard line matches the user's choice");
      Tuning t; Check(t.hp0 == 5000 && std::abs(TargetHp(t, 50) - 5000 * std::pow(1.05, 49)) < 1e-6, "acceptance HP curve: 5000 on floor 1, +5% per floor");
      Run a, b; a.Start(1, 3); b.Start(2, 3); Check(a.standard.attack == b.standard.attack && a.standard.maxHp == b.standard.maxHp, "every run starts from the same line"); }
    // 钟菜单回归（2026-10-31 实机空接收者）：先开钟菜单、还没弹过对话框时，文本库默认对象按需取到；
    // 取不到就返回空（调用方放弃），不会拿空对象调函数；失效后重新取；同一实例两个激活回调只排一次
    {
        int finds = 0;
        uint8_t fake = 0;
        bool alive = true, present = false;
        tower::ui::Lazy<uint8_t> text;
        auto find = [&]() -> uint8_t* { ++finds; return present ? &fake : nullptr; };
        auto live = [&](uint8_t*) { return alive; };
        Check(text.Get(find, live) == nullptr && finds == 1, "bell first: text object missing -> null, caller bails out");
        present = true;
        Check(text.Get(find, live) == &fake && finds == 2, "bell first: text object resolved on demand before any dialog");
        Check(text.Get(find, live) == &fake && finds == 2, "text object cached while alive");
        alive = false; present = false;
        Check(text.Get(find, live) == nullptr && finds == 3, "dead text object dropped and re-resolved");
        tower::ui::Once q;
        int menu = 0;
        Check(q.Add(&menu) && !q.Add(&menu), "two activation callbacks for one bell menu queue once");
        q.Done(&menu);
        Check(q.Add(&menu), "next open of the bell menu queues again");
    }

    if (g_failed) { std::printf("%d/%d failed\n", g_failed, g_checks); return 1; }
    { // 1.4.0：回退后重打的层不给任何奖励；最高通过层随局存档
      Run r; r.Start(7, 50); r.phase = Phase::Fighting; r.floor = 3;
      r.FloorCleared({});
      const int jade = r.jade;
      Check(r.peak == 3 && jade > 0 && r.phase == Phase::Reward, "first clear pays out and raises the high-water mark");
      r.phase = Phase::Fighting; r.floor = 2;
      r.FloorCleared({}, true);
      Check(r.jade == jade + r.last.jade && r.last.jade >= 1 && r.last.jade <= JadeFor(2, false, 1.0) / 4 + 1 && r.phase == Phase::Ready && r.peak == 3, "a replayed floor pays quarter jade and goes straight back to ready");
      Run s; Check(s.Load(r.Save()) && s.peak == 3, "high-water mark survives the run save");
      r.mobFloor = 4; r.mobWave = 2;
      Run t; Check(t.Load(r.Save()) && t.mobFloor == 4 && t.mobWave == 2, "mob wave progress survives the run save");
      Check(StageJade(10, false) == 84 && StageJade(10, true) == 21 && StageJade(1, true) >= 1, "wave layers pay increased jade, a quarter when replayed");
      r.shopFloor = 9;
      Run v; Check(v.Load(r.Save()) && v.shopFloor == 9, "the last shop floor survives the run save");
      Run d; d.Start(7, 50); d.phase = Phase::Fighting; d.floor = 1;
      d.FloorCleared({"/Game/RenewalContent/Actors/Mobs/Normal/BP_Monster_MobMan_Elite_Sword.BP_Monster_MobMan_Elite_Sword_C"}, false, true);
      Check(d.last.weapons.empty() && d.weapons.empty(), "growth runs: a beaten boss drops no weapon");
      Run u; u.Start(7, 50); u.phase = Phase::Fighting; u.floor = 6; u.inMobWave = true; u.UndoFloor();
      Check(u.floor == 6 && u.phase == Phase::Ready && !u.inMobWave, "cancelling a mob wave does not take a floor off the run"); }
    {   // 3.6.0 魂玉账单（方案第 5 条）：满血、积少成多 60%、钱生钱，200 → 1280，额外 1080
        auto b = tower::BillFor(99, 200, 1.0, 2.0, 0.6);
        Check(b.open && tower::BillTotal(b) == 1280 && tower::BillText(b) == "本层魂玉 200，额外获得 1080，共 1280", "3.6.0 bill: 200 x1.6 x2 x2 = 1280, extra 1080");
        b.picked = 100;  // 奖励页魂玉进账单
        Check(tower::BillTotal(b) == 1920 && tower::BillText(b) == "本层魂玉 200，额外获得 1720，共 1920", "3.6.0 bill: reward page jade takes every multiplier");
        Check(tower::BillTotal(tower::BillFor(99, 200, 0.0, 3.0, 0.0)) == 600, "3.6.0 bill: bounty x3 alone");
        Check(!tower::BillFor(9, 0, 1, 2, 1).open && tower::BillTotal(tower::BillFor(9, 0, 1, 2, 1)) == 0, "3.6.0 bill: nothing to settle without base jade");
        Check(tower::BillTotal(tower::BillFor(3, 101, 0.37, 1.0, 0.0)) == 138, "3.6.0 bill: partial bowl rounds like JadeTotal");
        Run r; r.Start(77, 50); r.NextFloor(40); r.FloorCleared({}); r.bill = b;
        Run back; Check(back.Load(r.Save()) && back.bill.open && back.bill.base == 200 && back.bill.picked == 100 && back.bill.mulPm == 2000 && back.bill.snowPm == 600 && back.bill.addPm == 1000,
                        "3.6.0 bill survives save / load");
        r.bill = {}; Run none; Check(none.Load(r.Save()) && !none.bill.open && r.Save().find("bill=") == std::string::npos, "3.6.0 no bill line when settled; old saves load without it");
        Run bad; Check(bad.Load(r.Save() + "bill=1,2,3\n") && !bad.bill.open, "3.6.0 malformed bill line ignored");
    }
    std::printf("TOWER_LOGIC_OK %d checks (no game process)\n", g_checks);
    return 0;
}
