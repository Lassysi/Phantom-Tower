#pragma once
#include <algorithm>
#include <cmath>

// Credits are denominated in health, not enemy damage. Small hits accumulate;
// damage sampled while at full health is never banked for a later injury.
namespace tower::leech {
#ifdef PBZ_PLAYTEST
inline constexpr int kPlaytestLeech = 0;  // 玩家测试包：吸血已验证（2026-11-12），3.1.0 后去掉固定 +10%，以免掩盖斩马刀临时损血
#else
inline constexpr int kPlaytestLeech = 0;
#endif
inline int Rate(int permanent, bool charm) { return std::max(0, permanent) + (charm ? 100 : 0); }
struct Credit {
    double pending = 0;
    void Reset() { pending = 0; }
    void Damage(double before, double after, int permille) {
        if (permille > 0 && std::isfinite(before) && std::isfinite(after) && before > after && after >= 0)
            pending += (before - after) * permille / 1000.0;
    }
    double Quote(double current, double maximum, bool final = false) {
        if (!std::isfinite(current) || !std::isfinite(maximum) || maximum <= 0 || current <= 0) return 0;
        if (current >= maximum - 0.001) { Reset(); return 0; }
        if (pending < 0.5 && !final) return 0;
        return std::min(pending, maximum - current);
    }
    void Applied(double amount, bool full) {
        if (full) Reset(); else pending = std::max(0.0, pending - amount);
    }
};
}
