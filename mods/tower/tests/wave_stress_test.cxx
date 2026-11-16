// Repeatable stress of actual wave selection, retry policy and shuffled encounter
// contracts. This is not a game/physics/AI or late-callback integration test.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>
#include "../src/tower_logic.h"
#include "../src/tower_mobs.h"
#include "../src/tower_wave_guard.h"
#include "../src/tower_floorplay.h"

int main() {
    using namespace tower;
    namespace m = tower::mobs;
    namespace w = tower::wave;
    namespace f = tower::floorplay;
    size_t waves = 0, monsterRetries = 0, venueRetries = 0, bossContracts = 0;
    auto require = [](bool ok, const char* why, int seed, int floor, int wave) {
        if (!ok) { std::printf("FAIL %s seed=%d floor=%d wave=%d\n", why, seed, floor, wave); std::exit(1); }
    };
    for (int seed = 1; seed <= 512; ++seed) {
        for (int floor = 1; floor <= 50; ++floor) {
            const auto shuffled = f::ScheduledFloor(seed, floor, true);
            const auto* source = f::Find(f::EncounterFloor(seed, floor, true));
            require(source && shuffled.mode == source->mode && shuffled.count == source->count &&
                    shuffled.venue == source->venue && shuffled.replay == source->replay, "shuffled encounter contract", seed, floor, 0);
            for (int i = 0; i < 3; ++i) require(shuffled.bosses[i] == source->bosses[i] && shuffled.allies[i] == source->allies[i], "shuffled boss identity", seed, floor, i);
            for (int i = 0; i < 2; ++i) require(shuffled.alts[i] == source->alts[i], "shuffled boss backup", seed, floor, i);
            ++bossContracts;
        }
        for (int floor = 2; floor <= 50; ++floor) {
            const int zone = (floor - 1) / 10 + 1, total = m::WaveCount(floor);
            for (int wave = 0; wave < total; ++wave) {
                const auto plan = m::Plan(floor, wave, total);
                const auto picks = m::Pick(seed, floor, wave, zone, plan, [](int) { return false; });
                require(picks.size() == static_cast<size_t>(plan.normal + plan.elite), "complete wave", seed, floor, wave);
                std::unordered_set<int> excluded;
                w::Recovery recovery; require(recovery.Begin(floor, wave), "new wave", seed, floor, wave);
                require(recovery.Next(w::Failure::Monster, picks.front(), true) == w::Action::ReplaceMonster, "replace vanished mob", seed, floor, wave);
                excluded.insert(picks.front());
                const auto retry = m::Pick(seed, floor, wave, zone, plan, [&](int i) { return excluded.count(i) != 0; });
                require(retry.size() == picks.size(), "replacement keeps full wave", seed, floor, wave);
                for (int i : retry) require(!excluded.count(i), "failed monster excluded", seed, floor, wave);
                ++monsterRetries;
                std::array<int, 5> venues{0, 1, 2, 3, 4}; std::unordered_set<int> bad{0};
                require(recovery.Next(w::Failure::Venue, -1, true) == w::Action::ChangeVenue, "change bad venue", seed, floor, wave);
                const int venue = w::ChooseVenue(0, 0, 0, venues, [&](int v) { return v >= 0 && !bad.count(v); });
                require(venue == 1, "venue actually changes", seed, floor, wave);
                ++venueRetries; ++waves;
                require(!recovery.Begin(floor, wave) && recovery.attempts == 2, "retry budget survives same wave", seed, floor, wave);
                require(recovery.Next(w::Failure::Outstanding, -1, true) == w::Action::Stop, "unknown in-flight spawn cannot retry", seed, floor, wave);
                require(recovery.Next(w::Failure::Venue, -1, false) == w::Action::Stop, "failed cleanup cannot retry", seed, floor, wave);
            }
        }
    }
    w::Recovery r; r.Begin(3, 0);
    for (int i = 0; i < 6; ++i) require(r.Next(w::Failure::Monster, i, true) == w::Action::ReplaceMonster, "bounded replacement", 0, 3, i);
    require(r.Next(w::Failure::Monster, 8, true) == w::Action::Stop, "retry limit", 0, 3, 0);
    require(r.Begin(3, 1) && r.attempts == 0, "new wave resets retries", 0, 3, 1);
    require(r.Next(w::Failure::NoCandidates, -1, true) == w::Action::Stop, "empty pool cannot be a clear", 0, 3, 1);
    w::Recovery c; c.Begin(4, 0);
    require(c.Next(w::Failure::Context, -1, true) == w::Action::Retry, "one transient retry", 0, 4, 0);
    for (int i = 0; i < 3; ++i) require(c.Next(w::Failure::Venue, -1, true) == w::Action::ChangeVenue, "bounded venue changes", 0, 4, i);
    require(c.Next(w::Failure::Venue, -1, true) == w::Action::Stop, "exhausted venues stop", 0, 4, 0);
    std::array<int, 5> none{0, 1, 2, 3, 4};
    require(w::ChooseVenue(-1, 0, 1, none, [](int) { return false; }) == -1, "unusable venues cannot be selected", 0, 4, 0);
    std::printf("WAVE_STRESS_OK %zu waves, %zu monster replacements, %zu venue fallbacks, %zu shuffled boss contracts (offline)\n", waves, monsterRetries, venueRetries, bossContracts);
    return 0;
}
