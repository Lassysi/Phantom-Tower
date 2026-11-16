#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../src/tower_weapon_cap_sig.h"
static void Check(bool ok, const char* why) { if (!ok) { std::printf("FAIL %s\n", why); std::exit(1); } }
int main() {
    // Actual stable prologue/formula bytes from build 26266538; call targets
    // and addresses are not part of the signature.
    std::array<uint8_t, 224> native{};
    const uint8_t head[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57};
    const uint8_t tail[] = {0xb9,0x06,0,0,0,0x48,0x8b,0x6c,0x24,0x38,0x3b,0xf1,0x0f,0x4c,0xce,0x48,0x8b,0x74,0x24,0x40,0x83,0xc1,0x06,0x8d,0x0c,0x89,0x3b,0xcf,0x0f,0x4c,0xf9,0x8b,0xc7,0x48,0x83,0xc4,0x20,0x5f,0xc3};
    std::memcpy(native.data(), head, sizeof head); std::memcpy(native.data()+0xa7, tail, sizeof tail);
    Check(tower::weaponcap::NativeSignature(native.data(),native.size()), "matching cap function accepted");
    native[0xb1] ^= 1; Check(!tower::weaponcap::NativeSignature(native.data(),native.size()), "formula mismatch rejected");
    native[0xb1] ^= 1; Check(!tower::weaponcap::NativeSignature(native.data(),0xc0), "short read rejected");
    std::array<uint8_t, 224> wrapper{};
    const uint8_t suffix[] = {0x48,0x8b,0x5c,0x24,0x40,0x89,0x06,0x48,0x83,0xc4,0x20,0x5f,0x5e,0x5d,0xc3};
    wrapper[0xb7] = 0xe8; std::memcpy(wrapper.data()+0xbc, suffix, sizeof suffix);
    Check(tower::weaponcap::WrapperCall(wrapper.data(),wrapper.size()) == 0xb7, "unique wrapper call found");
    wrapper[0x10] = 0xe8; std::memcpy(wrapper.data()+0x15, suffix, sizeof suffix);
    Check(tower::weaponcap::WrapperCall(wrapper.data(),wrapper.size()) < 0, "ambiguous call rejected");
    std::puts("TOWER_WEAPON_CAP_SIGNATURE_OK");
}
