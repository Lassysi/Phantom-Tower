#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string_view>
#include "../src/tower_special.h"
#include "../src/tower_logic.h"
static void Check(bool ok, const char* why) { if (!ok) { std::printf("FAIL %s\n", why); std::exit(1); } }
int main() {
    using namespace tower::special;
    for (uint64_t seed = 1; seed <= 300; ++seed) {
        const auto plan = MainPlan(seed);
        Check(plan.size() == 8, "main run: exactly 8 special floors");
        std::map<Kind, int> n;
        int prevF = 0, survivals = 0;
        for (const auto& [F, k] : plan) {
            Check(2 * F - 2 >= 50 && F <= 50, "only mob stages shown >= 50");
            Check(F > prevF, "distinct floors, sorted");
            prevF = F; ++n[k];
            const auto f = MainFloor(seed, F);
            Check(f.kind == k, "MainFloor agrees with plan");
            if (k == Kind::Survival) { ++survivals; Check(f.survivalSec == (survivals == 1 ? 90 : 120), "first survival 1:30, second 2:00"); }
            else Check(f.survivalSec == 0, "only survival has a timer");
        }
        Check(n[Kind::Duel] == 1 && n[Kind::Outnumbered] == 1 && n[Kind::Melee] == 2 && n[Kind::Team] == 2 && n[Kind::Survival] == 2, "quota 1/1/2/2/2");
        {   // 4.1.0：前 50 层 5 个创意层，F 3–25 分 5 段各一个，5 种各一次；前期规则（early、生存 1:00）
            const auto early = EarlyPlan(seed);
            Check(early.size() == 5, "4.1.0 early: exactly 5 special floors before shown 50");
            std::map<Kind, int> ek; int prev = 0, band = 0;
            for (const auto& [F, k] : early) {
                const int lo = kEarlyFirstF + 23 * band / 5, hi = kEarlyFirstF + 23 * (band + 1) / 5 - 1;
                Check(F >= lo && F <= hi && F > prev && 2 * F - 2 < 50 && F >= 3, "4.1.0 early: one per band, spread out, mob stages shown 4-48");
                prev = F; ++band; ++ek[k];
                const auto f = MainFloor(seed, F);
                Check(f.kind == k && f.early && f.survivalSec == (k == Kind::Survival ? 60 : 0), "4.1.0 early: MainFloor marks early, survival 1:00");
            }
            Check(ek.size() == 5, "4.1.0 early: all five kinds once");
            int earlyCount = 0; for (int F = 2; F < 26; ++F) earlyCount += MainFloor(seed, F).kind != Kind::None;
            Check(earlyCount == 5 && MainFloor(seed, 2).kind == Kind::None, "4.1.0 early: no other early stage converted, first stage never");
            for (const auto& [F, k] : plan) Check(!MainFloor(seed, F).early, "4.1.0: shown 50+ specials are not early");
        }
    }
    Check(MainPlan(42) == MainPlan(42), "same seed, same plan");
    {   // 无尽：约 8%，不连续
        int hits = 0, total = 0;
        for (uint64_t seed = 1; seed <= 200; ++seed) {
            Floor prev{};
            for (int fl = 51; fl <= 250; ++fl) {
                const auto f = EndlessFloor(seed, fl);
                ++total; hits += f.kind != Kind::None;
                Check(!(f.kind != Kind::None && prev.kind != Kind::None), "endless specials never back to back");
                if (f.kind == Kind::Survival) Check(f.survivalSec == 90 || f.survivalSec == 120, "endless survival duration");
                prev = f;
            }
            Check(EndlessFloor(seed, 50).kind == Kind::None, "endless only above floor 50");
        }
        const double pct = 100.0 * hits / total;
        Check(pct > 5.0 && pct < 9.0, "endless rate ~8% (minus back-to-back)");
        std::printf("info: endless special rate %.1f%%\n", pct);
    }
    {   // 阵容
        const auto m = LineupOf(Kind::Melee), s = LineupOf(Kind::Survival), t = LineupOf(Kind::Team);
        Check(m.enemyBosses == 2 && m.enemy2Bosses == 2 && m.allyBosses == 1, "melee 1+player vs 2 vs 2");
        Check(s.enemyBosses == 4 && s.allyBosses == 1 && s.unkillable, "survival: 4 unkillable vs player + allies");
        Check(t.enemyBosses == 3 && t.allyBosses == 2, "team 3v3");
        Check(AllyShare(Kind::Melee, 60) == 0.30f && AllyShare(Kind::Team, 60) == 0.30f, "ally share 30% outside survival");
        Check(std::abs(SurvivalAllyShare(50) - 0.10f) < 1e-5f && std::abs(SurvivalAllyShare(98) - 0.15f) < 1e-5f && std::abs(SurvivalAllyShare(150) - 0.15f) < 1e-5f &&
              SurvivalAllyShare(74) > 0.12f && SurvivalAllyShare(74) < 0.13f && AllyShare(Kind::Survival, 50) == SurvivalAllyShare(50), "survival allies 10% -> 15% with floor");
        int ones = 0, twos = 0;
        for (uint64_t seed = 1; seed <= 400; ++seed) for (Kind k : {Kind::Team, Kind::Survival}) { const int c = AllyCount(seed, 30, k); Check(c == 1 || c == 2, "ally bosses 1-2"); (c == 1 ? ones : twos)++; }
        Check(ones > 250 && twos > 250, "ally count roughly half 1 / half 2");
        Check(AllyCount(1, 30, Kind::Melee) == 1 && AllyCount(99, 31, Kind::Melee) == 1, "melee: exactly 1 boss ally");
        Check(AllyCount(5, 30, Kind::Duel) == 0 && AllyCount(5, 30, Kind::Outnumbered) == 0, "duel / outnumbered have no boss ally");
        Check(kSurvivalAllyTaken > 1.0f && kSurvivalEnemyHp >= 10000.0 && kSurvivalRefillAt >= 0.9f && kSurvivalEnemyAtk == 2.0 && SurvivalAllyBanned("gold_mask_guard") && SurvivalAllyBanned("GuWanYue") && SurvivalAllyBanned("YanQianSui") && !SurvivalAllyBanned("LeiDang"), "survival: enemy attack x2, never-dying bosses banned as allies");
        Check(HudNote(Kind::Melee) == "混战" && HudNote(Kind::Survival) == "生存时长" && HudNote(Kind::Duel).empty(), "HUD notes");
    }
    {   // 抽 Boss：身份不重复、solo 只单出
        std::vector<int> pool{0, 1, 2, 3, 4, 5, 6, 7};
        auto id = [](int b) { return b / 2; };   // 0/1 同身份，2/3 同身份…
        auto solo = [](int b) { return b == 7; };
        for (uint64_t seed = 1; seed <= 100; ++seed) {
            const auto got = PickBosses(seed, 30, 4, pool, id, solo);
            Check(got.size() == 4, "four distinct identities available");
            for (size_t i = 0; i < got.size(); ++i) { Check(got[i] != 7, "solo-only boss never in a multi-boss lineup"); for (size_t j = 0; j < i; ++j) Check(id(got[i]) != id(got[j]), "no duplicate identity"); }
        }
        Check(PickBosses(3, 30, 9, pool, id, solo).size() == 4, "pool runs dry gracefully (4 identities; the solo variant is skipped, its twin is used)");
    }
    {   // 4.0.0 老局已过 50 层：剩余小兵段按比例放创意层
        Check(ScaledCount(1) == 8 && ScaledCount(26) == 8 && ScaledCount(37) == 4 && ScaledCount(31) == 6 && ScaledCount(49) == 1 && ScaledCount(50) == 0 && ScaledCount(51) == 0,
              "4.0.0 scaled count = round(8 * left / 25)");
        for (uint64_t seed = 1; seed <= 300; ++seed) {
            Check(MainPlan(seed, 1) == MainPlan(seed) && MainPlan(seed, 26) == MainPlan(seed) && MainPlan(seed, 10) == MainPlan(seed), "4.0.0 new / early runs: plan unchanged from 3.7.0");
            for (int from = 27; from <= 51; ++from) {
                const auto plan = MainPlan(seed, from);
                Check(static_cast<int>(plan.size()) == ScaledCount(from), "4.0.0 old run: count follows remaining share");
                std::map<Kind, int> n; int survivals = 0;
                for (const auto& [F, k] : plan) {
                    Check(F >= from && F <= 50, "4.0.0 old run: only mob stages not yet played");
                    ++n[k];
                    const auto f = MainFloor(seed, F, from);
                    Check(f.kind == k, "4.0.0 MainFloor(from) agrees with plan");
                    if (k == Kind::Survival) { ++survivals; Check(f.survivalSec == (survivals == 1 ? 90 : 120), "4.0.0 survival timers keep order"); }
                }
                if (plan.size() <= 5) for (auto& [k, c] : n) Check(c == 1, "4.0.0 old run: kinds distinct while <= 5");
                for (int F = 2; F < from; ++F) Check(MainFloor(seed, F, from).kind == Kind::None, "4.0.0 old run: nothing before from");
            }
        }
        Check(FromProgress(10, 10, 3) == 1 && FromProgress(24, 25, 0) == 1 && FromProgress(25, 26, 2) == 27 && FromProgress(35, 36, 0) == 36 &&
              FromProgress(35, 36, 1) == 37 && FromProgress(60, 0, 0) == 61, "4.0.0 FromProgress: next unplayed mob stage, early runs keep full plan");
        // 局存档：没有 spfrom 的老局按进度算；新局写 spfrom=1；往返不变
        tower::Run fresh; fresh.Start(12345, 50);
        Check(fresh.specialFrom == 1, "4.0.0 new run starts with the full plan");
        auto text = fresh.Save();
        Check(text.find("spfrom=1\n") != std::string::npos, "4.0.0 save writes spfrom");
        tower::Run back; Check(back.Load(text) && back.specialFrom == 1, "4.0.0 round trip keeps spfrom");
        tower::Run old = fresh; old.floor = 35; old.mobFloor = 36; old.mobWave = 1;
        auto oldText = old.Save();
        const auto at = oldText.find("spfrom=1\n"); oldText.erase(at, 9);
        tower::Run oldBack; Check(oldBack.Load(oldText) && oldBack.specialFrom == 37, "4.0.0 pre-4.0 save past 50: from = next unplayed stage");
        old.floor = 12; old.mobFloor = 12; old.mobWave = 3; oldText = old.Save(); oldText.erase(oldText.find("spfrom=1\n"), 9);
        Check(oldBack.Load(oldText) && oldBack.specialFrom == 1, "4.0.0 pre-4.0 save before 50: full plan");
        auto again = oldBack.Save(); Check(again.find("spfrom=1\n") != std::string::npos, "4.0.0 resolved from is saved");
    }
    std::puts("TOWER_SPECIAL_OK");
}
