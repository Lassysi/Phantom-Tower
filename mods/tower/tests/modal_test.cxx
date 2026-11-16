#include "../src/tower_modal_logic.h"
#include <cstdio>
using namespace towerwidgets::modal;
int main() {
    int checks = 0;
    auto test = [&](bool ok, const char* name) { ++checks; if (!ok) { std::printf("FAIL %s\n", name); return false; } return true; };
    // Read-only live vector: HUD active=1, focus-support=1, modal=0; router
    // active input mode=Game and IsMenuInputModeUI=0. Old predicate blocked it.
    const bool oldHudBlock = true && (false || true);
    if (!test(oldHudBlock && !Blocks(true, InputMode::Game, false, true, true, false), "observed HUD false-positive rejected")) return 1;
    if (!test(Blocks(true, InputMode::Menu, true, true, true, false), "real external weapon modal still blocks")) return 1;
    if (!test(Blocks(true, InputMode::All, true, true, true, false), "native mixed mode with menu capture blocks")) return 1;
    if (!test(!Blocks(true, InputMode::All, false, true, true, false), "passive overlay without menu capture does not block")) return 1;
    if (!test(!Blocks(true, InputMode::Menu, true, true, true, true), "Tower's own dialog does not block itself")) return 1;
    if (!test(!Blocks(true, InputMode::Menu, true, true, false, false), "deactivated pooled widget does not block")) return 1;
    if (!test(!Blocks(true, InputMode::Menu, true, false, true, false), "stale widget does not block")) return 1;
    if (!test(!Blocks(false, InputMode::Menu, true, true, true, false), "missing router cannot manufacture a modal")) return 1;
    if (!test(!Blocks(true, InputMode::Game, true, true, true, false), "inconsistent game-mode observation is not a modal")) return 1;
    if (!test(!Blocks(true, InputMode::Unknown, true, true, true, false), "unrecognized enum is not guessed")) return 1;
    std::printf("TOWER_MODAL_OK %d checks (recorded live HUD vector and native menu cases)\n", checks);
}
