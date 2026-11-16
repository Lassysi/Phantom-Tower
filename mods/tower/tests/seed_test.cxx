#include <cstdio>
#include <cstdlib>
#include "../src/tower_seed.h"
static void Check(bool ok, const char* why) { if (!ok) { std::printf("FAIL %s\n", why); std::exit(1); } }
int main() {
    using namespace tower::seedshare;
    const std::string good = "format=pbztower-seed-1\nmod=2.2.0\nplan=14\nseed=2204130\ndifficulty=easy\n";
    Seed s;
    Check(Parse(good, &s) && s.value == 2204130 && s.difficulty == 1, "valid imported seed");
    Check(!Parse(good + "seed=2\n", &s), "duplicate fields refused");
    auto t=good; t.replace(t.find("2.2.0"),5,"3.0.0"); Check(Parse(t,&s) && s.shuffle==0,"3.1.0: mod version is informational (3.0.0 installer seeds import)");
    t=good; t.replace(t.find("plan=14"),7,"plan=13"); Check(!Parse(t,&s),"plan mismatch refused");
    const std::string v2 = "format=pbztower-seed-2\nmod=3.1.0\nplan=14\nseed=77\ndifficulty=hard\nshuffle=2\n";
    Check(Parse(v2,&s) && s.value==77 && s.shuffle==2 && s.difficulty==0, "format 2 carries the boss shuffle");
    t=v2; t.replace(t.find("shuffle=2"),9,"shuffle=9"); Check(!Parse(t,&s),"bad shuffle refused");
    t=v2; t.replace(t.find("shuffle=2\n"),10,""); Check(!Parse(t,&s),"format 2 needs shuffle");
    t=good; t.replace(t.find("seed=2204130"),12,"seed=0"); Check(!Parse(t,&s),"zero seed refused");
    t=good; t.replace(t.find("seed=2204130"),12,"seed=18446744073709551616"); Check(!Parse(t,&s),"overflow refused");
    t=good; t.replace(t.find("difficulty=easy"),15,"difficulty=hard"); Check(Parse(t,&s) && s.difficulty==0,"hard difficulty preserved");
    std::puts("TOWER_SEED_OK");
}
