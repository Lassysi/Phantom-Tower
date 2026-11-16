#include <cmath>
#include "../src/tower_weapon_power_sig.h"
#include <array>
#include <cstdio>
#include <cstring>
int main() {
    const unsigned char prefix[]={0x48,0x8b,0x43,0x38,0x4c,0x8d,0x4c,0x24,0x58,0x48,0x8b,0x54,0x24,0x20,0x48,0x85,0xc0,0x48,0x8b,0x4c,0x24,0x28,0x4c,0x8b,0xc6,0x4c,0x0f,0x45,0xc8,0x48,0x8b,0x43,0x20,0x48,0x85,0xc0,0x40,0x0f,0x95,0xc7,0x48,0x03,0xf8,0x48,0x89,0x7b,0x20};
    const unsigned char suffix[]={0x48,0x8b,0x5c,0x24,0x40,0x48,0x8b,0x74,0x24,0x50,0x48,0x83,0xc4,0x30,0x5f,0xc3};
    const unsigned char start[]={0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x56,0x57,0x41,0x56,0x48,0x83,0xec,0x60,0x49,0x8b,0xf9,0x49,0x8b,0xf0,0x4c,0x8b,0xf2,0x48,0x8b,0xd9};
    const unsigned char outputs[]={0xf3,0x0f,0x11,0x07,0xf3,0x0f,0x11,0x06};
    std::array<unsigned char,384> w{}, b{};
    std::memcpy(w.data()+0x124,prefix,sizeof prefix); w[0x153]=0xe8; std::memcpy(w.data()+0x158,suffix,sizeof suffix);
    std::memcpy(b.data(),start,sizeof start); std::memcpy(b.data()+0x57,outputs,sizeof outputs);
    if(tower::weaponpower::WrapperCall(w.data(),w.size())!=0x153 || !tower::weaponpower::NativeSignature(b.data(),b.size()))return 1;
    b[4]^=1; if(tower::weaponpower::NativeSignature(b.data(),b.size()))return 2;
    w[0x158]^=1; if(tower::weaponpower::WrapperCall(w.data(),w.size())!=-1)return 3;
    float result=0;
    if (!tower::weaponpower::AddHealthPower(3, 1000, 2000, &result) || std::abs(result-3.3f)>0.0001f) return 4;
    if (!tower::weaponpower::AddHealthPower(3, 1000, 10000, &result) || std::abs(result-4.5f)>0.0001f) return 5;
    if (tower::weaponpower::AddHealthPower(3, 0, 10000, &result)) return 6;
    // N9l：攻击% ×2 时生命那部分也翻倍（先加到武器、再乘攻击%）。基础攻击 500、当前 1000、生命 2000：
    // 期望伤害 = (3×500 + 300) × 2 = 3600 = 强度' × 1000 → 强度' = 3.6
    if (!tower::weaponpower::AddHealthPower(3, 1000, 2000, &result, 2.0) || std::abs(result-3.6f)>0.0001f) return 7;
    if (!tower::weaponpower::AddHealthPower(3, 1000, 2000, &result, 1.0) || std::abs(result-3.3f)>0.0001f) return 8;  // 无加成与旧算法一致
    if (!tower::weaponpower::AddHealthPower(3, 1000, 2000, &result, 0.5) || std::abs(result*1000-(3*2000+300)*0.5f)>0.01f) return 9;  // 攻击% 为负也一起缩
    if (tower::weaponpower::AddHealthPower(3, 1000, 2000, &result, std::nan(""))) return 10;
    std::puts("TOWER_WEAPON_POWER_SIGNATURE_OK");
}
