#pragma once
#include <functional>
#include <string>
#include "../../../src/core/services/ue.h"

// Exact script-function boundary, including local Blueprint calls which bypass
// ProcessEvent. Module-owned, game-thread installed, executable fingerprint gated.
namespace towervm {
enum class Action { Pass, Block, Cleanup };
using Handler=std::function<Action(pbz::ue::UObject*, pbz::ue::UObject*, void*)>;
bool Add(pbz::ue::UObject* function, Handler handler, std::string* why);
void Class(std::string name, Handler handler); // catches first call of a newly loaded Blueprint class
void Remove(pbz::ue::UObject* function);
bool Ready();
bool Faulted();
void End();
}
