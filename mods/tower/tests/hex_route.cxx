#define PBZ_HEX_TEST
#include <cstdio>
#include "../src/tower_hex.h"
using namespace tower::hex;
int main() {
    constexpr int order[] = {Tank, Swift, Icathia, Leech, Ocean, Battle, BreakGrowth, Bell, Sturdy, Sharp, Keen, Pursuit, Bulwark, Desperate};
    State s; s.Init(20261110);
    for (int floor = 1; floor <= Count; ++floor) {
        if (!s.Begin(floor)) return 1;
        int slot = -1;
        for (int id : order) {
            for (int i = 0; i < 3; ++i) if (s.offers[i] == id && !s.Has(static_cast<Id>(id))) { slot = i; break; }
            if (slot >= 0) break;
        }
        if (slot < 0) return 2;
        const int id = s.offers[slot];
        std::printf("%02d slot=%d id=%d %.*s\n", floor, slot + 1, id, static_cast<int>(defs[id].name.size()), defs[id].name.data());
        if (!s.Pick(slot)) return 3;
        s.ClearFloor(floor);
    }
}
