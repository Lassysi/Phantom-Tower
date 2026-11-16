#pragma once
#include <utility>
#include "tower_logic.h"
#include "tower_floorplay.h"

namespace tower {
// An entry failure rolls the run back to Ready. Keep only the identity of the
// failed attempt, so a later explicit skip does not roll back player resources.
struct FailedStage {
    std::string run;
    int readyFloor = -1, floor = 0, mobFloor = 0, mobTotal = 0;
    bool wave = false;
    std::vector<int> bosses;

    // A scheduled encounter can fail before ForcedFloor: retain its exact
    // identity only for a subsequent explicit, confirmed skip.
    static FailedStage Planned(const Run& current, const floorplan::Floor& plan) {
        if (current.phase != Phase::Ready || current.id.empty() || plan.floor != current.floor + 1) return {};
        const int count = floorplay::Later(plan.mode) ? 1 : plan.count;
        if (count < 1 || count > 3) return {};
        std::vector<int> bosses;
        for (int i = 0; i < count; ++i) {
            const int b = floorplay::PoolIndex(plan.bosses[i]);
            if (b < 0) return {};
            bosses.push_back(b);
        }
        return {current.id, current.floor, plan.floor, 0, 0, false, std::move(bosses)};
    }

    bool Available(const Run& current) const {
        return current.phase == Phase::Ready && !run.empty() && run == current.id && readyFloor == current.floor;
    }
    bool RestoreBoss(Run& current) const {
        if (!Available(current) || wave || floor != current.floor + 1 || bosses.empty()) return false;
        return current.ForcedFloor(bosses) > 0;
    }
};
}
