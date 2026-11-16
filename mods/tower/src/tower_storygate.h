#pragma once
#include <string>
#include "tower_replay.h"

// Native Blueprint Flow-node entry gate, before camera/input changes and async sequence work.
// Session-scoped: rewards/ready screens must not release parked post-kill flows.
namespace towerstorygate {
void Session(bool inside);
void SetReplay(const tower::replay::Contract* contract);
bool Ready(std::string* why);
bool Protected(std::string* why); // shared VM + exact Flow identities, including newly loaded scene classes
bool Cinematic(); // only observed live sequence players (playing/paused), no guessed callback timing
bool HadCinematic();
enum class Entry { Waiting, Started, Failed };
Entry Start(const tower::replay::Contract& contract,std::string* why);
}
