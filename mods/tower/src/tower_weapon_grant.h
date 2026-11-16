#pragma once
#include <cstdint>
#include "../../../src/core/services/ue.h"
namespace towerweapongrant {
// One native grant (not a second presentation grant), only in the sandbox Tower session.
bool Grant(pbz::ue::UObject* bag, uint64_t weaponName, bool normalWeapon);
void Reset(); // only after leaving the sandbox trip, never between floors
}
