// Exercise the real attribute arbiter with an in-memory UE target, never the game.
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../src/tower_rewards.h"
#include "../../../src/core/services/attributes.cpp"

namespace pbz::ue {
bool Read(const void* p, void* out, size_t n) { if (!p) return false; std::memcpy(out, p, n); return true; }
bool Write(void* p, const void* in, size_t n) { if (!p) return false; std::memcpy(p, in, n); return true; }
bool Live(UObject* p) { return p != nullptr; }
UObject* FindClass(std::string_view) { return nullptr; }
int PropOffset(UObject*, std::string_view) { return -1; }
int Num() { return 0; }
int ObjBatch(int, int, UObject**) { return 0; }
}
static void Check(bool ok, const char* s) { if (!ok) { std::printf("FAIL %s\n", s); std::exit(1); } }
int main() {
    namespace a = pbz::attr; namespace rw = tower::rewards;
    alignas(16) std::array<uint8_t, 128> actor{}, cls{};
    alignas(16) std::array<uint8_t, 256> set{};
    auto* classPtr = cls.data(); auto* setPtr = set.data();
    std::memcpy(actor.data() + pbz::ue::Layout::ObjClass, &classPtr, sizeof(classPtr));
    std::memcpy(actor.data() + 0x28, &setPtr, sizeof(setPtr));
    a::g_setOff = 0x28;
    a::Target t{actor.data(), set.data(), a::Who::Player, cls.data()};
    rw::AttrTotals totals;
    totals.Add(0, 100); totals.Add(1, 50); totals.Add(2, 200); totals.Add(3, 150); totals.Add(4, 80);
    const int offs[] = {0x00, 0x20, 0x40, 0x60, 0x80};
    const float base[] = {200, 100, 1000, 400, 1};
    for (int i = 0; i < 5; ++i) *a::AttrCurrent(t, offs[i]) = base[i];
    for (int frame = 0; frame < 30; ++frame) {
        for (int i = 0; i < 5; ++i) {
            float m = i == 4 ? 1.0f - totals.pm[i] / 1000.0f : static_cast<float>(totals.Mul(static_cast<rw::AttrKind>(i)));
            a::Want(t, a::AttrCurrent(t, offs[i]), a::Kind::Scale, m);
        }
        a::Commit();
    }
    const float expected[] = {220, 105, 1200, 460, 0.92f};
    for (int i = 0; i < 5; ++i) Check(std::abs(*a::AttrCurrent(t, offs[i]) - expected[i]) < 0.001f, "correct value without per-frame compounding");
    a::ReleaseAll();
    for (int i = 0; i < 5; ++i) Check(std::abs(*a::AttrCurrent(t, offs[i]) - base[i]) < 0.001f, "release restores original value");
    std::puts("TOWER_GROWTH_EFFECT_OK");
}
