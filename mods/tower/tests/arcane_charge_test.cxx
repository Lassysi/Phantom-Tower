#include "../src/tower_arcane_charge.h"
#include <cmath>
#include <cstdio>
#include <limits>
int main() {
    using tower::arcane::NativeAttackGain;
    if (NativeAttackGain(0,100,0,1)!=40 || NativeAttackGain(75,100,0,1)!=40) return 1;
    if (NativeAttackGain(0,100,1,1)!=0 || NativeAttackGain(0,100,1,2)!=40) return 2;
    if (NativeAttackGain(55,200,0,2)!=80) return 3;
    if (NativeAttackGain(-1,100,0,1)!=0 || NativeAttackGain(0,0,0,1)!=0 ||
        NativeAttackGain(0,100,0.5f,1)!=0 || NativeAttackGain(0,std::numeric_limits<float>::infinity(),0,1)!=0) return 5;
    std::unordered_map<void*,int> targets;int a,b;
    if (!tower::arcane::RegisterEnemy(targets,&a,1) || tower::arcane::RegisterEnemy(targets,&a,1) ||
        !tower::arcane::RegisterEnemy(targets,&b,2) || !tower::arcane::RegisterEnemy(targets,&a,3) ||
        tower::arcane::RegisterEnemy(targets,nullptr,1)) return 7;
    std::puts("TOWER_ARCANE_CHARGE_OK");
}
