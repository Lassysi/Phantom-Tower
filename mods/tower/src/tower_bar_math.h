#pragma once
#include <algorithm>
#include <cmath>

namespace tower::bar {
// Match the native capped length mapping. Fill is based on actual current/max,
// independently of the native visual cap.
inline bool Calculate(double current, double maximum, double nativeBase,
                      double nativeCap, double initialLength, double capLength,
                      double* length, double* fill) {
    if (!length || !fill || !std::isfinite(current) || !std::isfinite(maximum) ||
        !std::isfinite(nativeBase) || !std::isfinite(nativeCap) ||
        !std::isfinite(initialLength) || !std::isfinite(capLength) || maximum <= 0 ||
        nativeBase <= 0 || nativeCap <= nativeBase || initialLength <= 0 ||
        capLength < initialLength || capLength > 1) return false;
    const double t = std::clamp((maximum - nativeBase) / (nativeCap - nativeBase), 0.0, 1.0);
    *length = initialLength + t * (capLength - initialLength);
    *fill = std::clamp(current / maximum, 0.0, 1.0);
    return true;
}

// Live current value + the tower's confirmed maximum. While the game recomputes
// a pool attribute its maximum briefly reads as the unscaled native value, so
// the maximum is never taken from that live field; the current value is
// clamped into [0, maximum] (a recompute can scale it above for one frame).
inline bool Source(double current, double maximum, float* outCurrent, float* outMaximum) {
    if (!outCurrent || !outMaximum || !std::isfinite(current) || !std::isfinite(maximum) || maximum <= 0 || current < 0) return false;
    *outMaximum = static_cast<float>(maximum);
    *outCurrent = static_cast<float>(std::min(current, maximum));
    return true;
}
// A native progress-bar Tick only needs the tower's rewrite when the native
// code moved its fill or visible length away from what the tower last wrote.
inline bool NeedsReapply(double nativeFill, double nativeLength, double towerFill, double towerLength) {
    if (towerFill < 0 || towerLength < 0) return true;
    if (!std::isfinite(nativeFill) || !std::isfinite(nativeLength)) return true;
    return std::abs(nativeFill - towerFill) > 0.0005 || std::abs(nativeLength - towerLength) > 0.0005;
}

// The widget is created on scene changes as well as on reward selection.  The
// first sample establishes a baseline; it must never look like a reward.
struct LengthTransition {
    double maximum = 0, from = 0, target = 0;
    unsigned long long started = 0;
    bool initialized = false;

    double Sample(unsigned long long now) const {
        if (!initialized || now < started) return target;
        const double t = std::clamp(static_cast<double>(now - started) / 650.0, 0.0, 1.0);
        return from + (target - from) * (t * (2.0 - t));
    }
    bool Update(double value, double length, unsigned long long now) {
        if (!std::isfinite(value) || value <= 0 || !std::isfinite(length)) return false;
        if (!initialized) { maximum = value; from = target = length; started = now; initialized = true; return false; }
        const bool increased = value > maximum + 0.5;
        if (std::abs(value - maximum) > 0.5 || std::abs(length - target) > 0.0001) {
            from = increased ? Sample(now) : length;
            target = length; maximum = value; started = now;
        }
        return increased;
    }
};
}
