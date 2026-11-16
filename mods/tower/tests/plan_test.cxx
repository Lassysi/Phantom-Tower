// 50 层方案 / 场地 / 层型逻辑离线测试（不连游戏）
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>

#include "../src/tower_arena.h"
#include "../src/tower_fate.h"
#include "../src/tower_floorplay.h"
#include "../src/tower_mobs.h"
#include "../src/tower_endless.h"
#include "../src/tower_endless_venues.h"
#include "../src/tower_wall.h"
#include "../src/tower_venue_real.h"
#include "../src/tower_input_gate.h"
#include "../src/tower_isolation.h"
#include "../src/tower_reach.h"
#include "../src/tower_replay.h"
#include "../src/tower_save.h"
#include "../src/tower_logic.h"
#include "../src/venues.generated.h"

static int g_checks = 0, g_failed = 0;
static void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_failed; std::printf("FAIL %s\n", what); }
}

using namespace tower;
using namespace tower::floorplay;

int main() {
    // ---- 显示层号：Boss 2F-1，波次段 2F-2，隐藏层 100；短局不变
    Check(tower::floorplay::ShownBoss(1, 50) == 1 && tower::floorplay::ShownMobs(2, 50) == 2 && tower::floorplay::ShownBoss(2, 50) == 3 && tower::floorplay::ShownMobs(3, 50) == 4, "显示层号：前几层连排");
    Check(tower::floorplay::ShownBoss(50, 50) == 99 && tower::floorplay::ShownBoss(51, 51) == 100 && tower::floorplay::ShownTotal(50) == 100, "显示层号：最后两层和总数");
    Check(tower::floorplay::ShownBoss(0, 50) == 0 && tower::floorplay::ShownBoss(3, 3) == 3 && tower::floorplay::ShownMobs(3, 3) == 3 && tower::floorplay::ShownTotal(3) == 3, "显示层号：开局和短局");
    {
        bool valid = true, varies = false, replayMoved = false;
        for (uint64_t seed = 1; seed <= 200; ++seed) {
            for (int group = 0; group < 5; ++group) {
                const int first = 1 + group * 10, count = group == 4 ? 9 : 10;
                std::array<bool, 10> seen{};
                for (int i = 0; i < count; ++i) {
                    const int floor = first + i, source = EncounterFloor(seed, floor, true);
                    if (source < first || source >= first + count) { valid = false; continue; }
                    if (seen[source - first]) valid = false;
                    seen[source - first] = true;
                    const auto assigned = ScheduledFloor(seed, floor, true);
                    const auto* original = Find(source); const auto* target = Find(floor);
                    valid = valid && assigned.floor == floor && assigned.bosses[0] == original->bosses[0] && assigned.replay == original->replay &&
                            assigned.venue == original->venue && std::abs(assigned.hpTarget / assigned.hpShare - target->hpTarget / target->hpShare * RampFactor(floor, true)) < 0.01 &&
                            std::abs(assigned.dmgMul / assigned.dmgShare - target->dmgMul / target->dmgShare * RampFactor(floor, false)) < 0.00001;
                    valid = valid && assigned.mode == original->mode && assigned.count == original->count && assigned.altN == original->altN &&
                            assigned.allyN == original->allyN && assigned.par == original->par &&
                            assigned.hpShare == (Relay(original->mode) ? 1.0 : original->hpShare) && assigned.dmgShare == (Relay(original->mode) ? 1.0 : original->dmgShare);
                    for (int k = 0; k < 3; ++k) valid = valid && assigned.bosses[k] == original->bosses[k] && assigned.allies[k] == original->allies[k];
                    for (int k = 0; k < 2; ++k) valid = valid && assigned.alts[k] == original->alts[k];
                    varies = varies || source != floor;
                    replayMoved = replayMoved || (source != floor && !original->replay.empty());
                }
            }
            valid = valid && EncounterFloor(seed, 50, true) == 50 && EncounterFloor(seed, 51, true) == 51 && EncounterFloor(seed, 10, false) == 10;
            const auto challenges = Challenges(seed, 50, 0, true);
            for (int f = 1; f <= 50; ++f) if (challenges[f] != Challenge::None) valid = valid && ChallengeFloor(f, seed, true);
        }
        Check(valid && varies && replayMoved, "random encounters: stable group permutations, target-floor scaling, replay/venue preserved, final and hidden fixed");
        {   // 用户 2026-11-12：新方案 20 / 20 / 9 + 第 50 固定；白赤练 / 拙壁不进前 10，烟千岁 / 骨婉约不进前 30，混沌车轮在它俩之前
            bool wide = true, spread = false; int chaosEarly = 0;
            for (uint64_t seed = 1; seed <= 3000; ++seed) {
                std::array<int, 51> at{};
                for (int f = 1; f <= 50; ++f) {
                    const int e = EncounterFloor(seed, f, kShuffleWide);
                    if (e < 1 || e > 50 || at[e]) { wide = false; continue; }
                    at[e] = f;
                }
                for (int e = 1; e <= 50; ++e) wide = wide && at[e] > 0;
                for (int e = 1; e <= 20; ++e) wide = wide && at[e] <= 20;
                for (int e : {21, 22, 23, 24, 25, 26, 27, 28, 29, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 44}) wide = wide && at[e] >= 21 && at[e] <= 40;
                for (int e : {30, 41, 42, 43, 45, 46, 47, 48, 49}) wide = wide && at[e] >= 41 && at[e] <= 49;
                wide = wide && at[50] == 50 && at[8] > 10 && at[9] > 10 && at[25] > 30 && at[29] > 30 && at[44] < at[25] && at[44] < at[29];
                spread = spread || at[20] <= 10 || at[1] > 10;
                chaosEarly += at[44] <= 30;
                // 旧存档方案 1 不变
                wide = wide && EncounterFloor(seed, 18, kShuffleTens) >= 11 && EncounterFloor(seed, 18, kShuffleTens) <= 20;
            }
            Check(wide && spread && chaosEarly > 0, "wide shuffle: 20/20/9 groups, early-floor limits, chaos relay before Yan Qiansui and Gu Wanyue");
            bool late = true;
            for (uint64_t seed = 1; seed <= 3000; ++seed) {
                std::array<int, 51> at{};
                for (int f = 1; f <= 50; ++f) { const int e = EncounterFloor(seed, f, kShuffleLate); if (e < 1 || e > 50 || at[e]) late = false; else at[e] = f; }
                for (int e = 1; e <= 50; ++e) late = late && at[e] > 0;
                for (int e : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13, 14, 15, 16, 18, 21, 22, 23, 24}) late = late && at[e] <= 20;
                late = late && at[50] == 50 && at[8] > 10 && at[9] > 10 && at[25] > 30 && at[29] > 30 && at[44] < at[25] && at[44] < at[29];
                late = late && at[17] > 30 && at[17] <= 40 && at[20] > 30 && at[20] <= 40 && at[28] > 30 && at[28] <= 40 && at[11] > 30 && at[11] <= 40 && at[19] > 30 && at[19] <= 40;  // 显示 61–79 层
            }
            Check(late, "3.0.1 shuffle: Huo Liuxing / A Jin / Da Shixiong not before encounter 31 (shown 61)");
            Run fresh; fresh.Start(7, 50);
            Run back; Check(fresh.shuffleBosses == kShuffleLater && back.Load(fresh.Save()) && back.shuffleBosses == kShuffleLater, "3.4.3: new runs save and reload shuffle 4");
            { Run l3; l3.Start(7, 50); l3.shuffleBosses = kShuffleLate; Run l3Back; Check(l3Back.Load(l3.Save()) && l3Back.shuffleBosses == kShuffleLate, "3.0.1 runs keep shuffle 3 after reload"); }
            bool later = true;
            for (uint64_t seed = 1; seed <= 3000; ++seed) {
                std::array<int, 51> at{};
                for (int f = 1; f <= 50; ++f) { const int e = EncounterFloor(seed, f, kShuffleLater); if (e < 1 || e > 50 || at[e]) later = false; else at[e] = f; }
                for (int e = 1; e <= 50; ++e) later = later && at[e] > 0;
                for (int e : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13, 14, 15, 16, 18, 21, 23, 26, 27}) later = later && at[e] <= 20;
                later = later && at[50] == 50 && at[22] > 30 && at[22] <= 40 && at[24] > 30 && at[24] <= 40 && at[17] > 30 && at[20] > 30 && at[28] > 30 && at[11] > 30 && at[19] > 30;
            }
            Check(later, "3.4.3 shuffle 4: You Long / Zhu Yuan not before encounter 31 (shown 61); Wan Jun / Yu Bi move to group 1");
            Check(EncounterFloor(7, 18, kShuffleLate) == EncounterFloor(7, 18, kShuffleLate), "shuffle 3 unchanged for existing runs");
            Run w2; w2.Start(7, 50); w2.shuffleBosses = kShuffleWide; Run w2Back;
            Check(w2Back.Load(w2.Save()) && w2Back.shuffleBosses == kShuffleWide, "3.0.0 runs keep the wide shuffle after reload");
            Run old; old.Start(7, 50); old.shuffleBosses = kShuffleTens; Run oldBack;
            Check(oldBack.Load(old.Save()) && oldBack.shuffleBosses == kShuffleTens, "old runs keep the ten-floor shuffle after reload");
        }
        bool ahuaFirst = false;
        for (uint64_t seed = 1; seed <= 10000 && !ahuaFirst; ++seed) if (EncounterFloor(seed, 1, true) == 3) {
            const auto a = ScheduledFloor(seed, 1, true);
            ahuaFirst = a.bosses[0] == Find(3)->bosses[0] && std::abs(a.hpTarget - Find(1)->hpTarget) < 0.01;
        }
        Check(ahuaFirst, "random encounters: Ahua can appear first with floor-one health");
        const auto* last = Find(50);
        const std::vector<std::string_view> finalPaths{last->bosses[0], last->bosses[1]};
        const int finalWeapon = FinalBossWeapon(50, finalPaths);
        Check(finalWeapon >= 0 && kWeaponDrops[finalWeapon].weapon == "ZiMingJian", "final Zuoshang grants Zi Ming sword before hidden stage");
        Check(FinalBossWeapon(49, finalPaths) < 0 && FinalBossWeapon(50, {Find(1)->bosses[0]}) < 0, "final weapon requires actual final-floor Zuoshang defeat");
        Run rebirth; rebirth.Start(96, 50); rebirth.floor = 20; rebirth.peak = 19; rebirth.shopFloor = 20;
        const std::string_view ahua = Find(3)->bosses[0];
        rebirth.RecordShopDrops({ahua});
        const auto unlocked = rebirth.shopDrops;
        const auto beforeOrder = EncounterFloor(rebirth.seed, 18, rebirth.shuffleBosses);
        rebirth.RewindForRebirth(14, 0);
        Run after; const bool loaded = after.Load(rebirth.Save());
        Check(loaded && after.floor == 14 && after.peak == 19 && after.shopFloor == 20 && after.seed == 96 && after.shuffleBosses && after.shopDrops == unlocked,
              "rebirth and reload preserve random seed, achieved peak and previously defeated boss weapons");
        Check(after.shuffleBosses == kShuffleLater && EncounterFloor(after.seed, 18, after.shuffleBosses) == beforeOrder && beforeOrder >= 1 && beforeOrder <= 27 && beforeOrder != 17 && beforeOrder != 20 && beforeOrder != 11 && beforeOrder != 19 && beforeOrder != 22 && beforeOrder != 24,
              "rebirth continues the same randomized 1-20 encounter order");
        Check(!after.CanOpenStageShop(15) && !after.CanOpenStageShop(19) && !after.CanOpenStageShop(20) && after.CanOpenStageShop(21),
              "rebirth does not reopen visited shops, including the shop before the death floor");
    }
    // ---- 刷怪失败换怪：同档、同区牌堆、不重复；没得换返回 -1
    {
        namespace m = tower::mobs;
        bool ok = true;
        for (int i = 0; i < static_cast<int>(std::size(m::kMobs)); ++i) {
            for (int zone = 1; zone <= 5; ++zone) {
                if (!(m::kMobs[i].elite ? m::InElitePool(m::kMobs[i].zone, zone) : m::InNormalPool(m::kMobs[i].zone, zone))) continue;
                const int r = m::Replacement(i, zone, [&](int j) { return j == i; });
                ok = ok && r != i && (r < 0 || m::kMobs[r].elite == m::kMobs[i].elite);
            }
        }
        Check(ok, "mobs: replacement stays in the same tier and never repeats the failed mob");
        Check(m::Replacement(0, 1, [](int) { return true; }) == -1, "mobs: no replacement when every mob is unusable");
    }
    // ---- 波次 1 / 2 / 3：全部 91 种都能出场，同一区相邻两波没有相同的怪
    {
        int waves = 0; bool adjacent = true; std::set<int> seen; std::vector<int> prev; int prevZone = 0;
        for (int F = 2; F <= 50; ++F) {
            const int zone = (F - 1) / 10 + 1, total = tower::mobs::WaveCount(F);
            for (int w = 0; w < total; ++w) {
                const auto picks = tower::mobs::Pick(20261108, F, w, zone, tower::mobs::Plan(F, w, total), [](int) { return false; });
                ++waves;
                if (zone != prevZone) prev.clear();
                for (int i : picks) { seen.insert(i); adjacent &= std::find(prev.begin(), prev.end(), i) == prev.end(); }
                prev = picks; prevZone = zone;
            }
        }
        Check(waves == 99 && tower::mobs::WaveCount(15) == 1 && tower::mobs::WaveCount(35) == 2 && tower::mobs::WaveCount(50) == 3, "波次：前 1 中 2 后 3，共 99 波");
        Check(adjacent, "波次：同一区相邻两波怪不重复");
        Check(seen.size() == std::size(tower::mobs::kMobs), "波次：全部怪物都会出场");
    }
    // ---- 重生符按显示层号退 10 层，波次层也算
    Check(RebirthDest(90) == 80 && RebirthBack(80) == 40 && RebirthToMobs(80), "重生符：第 90 层（波次段）回第 80 层（波次段）");
    Check(RebirthDest(25) == 15 && RebirthBack(15) == 7 && !RebirthToMobs(15), "重生符：第 25 层倒下回第 15 层 Boss");
    Check(RebirthDest(89) == 79 && RebirthBack(79) == 39 && !RebirthToMobs(79), "重生符：第 89 层（Boss）回第 79 层（Boss）");
    Check(RebirthDest(12) == 2 && RebirthBack(2) == 1 && RebirthToMobs(2), "重生符：第 12 层回第 2 层");
    Check(RebirthDest(11) == 1 && RebirthDest(5) == 1 && RebirthBack(1) == 0 && !RebirthToMobs(1), "重生符：前面倒下回第 1 层");
    // ---- 逐层表本身
    {
        int spice = 0, small = 0, early = 0, missing = 0;
        std::map<std::string_view, int> seen;
        std::set<int> floors;
        for (auto& f : floorplan::kFloors) {
            floors.insert(f.floor);
            for (int k = 0; k < f.count; ++k) missing += PoolIndex(f.bosses[k]) < 0;
            for (int k = 0; k < f.altN; ++k) missing += PoolIndex(f.alts[k]) < 0;
            for (int k = 0; k < f.allyN; ++k) missing += PoolIndex(f.allies[k]) < 0;
            if (f.mode != Mode::Bet) {
                std::set<std::string_view> ids;
                for (int k = 0; k < f.count; ++k) if (const int b = PoolIndex(f.bosses[k]); b >= 0) ids.insert(kBossPool[b].identity);
                for (auto i : ids) ++seen[i];
            }
            missing += PoolIndex(f.venue) < 0;
            if (f.mode != Mode::Solo && f.mode != Mode::SoloTimed && f.mode != Mode::Zone && f.mode != Mode::Replay && !Later(f.mode)) ++spice;  // 重放层是原生遭遇，不算调味层（与 floors_plan.py 一致）
            if (Later(f.mode)) ++small;
            if (f.zone <= 2 && ((f.mode == Mode::Duo && f.floor != 13) || f.mode == Mode::Trio || f.mode == Mode::SoloTimed || Later(f.mode))) ++early;
        }
        Check(floors.size() == 50 && *floors.begin() == 1 && *floors.rbegin() == 50, "plan: exactly floors 1..50");
        Check(missing == 0, "plan: every boss / backup / venue is a pool class");
        Check(spice <= 10, "plan: 1v2 / 1v3 / relay / brawl / final at most 10 floors (user 2026-11-01)");
        Check(early == 0, "plan: zones 1-2 have no 1v2, 1v3 or timed-challenge floors (except native group: guard pair 13; user 2026-11-03)");
        Check(small <= 3, "plan: team / bet small modes at most 3 floors");
        bool once = true;
        for (auto& [id, n] : seen) once = once && n <= (id == "gold_mask_guard" || id == "YuCong" ? 2 : 1);  // 玉琮：30 层初见战 + 44 层三形态（用户 2026-11-05）
        Check(once, "plan: every identity fought once (gold-mask group at most twice; bet floors not counted)");
        int classics = 0;
        for (auto p : floorplan::kZoneClassic) classics += PoolIndex(p) >= 0;
        Check(classics == 5, "plan: each zone has a classic fallback venue");
    }
    // ---- 解析：主选可用 / 换备选 / 无备选
    {
        const auto& f1 = *Find(1);
        const auto all = Resolve(f1, [](int) { return true; });
        Check(all.ok && all.bosses.size() == 1 && all.bosses[0] == PoolIndex(f1.bosses[0]) && all.venue == PoolIndex(f1.venue), "resolve: usable main boss kept with its venue");
        const int main = PoolIndex(f1.bosses[0]);
        const auto swapped = Resolve(f1, [&](int b) { return b != main; });
        Check(swapped.ok && swapped.bosses[0] == PoolIndex(f1.alts[0]) && swapped.venue == swapped.bosses[0] && !swapped.swaps.empty(), "resolve: unusable main boss falls back to the backup and its venue");
        const auto& f13 = *Find(13);
        const auto none = Resolve(f13, [&](int b) { return b != PoolIndex(f13.bosses[1]); });
        Check(!none.ok && !none.why.empty(), "resolve: no backup -> the floor refuses with a reason");
        Check(Find(31)->mode == Mode::Relay && Find(44)->mode == Mode::Relay && Find(44)->count == 3 && Find(50)->mode == Mode::Final,
              "plan v12: relay floors where the plan puts them (44 = Yu Cong three forms one at a time, user 2026-11-03)");
        // v5 (after floors 1-32): simultaneous stacks without native group evidence are gone
        int together = 0;
        for (auto& f : floorplan::kFloors) together += Together(f.mode);
        Check(together == 1 && Find(13)->mode == Mode::Duo && Find(13)->count == 2,
              "plan v12: the only simultaneous floor is the native guard pair at 13");
        Check(Find(50)->mode == Mode::Final && Find(50)->count == 2 && Find(50)->bosses[0].find("ZuoShang_Ending") != std::string_view::npos &&
              Find(50)->bosses[1].find("ZuoShang_P2") != std::string_view::npos, "plan v10: tower lord = Zuo Shang true-ending pair (prosthetic body -> true dragon)");
        Check(floorplan::kPlanVersion == 14 && tower::kPlanVersion == floorplan::kPlanVersion, "plan v14: version constant matches the run save");
        Check(Find(48)->bosses[0].find("LengTu.") != std::string_view::npos && Find(49)->bosses[0].find("MoTian.") != std::string_view::npos &&
              Find(50)->bosses[0].find("ZuoShang_Ending") != std::string_view::npos && Find(14)->bosses[0].find("IronMaskMan_Boss") != std::string_view::npos &&
              Find(14)->mode == Mode::Solo && Find(38)->bosses[0].find("MuXiaoKui.") != std::string_view::npos && Find(38)->mode == Mode::Replay && Find(38)->replay == "muxiaokui",
              "plan v14: last three floors are Leng Tu / Mo Yuan / Zuo Shang; Iron Mask at 14, Mu Xiaokui two-phase replay at 38 (user 2026-11-05)");
        {   // 隐藏第 51 层：魔渊场地，魔渊 + 沐小葵二阶段 + 左殇真龙之躯 1v3
            const auto* h = Find(51);
            Check(h && h->mode == Mode::Trio && h->count == 3 && h->venue == h->bosses[0] && h->bosses[0].find("ZuoShang_P2") != std::string_view::npos &&
                  h->bosses[1].find("MoTian.") != std::string_view::npos && h->bosses[2].find("MuXiaoKui_P2") != std::string_view::npos && !ChallengeFloor(51),
                  "hidden floor 51: Zuo Shang dragon body's native arena (his AI needs it), + Mo Yuan + Mu Xiaokui P2, no challenge");
            Check(std::size(floorplan::kFloors) == 50, "hidden floor stays out of the 50-floor table");
        }
        {   // v12（用户 2026-11-03）：按流程排；去金面具；魔渊 49、左殇 50；双卫在沐小葵前
            bool gold = false;
            for (auto& f : floorplan::kFloors) for (int i = 0; i < f.count; ++i) gold = gold || kBossPool[PoolIndex(f.bosses[i])].identity == "gold_mask_guard";
            Check(!gold && Find(49)->bosses[0].find("MoTian.") != std::string_view::npos && Find(13)->mode == Mode::Duo && Find(14)->replay.empty(),
                  "plan v12: no gold-mask floor, Mo Yuan second to last, guard pair before Mu Xiaokui");
            Check(Find(43)->bosses[0].find("Scavenger.") != std::string_view::npos && Find(39)->bosses[0].find("QiShu_Bing.") != std::string_view::npos &&
                  Find(45)->bosses[0].find("Danghui.") != std::string_view::npos, "plan v12: revisit bosses (scavenger, Han Sui, Dang Hui) are late");
            bool timedOk = true;
            for (auto& f : floorplan::kFloors) if (f.mode == Mode::SoloTimed) timedOk = timedOk && f.count == 1 && f.replay.empty();
            Check(timedOk, "plan v12: timed floors are single spawned bosses (no replay / relay / group)");
        }
        {   // v11: 左殇两段在自己的原生终章战场，且只许原生场地（技能扭曲到原生锚点，18bb300 第 50 层飞远）
            const auto& f50 = *Find(50);
            const int lead = PoolIndex(f50.bosses[0]), dragon = PoolIndex(f50.bosses[1]);
            Check(f50.venue == f50.bosses[0] && !NativeOnly(lead).empty() && !NativeOnly(dragon).empty() && LandingBad(PoolIndex(f50.venue)).empty(),
                  "plan v11: floor 50 fights Zuo Shang at his own final arena, both phases native-only");
            const auto none = Resolve(f50, [&](int b) { return b != lead; });
            Check(!none.ok, "plan v11: no classic-venue fallback for floor 50 when the native arena is unusable");
        }
        const auto& f26 = *Find(19);
        const auto lion = Resolve(f26, [](int) { return true; });
        Check(f26.mode == Mode::Replay && f26.replay == "lion" && lion.ok && kBossPool[lion.bosses[0]].path.find("LionDancer.") != std::string_view::npos &&
              LandingBad(lion.venue).empty(), "replay: floor 19 is the lion native encounter, lead = phase one at its own (landable) arena");
        int replays = 0;
        for (auto& f : floorplan::kFloors) replays += (f.mode == Mode::Replay) == !f.replay.empty() ? (f.mode == Mode::Replay) : 100;
        Check(replays == 12, "replay: every replay floor names a contract and only replay floors do (1.0.1: floor 26 Wan Jun moved to native arena + spawn)");
        // v9: every replay floor resolves to a real contract whose phase one is the floor lead (venue = its native arena),
        // every phase class is a pool boss, allow / freeze lists never overlap, and non-interaction entries carry an exact node
        bool contracts = true;
        for (auto& f : floorplan::kFloors) {
            if (f.mode != Mode::Replay) continue;
            const auto* c = tower::replay::Find(f.replay);
            if (!c || (c->phaseN < 2 && !c->spawnerKick && c->entryAsset == std::array<uint32_t,4>{})) { contracts = false; continue; }  // 单阶段：原生刷怪点直刷（赤影骨刃 / 拙壁 / 阿瑾）或精确原生入场节点（万钧 Seq074）
            const std::string_view lead = f.bosses[0];
            contracts = contracts && lead.size() > c->phases[0].size() && lead.substr(lead.size() - c->phases[0].size()) == c->phases[0] && f.venue == f.bosses[0];
            for (int i = 0; i < c->phaseN; ++i) {
                bool inPool = false;
                for (auto& b : kBossPool) inPool = inPool || (b.path.size() > c->phases[i].size() && b.path.substr(b.path.size() - c->phases[i].size()) == c->phases[i]);
                contracts = contracts && inPool;
            }
            for (int i = 0; i < c->freezeN; ++i) contracts = contracts && !tower::replay::In(c->allowSeq, c->allowN, c->freezeSeq[i]);
            contracts = contracts && c->spawnerN == c->phaseN && (c->spawnerKick || (c->resetN > 0 && (c->interactN > 0 || c->entryAsset != std::array<uint32_t,4>{})));
        }
        Check(contracts, "replay: contracts match their floors (lead phase, pool classes, disjoint allow / freeze, exact entry)");
        const auto* mxk = tower::replay::Find("muxiaokui");
        Check(mxk && !tower::replay::AllowSequence(mxk, "Seq046-1") && !tower::replay::AllowSequence(mxk, "Seq047") && tower::replay::AllowSequence(mxk, "Seq046new") &&
              tower::replay::In(mxk->resetTags, mxk->resetN, "ljc.沐小葵.满足额外剧情条件"),
              "replay: Mu Xiaokui runs the plain two-phase path; the extra-story guard branch and the ending are frozen");
        {   // 34 层力者和锯者：一阶段原生刷两个同类角色，第二个并入本阶段而不是判"阶段重复"
            tower::replay::Progress pr; pr.Begin(2); pr.Seen(0, 1);
            Check(pr.CanJoin(0) && !pr.CanAdopt(0) && pr.CanAdopt(1), "replay: a second same-phase native actor joins the current phase");
            pr.Dead(0, 2);
            Check(!pr.CanJoin(0) && pr.CanAdopt(1), "replay: once the phase is dead an old-phase respawn no longer joins");
        }
        const auto bro = Resolve(*Find(28), [](int) { return true; });
        Check(bro.ok && Find(28)->mode == Mode::Replay && Find(28)->replay == "jianzhen" && kBossPool[bro.bosses[0]].path.find("HuaShan_Blue.") != std::string_view::npos &&
              DepsOf(bro.bosses[0]).empty(), "replay: floor 28 is the native seven-star formation encounter (formation members / puppets supplied natively)");
        Check(DepsOf(PoolIndex(Find(1)->bosses[0])).empty(), "deps: ordinary bosses have no dependency gate");
        Check(Find(2)->bosses[0].find("PengMeng") != std::string_view::npos && Find(49)->bosses[0].find("MoTian.") != std::string_view::npos, "plan: native order (Peng Meng early, Mo Yuan late)");
        int creative = 0;
        for (auto& f : floorplan::kFloors) creative += Later(f.mode) || f.allyN > 0;
        Check(creative == 0, "plan v12: no team / bet creative floors yet (user 2026-11-03)");
    }
    // ---- 车轮
    {
        RelayQueue q;
        q.Begin({1, 2, 3});
        Check(q.Pending() && q.index == 1 && q.total == 3, "relay: first boss out, two queued");
        Check(q.Dead(10.0) && q.Due(11.9) < 0, "relay: 2 s gap after the confirmed death");
        Check(q.Due(12.0) == 2 && q.index == 2 && q.Guarded(12.5) && !q.Guarded(13.1), "relay: next boss at 2 s with a 1 s entry guard");
        Check(q.Dead(20.0) && q.Due(22.0) == 3 && !q.Pending(), "relay: third boss");
        Check(!q.Dead(30.0), "relay: last boss down -> the floor settles");
        q.Clear();
        Check(!q.Pending() && q.total == 0, "relay: cancel / leave clears it (floor restarts)");
        Check(RetryRelaySpawn(true, 0, true) && RetryRelaySpawn(true, 2, true), "relay: failed followup permits three bounded retries after cleanup");
        Check(!RetryRelaySpawn(true, 3, true) && !RetryRelaySpawn(true, 0, false) && !RetryRelaySpawn(false, 0, true),
              "relay: exhausted, uncleaned or non-relay spawns never retry through this path");
    }
    // ---- 混战阵营
    {
        std::vector<int> b47 = {PoolIndex(Find(1)->bosses[0]), PoolIndex(Find(2)->bosses[0]), PoolIndex(Find(3)->bosses[0])};
        const auto s = Factions(b47);
        Check(s.size() == 3 && s[0] != s[1] && s[1] != s[2] && s[0] != s[2], "brawl: three bosses on three sides");
        int gold = -1;
        for (int i = 0; i < static_cast<int>(std::size(kBossPool)); ++i) if (kBossPool[i].identity == "gold_mask_guard") { gold = i; break; }
        const auto pair = Factions({gold, gold, b47[0]});
        Check(pair[0] == pair[1] && pair[1] != pair[2], "brawl: a same-identity pair fights on one side");
    }
    // ---- 附加挑战与奖励
    {
        const auto a = Challenges(42, 50), b = Challenges(42, 50), c = Challenges(43, 50);
        int n = 0;
        for (int f = 1; f <= 50; ++f) n += a[f] != Challenge::None;
        Check(a == b && a != c, "challenges: fixed by the run seed");
        int kinds[5] = {};
        for (int f = 1; f <= 50; ++f) ++kinds[static_cast<int>(a[f])];
        Check(n == 10 && kinds[static_cast<int>(Challenge::Berserk)] == 5 && kinds[static_cast<int>(Challenge::Timed)] == 2 &&
              kinds[static_cast<int>(Challenge::NoHit)] == 1 && kinds[static_cast<int>(Challenge::NoPotion)] == 2 && a[50] == Challenge::None,
              "challenges 1.0.1: ten per run (5 berserk-timed, 2 bonus-timed, 1 no-hit, 2 no-potion), never on the final floor");
        bool onlySolo = true, berserkLate = true;
        for (uint64_t seed = 1; seed <= 200; ++seed) {
            const auto x = Challenges(seed, 50);
            int count = 0;
            for (int f = 1; f <= 50; ++f) {
                if (x[f] == Challenge::None) continue;
                ++count;
                onlySolo = onlySolo && ChallengeFloor(f) && Find(f)->replay.empty() && Find(f)->count == 1;
                berserkLate = berserkLate && (x[f] != Challenge::Berserk || f >= 11);
            }
            onlySolo = onlySolo && count == 10;
        }
        Check(onlySolo && !ChallengeFloor(19) && !ChallengeFloor(13) && !ChallengeFloor(44) && !ChallengeFloor(50) && !ChallengeFloor(4),
              "challenges: only on single spawned 3/2/1 floors, never replay / relay / group, ten distinct floors");
        Check(berserkLate, "challenges: berserk time limits start from floor 11");
        const auto shortRun = Challenges(42, 3);
        Check(std::all_of(shortRun.begin(), shortRun.end(), [](Challenge x) { return x == Challenge::None; }), "challenges: none in trial runs");
        Fight clean{80, false, false}, hurt{80, true, false}, slow{301, false, false};
        Check(!BonusItem(Challenge::None, clean) && BonusItem(Challenge::NoHit, clean) && !BonusItem(Challenge::NoHit, hurt), "bonus: no-hit only when no Boss damage");
        Check(BonusItem(Challenge::NoPotion, hurt) && !BonusItem(Challenge::NoPotion, Fight{80, false, true}), "bonus: no-potion ignores hits, fails on potion use");
        Check(BonusItem(Challenge::Timed, clean) && !BonusItem(Challenge::Timed, slow), "bonus: timed challenge = inside 5:00");
        Check(!BonusItem(Challenge::Berserk, clean), "bonus: the berserk time limit itself carries no extra item");
        Check(Berserk(Challenge::Berserk, 181) && !Berserk(Challenge::Berserk, 179) && !Berserk(Challenge::Timed, 500) && !Berserk(Challenge::None, 500),
              "berserk only on the berserk challenge after 3:00; the bonus time limit never enrages");
        Check(ChallengeHud(Challenge::Berserk, clean, 60) == "限时挑战 2:00" && ChallengeHud(Challenge::Berserk, clean, 181) == "限时挑战　超时，Boss 狂暴" &&
              ChallengeHud(Challenge::Timed, clean, 0) == "额外挑战：限时 5:00" && ChallengeHud(Challenge::NoPotion, clean, 10) == "额外挑战：禁药" &&
              ChallengeHud(Challenge::NoHit, hurt, 10) == "额外挑战：无伤　未达成" && ChallengeHud(Challenge::None, clean, 10).empty(), "challenge HUD: short name, time and state");
        Check(Clock(75.3) == "01:15.3" && Clock(-1) == "--:--", "clock format");
        {   // 1.4.0 小怪波次
            namespace mb = tower::mobs;
            Check(mb::WaveCount(2) == 1 && mb::WaveCount(15) == 1 && mb::WaveCount(16) == 2 && mb::WaveCount(35) == 2 && mb::WaveCount(36) == 3 && mb::WaveCount(50) == 3, "mobs: 1 / 2 / 3 waves by stage");
            Check(!mb::HasStage(1, 50) && mb::HasStage(2, 50) && mb::HasStage(50, 50) && !mb::HasStage(51, 50) && !mb::HasStage(2, 3), "mobs: between boss floors of a full run only");
            bool grow = true, cap = true;
            for (int f = 2; f <= 50; ++f) for (int w = 0, t = mb::WaveCount(f); w < t; ++w) {
                const auto p = mb::Plan(f, w, t);
                const bool last = w + 1 == t;
                cap = cap && p.normal >= 1 && p.normal <= 8 && (last ? p.elite >= 2 && p.elite <= 4 : p.elite == 0);
                if (w && !last) grow = grow && p.normal >= mb::Plan(f, w - 1, t).normal;
            }
            Check(cap && grow, "mobs: early waves are mobs only and grow; the last wave brings 2-4 elites");
            for (int z = 1; z <= 5; ++z) {
                int n = 0, e = 0;
                for (auto& m : mb::kMobs) if (m.zone == z) (m.elite ? e : n)++;
                Check(n >= 3 && e >= 2, "mobs: each zone has at least 3 mob and 2 elite types");
            }
            bool fresh = true;
            for (auto& m : mb::kMobs) for (auto& b : tower::kBossPool) fresh = fresh && m.path != b.path;
            Check(fresh, "mobs: no boss-pool class is used as a mob");
            const mb::WavePlan wp{5, 1, 3, 1};
            const auto a1 = mb::Pick(9, 11, 0, 2, wp, [](int) { return false; }), a2 = mb::Pick(9, 11, 0, 2, wp, [](int) { return false; });
            bool zoneOk = a1.size() == 6, normalsFirst = true;
            for (size_t i = 0; i < a1.size(); ++i) { zoneOk = zoneOk && mb::kMobs[a1[i]].zone <= 2; normalsFirst = normalsFirst && mb::kMobs[a1[i]].elite == (i == 5); }
            Check(a1 == a2 && zoneOk && normalsFirst, "mobs: a wave is fixed by the seed, from its zone, with the right mix");
            Check(mb::Pick(9, 11, 0, 2, wp, [](int) { return true; }).empty(), "mobs: an all-bad pool yields an empty wave");
            Check(std::abs(mb::HpTarget(1000, false, 50, false) - 4000 * tower::balance::MobHpGrow(98) * tower::balance::EarlyHpRelief(98)) < 0.01 && mb::HpTarget(1000, true, 10, false) > mb::HpTarget(1000, false, 10, false) && mb::HpTarget(1000, false, 10, true) < mb::HpTarget(1000, false, 10, false), "mobs: HP is reduced 20%, elites remain tougher and easy remains softer");
            const auto mg = [](int f) { return tower::balance::MobDmgGrow(tower::balance::ShownOfWave(f)) * tower::balance::EarlyDmgRelief(tower::balance::ShownOfWave(f)); };  // 3.0.1 前期减负  // 3.0.0 分段增长  // v3 增长
            Check(std::abs(mb::DmgMul(2, false) - 0.5 * mg(2)) < 1e-9 && std::abs(mb::DmgMul(16, false) - 0.65 * mg(16)) < 1e-9 &&
                  std::abs(mb::DmgMul(36, false) - 0.8 * mg(36)) < 1e-9 && std::abs(mb::DmgMul(2, true) - 0.3 * mg(2)) < 1e-9 &&
                  std::abs(mb::DmgMul(16, true) - 0.35 * mg(16)) < 1e-9 && std::abs(mb::DmgMul(36, true) - 0.4 * mg(36)) < 1e-9,
                  "mobs: normal and elite wave damage is halved across all tiers and difficulties");
        }
        {   // 1.0.1 难度：简单 = 加强前的 Boss 数值，困难 = 逐层表现值
            const auto e = Challenges(42, 50, 1);
            int berserkEasy = 0, totalEasy = 0;
            for (int f = 1; f <= 50; ++f) { berserkEasy += e[f] == Challenge::Berserk; totalEasy += e[f] != Challenge::None; }
            Check(berserkEasy == 2 && totalEasy == 7, "difficulty: easy keeps the old 2 berserk floors (7 challenges)");
            Check(Berserk(Challenge::Berserk, 301, 1) && !Berserk(Challenge::Berserk, 299, 1) && Berserk(Challenge::Berserk, 181, 0), "difficulty: easy berserk at 5:00, hard at 3:00");
            Check(ChallengeHud(Challenge::Berserk, clean, 60, 1) == "限时挑战 4:00", "difficulty: easy berserk HUD counts down from 5:00");
            Check(std::abs(DiffHp(0, 1) - 0.88 * 1.15 * 0.55 * 0.85 * 0.8) < 1e-9 && std::abs(DiffHp(1, 1) - 0.5 * 1.15 * 0.55 * 0.85 * 0.8) < 1e-9 && DiffHp(0, 35) > DiffHp(0, 18) && DiffHp(0, 50) > DiffHp(0, 40) * 1.3, "v3 HP: old curve x E(s) x early relief; late growth accelerates");
            Check(tower::balance::BossHpShift(1) == 0.85 && tower::balance::BossHpShift(20) == 0.85 && std::abs(tower::balance::BossHpShift(25) - 1.025) < 1e-9 && tower::balance::BossHpShift(30) == 1.20 && tower::balance::BossHpShift(50) == 1.20 && std::abs(tower::balance::BossHpShift(55) - 1.0) < 1e-9 && tower::balance::BossHpShift(60) == 0.80 && tower::balance::BossHpShift(150) == 0.80,
                  "3.5.0 boss HP shift: 1-20 x0.85, 30-50 x1.20, from 60 x0.80");
            Check(tower::balance::BossDmgShift(50) == 1.0 && tower::balance::BossDmgShift(60) == 0.85 && tower::balance::BossDmgShift(200) == 0.85, "3.5.0 boss damage x0.85 from floor 60");
            Check(LateBossDmg("BP_Monster_LeiDang_P2_C") == 0.6 && LateBossDmg("BP_Monster_ChangHong_Spear_C") == 0.6 && LateBossDmg("BP_Monster_QiShu_Huo_C") == 0.6 && LateBossDmg("BP_Monster_QiShu_Bing_C") == 1.0,
                  "3.4.3 Lei Dang / Chang Hong / Yan Ru deal 20% less");
            Check(EarlyHpRelief(1) == 0.55 && EarlyHpRelief(30) == 0.55 && std::abs(EarlyHpRelief(50) - (0.55 + 0.35 * 20 / 30.0)) < 1e-9 && std::abs(EarlyHpRelief(75) - (EarlyHpRelief(50) + 1.0) / 2) < 1e-9 && EarlyHpRelief(100) == 1.0 && EarlyHpRelief(200) == 1.0 &&
                  EarlyDmgRelief(1) == 0.75 && EarlyDmgRelief(50) == 0.75 && std::abs(EarlyDmgRelief(60) - 0.70) < 1e-9 && EarlyDmgRelief(80) == 0.70 && EarlyDmgRelief(100) == 0.70 && EarlyDmgRelief(300) == 0.70, "relief: <=50 unchanged; HP rises linearly to 1.0 at 100 (user 2026-11-13); damage stays 0.70 after 60 (3.4.0)");
            { bool hpUp = true, dmgDown = true;  // 50 层后血量每层不降、伤害相对旧曲线更低
              for (int s = 51; s <= 300; ++s) { hpUp = hpUp && EarlyHpRelief(s) >= EarlyHpRelief(s - 1) - 1e-12; dmgDown = dmgDown && EarlyDmgRelief(s) <= 0.9 + 1e-12; }
              for (int s = 1; s <= 50; ++s) { hpUp = hpUp && EarlyHpRelief(s) <= 0.79; }
              Check(hpUp && dmgDown, "N6 relief: HP never drops after 50, damage relief capped at 0.9"); }
            { const double g1 = std::pow(1.15, 0.7);  // 3.1.0：60 层前伤害按 E 的 0.7 次方
              Check(std::abs(DiffDmg(0, 1) - 0.32 * g1 * 0.75) < 1e-9 && std::abs(DiffDmg(1, 1) - 0.22 * g1 * 0.75) < 1e-9 && std::abs(DiffDmg(0, 4) - 0.42 * g1 * 0.75) < 1e-9 && DiffDmg(0, 18) > 0.80 * g1 * 0.75 && DiffDmg(0, 50) > DiffDmg(0, 35) * 1.3 && DiffDmg(0, 50) < DiffDmg(0, 35) * 1.6, "damage keeps rising after floor 70, slower since 3.4.0"); }
            {   // 3.6.0 穿透表（方案第 8 条）：层 / 穿透 / 叠满 80% 的有效减伤 / 艾卡西亚 90%
                namespace b = tower::balance;
                struct Row { int s; double pen, eff80, eff90; };
                const Row rows[] = {{100, .15, .65, .75}, {150, .28, .52, .62}, {200, .37, .43, .53}, {300, .49, .31, .41}, {500, .62, .18, .28},
                                    {1000, .74, .06, .16}, {1660, .80, .00, .10}, {3000, .84, .00, .06}};
                bool ok = b::DrPenetration(60) == 0.0 && b::DrPenetration(30) == 0.0 && b::DrPenetration(100000) < 0.9;
                for (const auto& r : rows) {
                    const double p = b::DrPenetration(r.s);
                    ok = ok && std::abs(p - r.pen) < 0.006 && std::abs((1 - b::TakenFactor(3.0, .8, p)) - r.eff80) < 0.006 && std::abs((1 - b::TakenFactor(3.0, .9, p)) - r.eff90) < 0.006;
                    if (std::abs(p - r.pen) >= 0.006) std::printf("info: pen %d = %.4f\n", r.s, p);
                }
                ok = ok && std::abs(b::DrPenetration(100) - 0.15) < 1e-12 && b::TakenFactor(3.0, .8, b::DrPenetration(1660)) == 1.0 && b::TakenFactor(-0.3, .8, .5) == 1.3 && std::abs(b::TakenFactor(0.5, .8, .15) - 0.65) < 1e-12;
                Check(ok, "3.6.0 penetration 0.9(s-60)/(s-60+200) subtracts from capped additive reduction (plan table)");
            }
            {   // 3.6.0 无尽层：100 层一次性抬高 + 更陡的次方；小怪抬高部分 ×80%
                namespace b = tower::balance;
                const double g = std::pow(b::E(120) / b::E(100), 1.0);
                Check(std::abs(b::BossHpGrow(100) / (b::Grow(100, 1.0, 0.7, 0.85)) - 2.5) < 1e-9 && std::abs(b::BossDmgGrow(100) / b::Grow(100, 0.7, 0.4, 0.55) - 4.4) < 1e-9 &&
                      std::abs(b::BossBreakGrow(100) / b::Grow(100, 0.6, 0.9, 1.0) - 2.0) < 1e-9 && std::abs(b::BossShachiGrow(100) / b::Grow(100, 0.8, 0.5, 0.6) - 1.5) < 1e-9 &&
                      b::BossHpGrow(99) == b::Grow(99, 1.0, 0.7, 0.85) && std::abs(b::BossHpGrow(120) / b::BossHpGrow(100) - std::pow(g, 0.85)) < 1e-9,
                      "4.0.0 endless step hp x2.5 / atk x4.4 / brk x2 / shachi x1.5 at floor 100 (user 2026-11-15), then steeper exponents");
                Check(std::abs(b::MobHpGrow(100) / b::Grow(100, 0.9, 0.55, 0.5) - 2.2) < 1e-9 && std::abs(b::MobDmgGrow(100) / b::Grow(100, 0.65, 0.6, 0.75) - 3.72) < 1e-9 &&
                      std::abs(b::MobBreakGrow(100) / b::Grow(100, 0.5, 0.8, 0.9) - 1.8) < 1e-9 && std::abs(b::MobShachiGrow(100) / b::Grow(100, 0.7, 0.45, 0.6) - 1.4) < 1e-9,
                      "3.6.0 mobs get 80% of the boss step");
                const double r120 = b::BossHpGrow(120) / b::BossHpGrow(99), r200 = b::BossHpGrow(200) / b::BossHpGrow(99), a200 = b::BossDmgGrow(200) / b::BossDmgGrow(99);
                std::printf("info: 3.6.0 endless vs 99: hp120 %.1f hp200 %.1f atk200 %.1f\n", r120, r200, a200);
                Check(r120 > 1.5 && r200 < 19.9 && r200 > 5 && a200 > 13.3, "3.6.0 endless: HP lower than the first draft, attack higher (user 2026-11-15)");
                Check(b::RegenExempt("守门人拙壁") && b::RegenExempt("亡命徒：荆野") && b::RegenExempt("琴女烟千岁") && b::RegenExempt("女妖骨婉约") && !b::RegenExempt("剑痴：花千笑"),
                      "3.6.0 shield / phasing bosses never regenerate (user 2026-11-15)");
                Check(b::EnemyHpRegen(99) == 0 && b::EnemyHpRegen(100) == 0.02 && b::EnemyHpRegen(199) == 0.02 && std::abs(b::EnemyHpRegen(200) - 0.03) < 1e-12 &&
                      std::abs(b::EnemyHpRegen(450) - 0.05) < 1e-12 && std::abs(b::EnemyHpRegen(5000) - 0.05) < 1e-12 && b::EnemyShachiRegen(99) == 0 && b::EnemyShachiRegen(150) == 0.10 &&
                      b::EnemyShachiRegen(300) == 0.15 && b::kRegenDelay == 15.0, "3.6.0 endless enemy regen: 15 s quiet, HP 2% (+1% per 100 floors, max 5%), sha-chi 10% / s");
                const auto keep = b::g_endlessStep;
                b::g_endlessStep.hp = 5.0;
                Check(std::abs(b::BossHpGrow(100) / b::Grow(100, 1.0, 0.7, 0.85) - 5.0) < 1e-9 && std::abs(b::MobHpGrow(100) / b::Grow(100, 0.9, 0.55, 0.5) - 4.2) < 1e-9, "3.6.0 QA endless_scale changes the step live");
                b::g_endlessStep = keep;
            }
            Check(std::abs(LateGrowth(71) - 1.1) < 1e-9 && std::abs(LateGrowth(72) - 1.21) < 1e-9 && std::abs(LateGrowth(99) - std::pow(1.1, 29)) < 1e-9, "late growth compounds per displayed floor");
            Check(BossShachiMul(37, 0) > BossShachiMul(36, 0) && std::abs(BossShachiMul(37, 0) / BossShachiMul(36, 0) - BossShachiMul(37, 1) / BossShachiMul(36, 1)) < 1e-9, "v3: both difficulties grow boss shachi at the same rate");
            Check(BossHpScale(100, 50000) == 500.0 && BossHpScale(100, 2000000) == 10000.0 && BossHpScale(0, 200) == 1.0, "boss HP no longer clipped at 100x; bounded for bad data");
            Check(std::abs(EncounterDmg(19, true, "BP_Monster_ChiLian_C") - 0.75) < 1e-9 && EncounterDmg(21, true, "BP_Monster_ChiLian_C") == 1.0 && EncounterDmg(19, false, "BP_Monster_ChiLian_C") == 1.0, "early multi-phase extra reduction uses displayed floor");
            Check(std::abs(EncounterDmg(19, true, "BP_Monster_LionDancer_P2_C") - 0.16875) < 1e-9 && std::abs(EncounterDmg(33, true, "BP_Monster_GuiChai_Red_P2_C") - 0.75) < 1e-9 && EncounterDmg(33, true, "BP_Monster_LionDancer_C") == 1.0 && EncounterDmg(33, true, "BP_Monster_PowerMan_P2_C") == 0.45, "specific boss actor damage multipliers");
            Check(PlayerCloneDmg("BP_Monster_Hun_Summon_Double_C", true) == 1.0 && PlayerCloneDmg("BP_Monster_Hun_Summon_Double_C", false) == 1.0, "clone native damage restored");
            { const double g = std::pow(tower::balance::E(39), tower::balance::kBossShachiExp), g5 = std::pow(tower::balance::E(9), tower::balance::kBossShachiExp);
              const double lift = std::pow(3.5, 0.7) * 0.8;  // 4.0.1：显示 39 层杀气跟血量上抬（0.7 次方）、基数 ×0.8
              Check(std::abs(BossShachiMul(20, 1) - 1.2 * g * lift) < 1e-9 && std::abs(BossShachiMul(20, 0) - 2.0 * g * lift) < 1e-9 && std::abs(BossShachiMul(5, 0) - 1.5 * g5 * tower::balance::MidShachiLift(9) * 0.8) < 1e-9, "difficulty: easy shachi is raised (v3 x E(s)^0.8)"); }
            {   // 4.0.1（用户 2026-11-16）：显示 5 层起平滑上抬，5→20 层 ×1→×2.2，20→30 层 ×2.2→×3.5，没有跳变
                namespace b = tower::balance;
                bool mono = true, smooth = true;
                for (int x = 1; x < 200; ++x) { mono = mono && b::MidHpLift(x + 1) >= b::MidHpLift(x); smooth = smooth && b::MidHpLift(x + 1) - b::MidHpLift(x) <= 0.14; }
                Check(b::MidHpLift(1) == 1.0 && b::MidHpLift(5) == 1.0 && std::abs(b::MidHpLift(12) - (1.0 + 1.2 * 7 / 15)) < 1e-12 &&
                      std::abs(b::MidHpLift(20) - 2.2) < 1e-12 && std::abs(b::MidHpLift(30) - 3.5) < 1e-12 && std::abs(b::MidHpLift(120) - 3.5) < 1e-12 && mono && smooth,
                      "4.0.1 HP lift ramps from shown 5, continuous at 20, holds 3.5 from 30");
                Check(b::kBossBase == 0.8 && std::abs(DiffHp(0, 1) / (b::kBossBase * (0.88) * b::BossHpGrow(1) * b::EarlyHpRelief(1) * b::BossHpShift(1)) - 1.0) < 1e-9,
                      "4.0.1 boss HP base x0.8 from floor 1");
            }
            {   // 4.0.1（用户 2026-11-16）：杀气上限跟血量上抬一起抬（0.7 次方），基数同样 ×0.8
                namespace b = tower::balance;
                Check(b::MidShachiLift(5) == 1.0 && std::abs(b::MidShachiLift(20) - std::pow(2.2, 0.7)) < 1e-12 &&
                      std::abs(b::MidShachiLift(30) - std::pow(3.5, 0.7)) < 1e-12 && std::abs(b::MidShachiLift(150) - std::pow(3.5, 0.7)) < 1e-12, "4.0.1 shachi lift = HP lift ^ 0.7, endless included");
                Check(std::abs(BossShachiMul(3, 0) / (1.5 * b::BossShachiGrow(5) * 0.8) - 1.0) < 1e-9, "4.0.1 shachi below shown 5 = old x0.8");
                std::printf("info: 4.0.1 boss HP lift x0.8: shown10 %.2f shown20 %.2f shown30+ %.2f; shachi shown20 %.2f shown30+ %.2f\n",
                            b::MidHpLift(10) * 0.8, b::MidHpLift(20) * 0.8, b::MidHpLift(30) * 0.8, b::MidShachiLift(20) * 0.8, b::MidShachiLift(30) * 0.8);
            }
        }
    }
    // ---- 场地表
    {
        bool ranges = true;
        for (auto& v : kVenues) {
            ranges = ranges && v.doorAt >= 0 && v.doorAt + v.doorN <= static_cast<int>(std::size(kVenueDoors));
            ranges = ranges && v.hullAt >= 0 && (v.hullAt + v.hullN) * 2 <= static_cast<int>(std::size(kVenueHull));
        }
        Check(std::size(kVenues) == 77 && ranges, "venues: 77 entries, door / hull slices in range");
        int known = 0;
        for (auto& f : floorplan::kFloors)
            for (auto& v : kVenues) if (v.path == f.venue) { ++known; break; }
        Check(known >= 45, "venues: almost every planned venue has extracted venue data");
        int classicOk = 0;
        for (auto p : floorplan::kZoneClassic)
            for (auto& v : kVenues) if (v.path == p && v.doorN > 0 && !v.sky) { ++classicOk; break; }
        Check(classicOk == 5, "venues: every classic fallback has fog doors at normal height");
    }
    // ---- 凸包边界
    {
        using arena::P3;
        const std::vector<P3> sq = {{0, 0, 0}, {1000, 0, 0}, {1000, 1000, 0}, {0, 1000, 0}};
        Check(arena::InHull(sq, {500, 500, 0}) && !arena::InHull(sq, {1500, 500, 0}) && arena::InHull(sq, {0, 500, 0}), "hull: inside / outside / edge");
        arena::Boundary b;
        b.ArmHull(sq, {500, 500, 100}, 0, 1000);
        Check(!b.hull.empty() && b.Player({500, 500, 100}) == arena::Act::None && b.Player({2000, 500, 100}) == arena::Act::PushBack, "hull boundary: player outside the fog doors is pushed");
        const auto in = b.Inside({2000, 500, 100});
        Check(arena::InHull(sq, in) && in.x < 1000, "hull boundary: pushed back inside the hull");
        Check(b.Player({500, 500, 1500}) == arena::Act::PushBack, "hull boundary: height range enforced");
        b.slack = 400;  // 凸包软边：退回中心方向 4 米仍在凸包里 = 不推
        Check(b.Player({1300, 500, 100}) == arena::Act::None && b.Player({1500, 500, 100}) == arena::Act::PushBack, "hull boundary: slack along the centre direction");
        b.slack = 0;
        arena::Boundary c;
        c.ArmHull(sq, {5000, 5000, 0}, 0, 1000);
        Check(c.hull.empty(), "hull boundary: a hull not containing the spawner falls back to the circle");
    }
    // ---- session isolation verdicts (A / B)
    {
        using towerisolate::Grant;
        Check(towerisolate::JudgeGrant(true, false, false, false) == Grant::Block, "isolation: a native grant inside the tower session is refused (14 / 15 / 17 / 29 / 31)");
        Check(towerisolate::JudgeGrant(true, true, false, false) == Grant::Allow, "isolation: the tower's own grant is allowed");
        Check(towerisolate::JudgeGrant(true, false, true, false) == Grant::Allow && towerisolate::JudgeGrant(true, false, false, true) == Grant::Allow,
              "isolation: save-load restore and travel reloads are allowed (cross-map bell travel reloads the tower save)");
        Check(towerisolate::JudgeGrant(false, false, false, false) == Grant::Allow, "isolation: outside the session the game is untouched (original save restore)");
        Check(towerisolate::SideEffectEvent("OnGroupCleared") && towerisolate::SideEffectEvent("OnGroupMemberDead") && towerisolate::SideEffectEvent("OnGroupDeactivated") &&
              towerisolate::SideEffectEvent("OnGroupMemberDespawn"), "isolation: group clear / death / despawn / deactivate scripts are side effects (weapon loot is in OnGroupCleared)");
        Check(!towerisolate::SideEffectEvent("OnGroupInCombat") && !towerisolate::SideEffectEvent("OnGroupActivated") && !towerisolate::SideEffectEvent("OnGroupMemberSpawn"),
              "isolation: combat / activation events stay (arena mechanics, MetaAI)");
        Check(towerisolate::IsStory(true, false, false, false, false) && !towerisolate::IsStory(true, false, false, true, false) && !towerisolate::IsStory(false, true, true, false, false),
              "isolation: flow cutscenes are story; tower boss performances and looping ambience are not");
    }
    // ---- input gate (floor 4: chosen 158 ms after opening)
    {
        tower::inputgate::Gate g;
        g.Open(1000, 1);
        Check(!g.Accept(1158, 0), "input gate: a click 158 ms after opening is swallowed");
        g.Sample(1300, 1);
        Check(!g.Accept(1500, 0), "input gate: a key still held after opening keeps it closed until quiet");
        Check(g.Accept(1801, 1), "input gate: after 300 ms quiet a fresh press selects (arming is sticky)");
        tower::inputgate::Gate m;
        m.Open(0, 0);
        for (unsigned long long t = 100; t <= 1000; t += 150) m.Sample(t, 1);
        Check(!m.Accept(1050, 0), "input gate: mashing keeps it closed");
        Check(m.Accept(1400, 0), "input gate: stops mashing -> opens");
        tower::inputgate::Gate q;
        q.Open(0, 0);
        Check(q.Accept(450, 1), "input gate: a calm player chooses after 0.4 s, no manual wait");
    }
    // ---- fate contract (G1 / G2)
    {
        using tower::fate::Verdict;
        Check(tower::fate::JudgePlayer(1.0f, 5, true, true) == Verdict::Assisted, "fate: god / demigod on -> a throw to HP1 is caught, not a defeat (21)");
        Check(tower::fate::JudgePlayer(1.0f, 5, false, true) == Verdict::Revive && tower::fate::JudgePlayer(1.0f, 5, false, false) == Verdict::Lost,
              "fate: assists off -> revive charm, then a legal defeat");
        Check(tower::fate::JudgePlayer(1.0f, 0.5, false, false) == Verdict::Fine && tower::fate::JudgePlayer(500, 5, false, false) == Verdict::Fine,
              "fate: not judged in the first second, nor above the floor");
        tower::fate::DeathTrack d;
        Check(d.Sample(10, 0) == tower::fate::DeathTrack::Ev::Zero && !d.Dead(), "death: HP 0 is only 'zeroed' (20)");
        Check(d.Sample(10.5, 0) == tower::fate::DeathTrack::Ev::None && d.Sample(10.6, 3000) == tower::fate::DeathTrack::Ev::Resumed && !d.Dead() && d.resumes == 1,
              "death: HP back inside the confirmation window = phase continues, no reward");
        d.Sample(20, 0);
        Check(d.Sample(21.0, 0) == tower::fate::DeathTrack::Ev::Confirmed && d.Dead(), "death: 1 s at zero = confirmed");
        Check(d.Sample(22, 0) == tower::fate::DeathTrack::Ev::None, "death: confirmed once only");
    }
    // ---- admission before the countdown (C)
    {
        using arena::P3;
        arena::Boundary b;
        b.ArmHull({{0, 0, 0}, {4000, 0, 0}, {4000, 4000, 0}, {0, 4000, 0}}, {2000, 2000, 0}, -500, 1500);
        Check(b.Describe() == "雾门凸包 4 点", "admission: boundary text names the real hull (not 'radius 35 m')");
        Check(arena::Admit(b, {2000, 2000, 0}, {{"Boss", {2500, 2500, 0}}}).ok, "admission: both inside -> countdown");
        const auto wall = arena::Admit(b, {2000, 2000, 0}, {{"拙壁", {4100, 2000, 0}}});
        Check(!wall.ok && wall.why.find("拙壁") != std::string::npos && wall.why.find("凸包") != std::string::npos, "admission: boss behind the wall refused before the countdown (13 / 24)");
        Check(!arena::Admit(b, {2000, 2000, 0}, {{"Boss", {3900, 2000, 0}}}).ok, "admission: 2 m margin from the edge");
        Check(!arena::Admit(b, {2000, 2000, -5000}, {}).ok, "admission: player in the void below the arena is refused (25)");
        arena::Boundary c;
        c.Arm({0, 0, 0}, 3200);
        Check(c.Describe() == "圆，半径 32 米" && !arena::Admit(c, {3100, 0, 0}, {}).ok && arena::Admit(c, {2900, 0, 0}, {}).ok, "admission: circle text and margin");
    }
    // ---- ground / same-side reachability (C, rest)
    {
        using towerreach::Decide; using towerreach::Verdict; using towerreach::Probe;
        Probe air; air.groundKnown = true; air.ground = false;
        Check(Decide(air) == Verdict::NoGround, "reach: no ground under the player (7 / 25 void) -> no countdown");
        Probe farFloor;farFloor.groundKnown=true;farFloor.ground=true;farFloor.groundGap=1500;
        Check(Decide(farFloor)==Verdict::NoGround,"reach: floor 15m below does not support a floating player");
        farFloor.groundGap=6;Check(Decide(farFloor)==Verdict::Pass,"reach: normal capsule/feet skin tolerance remains valid");
        Probe wall; wall.groundKnown = wall.ground = true; wall.lineKnown = wall.lineBlocked = true; wall.navKnown = true; wall.navOk = false;
        Check(Decide(wall) == Verdict::Walled, "reach: line blocked AND nav partial -> walled off (13)");
        Probe pillar = wall; pillar.navOk = true;
        Check(Decide(pillar) == Verdict::Pass, "reach: a pillar blocking sight with a full nav path is fine (lion poles)");
        Probe sky = wall; sky.navKnown = false;
        Check(Decide(sky) == Verdict::Pass, "reach: no nav data (sky spaces) is unknown, not a refusal");
        Probe untested;
        Check(Decide(untested) == Verdict::Pass && std::string(towerreach::Why(Verdict::Walled)).find("隔开") != std::string::npos, "reach: untested passes; reasons are spelled out");
    }
    // ---- 33-38: spawn chain (floor audit, failure classes, landing-bad venues)
    {
        using towerreach::FloorAudit; using towerreach::FloorVerdict; using towerreach::Classify;
        FloorAudit bifeng; bifeng.n = 12; bifeng.dynamicHits = 12;
        Check(Classify(bifeng) == FloorVerdict::NotStatic, "floor audit: only WorldDynamic floor -> not-static verdict (stop the 6 futile retries)");
        FloorAudit roof; roof.n = 12; roof.startSolid = 7; roof.dynamicHits = 5;
        Check(Classify(roof) == FloorVerdict::StartInSolid, "floor audit: trace starts inside solid -> its own verdict");
        FloorAudit ok; ok.n = 12; ok.staticHits = 5; ok.none = 7;
        Check(Classify(ok) == FloorVerdict::Ok, "floor audit: enough WorldStatic floor -> Core rule should find a spot");
        FloorAudit edge; edge.n = 12; edge.none = 12;
        Check(Classify(edge) == FloorVerdict::NoFloor && Classify(FloorAudit{}) == FloorVerdict::Unknown, "floor audit: really nothing / not run");
        Check(TransientSpawnError(R"({"ok":false,"message":"游戏暂停中"})") && !TransientSpawnError(R"({"ok":false,"message":"角色附近没有足够可落地且不重叠的位置"})") &&
              !TransientSpawnError(R"({"ok":false,"message":"指定蓝图不是 PBZCharacter_NPC"})"), "spawn errors: pause / loading is context, landing is venue, class errors are the boss");
        int bad = 0, onFloor = 0;
        for (auto& d : floorplan::kLandingBad) bad += PoolIndex(d.path) >= 0;
        for (auto& f : floorplan::kFloors) {
            const int v = PoolIndex(f.venue);
            if (!LandingBad(v).empty() && NativeOnly(PoolIndex(f.bosses[0])).empty()) ++onFloor;
        }
        Check(bad == 2 && onFloor == 0, "landing-bad venues (毕峰 / 舞狮；寒岁、蛊鹤、玉面 2026-11-04/05 移出) are never a floor's venue");
        int classicBad = 0;
        for (auto c : floorplan::kZoneClassic) classicBad += !LandingBad(PoolIndex(c)).empty();
        Check(classicBad == 0 && Find(31)->venue.find("FengXueDaoKe") != std::string_view::npos && Find(7)->venue.find("JingYe") != std::string_view::npos && Find(30)->bosses[0].find("YuCong_FirstTime") != std::string_view::npos, "no zone fallback is landing-bad; floor 7 native, floor 31 wheel back on C2 (Bi Feng cannot land in DengDingShanDao), floor 30 = three-faced puppet");
    }
    // ---- native encounter replay (lion prototype)
    {
        using namespace tower::replay;
        const auto* lion = tower::replay::Find("lion");
        Check(lion && lion->phaseN == 2 && PhaseOf(*lion, "BP_Monster_LionDancer_C") == 0 && PhaseOf(*lion, "BP_Monster_LionDancer_P2_C") == 1 &&
              PhaseOf(*lion, "BP_Monster_DaTouWaWa_HouHouHou_C") < 0, "replay: lion contract phases (summons are not phases)");
        Check(AllowSequence(lion, "Seq062") && AllowSequence(lion, "Seq063") && !AllowSequence(lion, "Seq064") && !AllowSequence(lion, "Seqs025-1") && !AllowSequence(nullptr, "Seq062"),
              "replay: intro / transition play; post-kill Seq064 and everything else stay frozen; non-replay floors allow nothing");
        Check(PreserveActor(lion, "BP_NPC_LionDancer_C") && PhaseOf(*lion, "BP_NPC_LionDancer_C") < 0,
              "replay: intro lion NPC survives sweep but is never a battle phase or victory target");
        Check(PreserveActor(lion, "BP_Monster_LionDancer_C") && PreserveActor(lion, "BP_Monster_LionDancer_P2_C"),
              "replay: both actual battle phases remain protected from native cleanup");
        Check(!PreserveActor(nullptr, "BP_NPC_LionDancer_C") && !PreserveActor(lion, "BP_NPC_LionDancer_C_Extra") &&
              !PreserveActor(lion, "BP_Monster_LionDancer_P2_NPC_C") && !PreserveActor(lion, "BP_Monster_DuYanBoss_C"),
              "replay: support exemption is exact and scoped; other floors, cold-tea NPC variants and unrelated bosses remain isolated");
        Progress p; p.Begin(2);
        Check(!p.Started() && !p.Settle(), "replay: nothing before the player opens the encounter");
        p.Seen(0, 100); p.Dead(0, 160);
        Check(p.Started() && p.Between() && !p.Settle() && !p.TimedOut(200, 90) && p.TimedOut(251, 90), "replay: phase one dead -> wait for phase two (bounded), no reward");
        p.Seen(1, 195);
        Check(!p.Between(), "replay: phase two arrived");
        p.Dead(1, 300);
        Check(p.Settle(), "replay: final phase dead -> settle (no post-kill cutscene)");
        tower::json::Value save;
        Check(tower::json::Parse(R"({"EventPipeline":{"PBZ_Houjie_WP":{"BP_FlowActor_C_UAID_x":{"EventTag":["pz.舞狮.点睛","pz.击杀.舞狮尊主烈焰P1","pz.舞狮.打开大门"]}}},"Player":{"EventTag":["pz.舞狮.点睛"]}})", &save) &&
              tower::save::ResetReplayTags(save) == 2, "replay: tower save drops the encounter's completion tags in EventPipeline only");
        const auto* tags = save.Find("EventPipeline")->Find("PBZ_Houjie_WP")->Find("BP_FlowActor_C_UAID_x")->Find("EventTag");
        Check(tags && tags->items.size() == 1 && tags->items[0].text == "pz.舞狮.打开大门" && save.Find("Player")->Find("EventTag")->items.size() == 1,
              "replay: unrelated quest tags (door) and other sections untouched");
    }
    // ---- 用时存读档
    {
        Run r;
        r.Start(7, 50);
        r.times = {753, 0, 1201};
        r.chalOffered = 4; r.chalDone = 3;
        Run s;
        Check(s.Load(r.Save()) && s.times == r.times && s.chalOffered == 4 && s.chalDone == 3, "run save keeps floor times and challenge counts");
        Check(s.plan == tower::kPlanVersion, "run save records the plan version");
        {   // 第 51 层：本轮额外·限时 / 禁药计数与隐藏局标记随局存档；解锁快照的层数 51 能读回（层号 50 ≤ 51）
            Run h; h.Start(11, 50); h.floor = 50; h.doneTimed = 2; h.donePotion = 2; h.floors = 51; h.hidden = true;
            Run hb;
            Check(hb.Load(h.Save()) && hb.doneTimed == 2 && hb.donePotion == 2 && hb.hidden && hb.floors == 51 && hb.floor == 50, "floor 51 snapshot keeps challenge counts, hidden flag and 51-floor length");
        }
        Run ra; ra.Start(9, 50); ra.audit = true;
        Run rb;
        Check(rb.Load(ra.Save()) && rb.audit, "run save keeps the audit pool for resume (dev acceptance)");
        Run rc; rc.Start(9, 50);
        Run rd;
        Check(rd.Load(rc.Save()) && !rd.audit, "a formal run resumes on the verified pool");
        Run old;
        Check(old.Load("v=1\nid=8b82b42d6c18802d\nseed=12345\nfloors=50\nfloor=32\nphase=ready\njade=1071\ntimes=1,2\n") && old.plan == 0 && old.floor == 32 && old.jade == 1071,
              "an old v4 run (floor 32 / ready) still loads: plan 0, progress kept for migration");
    }

    if (const auto* ajin = tower::replay::Find("ajin")) {
        const std::string_view soldier = "BP_Monster_HuaShan_RedTroop_Soldier_C";
        Check(ajin->kickGroup && tower::replay::UsesCountdown(*ajin) &&
              tower::replay::In(ajin->supportClasses, ajin->supportN, soldier) &&
              tower::replay::PhaseOf(*ajin, soldier) < 0 && tower::replay::PhaseOf(*ajin, ajin->phases[0]) == 0,
              "Ajin's group support is freeze eligible but cannot become a victory phase");
    } else Check(false, "Ajin native encounter exists");
    for(const auto& c:tower::replay::kContracts) {
        if(c.spawnerKick && c.interactN==0 && c.allowN==0) Check(tower::replay::UsesCountdown(c),"spawner-only encounter freezes both sides and uses countdown");
        if(c.interactN || c.allowN) Check(!tower::replay::UsesCountdown(c),"native interaction and cinematics keep their own entry control");
    }
    {  // 无尽混编层：3000 个种子 × 若干层，波数 / 每波 Boss / 小怪上限、同波身份不重复、第一波必有 Boss、可复现
        tower::endless::Pools ep;
        for (int i = 0; i < static_cast<int>(std::size(kBossPool)); ++i) if (!kBossPool[i].scene) ep.bosses.push_back(i);
        ep.identity = [](int i) { return kBossPool[i].identity; };
        ep.mobCount = static_cast<int>(std::size(tower::mobs::kMobs));
        ep.mobOk = [](int m) { return m != 0; };
        ep.solo = [](int b) { return kBossPool[b].identity == std::string_view("MengHua"); };
        ep.maxBosses = 3;
        bool ok = true, sawSolo = false, sawMulti = false, sawThree = false, sawMobOnly = false, sawThreeBoss = false;
        for (uint64_t s = 1; s <= 3000; ++s) for (int f : {101, 150, 151, 222}) {
            const auto w = tower::endless::Generate(s, f, ep);
            ok = ok && !w.empty() && w.size() <= 3 && !w[0].bosses.empty();
            for (const auto& x : w) {
                ok = ok && x.bosses.size() <= 3 && x.mobs.size() <= 4 && !(x.bosses.empty() && x.mobs.size() < 2) && !(x.bosses.size() == 3 && x.mobs.size() > 2);
                for (size_t i = 0; i < x.bosses.size(); ++i) for (size_t j = i + 1; j < x.bosses.size(); ++j) ok = ok && kBossPool[x.bosses[i]].identity != kBossPool[x.bosses[j]].identity;
                for (int m : x.mobs) ok = ok && m != 0 && m < ep.mobCount;
                for (int b : x.bosses) ok = ok && !kBossPool[b].scene;
                sawMobOnly |= x.bosses.empty(); sawThreeBoss |= x.bosses.size() == 3;
                for (int b : x.bosses) if (ep.solo(b)) { sawSolo = true; ok = ok && x.bosses.size() == 1 && x.mobs.empty(); }
            }
            sawMulti |= w.size() >= 2; sawThree |= w.size() == 3;
            const auto again = tower::endless::Generate(s, f, ep);
            ok = ok && again.size() == w.size() && (w.empty() || again[0].bosses == w[0].bosses);
        }
        Check(ok, "endless waves respect caps (<=3 waves, <=3 bosses, <=4 mobs), unique identities per wave, first wave has a boss, reproducible");
        Check(sawMulti && sawThree && sawMobOnly && sawThreeBoss && sawSolo, "endless waves actually vary: multi-wave, 3 waves, mob-only wave, 3-boss wave, solo MengHua all occur");
        tower::endless::Pools al = ep; al.allies = 3; bool allyOk = true;
        for (uint64_t s = 1; s <= 2000; ++s) for (const auto& x : tower::endless::Generate(s, 120, al))
            allyOk = allyOk && x.bosses.size() + x.mobs.size() <= 3 + (x.bosses.empty() ? 0u : 0u) && (x.bosses.empty() ? x.mobs.size() == 2 : true);
        Check(allyOk, "endless with 3 servant allies: each wave at most 3 enemies (room left for allies)");
        {  // 无尽模式（正式）：第 50 层后显示 100 起逐层 +1；局存档带 endless；开了无尽的第 51+ 层打完不通关结算，照常进奖励
            Check(ShownBoss(50, 50) == 99 && ShownBoss(51, 50) == 100 && ShownBoss(52, 50) == 101 && ShownBoss(151, 50) == 200, "endless shown floors: 50 -> 99, 51 -> 100, 52 -> 101");
            tower::Run r; r.Start(9, 50); r.endless = true; r.floor = 52; r.peak = 52;
            tower::Run back; Check(back.Load(r.Save()) && back.endless && back.floors == 50, "endless flag survives save/load, floors stay 50");
            tower::Run fresh; fresh.Start(9, 50); std::string legacyText = fresh.Save();
            legacyText.erase(legacyText.find("endless="), legacyText.find('\n', legacyText.find("endless=")) - legacyText.find("endless=") + 1);
            tower::Run old; Check(old.Load(legacyText) && !old.endless, "old saves without the key load as non-endless");
            tower::Run past = r; past.endless = false; tower::Run bad; Check(!bad.Load(past.Save()), "counterexample kept: non-endless save with floor > floors is still rejected");
            r.phase = tower::Phase::Ready; r.floor = 51; r.phase = tower::Phase::Fighting; r.bosses = {0};
            r.FloorCleared({});
            Check(r.phase == tower::Phase::Reward, "endless floor 51 cleared -> reward, not Won");
            tower::Run h; h.Start(9, 50); h.floor = 51; h.floors = 51; h.phase = tower::Phase::Fighting; h.bosses = {0}; h.FloorCleared({});
            Check(h.phase == tower::Phase::Won, "counterexample kept: legacy floor 51 without endless still ends the run");
        }
        tower::endless::Pools two = ep; two.maxBosses = 2; bool capOk = true, saw2 = false;
        for (uint64_t s = 1; s <= 3000; ++s) for (const auto& x : tower::endless::Generate(s, 150, two)) { capOk = capOk && x.bosses.size() <= 2; saw2 |= x.bosses.size() == 2; }
        Check(capOk && saw2, "endless before shown floor 200: at most 1v2 per wave, and 1v2 does occur");
        tower::endless::Pools none = ep; none.bosses.clear();
        Check(tower::endless::Generate(7, 101, none).empty(), "endless counterexample: no eligible boss -> no floor");
        {   // 3.6.0 打包前（用户）：按身份等概率——一个身份 3 条变体不能比单条的多出 3 倍
            tower::endless::Pools fair; fair.bosses = {0, 1, 2, 3, 4};
            fair.identity = [](int i) { return i <= 2 ? std::string_view("LeiDang") : i == 3 ? std::string_view("B") : std::string_view("C"); };
            fair.maxBosses = 1; double cnt[3] = {};
            for (uint64_t s = 1; s <= 6000; ++s) { const auto w = tower::endless::Generate(s, 130, fair); if (!w.empty() && !w[0].bosses.empty()) { const int b = w[0].bosses[0]; ++cnt[b <= 2 ? 0 : b == 3 ? 1 : 2]; } }
            const double tot = cnt[0] + cnt[1] + cnt[2];
            Check(tot > 0 && std::abs(cnt[0] / tot - 1.0 / 3) < 0.04 && std::abs(cnt[1] / tot - 1.0 / 3) < 0.04, "3.6.0 endless bosses: equal chance per boss identity, not per pool entry");
        }
    }
    {   // 3.6.0 无尽选场：榕树约 30%、其余按面积加权、不连续两层同一场地；小场地名单
        namespace en = tower::endless;
        const std::vector<en::Venue> cands = {{en::kRongshu, 2000}, {"/A.A_C", 6000}, {"/B.B_C", 3000}, {"/C.C_C", 1000}};
        int hits[4] = {}, repeats = 0;
        std::string_view last = "";
        for (uint64_t i = 1; i <= 20000; ++i) {
            uint64_t x = i * 0x9E3779B97F4A7C15ull; x ^= x >> 31;
            const int k = en::Pick(cands, last, x);
            if (k < 0) { repeats = 1 << 20; break; }
            ++hits[k]; repeats += cands[k].key == last; last = cands[k].key;
        }
        const double n = 20000, others = hits[1] + hits[2] + hits[3];
        std::printf("info: 3.6.0 endless venue picks rongshu %.3f A %.3f B %.3f C %.3f\n", hits[0] / n, hits[1] / n, hits[2] / n, hits[3] / n);
        Check(repeats == 0 && std::abs(hits[0] / n - 0.30) < 0.03 && hits[1] > hits[2] && hits[2] > hits[3] && hits[1] / others > 0.42,
              "3.6.0 endless venue pick: rongshu ~30%, area-weighted, never twice in a row");
        Check(en::Pick({}, "", 5) == -1 && en::Pick({{en::kRongshu, 1}}, en::kRongshu, 7) == 0 && en::Pick({{en::kRongshu, 1}, {"/A.A_C", 5}}, "/A.A_C", 1) == 0 &&
              en::Pick({{en::kRongshu, 1}, {"/A.A_C", 5}}, en::kRongshu, 1) == 1, "3.6.0 endless venue pick: edge cases");
        Check(en::SmallVenue("/PBZCombatAsset/ActorData/Mobs/070_GuiChai_Cyan/BP_Monster_GuiChai_Cyan_ShuangYue.BP_Monster_GuiChai_Cyan_ShuangYue_C") &&
              !en::SmallVenue("/PBZCombatAsset/ActorData/Mobs/069_GuiChai_Red/BP_Monster_GuiChai_Red.BP_Monster_GuiChai_Red_C") &&
              !en::SmallVenue("/PBZCombatAsset/ActorData/Mobs/HuaQianXiao/BP_Monster_HuaQianXIao.BP_Monster_HuaQianXIao_C") &&
              en::SmallVenue("/PBZCombatAsset/ActorData/Mobs/YingMei/BP_Monster_YingMei_RedShadowBoneBlade.BP_Monster_YingMei_RedShadowBoneBlade_C") &&
              !en::SmallVenue("/PBZCombatAsset/ActorData/Mobs/045_JingYe/BP_Monster_JingYe.BP_Monster_JingYe_C") && !en::SmallVenue("/PBZCombatAsset/ActorData/Mobs/042_AHua/BP_Monster_AHua_GuiTouDao.BP_Monster_AHua_GuiTouDao_C") &&
              !en::SmallVenue("/PBZCombatAsset/ActorData/Mobs/QiShu/BP_Monster_QiShu_Du.BP_Monster_QiShu_Du_C") && !en::SmallVenue(""),
              "3.6.0 small venue list: named + measured < 20 m; Huo Liuxing excluded (user 2026-11-15)");
        bool noSmall = true; for (const auto& v : en::kVenues) noSmall = noSmall && !en::SmallVenue(v.key) && !en::MeasuredSmall(v.key) && v.key.find("HuaQianXIao") == std::string_view::npos;
        Check(noSmall && std::size(en::kVenues) >= 1 && en::kVenues[0].key == en::kRongshu, "3.6.0 endless table keeps no small venue and keeps rongshu");
        bool noHuo = true; for (const auto& v : en::kVenues) noHuo = noHuo && v.key.find("GuiChai_Red") == std::string_view::npos;
        Check(noHuo && std::size(en::kVenues) >= 8, "4.0.0 endless venues exclude Huo Liuxing (user 2026-11-15), big venues kept");
    }
    {   // 3.6.0 统一出界拉回 + 硬墙（tower_wall.h）
        namespace w = tower::wall;
        w::Circle c; c.x = 1000; c.y = -500; c.r = 3000; c.ok = true;
        w::P out{1000 + 3500, -500, 0}, in{1000 + 2000, -500, 0};
        Check(w::Outside(c, out) && !w::Outside(c, in) && !w::Outside(c, {1000 + 2840, -500, 0}) && w::Outside(c, {1000 + 2860, -500, 0}), "3.6.0 wall: judgment line 1.5 m inside the wall");
        w::P q = out; Check(w::ClampXY(c, q) && !w::Outside(c, q) && std::abs(w::Dist2D(q, c.x, c.y) - (3000 - 150 - 20)) < 1 && q.y == -500, "3.6.0 wall: clamp back onto the line");
        double vx = 500, vy = 300; Check(w::CutOutward(c, {4000, -500, 0}, vx, vy) && vx == 0 && vy == 300, "3.6.0 wall: outward velocity cut, along-wall kept");
        double ix = -200, iy = 0; Check(!w::CutOutward(c, {4000, -500, 0}, ix, iy) && ix == -200, "3.6.0 wall: inward velocity kept");
        w::Track t;
        { const w::P edge{1000 + 2800, -500, 0}, step{1000 + 2950, -500, 0}; Check(!t.Due(c, edge, 0.0) && !t.Due(c, step, 0.1) && !t.Due(c, step, 2.9) && t.Due(c, step, 3.2), "3.6.0 wall: walking out needs > 3 s outside (user: no snapping near the door)"); }
        w::Track j; j.Due(c, in, 0.0);
        Check(!j.Due(c, out, 0.05) && !j.Due(c, out, 0.4) && j.Due(c, out, 0.6), "3.6.0 wall: teleport (> 8 m in one frame) landing outside pulled after 0.5 s");
        { w::Track e; e.Due(c, in, 0.0); const w::P nearEdge{1000 + 2800, -500, 0}; Check(!e.Due(c, nearEdge, 0.1) && !e.Due(c, nearEdge, 30), "3.6.0 wall: standing at the door edge (inside the line) is never pulled"); }
        w::Track h; h.Pulled(1); h.Pulled(5); h.Pulled(10); Check(!h.Hard(12), "3.6.0 wall: no direct-to-center fallback any more");
        w::Track wl; wl.Walled(true, 0); Check(!wl.Due(c, in, 1.5) && !wl.Due(c, in, 5), "3.6.0 wall: being walled off inside the line no longer pulls");
        const w::P player{1000, -500, 0};
        const auto cands = w::PullCandidates(c, out, player, false, 3);
        bool candOk = cands.size() > 1;
        for (size_t i = 0; i + 1 < cands.size(); ++i) candOk = candOk && !w::Outside(c, cands[i]) && w::Dist2D(cands[i], player.x, player.y) >= 400;
        Check(candOk && std::abs(w::Dist2D(cands[0], c.x, c.y) - (3000 - 200)) < 1 && std::abs(cands[0].y - (-500)) < 1 && cands[0].x > c.x,
              "3.6.0 wall: first pull point is on the same side, 2 m inside the wall (pinned to the door edge, not the center)");
        w::Circle poly; poly.x = 0; poly.y = 0; poly.ok = true; poly.rays = {4000, 2000, 4000, 2000}; poly.r = 4000;
        Check(std::abs(poly.RadiusAt(100, 0) - 4000) < 1 && std::abs(poly.RadiusAt(0, 100) - 2000) < 1 && std::abs(poly.RadiusAt(100, 100) - 3000) < 1 &&
              w::Outside(poly, {0, 1900, 0}) && !w::Outside(poly, {3800, 0, 0}) && std::abs(poly.Line() - 1850) < 1, "3.6.0 wall: measured per-direction radii (non-circular venues)");
    }
    {   // 4.1.1（用户：创意层 Boss 打人不掉血）：主线创意层按所在 Boss 层的单 Boss 目标取值，不能再低于早期公式
        using namespace tower::floorplay;
        bool ok = true; double worstHp = 1e9, worstDmg = 1e9;
        for (int F = 3; F <= 50; ++F) {
            const double oldHp = 5000 * std::pow(1.05, F - 2), oldDmg = 1 + 0.03 * (F - 2);  // 4.1.0 及以前创意层用的 Run::BossHpTarget / BossDmg
            const double hp = SoloHp(F) * 0.85, dmg = SoloDmg(F) * 0.85;                    // 多 Boss 每只 ×0.85
            worstHp = std::min(worstHp, hp / oldHp); worstDmg = std::min(worstDmg, dmg / oldDmg);
            ok = ok && SoloHp(F) > 0 && SoloDmg(F) > 0;
        }
        std::printf("info: 4.1.1 creative floors vs old formula (multi-boss x0.85): HP at least x%.1f, attack at least x%.1f\n", worstHp, worstDmg);
        Check(ok && worstHp > 3 && worstDmg > 1.2, "4.1.1 main-run creative floors take their boss floor's solo HP / attack");
        Check(std::abs(SoloHp(13) - 20738 / 0.65) < 1 && std::abs(SoloDmg(9) - 2.435) < 1e-9, "4.1.1 solo target undoes the floor-type share");
    }
    if (g_failed) { std::printf("TOWER_PLAN_FAILED %d/%d\n", g_failed, g_checks); return 1; }
    std::printf("TOWER_PLAN_OK %d checks (no game process)\n", g_checks);
    return 0;
}
