#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "../src/tower_burn.h"
static void Check(bool ok, const char* why) { if (!ok) { std::printf("FAIL %s\n", why); std::exit(1); } }
int main() {
    using tower::burn::Effect;
    Effect a, b;
    a.Hit(10); Check(a.Due(10.99) == 0 && a.Due(11) == 1, "first tick at one second");
    Check(b.Due(11) == 0, "independent enemy is not affected");
    a.Hit(11.5); Check(a.Due(12) == 1 && a.Due(13) == 1 && a.Due(13.5) == 0 && !a.active, "hits during the burn do not refresh it: exactly 3 ticks");
    Check(std::abs(Effect::Damage(2000, 1) - 20) < 1e-9, "tick is one percent of maximum HP");
    a.Hit(14); Check(!a.active, "no burn during the 5 s cooldown");
    a.Hit(17.9); Check(!a.active, "still cooling down just before 18 s");
    a.Hit(18.0); Check(a.active && a.Due(19) == 1, "after the cooldown a new hit burns again");
    a.Clear(); Check(a.Due(21) == 0 && a.Ready(0), "clearing target cancels effect and cooldown");
    // 观察血量：自己的灼烧扣血不算受伤
    Effect o;
    Check(!o.Observe(0, 1000), "first enemy snapshot is baseline");
    Check(o.Observe(1, 900), "player damage starts burn");
    Check(o.Due(2) == 1, "burn tick due");
    o.Applied(890);
    Check(!o.Observe(2.1, 890), "own tick is not damage");
    Check(!o.Observe(2.5, 850), "damage during the burn does not refresh");
    Check(o.Due(3) == 1 && o.Due(4) == 1 && !o.active, "burn ends after three seconds");
    Check(!o.Observe(6, 800), "damage during the cooldown does not trigger");
    Check(!o.Observe(8.9, 790), "cooldown lasts five seconds after the burn ends");
    Check(o.Observe(9.0, 780) && o.active, "damage after the cooldown burns again");
    // 一个 100 秒的 Boss 战最多灼烧约 100/8×3 = 37 跳（原来约 100 跳）
    Effect c; int ticks = 0; double hp = 1e6;
    for (int ms = 0; ms <= 100000; ms += 50) { const double t = ms / 1000.0; hp -= 1; c.Observe(t, hp); ticks += c.Due(t); }
    Check(ticks >= 33 && ticks <= 39, "continuous hitting: about 3 ticks per 8 seconds");
    std::puts("TOWER_BURN_OK");
}
