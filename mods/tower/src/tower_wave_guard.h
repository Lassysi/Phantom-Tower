#pragma once

#include <cmath>
#include <initializer_list>

namespace tower::wave {
enum class Failure { Context, Monster, Venue, NoCandidates, Outstanding };
enum class Action { Retry, ReplaceMonster, ChangeVenue, Stop };

// Limits are per unfinished wave. Never promote a failure to a kill/clear.
struct Recovery {
    int floor = -1, wave = -1, attempts = 0, venues = 0;
    bool Begin(int f, int w) {
        if (f == floor && w == wave) return false;
        floor = f; wave = w; attempts = venues = 0; return true;
    }
    Action Next(Failure failure, int monster, bool cleaned) {
        if (!cleaned || failure == Failure::Outstanding || failure == Failure::NoCandidates || attempts >= 6) return Action::Stop;
        ++attempts;
        if (failure == Failure::Monster && monster >= 0) return Action::ReplaceMonster;
        if (failure == Failure::Context && attempts == 1) return Action::Retry;
        if (venues >= 3) return Action::Stop;
        ++venues;
        return Action::ChangeVenue;
    }
};

template <class Range, class Usable>
int ChooseVenue(int saved, int previous, int local, const Range& classics, Usable usable) {
    for (int candidate : {saved, previous, local}) if (usable(candidate)) return candidate;
    for (int candidate : classics) if (usable(candidate)) return candidate;
    return -1;
}

// A request is abandoned only to retry the unfinished wave, never as a kill.
struct RequestWatch {
    static constexpr double kTimeout = 30.0;
    bool pending = false;
    double waited = 0;
    void Begin() { pending = true; waited = 0; }
    void Finish() { pending = false; waited = 0; }
    bool Tick(double dt, bool paused) {
        if (!pending || paused || !std::isfinite(dt) || dt <= 0) return false;
        waited += dt;
        return waited >= kTimeout;
    }
};

struct TargetWatch {
    static constexpr double kTimeout = 20.0;
    static constexpr double kLastEnemyTimeout = 5.0;
    double since = -1;
    double nextDeathCheck = -1;
    void Found() { since = nextDeathCheck = -1; }
    bool CheckNativeDeath(double now) {
        if (!std::isfinite(now) || now < nextDeathCheck) return false;
        nextDeathCheck = now + 0.25;
        return true;
    }
    bool Missing(double now, bool lastUnfinished = false) {
        if (!std::isfinite(now)) return false;
        if (since < 0) since = now;
        return now - since >= (lastUnfinished ? kLastEnemyTimeout : kTimeout);
    }
};

// Preserve the existing zero-observed rule, including on subsequent frames.
inline bool MissingFinished(bool wave, bool dead, bool zeroObserved, bool nativeDead = false) {
    return wave && (dead || zeroObserved || nativeDead);
}
}
