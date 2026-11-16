#include <cstdio>
#include "../src/tower_entry_logic.h"
#include "../src/tower_arena.h"
#include "../src/tower_isolation.h"
#include "../src/tower_admission.h"
#include "../src/tower_arsenal_selection.h"
#include "../src/tower_growth.h"
#include "../src/tower_points_logic.h"
#include "../src/tower_save.h"
#include "../src/tower_failed_stage.h"
#include "../src/tower_wave_guard.h"

int main() {
    int n = 0, failed = 0;
    auto check = [&](bool ok, const char* msg) { ++n; if (!ok) { ++failed; std::printf("FAIL %s\n", msg); } };
    using namespace tower;
    {
        // A killed mob can leave the owned set before the gate releases. It
        // must remain finished on every later frame, not just the first one.
        bool dead = false;
        for (int frame = 0; frame < 5; ++frame) {
            const bool finished = wave::MissingFinished(true, dead, frame == 0);
            check(finished, "removed, zero-observed wave actor stays finished during entry");
            dead = dead || finished;
        }
        check(!wave::MissingFinished(true, false, false), "unobserved disappearance cannot become a kill");
        check(wave::MissingFinished(true, false, false, true), "an owned wave enemy with confirmed native death finishes without a readable attribute target");
        check(!wave::MissingFinished(false, true, true), "boss phase is not treated as an ordinary wave mob");
        check(!wave::MissingFinished(false, false, false, true), "native wave death fallback does not advance boss phases");
        wave::RequestWatch watch;
        watch.Begin();
        check(!watch.Tick(29, false) && !watch.Tick(120, true), "paused request never times out");
        check(watch.Tick(1, false), "stalled request produces an actionable timeout");
        watch.Finish();
        check(!watch.Tick(60, false), "completed or cancelled request cannot time out later");
        watch.Begin();
        check(!watch.Tick(1, false), "next request starts with its own timeout budget");
        wave::TargetWatch target;
        check(target.CheckNativeDeath(100) && !target.CheckNativeDeath(100.1) && target.CheckNativeDeath(100.25), "native fallback is rate-limited while the target remains unreadable");
        check(!target.Missing(100) && !target.Missing(119.9), "brief target loss cannot cancel a wave");
        target.Found();
        check(!target.Missing(120) && !target.Missing(139.9) && target.Missing(140), "recovered targets reset the missing-target timeout");
        target.Found();
        check(!target.Missing(200, true) && !target.Missing(204.9, true) && target.Missing(205, true), "last unreadable wave enemy triggers a faster retry, never an invented kill");
        target.Found();
        check(!target.Missing(300, false) && !target.Missing(306, false) && target.Missing(320, false), "other living enemies retain normal recovery grace");
    }
    {
        Run r; r.Start(91, 50); r.id = "wave-venue"; r.floor = 29;
        r.mobFloor = 30; r.mobWave = 1; r.mobVenueFloor = 30;
        r.mobVenuePath = "/Game/Test/BP_Test.BP_Test_C";
        Run resumed;
        check(resumed.Load(r.Save()) && resumed.mobFloor == 30 && resumed.mobWave == 1 &&
              resumed.mobVenueFloor == 30 && resumed.mobVenuePath == r.mobVenuePath,
              "a partially cleared wave resumes with its own venue and progress");
        Run old; check(old.Load("v=1\nseed=91\nfloors=50\nfloor=29\nmob=30,1\n") && old.mobVenuePath.empty(),
                       "old 2.0 saves without a wave venue remain loadable");
        Run invalid; check(invalid.Load("v=1\nseed=91\nfloors=50\nfloor=29\nmob=30,1\nmobvenue=30|relative-path\n") && invalid.mobVenuePath.empty(),
                           "an invalid saved venue cannot become a travel target");
        resumed.RewindForRebirth(19, 0);
        check(resumed.mobVenuePath.empty() && resumed.mobVenueFloor == 0, "rebirth discards the previous wave venue");
    }
    {
        Run r; r.Start(123, 50); r.id = "foresight-run";
        r.items[static_cast<int>(Item::Foresight)] = 1;
        Run restored;
        check(restored.Load(r.Save()) && restored.items[static_cast<int>(Item::Foresight)] == 1, "permanent foresight survives save and resume");
        restored.RewindForRebirth(0, -1);
        check(restored.items[static_cast<int>(Item::Foresight)] == 1, "foresight survives rebirth rewind");
        restored.Start(456, 50);
        check(restored.items[static_cast<int>(Item::Foresight)] == 0, "a new run does not inherit foresight");
    }
    {
        Run r; r.Start(42, 50); r.id = "attempt-a"; r.floor = 7; r.jade = 150;
        check(r.ForcedFloor({3}) == 1 && r.floor == 8, "fixture entered boss floor");
        const FailedStage failure{r.id, 7, r.floor, 0, 0, false, r.bosses};
        r.UndoFloor();
        check(r.floor == 7 && r.phase == Phase::Ready && r.jade == 150, "failed encounter rolls back without awarding a floor");
        r.jade = 110; // Spending in preparation must not be undone by a later manual skip.
        check(failure.Available(r) && failure.RestoreBoss(r) && r.floor == 8 && r.jade == 110 && r.bosses == std::vector<int>{3}, "confirmed skip restores only the failed boss identity");
        r.UndoFloor(); r.id = "another-run";
        check(!failure.Available(r) && !failure.RestoreBoss(r), "stale failure cannot skip a different run");
        r.id = "attempt-a"; ++r.floor;
        check(!failure.Available(r), "stale failure cannot skip another floor");
        const FailedStage wave{"attempt-a", r.floor, r.floor, r.floor + 1, 2, true, {}};
        check(wave.Available(r) && !wave.RestoreBoss(r), "wave failure cannot be restored as a boss");
    }
    {
        // Reproduce the user-visible 82 -> 83 route without depending on a
        // naturally occurring shuffle: find a seed that puts relay 44 at 42.
        uint64_t seed = 1;
        while (seed < 10000 && floorplay::EncounterFloor(seed, 42, true) != 44) ++seed;
        check(seed < 10000, "found a deterministic shuffle for the three-form encounter at boss 42");
        const auto plan = floorplay::ScheduledFloor(seed, 42, true);
        Run r; r.Start(seed, 50); r.id = "relay-83"; r.floor = 41; r.jade = 250;
        const int chaos = floorplay::PoolIndex(plan.bosses[1]);
        const auto denied = floorplay::Resolve(plan, [chaos](int b) { return b != chaos; }, floorplay::ShownBoss(42, 50));
        check(!denied.ok && denied.why.find("第 83 层") != std::string::npos, "denied next encounter reports displayed floor 83");
        const auto failed = FailedStage::Planned(r, plan);
        check(failed.Available(r) && failed.bosses.size() == 3 && r.floor == 41 && r.jade == 250,
              "preflight failure offers an explicit skip without changing progress or jade");
        Run other = r; other.id = "other";
        check(!failed.Available(other) && !failed.RestoreBoss(other), "a preflight failure cannot affect another run");
        check(failed.RestoreBoss(r) && r.floor == 42 && r.jade == 250, "confirmed skip can restore exactly the failed encounter");
        r.UndoFloor();
        check(r.floor == 41 && r.phase == Phase::Ready && r.jade == 250, "cancelled skip leaves the run and jade intact");
        auto invalidPlan = plan; invalidPlan.bosses[1] = "/missing/class";
        check(!FailedStage::Planned(r, invalidPlan).Available(r), "unknown encounter identity is never invented for a skip");
    }
    check(entry::ActorReady(true, true, true, true), "dead wave mob does not block the other mobs from starting");
    check(!entry::ActorReady(true, false, false, true), "wave mob whose attributes are uninitialized still blocks entry");
    check(entry::ActorReady(true, false, true, false), "living normalized wave mob is ready");
    check(!entry::ActorReady(false, true, true, true), "boss readiness keeps its existing phase protection");
    {
        arena::Boundary b;
        b.ArmHull({{-1000,-1000,0}, {1000,-1000,0}, {1000,1000,0}, {-1000,1000,0}}, {0,0,0}, -1000, 1000);
        check(std::abs(b.InnerRadius(250, 1400) - 750) < .01, "spawn search stays inside all convex edges with a margin");
        b.Arm({0,0,0}, 3500);
        check(b.InnerRadius(250, 1400) == 1400, "circular search radius is capped near the stable center");
    }
    // These exact aliases kept protection on after an otherwise correct original-save restore.
    json::Value map;
    check(json::Parse(R"({"MoJian":{"ShaChiWeaponLevel":5},"NiTouche":{"ShaChiWeaponLevel":1},"Kuye":{"CurrentLevel":0}})", &map), "alias fixture parses");
    auto sword = ids::Find(&map, "Mojian"), cart = ids::Find(&map, "NiTouChe"), med = ids::Find(&map, "KuYe");
    check(sword.value && !sword.ambiguous && sword.value->Find("ShaChiWeaponLevel")->Int() == 5, "MoJian native FName spelling preserves actual level");
    check(cart.value && med.value, "NiTouche and Kuye aliases restore");
    check(!ids::Find(&map, "Mo_Jian").value, "different identifiers never merge");
    check(!map.Find("mojian"), "ordinary JSON schema lookup stays case sensitive");
    check(sword.value && sword.value->Find("ShaChiWeaponLevel")->Int() != 4, "a true level mismatch remains a mismatch");
    map.fields.emplace_back("moJIAN", json::Value::Obj());
    const auto duplicate = ids::Find(&map, "MoJian");
    check(duplicate.ambiguous && !duplicate.value, "case-colliding IDs fail closed even with an exact match");
    check(ids::Equal(std::vector<std::string>{"Tang", "MoJian"}, std::vector<std::string>{"tang", "Mojian"}), "slot spelling follows FName semantics");
    check(!ids::Equal(std::vector<std::string>{"Tang", "MoJian"}, std::vector<std::string>{"MoJian", "Tang"}), "slot order still matters");
    check(growth::WeaponIndex("MoJian", 1) == growth::WeaponIndex("Mojian", 1) && growth::WeaponIndex("Mojian", 1) >= 0, "growth bookkeeping accepts native weapon spelling");
    check(growth::MedicineIndex("Kuye") == growth::MedicineIndex("KuYe") && growth::MedicineIndex("KuYe") >= 0, "growth bookkeeping accepts native medicine spelling");

    {
        entry::Gate quick;
        quick.Begin(1000, true);
        check(quick.Step(1100, true) == 0 && quick.Holding(), "wave entry still waits a few frames for the top-up");
        check(quick.Step(1351, true) == 0 && quick.state == entry::State::Running, "wave entry starts without a countdown");
    }
    entry::Gate gate;
    check(!gate.Holding(), "idle never freezes"); gate.Begin(100);
    check(gate.Holding() && gate.Step(20000, false) == 0 && gate.state == entry::State::Waiting, "elapsed loading time never starts combat");
    check(gate.Step(19201, true) == 0 && gate.state == entry::State::Waiting, "first ready frame only starts settling");
    check(gate.Step(20000, true) == 0 && gate.state == entry::State::Waiting, "ready must hold for the settle window");
    check(gate.Step(20001, true) == 3 && gate.state == entry::State::Countdown, "3 begins only after player, boss, loading and visible screen stay ready");
    check(gate.Step(21000, true) == 3, "full first second is frozen");
    check(gate.Step(21001, true) == 2 && gate.Step(22001, true) == 1, "visible 3/2/1 sequence");
    check(gate.Step(23000, true) == 1 && gate.Holding(), "does not release a frame early");
    check(gate.Step(23001, true) == 0 && gate.state == entry::State::Running && !gate.Holding(), "release after three complete seconds");
    gate.Begin(30000); gate.Step(30010, true); gate.Step(31000, true); gate.Step(32000, false);
    check(gate.Step(35000, true) == 0 && gate.Step(35800, true) == 3, "new loading/pause resets countdown and settle instead of completing behind it");
    gate.Begin(40000); gate.Step(40000, true); gate.Step(40500, false); gate.Step(40600, true);
    check(gate.Step(41300, true) == 0 && gate.Step(41400, true) == 3, "a cover flicker restarts the settle window");
    // 身份匹配按完整片段（清场：别的 Boss / 同身份替身）
    check(tower::IdentityIn("BP_Monster_ChiLian_P2_C", "ChiLian") && tower::IdentityIn("BP_NPC_MengHua_C", "MengHua"), "boss identity matches whole name segments");
    check(!tower::IdentityIn("BP_Monster_YuCong_HunDun_C", "Hun") && tower::IdentityIn("BP_Hun_C", "Hun"), "a short identity never matches inside another word (Hun vs HunDun)");
    check(tower::IdentityIn("BP_Monster_AHua_GuiTouDao_C", "AHua_GuiTouDao") && !tower::IdentityIn("BP_Monster_X_C", ""), "multi-part identities match; empty never matches");
    // 剧情冻结规则（313987b 白赤练：Flow 图放的剧情被旧的名字 / 设置位判据漏掉）
    check(towerisolate::IsStory(true, true, false, false, false), "a flow-driven sequence is story even if it was already playing when first seen");
    check(towerisolate::IsStory(false, false, false, false, false), "a non-looping sequence that starts during the floor is story");
    check(!towerisolate::IsStory(false, false, true, false, false) && !towerisolate::IsStory(false, true, false, false, false), "looping or pre-existing ambient sequences are left alone");
    check(!towerisolate::IsStory(true, false, false, true, false), "a tower boss's own performance (throws, phases) is never skipped");
    check(!towerisolate::IsStory(true, false, false, false, true), "the native teleport performance while travelling is never skipped");
    gate.Begin(50000); gate.Step(50000, true); check(gate.Step(50800, true) == 3, "fixture: countdown started");
    gate.Restart(51580);  // 补满耗时 780ms（313987b 实测 3→2 只隔 0.22 秒）
    check(gate.Step(52000, true) == 3 && gate.Step(52579, true) == 3 && gate.Step(52580, true) == 2, "a slow top-up inside the first tick no longer shortens the visible 3");
    check(gate.Step(54579, true) == 1 && gate.Step(54580, true) == 0 && gate.state == entry::State::Running, "after a restart the countdown still runs exactly three seconds");
    gate.Reset(); check(!gate.Holding() && gate.state == entry::State::Idle, "stop clears an unfinished gate");
    gate.Begin(1); gate.Step(90001, false);
    check(gate.state == entry::State::Failed && !gate.Holding(), "missing readiness times out and releases rather than fighting blind");

    check(save::IsSoulMaterial("TU_19003") && save::IsSoulMaterial("tu_0701") && !save::IsSoulMaterial("TU_") && !save::IsSoulMaterial("Weapon_Material_01"), "weapon-specific soul materials are recognised, upgrade materials are not");
    check(!save::IsUpgradeMaterial("TU_19003"), "soul materials never fall into the upgrade-material blanket answer");
    points::Flight tx;
    const auto first = tx.Begin(); check(first && !tx.Begin(), "reentrant native hold event cannot double charge");
    check(tx.Finish(first) && !tx.Finish(first), "one readback can settle only once");
    const auto second = tx.Begin(); check(second != first && !tx.Finish(first) && tx.pending, "late callback cannot settle the next upgrade");
    tx.Reset(); const auto third = tx.Begin();
    check(!tx.Finish(second) && tx.pending && tx.Finish(third), "exit/restart invalidates pending callbacks without touching the next session");
    const uint64_t loadout[]{10, 20}, replaceFirst[]{30, 20}, replaceSecond[]{10, 40}, unrelated[]{30, 40}, incomplete[]{10};
    check(arsenal::ReplacedSlot(loadout, replaceFirst) == 0 && arsenal::ReplacedSlot(loadout, replaceSecond) == 1,
          "native confirmation identifies the one original slot to restore");
    check(arsenal::ReplacedSlot(loadout, loadout) == -1 && arsenal::ReplacedSlot(loadout, unrelated) == -1 &&
          arsenal::ReplacedSlot(loadout, incomplete) == -1, "cancel, external loadout changes and incomplete snapshots are not extra-slot choices");
    auto p = growth::Profile::Kit(); p.TrialKit();
    const int w = growth::WeaponIndex("Tang", 0);
    const int pointsBefore = p.temper;
    check(p.Upgrade(w) == growth::Err::Ok && p.temper == pointsBefore - 1, "native entry can account exactly one upgrade");
    p.soulTokens = 0;
    check(p.Snap(w) != growth::Err::Ok && p.soulTokens == 0, "no soul token means no soul transaction");
    std::printf("TOWER_FLOW_%s %d checks (no game process)\n", failed ? "FAILED" : "OK", n);
    return failed ? 1 : 0;
}
