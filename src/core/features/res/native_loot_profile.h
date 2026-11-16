#pragma once
#include <cstdint>
namespace pbz::res::loot::signature {
struct Spec { const char* name; unsigned bytes; uint64_t hash; };
struct Profile {
    uint32_t timestamp, imageSize;
    const Spec* execs;
    unsigned count, addCallOffset;
    uint64_t addPrologueHash;
};
}
