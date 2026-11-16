#include <cstdio>
#include "../src/tower_summon_permit.h"

int main() {
    using namespace tower::summon;
    int failures = 0, checks = 0, own = 0, native = 0, calls = 0;
    auto check = [&](bool ok, const char* text) { ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n", text); } };
    int pawn, asc, otherPawn, otherAsc;
    const Identity one{&pawn, &asc, 1, 2}, two{&otherPawn, &otherAsc, 3, 4};
    bool alive = true;
    auto live = [&](Identity x) { return alive && x.Valid(); };
    auto add = [&](Identity) { ++calls; if (native + own) return false; ++own; return true; };
    auto remove = [&](Identity) { --own; };
    Lease lease;
    lease.Update(true, one, live, add, remove);
    check(lease.held && own == 1 && calls == 1, "fight acquires a single tag");
    lease.Update(true, one, live, add, remove);
    check(own == 1 && calls == 1, "repeated ticks do not stack tags");
    native = 1;
    lease.Update(false, one, live, add, remove);
    check(!lease.held && own == 0 && native == 1, "exit removes only the tower's tag and keeps the native source");
    lease.Update(true, one, live, add, remove);
    check(!lease.held && own == 0 && native == 1, "existing native permission is not claimed");
    native = 0;
    lease.Update(true, one, live, add, remove);
    lease.Update(true, two, live, add, remove);
    check(lease.held && lease.owner == two && own == 1, "character replacement releases the old lease before acquiring the new one");
    alive = false; lease.Update(false, two, live, add, remove);
    check(!lease.held && own == 1, "destroyed objects are never passed to removal");
    std::printf("TOWER_SUMMON_PERMIT_%s %d checks\n", failures ? "FAIL" : "OK", checks);
    return failures ? 1 : 0;
}
