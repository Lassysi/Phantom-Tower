#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace tower::attackspeed {
// Identify the hidden current rate by matching the native setter's read and
// write of the same field; never accept a field offset by itself.
inline int DecodeRateOffset(const uint8_t* code, size_t n, int objectSize) {
    if (!code || n < 20 || objectSize < 1) return -1;
    constexpr uint8_t prefix[] = {0xf3,0x0f,0x11,0x4c,0x24,0x10,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9};
    if (std::memcmp(code, prefix, sizeof prefix) != 0) return -1;
    int found = -1, reads = 0, writes = 0;
    for (size_t i = sizeof prefix; i + 8 <= n; ++i) {
        const bool read = code[i] == 0x0f && code[i+1] == 0x2e && code[i+2] == 0x83;
        const bool write = code[i] == 0xf3 && code[i+1] == 0x0f && code[i+2] == 0x11 && code[i+3] == 0x83;
        if (!read && !write) continue;
        int32_t off = -1; std::memcpy(&off, code + i + (read ? 3 : 4), 4);
        if (off < 0x28 || off + 4 > objectSize || (found >= 0 && found != off)) return -1;
        found = off; reads += read ? 1 : 0; writes += write ? 1 : 0;
    }
    // The verified setter has two return paths, both storing the same field.
    return reads == 1 && writes >= 1 && writes <= 2 ? found : -1;
}
}

namespace towerspeed {
bool Resolve();
bool Ready();
void Tick(void* pawn, bool enabled, float rate = 1.5f);
void Reset();
bool SetMultiplier(float multiplier);
float Multiplier();
}
