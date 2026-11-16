#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "../src/tower_leech.h"
static void Check(bool ok, const char* message) { if (!ok) { std::printf("FAIL %s\n", message); std::exit(1); } }
int main() {
    Check(tower::leech::Rate(60, false) == 60 && tower::leech::Rate(60, true) == 160 && tower::leech::Rate(0, true) == 100, "talisman adds ten percentage points only while active");
    tower::leech::Credit c;
    c.Damage(100, 96, 50);
    Check(c.Quote(50, 100) == 0, "small heals are retained");
    c.Damage(96, 90, 50);
    Check(std::abs(c.Quote(50, 100) - 0.5) < 1e-6, "small hits accumulate");
    c.Applied(0.5, false);
    c.Damage(1000, 800, 50);
    Check(c.Quote(80, 100) == 10, "five percent actual enemy hp loss");
    Check(c.Quote(98, 100) == 2, "heal capped at max hp");
    c.Applied(2, true);
    Check(c.pending == 0, "excess is not banked");
    c.Damage(100, 80, 50); c.Quote(100, 100);
    Check(c.pending == 0, "damage at full health cannot be used later");
    c.Damage(100, 96, 50);
    Check(c.Quote(50, 100, true) > 0.19, "last hit settles sub-half-point credit");
    Check(c.Quote(0, 100, true) == 0, "cannot revive dead player");
    c.Reset(); c.Damage(100, 110, 50); c.Damage(100, -1, 50);
    Check(c.pending == 0, "healing and invalid samples do not count as damage");
    std::puts("TOWER_LEECH_OK");
}
