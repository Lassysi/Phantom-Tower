#pragma once
#include <algorithm>
#include <cmath>

namespace tower::burn {
// 火上浇油（3.2.0 用户定）：敌人被打掉血 → 灼烧 3 秒（每秒 1% 最大生命，期间再挨打不刷新）→ 冷却 5 秒（期间无论发生什么都不触发）
// → 冷却结束后再被打掉血才重新灼烧。影塔自己的灼烧扣血记为基准，不算"受伤"。每个敌人一份
struct Effect {
    static constexpr double kSeconds = 3.0, kCooldown = 5.0;
    double ends = 0, next = 0, cooldownUntil = -1;
    double lastHealth = -1;
    bool active = false;
    void Clear() { *this = {}; }
    bool Ready(double now) const { return !active && now >= cooldownUntil; }
    void Hit(double now) {
        if (!std::isfinite(now) || now < 0 || !Ready(now)) return;  // 灼烧中不刷新、冷却中不触发
        active = true; next = now + 1.0; ends = now + kSeconds;
    }
    bool Observe(double now, double health) {
        if (!std::isfinite(health) || health < 0) { lastHealth = -1; return false; }
        const bool dropped = lastHealth >= 0 && health < lastHealth - .001;
        lastHealth = health;
        if (!dropped || !Ready(now)) return false;
        Hit(now);
        return true;
    }
    void Applied(double health) { lastHealth = std::isfinite(health) && health >= 0 ? health : -1; }
    int Due(double now) {
        if (!active || !std::isfinite(now)) return 0;
        int n = 0;
        while (next <= now && next <= ends + 1e-9 && n < 3) { ++n; next += 1.0; }
        if (now >= ends) { active = false; cooldownUntil = ends + kCooldown; }
        return n;
    }
    static double Damage(double maximum, int ticks, double rate = .01) {  // 3.6.0 火上浇油 Lv2 1.5%
        return maximum > 0 && std::isfinite(maximum) ? maximum * rate * std::clamp(ticks, 0, 3) : 0;
    }
};
}
