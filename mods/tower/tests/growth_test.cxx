// 影塔塔内成长 / 进出塔流程离线测试（不碰游戏）。run.cmd 编译运行，全部通过打印 TOWER_GROWTH_OK。
#include <cstdio>
#include <map>
#include <set>
#include <string>

#include "../src/tower_growth.h"
#include "../src/tower_growth_text.h"
#include "../src/tower_session.h"
#include "../src/tower_growth_run.h"
#include "../src/tower_logic.h"

static int g_checks = 0, g_failed = 0;
static void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_failed; std::printf("FAIL %s\n", what); }
}

using namespace tower::growth;
namespace ses = tower::session;

// ---- 假的游戏端：内存里的"背包"，外加一份"磁盘存档"；沙盒关着时自动存档会把内存写到磁盘
struct FakeGame : ses::IGame {
    struct State {
        std::map<std::string, int> weapons;  // "kind:id" → 等级
        std::map<std::string, int> nodes, medLevel;
        std::set<std::string> meds, accs, snapped;
        bool operator==(const State&) const = default;
    } mem, disk;
    bool sandbox = false, achBlocked = false, readThrough = false, failBackup = false, failSandbox = false, failReload = false, failHighWeapon = false;
    int reloadPolls = -1;
    std::vector<std::string> calls;
    static std::string K(std::string_view id, int kind) { return std::to_string(kind) + ":" + std::string(id); }

    FakeGame() {  // 一个"老存档"：好几把兵器有等级，点过一些能力
        mem.weapons = {{K("Tang", 0), 12}, {K("HeiMoShang", 0), 3}, {K("ZhanMaBlade", 0), 10}, {K("BrutalAxe", 1), 10}, {K("UnnamedBow", 1), 1}};
        mem.nodes = {{"HeiShi1", 1}, {"HeiShi2", 1}, {"HeiShi8", 4}};
        mem.meds = {"Hulu", "Medicine_Recover"};
        mem.medLevel = {{"Hulu", 3}, {"Medicine_Recover", 2}};
        mem.accs = {"AC_Attack"};
        disk = mem;
    }
    void Autosave() { if (!sandbox) disk = mem; }  // 沙盒开着：写进沙盒，磁盘不变

    bool BackupSave(std::string* w) override { calls.push_back("backup"); *w = "bk"; return !failBackup; }
    bool Sandbox(bool on) override { calls.push_back(on ? "sandbox_on" : "sandbox_off"); if (failSandbox && on) return false; sandbox = on; if (!on) readThrough = false; return true; }
    bool SandboxReadThrough() override { calls.push_back("read_through"); readThrough = true; return sandbox; }
    void Achievements(bool b) override { calls.push_back(b ? "ach_block" : "ach_unblock"); achBlocked = b; }
    int WeaponLevel(std::string_view id, int kind) override { auto it = mem.weapons.find(K(id, kind)); return it == mem.weapons.end() ? -1 : it->second; }
    bool SetWeaponLevel(std::string_view id, int kind, int level) override { if (!mem.weapons.count(K(id, kind)) || (failHighWeapon && level > 30)) return false; mem.weapons[K(id, kind)] = level; return true; }
    bool GrantWeapon(std::string_view id, int kind) override { calls.push_back("grant:"+K(id,kind)); mem.weapons.emplace(K(id, kind), 1); return true; }
    bool SnapWeapon(std::string_view id, int kind) override { mem.snapped.insert(K(id, kind)); return true; }
    std::vector<std::string> OwnedWeapons(int kind) override {
        std::vector<std::string> v;
        for (auto& [k, l] : mem.weapons) if (k[0] - '0' == kind) v.push_back(k.substr(2));
        return v;
    }
    int NodeLevel(std::string_view id) override { auto it = mem.nodes.find(std::string(id)); return it == mem.nodes.end() ? 0 : it->second; }
    bool SetNodeLevel(std::string_view id, int level) override { if (level) mem.nodes[std::string(id)] = level; else mem.nodes.erase(std::string(id)); return true; }
    bool HasMedicine(std::string_view id) override { return mem.meds.count(std::string(id)) > 0; }
    bool GrantMedicine(std::string_view id) override { mem.meds.insert(std::string(id)); mem.medLevel[std::string(id)] = 1; return true; }
    int MedicineLevel(std::string_view id) override { auto it = mem.medLevel.find(std::string(id)); return it == mem.medLevel.end() ? 0 : it->second; }
    bool SetMedicineLevel(std::string_view id, int level) override { mem.medLevel[std::string(id)] = level; return true; }
    bool HasAccessory(std::string_view id) override { return mem.accs.count(std::string(id)) > 0; }
    bool GrantAccessory(std::string_view id) override { mem.accs.insert(std::string(id)); return true; }
    bool ReloadFromTitle() override { calls.push_back("reload"); if (failReload) return false; reloadPolls = 3; return true; }
    bool ReloadFinished() override {
        if (reloadPolls < 0) return false;
        Autosave();  // 回标题时游戏会存档（2026-10-30 实测）
        if (--reloadPolls > 0) return false;
        mem = disk;  // 进档：内存换成磁盘上的存档
        reloadPolls = -1;
        calls.push_back("reloaded");
        return true;
    }
};

static int IndexOf(const std::vector<std::string>& v, const char* s) {
    for (size_t i = 0; i < v.size(); ++i) if (v[i] == s) return static_cast<int>(i);
    return -1;
}

int main() {
    {
        FakeGame game; tower::growthrun::Runner r;
        auto p = Profile::Kit(); const int w = WeaponIndex("Tang", 0); const int i = p.Find(w);
        p.weapons[i].level = 30; p.temper = 2; game.mem.weapons[FakeGame::K("Tang", 0)] = 30;
        r.Adopt(p);
        Check(r.Upgrade(game, w) == Err::AtCap, "forge: ordinary run remains capped at 30");
        r.hexes.Init(123); r.hexes.picks[0] = tower::hex::Forge; r.hexes.pickedAt[0] = 1; r.hexes.completed = 1;
        Check(r.Upgrade(game, w) == Err::Ok && r.profile.weapons[i].level == 31 && game.WeaponLevel("Tang", 0) == 31, "forge: unlock and synchronize level 31");
        tower::growthrun::Runner loaded;
        Check(tower::growthrun::Runner::Load(r.Save(), &loaded) && loaded.profile.weapons[i].level == 31 && loaded.hexes.Has(tower::hex::Forge), "forge: level above 30 survives save and reload");
        auto without = r; without.hexes = {};
        Check(tower::growthrun::Runner::Load(without.Save(), &loaded) && loaded.profile.weapons[i].level == 30, "forge: legacy run remains capped when loaded");
        r.profile.weapons[i].level = 30; game.mem.weapons[FakeGame::K("Tang", 0)] = 30; game.failHighWeapon = true;
        const int beforePoints = r.profile.temper;
        Check(r.Upgrade(game, w) == Err::Bad && r.profile.weapons[i].level == 30 && r.profile.temper == beforePoints, "forge: failed native upgrade restores points and level");
    }
    // 饰品槽按层开放（11 / 21 / 31 / 41，开满 6 格）
    Check(AccessorySlotUnlocks(1) == 0 && AccessorySlotUnlocks(10) == 0 && AccessorySlotUnlocks(11) == 1 && AccessorySlotUnlocks(20) == 1 && AccessorySlotUnlocks(21) == 2 && AccessorySlotUnlocks(31) == 3 && AccessorySlotUnlocks(41) == 4 && AccessorySlotUnlocks(51) == 4, "accessory slots open at 11/21/31/41");
    // ---------------- 数据表
    { int mains = 0, shaqi = 0;
      for (auto& w : kWeapons) (w.kind == 0 ? mains : shaqi)++;
      Check(mains >= 25 && shaqi >= 25, "weapon tables loaded (main and shadow arms)");
      Check(kNodeCount == 32, "32 ability nodes");
      Check(kMedicineCount == 17, "17 medicines");
      Check(kAccessoryCount >= 60, "accessories with names");
      Check(WeaponIndex(kStarterMain, 0) >= 0, "starter main weapon exists");
      bool ok = true;
      for (auto id : kStarterShaqi) ok &= WeaponIndex(id, 1) >= 0;
      Check(ok, "starter shadow arms exist");
      ok = MedicineIndex(kStarterMedicine) >= 0 && MedicineIndex(kMedRare) >= 0;
      for (auto id : kMedTier1) ok &= MedicineIndex(id) >= 0;
      for (auto id : kMedTier2) ok &= MedicineIndex(id) >= 0;
      for (auto id : kMedTier3) ok &= MedicineIndex(id) >= 0;
      Check(ok, "all medicine tier ids exist");
      const int tang = WeaponIndex("Tang", 0);
      Check(kWeapons[tang].perks[0] == 5 && kWeapons[tang].perks[5] == 30, "weapon perks at 5..30");
      int stat = 0;
      for (int i = 0; i < kNodeCount; ++i) stat += IsStatNode(i);
      Check(stat == 3, "three point-investment nodes");
      { int caps = 0; for (int i = 0; i < kNodeCount; ++i) caps += NodeCap(i); Check(AllNodesCost() == caps && caps >= 50, "all nodes cost 1 cultivation point per level (user 2026-11-05)"); }
      bool parents = true;
      for (auto& n : kNodes) for (int p : n.parents) parents &= p >= -1 && p < kNodeCount;
      Check(parents, "node parents are valid indices"); }

    // ---------------- 每层安排
    { int temper = 0, tokens = 0, boxes = 0, medMax = 1;
      for (int f = 1; f <= 35; ++f) { auto p = PlanFor(f, 50); temper += p.temper; tokens += p.soulToken; if (f <= 33) boxes += p.accessoryBox; if (p.medLevel) medMax = p.medLevel; }
      Check(PlanFor(1, 50).temper == 0 && PlanFor(3, 50).temper == 0 && PlanFor(15, 50).temper == 0 && PlanFor(10, 50).cultivate == 0 && PlanFor(32, 50).cultivate == 0,
            "2.0.0 points: no fixed points per floor");
      Check(PlanFor(8, 50).kind == Floor::TwoPhase && PlanFor(8, 50).cultivate == 0 && PlanFor(8, 50).temper == 0 && PlanFor(31, 50).kind == Floor::Gauntlet, "replay / relay floors keep their kind; points by floor number");
      Check(PlanFor(36, 50).temper == 0 && PlanFor(50, 50).cultivate == 0 && PlanFor(51, 50).cultivate == 0 && PlanFor(3, 3).temper == 0 && PlanFor(3, 3).cultivate == 1, "no fixed points through 51; trial runs keep 1 cultivation");
      Check(PlanFor(50, 50).kind == Floor::Final, "floor 50 is the tower master");
      { int tok = 0; for (int f = 1; f <= 50; ++f) tok += PlanFor(f, 50).soulToken; Check(tok == 0, "v15: no soul tokens from floor clears (rewards only)"); }
      { int trio = 0; for (int f = 1; f <= 50; ++f) trio += PlanFor(f, 50).kind == Floor::Trio;
        Check(trio == 0 && PlanFor(51, 50).kind == Floor::Trio && PlanFor(51, 50).jadeMul == 2, "v14: no 1v3 inside 1-50; floor 51 is the hidden 1v3"); }
      Check(tokens == 0, "v15: no soul tokens by floor 35 from clears");
      Check(boxes == 0, "v3: no free accessory boxes (they are sold in the post station)");
      Check(medMax == 6 && PlanFor(35, 50).medLevel == 6, "medicine level 6 at floor 35");
      Check(temper == 0, "2.0.0: floors give no fixed tempering points");
      Check(PlanFor(2, 3).kind == Floor::Duo && PlanFor(3, 3).kind == Floor::ZoneBoss, "short run: duo on 2, boss on last");
      std::printf("info: floors 1-35 give %d tempering points (weapons need %d)\n", temper, 4 * (kWeaponCap - 1)); }

    // ---------------- 塔档：兵器
    { Profile p = Profile::Start(1);
      Check(p.weapons.size() == 2 && p.weapons[0].level == 1 && p.weapons[1].level == 1, "start: two weapons at level 1");
      Check(kWeapons[p.weapons[1].index].id == "XueDiZi", "start: chosen shadow arm");
      Check(p.Upgrade(p.weapons[0].index) == Err::NoPoints, "upgrade needs points");
      p.Earn(40, 0);
      const int w = p.weapons[0].index;
      for (int i = 0; i < 40; ++i) p.Upgrade(w);
      Check(p.weapons[0].level == kWeaponCap, "weapon caps at 30");
      Check(p.temper == 40 - (kWeaponCap - 1), "upgrades cost 1 point per level");
      Check(p.Downgrade(w, 10) == Err::Ok && p.weapons[0].level == 10, "downgrade sets level");
      Check(p.temper + p.TemperSpent() == p.temperEarned, "tempering points are conserved through refunds");
      Check(p.Downgrade(w, 10) == Err::Bad && p.Downgrade(w, 0) == Err::Bad, "downgrade rejects bad targets");
      Check(p.Snap(w) == Err::NoToken, "soul forging needs a token");
      p.soulTokens = 1;
      Check(p.Snap(w) == Err::Ok && p.Snap(w) == Err::Already, "soul forging once per weapon");
      Check(!p.AddWeapon(w) && p.AddWeapon(WeaponIndex("ZhanMaBlade", 0)), "weapons are not duplicated");
      Check(NextPerk(w, 10) == 15 && PerksUnlocked(w, 10) == 2, "perk lookup"); }

    // ---------------- 塔档：能力修行
    { Profile p;
      p.Earn(0, 200);
      const int h1 = NodeIndex("HeiShi1"), h2 = NodeIndex("HeiShi2"), h8 = NodeIndex("HeiShi8"), top1 = NodeIndex("TopRongShuZhiXin1");
      Check(p.Activate(h2) == Err::Locked, "line nodes go left to right");
      Check(p.Activate(h1) == Err::Ok && p.Activate(h2) == Err::Ok, "activate in order");
      Check(p.Activate(top1) == Err::Locked, "banyan node needs both parents");
      Check(p.Activate(h1) == Err::AtCap, "single-level node caps at 1");
      for (int i = 0; i < kNodeCount * 20; ++i) for (int n = 0; n < kNodeCount; ++n) p.Activate(n);
      Check(p.NodesMaxed(), "all nodes can be maxed");
      Check(p.nodes[h8] == kStatNodeCap, "stat node caps at 20 in the tower");
      Check(p.CultivateSpent() == AllNodesCost() && p.cultivate == 200 - AllNodesCost(), "node costs add up");
      p.ResetNodes();
      Check(p.cultivate == 200 && p.CultivateSpent() == 0, "reset refunds everything");
      // 1.1.1 末列（锋芒 / 凝神 / 易筋）：每通过 15 层 1 个轮回点解锁一个，同行先点满；45 层三个全开
      const int s8 = NodeIndex("ShaChiHuPo8"), m8 = NodeIndex("MemoryFlower8");
      Check(IsFinalNode(h8) && IsFinalNode(s8) && IsFinalNode(m8) && !IsFinalNode(h1) && !IsFinalNode(top1), "final column nodes");
      auto progress = GetCycleProgress(14, 0);
      Check(progress.current == 14 && progress.next == 15 && progress.remaining == 1 && progress.available == 0, "cycle progress follows actual stage gate before milestone");
      progress = GetCycleProgress(15, 0);
      Check(progress.next == 30 && progress.remaining == 15 && progress.available == 1, "cycle progress shows available point and next milestone");
      progress = GetCycleProgress(45, 3);
      Check(progress.next == 0 && progress.remaining == 0 && progress.available == 0, "no fourth milestone after all three final nodes");
      progress = GetCycleProgress(60, 3);
      Check(progress.next == 65 && progress.available == 0, "after all final nodes are unlocked, the next point is the first endless one (3.0.0)");
      progress = GetCycleProgress(14, 1);
      Check(progress.available == 0 && progress.remaining == 1, "rewound stage uses the same gate as node activation");
      Check(p.Activate(h8, 45) == Err::Locked || p.Activate(h8, 45) == Err::RowNotFull, "final node needs its row first");
      for (int i = 0; i < kNodeCount * 2; ++i) for (int n = 0; n < kNodeCount; ++n) if (!IsFinalNode(n)) p.Activate(n, 0);
      Check(p.Activate(h8, 14) == Err::NoCycle, "no cycle point before floor 15");
      Check(p.Activate(h8, 15) == Err::Ok && p.Activate(h8, 15) == Err::Ok, "floor 15: one final node unlocks, then levels freely");
      Check(p.Activate(s8, 29) == Err::NoCycle && p.Activate(s8, 30) == Err::Ok, "second final node at floor 30");
      Check(p.Activate(m8, 44) == Err::NoCycle && p.Activate(m8, 45) == Err::Ok, "third final node at floor 45");
      Check(p.FinalsUnlocked() == 3 && !p.CycleLeft(45), "all cycle points used at 45");
      p.ResetNodes(); }

    // ---------------- 塔档：药与饰品
    { Profile p = Profile::Start(0);
      const int hulu = MedicineIndex("Hulu");
      Check(p.MedUses(hulu) == 2, "hulu has 2 uses at level 1");
      p.medLevel = 6;
      Check(p.MedUses(hulu) == 4, "hulu has 4 uses at level 6");
      Check(!p.AddMedicine(hulu), "medicine not duplicated");
      int refinable = -1;
      for (int i = 0; i < kAccessoryCount; ++i) if (kAccessories[i].refine == 2) { refinable = i; break; }
      Check(refinable >= 0 && p.AddAccessory(refinable) && p.AddAccessory(refinable) && !p.AddAccessory(refinable), "accessory refines once, then stops"); }

    // ---------------- 存档往返
    { Profile p = Profile::Start(2);
      p.Earn(20, 30); p.Upgrade(p.weapons[0].index); p.Activate(NodeIndex("HeiShi1")); p.soulTokens = 2; p.medLevel = 3;
      p.AddMedicine(MedicineIndex("KuYe")); p.AddAccessory(AccessoryIndex("AC_Health"));
      p.Snap(p.weapons[0].index);
      Profile q;
      Check(Profile::Load(p.Save(), &q), "profile loads");
      Check(q.Save() == p.Save(), "profile round-trips exactly");
      Profile r;
      Check(Profile::Load("junk\ng.points=1,2,3,4,0,9\ng.weapons=NoSuchBlade:0:5:0;Tang:0:7:0\n", &r) && r.weapons.size() == 1 && r.weapons[0].level == 7 && r.medLevel == kMedLevelMax,
            "unknown ids dropped, values clamped");
      Check(!Profile::Load("nothing here", &r), "empty profile rejected"); }
    {   // 3.4.0（玩家）：打磨到静态表以上的饰品，存档重进不能被截回（以前截回了但淬炼点没退）
        Profile p; int a = -1; for (int i = 0; i < kAccessoryCount; ++i) if (kAccessories[i].refine == 1) { a = i; break; }
        Check(a >= 0, "an accessory with table refine 1 exists");
        p.accessories.push_back({a, 3}); p.temper = 4;
        Profile q; Check(Profile::Load(p.Save(), &q) && q.accessories.size() == 1 && q.accessories[0].refine == 3 && q.temper == 4, "accessory refine above the table survives save/load");
    }

    // ---------------- 祝福
    { Blessings b;
      Check(b.Add(0, 3) == 3 && b.Add(0, 3) == 0, "per-blessing stack cap");
      for (int i = 1; i < kBlessCount; ++i) b.Add(i, 3);
      Check(b.Total() == kBlessTotalCap, "total blessing cap 8");
      Blessings big;
      big.stacks[static_cast<int>(Bless::Temper)] = 20; big.stacks[static_cast<int>(Bless::Iron)] = 20;
      Check(big.AttackMul() == 1.5 && big.TakenMul() == 0.7, "blessing multipliers are capped");
      Xs r{12345};
      bool never = true;
      for (int i = 0; i < 2000; ++i) { const int x = DrawBlessing(r, Blessings{}); never &= x >= 0 && kBlessings[x].ready && kBlessings[x].kind != Bless::Spring; }
      Check(never, "unfinished blessings are never offered"); }

    // ---------------- 三选一 / 药 / 饰品匣
    { const char* items[] = {"回春丹", "定身符"};
      Profile p = Profile::Start(0);
      Blessings b;
      bool ok = true, lateOk = true, det = true;
      Xs a{99}, c{99};
      for (int f = 1; f <= 50; ++f)
          for (int k = 0; k < 40; ++k) {
              auto o = DrawPicks(a, f, 50, p, b, 2, 50);
              auto o2 = DrawPicks(c, f, 50, p, b, 2, 50);
              for (int s = 0; s < 3; ++s) { ok &= !text::PickLabel(o[s], items, 2).empty(); det &= o[s].kind == o2[s].kind && o[s].index == o2[s].index; }
              if (f > kGrowthEndsAfter) lateOk &= o[0].kind == Pick::Blessing || o[0].kind == Pick::Jade;
              else lateOk &= o[0].kind == Pick::TemperPack || o[0].kind == Pick::CultivatePack;
              if (o[1].kind == Pick::Weapon) ok &= p.Find(o[1].index) < 0;
          }
      Check(ok, "every pick has a label; offered weapons are new");
      Check(lateOk, "growth slot switches to blessings after floor 35");
      Check(det, "picks are deterministic per seed");
      Xs r{7};
      auto meds = DrawMedicines(r, 2, 3, p);
      Check(meds.size() == 3 && meds[0] != meds[1] && meds[1] != meds[2] && meds[0] != meds[2], "three different medicines");
      auto rare = DrawMedicines(r, 4, 2, p);
      Check(rare.size() == 2 && rare[0] == MedicineIndex(kMedRare), "tier 4 offers the rare medicine");
      auto box = DrawAccessoryBox(r, p);
      Check(box.size() == 3 && box[0] != box[1] && box[1] != box[2] && box[0] != box[2], "accessory box has three different items");
      bool pool = true;
      for (int i = 0; i < 500; ++i) { const int x = DrawAccessory(r, p); pool &= x >= 0 && AccessoryInPool(x); }
      Check(pool, "excluded accessories never drawn"); }

    // ---------------- 进出塔流程
    { FakeGame g;
      const auto original = g.disk;
      Profile p = Profile::Start(0);
      ses::Session s;
      Check(s.Enter(g, p) && s.phase() == ses::Phase::Inside, "enter succeeds");
      Check(IndexOf(g.calls, "backup") < IndexOf(g.calls, "sandbox_on") && IndexOf(g.calls, "sandbox_on") < IndexOf(g.calls, "ach_block"), "enter order: backup, sandbox, achievements");
      Check(g.mem.weapons[FakeGame::K("Tang", 0)] == 1, "tower kit: starter weapon at level 1");
      Check(g.mem.weapons[FakeGame::K("ZhanMaBlade", 0)] == 1 && g.mem.weapons[FakeGame::K("BrutalAxe", 1)] == 1, "weapons outside the kit lowered to 1");
      Check(g.mem.nodes.empty(), "abilities all off in the tower");
      Check(g.mem.medLevel["Hulu"] == 1, "medicine level set to the tower's");
      // 塔里成长、拿东西、中途自动存档（沙盒开着，磁盘不变）
      p.Earn(10, 5); p.Upgrade(p.weapons[0].index); p.Activate(NodeIndex("HeiShi1"));
      p.AddWeapon(WeaponIndex("SnakeDual", 0)); p.AddAccessory(AccessoryIndex("AC_Health")); p.AddMedicine(MedicineIndex("KuYe"));
      s.Sync(g, p);
      Check(g.mem.weapons[FakeGame::K("Tang", 0)] == 2 && g.mem.nodes["HeiShi1"] == 1 && g.mem.weapons.count(FakeGame::K("SnakeDual", 0)), "sync applies upgrades and grants");
      g.Autosave();
      Check(g.disk == original, "autosave inside the tower does not touch the real save");
      // 出塔
      Check(s.Exit(g) && s.phase() == ses::Phase::ExitReload, "exit starts reload");
      Check(g.sandbox, "sandbox stays on while reloading");
      Check(g.mem.weapons[FakeGame::K("ZhanMaBlade", 0)] == 10 && g.mem.weapons[FakeGame::K("Tang", 0)] == 12, "undo restores levels before reload");
      for (int i = 0; i < 10 && s.phase() != ses::Phase::Outside; ++i) { s.Tick(g); g.Autosave(); }
      Check(s.phase() == ses::Phase::Outside && !g.sandbox && !g.achBlocked, "exit finishes, sandbox and achievements restored");
      Check(IndexOf(g.calls, "reloaded") >= 0 && IndexOf(g.calls, "reloaded") < IndexOf(g.calls, "sandbox_off"), "sandbox turned off only after reload");
      Check(g.mem == original && g.disk == original, "after exit the game equals the pre-tower save"); }
    { FakeGame g;
      g.failBackup = true;
      ses::Session s;
      Check(!s.Enter(g, Profile::Start(0)) && !g.sandbox && s.phase() == ses::Phase::Outside, "failed backup: nothing changed");
      FakeGame h;
      h.failSandbox = true;
      Check(!s.Enter(h, Profile::Start(0)) && h.mem == h.disk && h.mem.weapons[FakeGame::K("Tang", 0)] == 12, "failed sandbox: game untouched"); }
    { FakeGame g;
      const auto original = g.disk;
      ses::Session s;
      Profile p = Profile::Start(0);
      s.Enter(g, p);
      g.failReload = true;
      Check(!s.Exit(g) && g.sandbox, "failed reload keeps the sandbox on");
      g.Autosave();
      Check(g.disk == original, "still safe after a failed reload"); }
    { // 崩溃恢复：账本落盘 → 新会话读回 → 重开沙盒并走出塔
      FakeGame g;
      const auto original = g.disk;
      ses::Session s;
      Profile p = Profile::Start(0);
      s.Enter(g, p);
      const std::string saved = s.Save();
      FakeGame after;  // "重启游戏"：沙盒没开，内存是磁盘存档（塔内状态没写进去）
      after.mem = after.disk = original;
      ses::Session r = ses::Session::Load(saved);
      Check(r.phase() == ses::Phase::Inside && r.journal().size() == s.journal().size(), "journal round-trips");
      Check(r.Recover(after) && after.sandbox, "recovery re-enables the sandbox and exits");
      for (int i = 0; i < 10 && r.phase() != ses::Phase::Outside; ++i) r.Tick(after);
      Check(r.phase() == ses::Phase::Outside && after.disk == original, "recovery leaves the real save intact"); }
    { // 运行库要停：先把等级撤回去
      FakeGame g;
      ses::Session s;
      s.Enter(g, Profile::Start(0));
      s.OnRuntimeStopping(g);
      Check(g.mem.weapons[FakeGame::K("Tang", 0)] == 12 && g.mem.nodes["HeiShi8"] == 4, "runtime stop restores levels"); }

    // ---------------- 菜单文字
    { Profile p = Profile::Start(0);
      p.Earn(3, 2);
      Blessings b;
      b.Add(0, 1);
      const auto prep = text::Prep(p, b, 0, 50);
      Check(prep.find("淬炼点 3") != std::string::npos && prep.find("修为点 2") != std::string::npos && prep.find("祝福") != std::string::npos, "prep shows points and blessings");
      Check(text::UpgradeButtons(p).size() <= static_cast<size_t>(text::kMaxButtons), "upgrade menu fits the dialog");
      p.Upgrade(p.weapons[0].index);
      Check(text::UpgradeButtons(p).size() == 4, "refund button appears once something is upgraded");
      Check(text::Gains(PlanFor(20, 50), 1).find("铸魂令") == std::string::npos, "v15: floor gains no longer list a soul token"); }

    // ---------------- 整局节奏：每层把点花完；成长格总是拿（最快）/ 从不拿（最慢）
    for (int take = 0; take < 2; ++take) {
        Profile p = Profile::Start(0);
        int weaponsFull = 0, nodesFull = 0;
        for (int f = 1; f <= 50; ++f) {
            const auto plan = PlanFor(f, 50);
            p.Earn(plan.temper, plan.cultivate);
            if (take) p.Earn(f % 2 ? 5 : 0, f % 2 ? 0 : 1);  // v15 点数奖励：淬炼 +5 / 修为 +1 轮流
            if (f == 2 || f == 4) p.AddWeapon(f == 2 ? WeaponIndex("IronBlade", 0) : WeaponIndex("BrutalAxe", 1));  // 前几层 Boss 掉的兵器补满四把
            p.soulTokens += plan.soulToken;
            for (bool spent = true; spent;) {
                spent = false;
                for (auto& w : p.weapons) spent |= p.Upgrade(w.index) == Err::Ok;
                for (int n = 0; n < kNodeCount; ++n) spent |= p.Activate(n) == Err::Ok;
            }
            if (!weaponsFull && p.WeaponsMaxed() >= 4) weaponsFull = f;
            if (!nodesFull && p.NodesMaxed()) nodesFull = f;
        }
        std::printf("info: %s growth packs: 4 weapons maxed at floor %d, abilities maxed at floor %d\n", take ? "with" : "without", weaponsFull, nodesFull);
        if (take) Check(weaponsFull == 0 || weaponsFull >= 20, "v15: with point rewards weapons do not max before floor 20");
        else Check(weaponsFull == 0 || weaponsFull > 32, "without growth packs it takes longer");
    }

    // ---- 一局原生成长：进塔 → 收获 → 菜单升级 → 原生界面对账（够就扣、不够就退）→ 领药 → 存读档 → 出塔
    {
        FakeGame g;
        const auto before = g.disk;
        tower::growthrun::Runner r;
        std::string why;
        Check(r.Start(g, 0, false, &why) && r.on && g.sandbox && g.achBlocked, "growth run: start enters the sandboxed tower");
        Check(g.WeaponLevel("Tang", 0) == 1 && g.WeaponLevel("UnnamedBow", 1) == 1 && g.WeaponLevel("ZhanMaBlade", 0) == 1 && g.NodeLevel("HeiShi1") == 0,
              "growth run: tower kit applied (starter level 1, other weapons lowered, abilities off)");
        r.catalog = tower::rewards::Contract();  // 引擎实际用的装备进度表合同
        const auto gains = r.FloorCleared(g, 1, 50, 7, {{"BrutalAxe", 1}});
        Check(r.profile.temper == 0 && r.profile.cultivate == 0 && gains.find("修为点") == std::string::npos && gains.find("淬炼点") == std::string::npos, "growth run: floor 1 gives no fixed points");
        r.profile.Earn(2, 2);  // 补到下面场景用的 2 + 2
        Check(r.profile.Find(WeaponIndex("BrutalAxe", 1)) >= 0 && g.WeaponLevel("BrutalAxe", 1) == 1, "growth run: boss weapon drop joins the kit at level 1");
        Check(r.Upgrade(g, WeaponIndex("Tang", 0)) == Err::Ok && g.WeaponLevel("Tang", 0) == 2 && r.profile.temper == 1, "growth run: menu upgrade costs 1 point and lands in game");
        // 原生界面里多升了 5 级：只够 1 点 → 接受 1 级，其余退回
        g.SetWeaponLevel("Tang", 0, 7);
        const auto note = r.Reconcile(g);
        Check(r.profile.temper == 0 && g.WeaponLevel("Tang", 0) == 3 && r.profile.weapons[r.profile.Find(WeaponIndex("Tang", 0))].level == 3, "reconcile: pays what it can, reverts the rest");
        Check(note.find("已记账") != std::string::npos && note.find("退回") != std::string::npos, "reconcile: note says what was charged and reverted");
        Check(r.Reconcile(g).empty(), "reconcile: nothing changed -> empty note");
        // 塔档以外的兵器被原生升过：full 时退回 1 级
        g.SetWeaponLevel("ZhanMaBlade", 0, 8);
        r.Reconcile(g, true);
        Check(g.WeaponLevel("ZhanMaBlade", 0) == 1, "reconcile full: non-tower weapon goes back to level 1");
        // 能力：原生点了第一列 → 扣修为点接受；跳着点了加点节点（前置没亮）→ 退回
        int first = -1;
        for (int n = 0; n < kNodeCount; ++n) if (kNodes[n].line == 0 && (first < 0 || kNodes[n].column < kNodes[first].column)) first = n;
        int stat = -1;
        for (int n = 0; n < kNodeCount; ++n) if (kNodes[n].line == 0 && IsStatNode(n)) stat = n;
        g.SetNodeLevel(kNodes[first].id, 1);
        g.SetNodeLevel(kNodes[stat].id, 5);
        r.Reconcile(g);
        Check(r.profile.nodes[first] == 1 && r.profile.cultivate == 2 - NodeCost(first) && g.NodeLevel(kNodes[first].id) == 1, "reconcile: native node activation is charged");
        Check(r.profile.nodes[stat] == 0 && g.NodeLevel(kNodes[stat].id) == 0, "reconcile: locked stat node is reverted");
        // R6：已核 Boss 掉落与随机装备进度分开；固定饰品匣仍服从进度合同。
        r.FloorCleared(g, 2, 50, 7, {{"GuiTouBlade", 0}});
        Check(r.profile.Find(WeaponIndex("GuiTouBlade", 0)) >= 0 && r.held.empty() && g.WeaponLevel("GuiTouBlade",0)==1, "earned verified boss drop is granted independently of the random pool");
        r.FloorCleared(g, 2, 50, 7, {{"HorizontalBlade", 0}});
        Check(r.profile.Find(WeaponIndex("HorizontalBlade",0))<0 && r.held==std::vector<std::string>{"HorizontalBlade"}, "unverified Boss association remains held even after defeat");
        r.FloorCleared(g, 3, 50, 7, {});
        {   // 用户 2026-11-06：非 Boss 饰品按区开放；第 3 层的匣只出 1 区已开放的
            Check(r.acc.empty(), "v3: floor clears no longer hand out accessory boxes");
        }
        r.acc = {AccessoryIndex("AC_Loot")};  // 旧局存档里的匣含塔池排除的聚宝盆
        const auto accBefore = r.profile.accessories.size();
        Check(r.PickAccessory(g, 0) == Err::Bad && r.profile.accessories.size() == accBefore, "contract: picking an excluded accessory from an old box is refused");
        r.Readmit(3, 50);
        Check(r.acc.empty(), "contract: resume re-filters a saved box");
        // 第 5 层：一档药二选一
        for (int f = 4; f <= 5; ++f) r.FloorCleared(g, f, 50, 7, {});
        // 固定收获按实际层型：编排多 Boss、实际单 Boss 按普通层（7c2668e 单铁面 +3 的修正）
        Check(tower::growth::PlanFor(2, 3).kind == tower::growth::Floor::Duo && tower::growth::PlanFor(2, 3).cultivate == 1, "plan follows actual boss count");
        Check(tower::growth::PlanFor(2, 3, 1).kind == tower::growth::Floor::Normal && tower::growth::PlanFor(2, 3, 1).cultivate == 1, "plan follows actual boss count");
        Check(tower::growth::PlanFor(2, 3, 2).kind == tower::growth::Floor::Duo, "plan follows actual boss count");
        Check(tower::growth::PlanFor(13, 50, 1).kind == tower::growth::Floor::Normal && tower::growth::PlanFor(13, 50, 2).kind == tower::growth::Floor::Duo && tower::growth::PlanFor(51, 50, 3).jadeMul == 2 && tower::growth::PlanFor(51, 50, 1).jadeMul == 1, "plan follows actual boss count");
        Check(r.med.empty(), "v3: floor 5 no longer hands out free medicines");
        { tower::growth::Xs mr{7}; r.med = tower::growth::DrawMedicines(mr, 1, 2, r.profile, [&](int m) { return r.Admit(tower::rewards::Cat::Medicine, m, 5, 50, true); }); }  // 旧局存档里待选的药仍可领取
        const int pickedMed = r.med.empty() ? -1 : r.med[0];
        Check(r.PickMedicine(g, 0) == Err::Ok && r.med.empty() && pickedMed >= 0 && g.HasMedicine(kMedicines[pickedMed].id), "picking a medicine grants it in game");
        // 存读档
        r.bonus = {{tower::rewards::Kind::Item, static_cast<int>(tower::Item::Rebirth), 1, 1}, {tower::rewards::Kind::Item, static_cast<int>(tower::Item::Revive), 1, 1}};
        tower::growthrun::Runner back;
        Check(tower::growthrun::Runner::Load(r.Save(), &back) && back.bonus.size() == 2 && back.bonus[0].index == static_cast<int>(tower::Item::Rebirth), "1.0.1: pending extra rewards survive save / load");
        r.bonus.clear(); back = {};
        Check(tower::growthrun::Runner::Load(r.Save(), &back) && back.profile.temper == r.profile.temper && back.profile.nodes == r.profile.nodes &&
              back.profile.Find(WeaponIndex("BrutalAxe", 1)) >= 0 && back.profile.HasMedicine(pickedMed), "growth run save/load round trip");
        // 出塔：撤销 + 读档，进完档才关沙盒；真存档始终是进塔前的
        g.Autosave();
        Check(g.disk == before, "growth run: autosaves inside the tower never reach the real save");
        Check(r.Exit(g, &why) && r.Exiting() && g.readThrough, "growth run: exit restores and waits for the reload");
        bool done = false;
        for (int i = 0; i < 10 && !done; ++i) done = r.Tick(g);
        Check(done && !r.on && !g.sandbox && !g.achBlocked && g.mem == before, "growth run: after reload everything is back and the sandbox is off");
    }
    // F34 live regression: PowerMan's Jigsaw association was approved in data,
    // but the runtime drop table still held it after a completed P2 defeat.
    {
        FakeGame g; tower::growthrun::Runner r; std::string why;
        Check(r.Start(g,0,false,&why), "scavenger: protected run starts");
        const auto disk = g.disk;
        const std::vector<std::string_view> boss{
            "/Game/x/BP_Monster_PowerMan.BP_Monster_PowerMan_C",
            "/Game/x/BP_Monster_PowerMan_P2.BP_Monster_PowerMan_P2_C"};
        tower::Run normal; normal.Start(7,50); normal.floor=34; normal.phase=tower::Phase::Fighting;
        normal.RecordShopDrops(boss); normal.FloorCleared(boss, false, true);
        Check(normal.last.weapons.empty() && !normal.shopDrops.empty(), "normal boss weapon unlocks shop without direct drop");
        tower::Run direct; direct.Start(7,50); direct.floor=34; direct.phase=tower::Phase::Fighting;
        direct.FloorCleared(boss, false, false);
        std::vector<std::pair<std::string,int>> drops;
        for (int w : direct.last.weapons) drops.push_back({std::string(tower::kWeaponDrops[w].weapon), tower::kWeaponDrops[w].shaqi ? 1 : 0});
        r.FloorCleared(g,34,50,7,drops);
        Check(direct.shopDrops.empty() && direct.last.weapons.size()==1 && r.profile.Find(WeaponIndex("Jigsaw",0))>=0 &&
              g.WeaponLevel("Jigsaw",0)==1 && std::count(g.calls.begin(),g.calls.end(),"grant:0:Jigsaw")==1,
              "scavenger: two boss phases grant one weapon and do not unlock shop");
        g.Autosave(); Check(g.disk == disk, "scavenger grant remains in tower sandbox");
        tower::Run replay; replay.Start(7,50); replay.floor=34; replay.phase=tower::Phase::Fighting;
        replay.FloorCleared(boss, true, false);
        Check(replay.last.weapons.empty(), "scavenger replay does not regrant a boss weapon");
    }
    {
        FakeGame g; tower::growthrun::Runner r; std::string why;
        Check(r.Start(g,0,false,&why), "Jigsaw drop: protected run starts");
        r.catalog=tower::rewards::Contract();
        const auto disk=g.disk;
        tower::Run combat; combat.Start(7,50); combat.floor=34; combat.phase=tower::Phase::Fighting;
        combat.FloorCleared({"/Game/RenewalContent/Actors/Mobs/Normal/BP_Monster_PowerMan_P2.BP_Monster_PowerMan_P2_C"});
        Check(combat.last.weapons.size()==1 && tower::kWeaponDrops[combat.last.weapons[0]].weapon=="Jigsaw", "PowerMan P2 actual class resolves to Jigsaw");
        r.FloorCleared(g,34,50,7,{{"Jigsaw",0},{"Jigsaw",0}});
        Check(r.held.empty() && r.profile.Find(WeaponIndex("Jigsaw",0))>=0 && g.WeaponLevel("Jigsaw",0)==1, "defeated PowerMan grants level-one Jigsaw instead of holding it");
        auto grants=[&](){return std::count(g.calls.begin(),g.calls.end(),"grant:0:Jigsaw");};
        Check(grants()==1, "one encounter with duplicate phase drops grants Jigsaw once");
        r.FloorCleared(g,35,50,7,{{"Jigsaw",0}});
        Check(grants()==1, "owned Jigsaw is not granted a second time");
        Check(!tower::EarnedBossWeapon("Jigsaw",1), "Jigsaw cannot be granted as a Shaqi weapon");
        g.Autosave(); Check(g.disk==disk, "Jigsaw reward remains inside protected tower state");
    }
    {
        FakeGame g; tower::growthrun::Runner r; std::string why;
        Check(r.Start(g, 0, false, &why), "final weapon: protected run starts");
        const std::vector<std::string_view> paths{
            "/PBZCombatAsset/ActorData/Mobs/076_ZuoShang_P2/BP_Monster_ZuoShang_P2.BP_Monster_ZuoShang_P2_C"};
        const int w = tower::FinalBossWeapon(50, paths);
        Check(w >= 0, "final weapon: actual Zuoshang phase resolves");
        if (w >= 0) r.FloorCleared(g, 50, 50, 7, {{std::string(tower::kWeaponDrops[w].weapon), 0}});
        Check(g.WeaponLevel("ZiMingJian", 0) == 1 && r.profile.Find(WeaponIndex("ZiMingJian", 0)) >= 0, "final weapon: sword granted in protected game state");
        tower::growthrun::Runner restored;
        Check(tower::growthrun::Runner::Load(r.Save(), &restored) && restored.profile.Find(WeaponIndex("ZiMingJian", 0)) >= 0, "final weapon: hidden-stage snapshot retains sword");
    }
    // R5 live regression: LionHead was zone -1 and never reached native Grant.
    {
        FakeGame g; tower::growthrun::Runner r; std::string why;
        Check(r.Start(g,0,false,&why), "lion drop: protected run starts");
        r.catalog=tower::rewards::Contract();
        const int lion=WeaponIndex("LionHead",1);
        Check(!r.Admit(tower::rewards::Cat::Shaqi,lion,26,50,false), "recorded LionHead random catalog is still pending");
        const auto disk=g.disk;
        r.FloorCleared(g,26,50,7,{{"LionHead",1},{"LionHead",1}});
        Check(r.held.empty() && r.profile.Find(lion)>=0 && g.WeaponLevel("LionHead",1)==1, "native ending frozen: completed Lion encounter still grants usable LionHead");
        auto grants=[&](){return std::count(g.calls.begin(),g.calls.end(),"grant:1:LionHead");};
        Check(grants()==1, "two native phases sharing one weapon cause a single native grant");
        r.FloorCleared(g,27,50,7,{{"LionHead",1}});
        Check(grants()==1, "already owned Boss weapon does not grant or show a popup twice");
        Check(!tower::EarnedBossWeapon("LionHead",0) && !tower::EarnedBossWeapon("WoodenSword",0) && !tower::EarnedBossWeapon("NoSuchWeapon",1), "wrong kind, story-only and unknown IDs cannot use Boss-drop admission");
        Check(!r.Admit(tower::rewards::Cat::Shaqi,lion,50,50,true) && !r.catalog.Find(tower::rewards::Cat::Shaqi,lion)->shop, "earned drop does not open pending random rewards or shops");
        g.Autosave();Check(g.disk==disk, "earned weapon grant remains confined to the sandbox");
        tower::Run combat;combat.Start(7,50);combat.floor=20;combat.phase=tower::Phase::Fighting;
        combat.FloorCleared({"/Game/Actors/BP_Monster_MuXiaoKui_P2.BP_Monster_MuXiaoKui_P2_C"});
        Check(combat.last.weapons.size()==1 && tower::kWeaponDrops[combat.last.weapons[0]].weapon=="YuuBlade", "MuXiaoKui phase2 actual class resolves to its connected native weapon reward, not orphan LuanYing nodes");
        r.FloorCleared(g,20,50,7,{{"YuuBlade",0},{"XueDiZi",1},{"BaiYing",0},{"JadeDualChakram",0},{"CiGuHunDun",0}});
        Check(r.held.empty() && g.WeaponLevel("YuuBlade",0)==1 && g.WeaponLevel("XueDiZi",1)==1 && g.WeaponLevel("BaiYing",0)==1 && g.WeaponLevel("JadeDualChakram",0)==1 && g.WeaponLevel("CiGuHunDun",0)==1, "newly proven reachable native Boss awards also use the independent grant bridge");
    }
    // 进塔失败（沙盒开不了）：不算在塔里，游戏没被改
    {
        FakeGame g;
        g.failSandbox = true;
        const auto before = g.mem;
        tower::growthrun::Runner r;
        std::string why;
        Check(!r.Start(g, 1, true, &why) && !r.on && !why.empty() && g.mem == before, "growth run: failed sandbox leaves the game untouched");
    }

    // ---- 新奖励模型接进 Runner：抽一组 → 领取（复核 → 发放 → 同步）→ 存读档
    {
        namespace rw = tower::rewards;
        FakeGame g;
        tower::growthrun::Runner r;
        std::string why;
        Check(r.Start(g, 0, false, &why), "rewards: start");
        std::string table;
        for (int i = 0; i < kWeaponCount; ++i) table += std::format("{} {} 0 0 1 1\n", kWeapons[i].kind == 1 ? "shaqi" : "main", kWeapons[i].id);
        r.catalog = rw::ParseCatalog(table, rw::Catalog::Default());
        std::array<int, tower::kItemCount> items{};
        int jade = 0;
        {   // 第 1 层后：三把随机武器，互不相同、没有已持有的、不受进度门槛；多局下来全部武器（含紫冥剑）都可能出
            std::set<int> seen;
            bool ok = true;
            for (uint64_t s = 1; s <= 600; ++s) {
                auto t = r;
                t.DrawRewards(1, 50, s, items);
                std::set<int> one;
                for (auto& o : t.offers) {
                    ok = ok && o.kind == rw::Kind::Weapon && o.quality == 9 && r.profile.Find(o.index) < 0;
                    ok = ok && rw::StillValid(o, t.Ctx(1, 50, items));
                    one.insert(o.index); seen.insert(o.index);
                }
                ok = ok && one.size() == 3 && !t.refresh;
            }
            int owned = 0;
            for (int i = 0; i < kWeaponCount; ++i) owned += r.profile.Find(i) >= 0;
            Check(ok, "starter weapons: three different unowned weapons, valid on floor 1");
            int retired = 0;
            for (int i = 0; i < kWeaponCount; ++i) retired += rw::RetiredWeapon(i) && r.profile.Find(i) < 0;
            Check(static_cast<int>(seen.size()) == kWeaponCount - owned - 1 - retired && seen.count(WeaponIndex("ZiMingJian", 0)) && !seen.count(WeaponIndex("XieHuo", 0)), "starter weapons: all eligible weapons can appear, soul form and prototypes excluded");
            Check(rw::Price(rw::Kind::Weapon, 0) == 100 && rw::Price(rw::Kind::Weapon, 1) == 200 && rw::Price(rw::Kind::Weapon, 4) == 612, "weapon prices: first two zones cheaper");
        }
        {
            const int bad = WeaponIndex("ZhuGuangBaoQi", 0);
            const rw::Offer old{rw::Kind::Weapon, bad, 1, 9};
            auto legacy = r;
            legacy.offered = true; legacy.offerFloor = 1; legacy.offers[0] = old;
            Check(!legacy.OffersFor(1), "saved reward containing a retired weapon is redrawn on resume");
            legacy.bonus = {old, {rw::Kind::CultivatePack, 0, 1, 1}};
            legacy.shop.floor = 1; legacy.shop.version = 10;
            legacy.shop.shelf = {{old, 100, false}, {{rw::Kind::CultivatePack, 0, 1, 1}, 15, false}};
            Check(legacy.profile.AddWeapon(bad), "fixture represents an already owned old-save weapon");
            legacy.RetirePendingWeapons(1, 50, 7, items);
            Check(legacy.bonus.size() == 1 && legacy.bonus[0].kind == rw::Kind::CultivatePack && legacy.shop.shelf.size() == 1 && legacy.shop.version == 11, "resume removes only retired pending bonus and shelf entries, invalidating old quotes");
            Check(legacy.profile.Find(bad) >= 0, "already owned legacy equipment is preserved");
            legacy.bonus = {old};
            legacy.RetirePendingWeapons(1, 50, 7, items);
            Check(!legacy.bonus.empty() && std::none_of(legacy.bonus.begin(), legacy.bonus.end(), rw::RetiredOffer), "fully retired bonus is replaced with a valid selection");
        }
        r.DrawRewards(3, 50, 7, items);
        Check(r.offered, "rewards: a group is pending after the draw");
        // 找一组里有兵器的（换种子重抽，直到出现）
        int slot = -1;
        for (int k = 0; k < 50 && slot < 0; ++k) {
            r.DrawRewards(3, 50, 7, items, true);
            for (int i = 0; i < 3; ++i) if (r.offers[i].kind == rw::Kind::Weapon) slot = i;
        }
        Check(slot < 0, "v3: the reward page never offers equipment (weapons come from the post station)");
        rw::Offer got;
        Check(r.Claim(g, 0, 3, 50, items, jade, &got) == Err::Ok && !r.offered, "rewards: a pending group can be claimed");
        Check(r.Claim(g, 0, 3, 50, items, jade, &got) == Err::Bad, "rewards: nothing to claim twice");
        // 复核：显示后状态变了（已持有）→ 不发放、保留这一组
        const int w = r.profile.weapons.empty() ? 0 : r.profile.weapons[0].index;  // 已持有的兵器
        jade = 0;
        r.offers = {rw::Offer{rw::Kind::Weapon, w, 1, 1}, rw::Offer{rw::Kind::Jade, -1, 10, 1}, rw::Offer{rw::Kind::Jade, -2, 10, 1}};
        r.offered = true;
        Check(r.Claim(g, 0, 3, 50, items, jade, &got) == Err::AtCap && r.offered, "rewards: stale option refused, group kept");
        Check(r.Claim(g, 1, 3, 50, items, jade, &got) == Err::Ok && jade == 10, "rewards: jade claim adds to the run ledger");
        // 祝福：发放进 Blessings
        r.offers = {rw::Offer{rw::Kind::Blessing, 0, 1, 1}, {}, {}};
        r.offered = true;
        Check(r.Claim(g, 0, 3, 50, items, jade, &got) == Err::Ok && r.bless.stacks[0] == 1, "rewards: blessing stacks");
        // 驿站 + 准备道具 + 存读档
        r.OpenShop(5, 50, 7, items);
        {
            int mods = 0, natives = 0;
            for (auto& w : r.shop.shelf) { mods += w.offer.kind == rw::Kind::Item; natives += w.offer.kind == rw::Kind::Weapon; }
            Check(r.shop.Open() && mods == 2 && natives > 0, "rewards: 2.0.0 shop lists two random MOD items and all open native goods");
        }
        int sunder = -1;
        for (int i = 0; i < tower::kItemCount; ++i) if (tower::kItems[i].item == tower::Item::Sunder) sunder = i;
        items[sunder] = 1;
        Check(r.prep.Arm(sunder, items), "rewards: arm prep item");
        r.DrawRewards(6, 50, 7, items);
        const auto text = r.Save();
        tower::growthrun::Runner back;
        Check(tower::growthrun::Runner::Load(text, &back), "rewards: runner save loads");
        bool same = back.offered && back.offers[0] == r.offers[0] && back.offers[1] == r.offers[1] && back.offers[2] == r.offers[2];
        same = same && back.bless.stacks == r.bless.stacks && back.rewardRng == r.rewardRng && back.seen == r.seen;
        same = same && back.shop.version == r.shop.version && back.shop.shelf.size() == r.shop.shelf.size() && back.prep.Armed(sunder);
        Check(same, "rewards: offers / blessings / rng / seen / shop / prep survive save-load");
        auto bad = text;
        bad.replace(bad.find("gr.offer="), 9, "gr.offer=1 6,9999,1,1 8,-1,1,1 8,-2,1,1\nx=");
        tower::growthrun::Runner badBack;
        Check(tower::growthrun::Runner::Load(bad, &badBack) && !badBack.offered, "rewards: out-of-range saved offer dropped (redrawn after load)");
    }

    // ---- v14 局内奖励状态（用户 2026-11-05）：每层重抽、刷新令不占领奖、属性累计随存档且不能被读档突破
    {
        namespace rw = tower::rewards;
        FakeGame g;
        tower::growthrun::Runner r;
        r.profile = tower::growth::Profile::Kit();
        r.catalog = rw::Contract();
        std::array<int, tower::kItemCount> items{};
        int jade = 0;
        r.DrawRewards(3, 50, 99, items);
        Check(r.OffersFor(3) && !r.OffersFor(4), "v14 runner: the group belongs to the floor it was drawn on");
        const auto first = r.offers;
        int found = -1;
        for (int k = 0; k < 40 && found < 0; ++k) { r.DrawRewards(3, 50, 99, items, true); for (int i = 0; i < 3; ++i) if (r.offers[i].kind == rw::Kind::Attr) found = i; }
        Check(found >= 0, "v14 runner: attribute rewards appear");
        // 刷新令：有就能用，用了重抽且仍可领一个
        r.refresh = true;
        const auto before = r.offers;
        Check(r.UseRefresh(3, 50, 99, items) && r.OffersFor(3), "v14 runner: refresh redraws and keeps the floor's pick");
        r.refresh = false;
        Check(!r.UseRefresh(3, 50, 99, items), "v14 runner: no refresh token -> no free redraw");
        found = -1;
        for (int k = 0; k < 60 && found < 0; ++k) { r.DrawRewards(3, 50, 99, items, true); for (int i = 0; i < 3; ++i) if (r.offers[i].kind == rw::Kind::Attr) found = i; }
        rw::Offer got;
        const int ai = r.offers[found].index, amount = r.offers[found].amount;
        Check(r.Claim(g, found, 3, 50, items, jade, &got) == Err::Ok && r.attrs.pm[ai] == amount && !r.offered && r.lastGroup.size() == 3,
              "v14 runner: claiming an attribute adds to the run total and remembers the group");
        // 下一层：必须重新抽（上一层缓存不复用）
        Check(!r.OffersFor(4), "v14 runner: next floor has no reused offers");
        r.DrawRewards(4, 50, 99, items);
        Check(r.OffersFor(4), "v14 runner: next floor draws fresh");
        // 存读档
        r.refresh = true;
        r.attrs.pm[0] = 70;
        const auto text = r.Save();
        tower::growthrun::Runner back;
        Check(tower::growthrun::Runner::Load(text, &back) && back.attrs.pm == r.attrs.pm && back.refresh && back.offerFloor == 4 && back.draws == r.draws &&
              back.lastGroup.size() == 3 && back.OffersFor(4), "v14 runner: attrs / refresh / floor / draw count / last group survive save-load");
        auto hacked = text;
        hacked.replace(hacked.find("gr.attr="), 8, "gr.attr=99999,9999,9999,9999,9999\nx=");
        tower::growthrun::Runner capped;
        Check(tower::growthrun::Runner::Load(hacked, &capped) && capped.attrs.pm[0] == 99999 && capped.attrs.pm[2] == 9999 && capped.attrs.pm[4] == 800,  // 用户 2026-11-13：除减伤外不封顶
              "v14 runner: loading keeps uncapped stats and clamps only damage reduction");
    }

    {   // Every displayed attribute choice is fully credited and survives save/load.
        namespace rw = tower::rewards;
        FakeGame game; tower::growthrun::Runner r; std::array<int, tower::kItemCount> items{};
        int jade = 0; rw::Offer granted;
        for (int i = 0; i < rw::kAttrCount; ++i) {
            r.offered = true; r.offerFloor = 16;
            r.offers[0] = {rw::Kind::Attr, i, rw::kAttrs[i].hard[1], 1};
            Check(r.Claim(game, 0, 16, 50, items, jade, &granted) == Err::Ok && r.attrs.pm[i] == granted.amount,
                  "each attribute claim credits exactly the displayed amount");
        }
        tower::growthrun::Runner loaded;
        Check(tower::growthrun::Runner::Load(r.Save(), &loaded) && loaded.attrs.pm == r.attrs.pm,
              "all attribute totals survive save and load");
        const auto easy = [&] { r.difficulty = 1; return r.Ctx(16, 50, items).difficulty; }();
        const auto hard = [&] { r.difficulty = 0; return r.Ctx(16, 50, items).difficulty; }();
        Check(easy == 1 && hard == 0, "reward context follows selected difficulty");
    }

    {   // A previously saved +5% lifesteal reward cannot exceed the new rules.
        namespace rw = tower::rewards;
        tower::growthrun::Runner r, back; r.profile = tower::growth::Profile::Kit();
        r.offered = true; r.offerFloor = 40;
        r.offers = {rw::Offer{rw::Kind::Attr, 5, 50, 1}, rw::Offer{rw::Kind::Jade, -1, 10, 1}, rw::Offer{rw::Kind::Jade, -1, 20, 1}};
        Check(tower::growthrun::Runner::Load(r.Save(), &back) && back.offered && back.offers[0].amount == 20, "old pending lifesteal offer becomes 2 percent");
        r.attrs.pm[5] = 150;
        Check(tower::growthrun::Runner::Load(r.Save(), &back) && back.attrs.pm[5] == 150 && back.offered, "life steal is no longer capped (user 2026-11-13)");
        r.attrs.pm[5] = 0; r.offerFloor = 35;
        Check(tower::growthrun::Runner::Load(r.Save(), &back) && !back.offered, "old early lifesteal offer triggers redraw");
    }

    {   // Damage reduction must be credited immediately and survive a resume.
        namespace rw = tower::rewards;
        FakeGame game; tower::growthrun::Runner r; std::array<int, tower::kItemCount> items{};
        r.profile = tower::growth::Profile::Kit(); r.catalog = rw::Contract();
        r.offered = true; r.offerFloor = 16;
        r.offers[2] = {rw::Kind::Attr, static_cast<int>(rw::AttrKind::Defense), 50, 1};
        int jade = 0; rw::Offer granted;
        const auto result = r.Claim(game, 2, 16, 50, items, jade, &granted);
        const int reduction = r.attrs.pm[static_cast<int>(rw::AttrKind::Defense)];
        Check(result == Err::Ok && granted.kind == rw::Kind::Attr && reduction == 50 && !r.offered,
              "damage reduction: choosing the offer credits 5% in the current run immediately");
        tower::growthrun::Runner resumed;
        Check(tower::growthrun::Runner::Load(r.Save(), &resumed) && resumed.attrs.pm[static_cast<int>(rw::AttrKind::Defense)] == 50,
              "damage reduction: 5% survives saving and continuing the run");
    }

    {   // 3.0.0：无尽层每 15 层 1 个轮回点，末列可越过塔内上限 20 继续升级（每点 +1 级，到原生 30）
        int fin = -1;
        for (int i = 0; i < kNodeCount && fin < 0; ++i) if (IsFinalNode(i)) fin = i;
        Profile p; for (int i = 0; i < kNodeCount; ++i) p.nodes[i] = NodeCap(i); p.cultivate = 100;
        Check(EndlessCycles(50) == 0 && EndlessCycles(64) == 0 && EndlessCycles(65) == 1 && EndlessCycles(80) == 2, "endless cycle points: one per 15 endless floors");
        Check(p.Activate(fin, 64) == Err::AtCap, "final node stays at the tower cap before the first endless cycle point");
        Check(p.Activate(fin, 65) == Err::Ok && p.nodes[fin] == NodeCap(fin) + 1 && p.FinalExtra() == 1, "an endless cycle point raises a final node one level");
        Check(p.Activate(fin, 65) == Err::AtCap && p.Activate(fin, 80) == Err::Ok, "each point is used once; the next point allows one more");
        Profile back; Check(Profile::Load(p.Save(), &back) && back.nodes[fin] == NodeCap(fin) + 2, "levels above the tower cap survive save and load");
        Profile top; top.nodes[fin] = kNodes[fin].maxLevel; top.cultivate = 10;
        Check(top.Activate(fin, 5000) == Err::AtCap, "never past the native maximum");
        const auto cp = GetCycleProgress(66, 3, 1);
        Check(cp.next == 80 && cp.remaining == 14 && cp.available == 0, "endless cycle progress: next point and spare count");
    }
    {   // 用户 2026-11-13：苦修与坦克引擎同时生效时生命加成相加；艾卡西亚的陷落让影塔减伤上限到 90%
        tower::hex::State s; s.Init(3); s.picks[0] = tower::hex::Tank; s.pickedAt[0] = 1; s.picks[1] = tower::hex::Ascetic; s.pickedAt[1] = 1;
        for (int f = 1; f <= 10; ++f) s.ClearFloor(f);
        s.asceticStacks = 5;
        Check(std::abs(s.Health() - (tower::hex::kTankLife * 10 + tower::hex::kAsceticStep * 5)) < 1e-9, "tank engine and ascetic stack life additively");
        tower::hex::State d; d.Init(3);
        Check(d.MinTaken() == tower::hex::kMinTakenFactor && std::abs(tower::hex::DamageFactor(2000, 2000, d.MinTaken()) - 0.2) < 1e-9, "damage reduction caps at 80% by default");
        d.picks[0] = tower::hex::Icathia; d.pickedAt[0] = 1;
        Check(d.MinTaken() == tower::hex::kMinTakenIcathia && std::abs(tower::hex::DamageFactor(2000, 2000, d.MinTaken()) - 0.1) < 1e-9, "Icathia raises the damage reduction cap to 90%");
    }
    if (g_failed) { std::printf("TOWER_GROWTH_FAILED %d/%d\n", g_failed, g_checks); return 1; }
    std::printf("TOWER_GROWTH_OK %d checks (no game process)\n", g_checks);
    return 0;
}
