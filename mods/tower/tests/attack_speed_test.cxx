#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include "../src/tower_attack_speed.h"
static void Check(bool ok, const char* why) { if (!ok) { std::printf("FAIL %s\n", why); std::exit(1); } }
int main(int argc, char** argv) {
    std::array<uint8_t, 160> b{};
    const uint8_t prefix[] = {0xf3,0x0f,0x11,0x4c,0x24,0x10,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9};
    std::memcpy(b.data(), prefix, sizeof prefix);
    const uint8_t read[] = {0x0f,0x2e,0x83,0xa8,0,0,0};
    const uint8_t write[] = {0xf3,0x0f,0x11,0x83,0xa8,0,0,0};
    std::memcpy(b.data()+30, read, sizeof read); std::memcpy(b.data()+120, write, sizeof write);
    Check(tower::attackspeed::DecodeRateOffset(b.data(),b.size(),0x320)==0xa8, "matching reads and writes");
    std::memcpy(b.data()+140, write, sizeof write);
    Check(tower::attackspeed::DecodeRateOffset(b.data(),b.size(),0x320)==0xa8, "two matching return paths");
    b[144]=0xac;
    Check(tower::attackspeed::DecodeRateOffset(b.data(),b.size(),0x320)<0, "conflicting second return path fails closed");
    b[144]=0xa8;
    b[124]=0xac;
    Check(tower::attackspeed::DecodeRateOffset(b.data(),b.size(),0x320)<0, "mismatched fields fail closed");
    b[124]=0xa8;
    Check(tower::attackspeed::DecodeRateOffset(b.data(),b.size(),0xa9)<0, "field must fit object");
    b[0]=0;
    Check(tower::attackspeed::DecodeRateOffset(b.data(),b.size(),0x320)<0, "unknown code fails closed");
    if (argc > 1) {
        std::ifstream fixture(argv[1], std::ios::binary);
        fixture.read(reinterpret_cast<char*>(b.data()), b.size());
        Check(fixture.gcount() == b.size(), "captured fixture size");
        Check(tower::attackspeed::DecodeRateOffset(b.data(), b.size(), 0x320) == 0xa8, "captured native setter signature");
    }
    std::puts("TOWER_ATTACK_SPEED_SIGNATURE_OK");
}
