#define PBZ_HEX_TEST
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include "../src/tower_hex.h"
using namespace tower::hex;
static void Check(bool ok, const char* why) { if (!ok) { std::printf("FAIL: %s\n", why); std::exit(1); } }
int main(int argc, char** argv) {
    {
        State s; s.Init(20261110); s.picks[13] = Scientist; s.pickedAt[13] = 14; s.completed = 1u << 13;
        Check(s.CanPick(Scientist, 16), "3.6.0: scientist Lv1 can be upgraded in QA");
        { State t = s; t.picks[14] = Scientist; t.pickedAt[14] = 15; t.completed |= 1u << 14; Check(!t.CanPick(Scientist, 16) && !t.CanPick(Scientist, 17), "scientist stops at Lv2 in QA"); }
        Check(s.ScientistForm(17, 20261110) == 2 && s.ScientistBoost(17, 20261110).shachiCap == 0, "QA speed branch");
        State loaded; Check(State::Load(s.Save(), &loaded), "QA scientist persists"); s = loaded;
        Check(s.ScientistForm(20, 20261110) == 1 && s.ScientistBoost(20, 20261110).shachiCap == 1, "QA health and shachi branch");
    }
    // 2026-11-11 E 批：QA 只在第 1–10 层各弹一次，第 k 层第一张固定是计划里的新海克斯
    const auto& plan = kQaPlan;
    for (uint64_t seed = 1; seed <= 1000; ++seed) {
        State s; s.Init(seed);
        const int planFloors = static_cast<int>(std::size(plan));
        for (int floor = 1; floor <= planFloors; ++floor) {
            Check(s.Begin(floor), "QA offers on plan floors");
            Check(s.offers[0] == plan[floor - 1], "planned E-batch hex is the first card");
            Check(s.offers[1] != s.offers[0] && s.offers[2] != s.offers[0] && s.offers[1] != s.offers[2], "three distinct cards");
            Check(std::find(s.offers.begin(), s.offers.end(), Invincible) == s.offers.end(), "retired hex excluded from every QA card");
            State back; Check(State::Load(s.Save(), &back), "pending QA choice survives resume"); s = back;
            Check(s.Pick(0), "planned hex selectable");
            s.ClearFloor(floor);
            Check(State::Load(s.Save(), &back), "QA selection survives resume"); s = back;
        }
        for (Id id : plan) Check(s.Has(id), "all planned hexes held");
        for (int floor = planFloors + 1; floor <= 100; ++floor) Check(!s.Begin(floor), "no QA offers after the plan");
    }
    {   // 原 QA 最后一层已生成的候选仍可继续选，新局少一轮。
        State s; s.Init(11);
        s.pending = 22;
        s.offers = {Invincible, Tank, Swift};
        s.seen = (1ull << Invincible) | (1ull << Tank) | (1ull << Swift);
        State resumed;
        Check(State::Load(s.Save(), &resumed) && resumed.ReplaceRetiredOffer(1ull << Bell) && resumed.Pick(0),
              "pending offer on the former final QA milestone stays playable");
    }
    {   // 旧顺序下已经选了玻璃大炮、血之契约的局：下一张是计划里第一个没拿的（3.0.0 计划：质变）
        State s; s.Init(20261112);
        Check(s.Begin(1), "floor 1"); s.offers[0] = GlassCannon; Check(s.Pick(0), "pick glass"); s.ClearFloor(1);
        Check(s.Begin(2), "floor 2"); s.offers[0] = BloodPact; Check(s.Pick(0), "pick pact"); s.ClearFloor(2);
        Check(s.Begin(3, kPlayableMask & ~(1ull << ServantMaster)) && s.offers[0] == Transmutation, "first unowned in priority order is offered next");
    }
    if (argc > 1) {
        std::ifstream f(argv[1]); std::string line, saved;
        while (std::getline(f, line)) if (line.rfind("gr.hex=", 0) == 0) saved = line.substr(7);
        State s;
        Check(State::Load(saved, &s), "current test save loads");
        Check(s.pending == 0, "current test save has no pending choice");
    }
    std::puts("TOWER_HEX_QA_OK");
}
