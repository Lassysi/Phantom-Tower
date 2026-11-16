#pragma once
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace tower::arcane {
// Multiple trace bullets can report the same enemy during one swing. Different
// enemy objects each count once; object index distinguishes pointer reuse.
inline bool RegisterEnemy(std::unordered_map<void*,int>& targets,void* enemy,int index) {
    if(!enemy || index<0)return false;
    auto it=targets.find(enemy);
    if(it!=targets.end() && it->second==index)return false;
    targets[enemy]=index;return true;
}
// Only calculate the additive amount. The game performs all charge-count
// rollover and delegate/UI work in its own attribute modification code.
inline float NativeAttackGain(float value, float maximum, float count, float countMax) {
    if (!std::isfinite(value) || !std::isfinite(maximum) || !std::isfinite(count) || !std::isfinite(countMax) ||
        maximum <= 0 || maximum > 1000000 || countMax < 1 || countMax > 100 ||
        value < 0 || value > maximum || count < 0 || count > countMax ||
        std::abs(count - std::round(count)) > 0.001f || std::abs(countMax - std::round(countMax)) > 0.001f ||
        count >= countMax) return 0;
    return maximum * 0.4f;
}
}
