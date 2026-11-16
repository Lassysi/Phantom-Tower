#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <cmath>
namespace tower::weaponpower {
inline int WrapperCall(const uint8_t* p, size_t n) {
    static constexpr uint8_t prefix[] = {0x48,0x8b,0x43,0x38,0x4c,0x8d,0x4c,0x24,0x58,0x48,0x8b,0x54,0x24,0x20,0x48,0x85,0xc0,0x48,0x8b,0x4c,0x24,0x28,0x4c,0x8b,0xc6,0x4c,0x0f,0x45,0xc8,0x48,0x8b,0x43,0x20,0x48,0x85,0xc0,0x40,0x0f,0x95,0xc7,0x48,0x03,0xf8,0x48,0x89,0x7b,0x20};
    static constexpr uint8_t suffix[] = {0x48,0x8b,0x5c,0x24,0x40,0x48,0x8b,0x74,0x24,0x50,0x48,0x83,0xc4,0x30,0x5f,0xc3};
    int found=-1;
    for(size_t i=0;p && i+sizeof(prefix)+5+sizeof(suffix)<=n;++i) {
        const size_t call=i+sizeof(prefix);
        if(!std::memcmp(p+i,prefix,sizeof(prefix)) && p[call]==0xe8 && !std::memcmp(p+call+5,suffix,sizeof(suffix))) {
            if(found>=0)return -1; found=static_cast<int>(call);
        }
    }
    return found;
}
inline bool NativeSignature(const uint8_t* p,size_t n) {
    static constexpr uint8_t start[]={0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x56,0x57,0x41,0x56,0x48,0x83,0xec,0x60,0x49,0x8b,0xf9,0x49,0x8b,0xf0,0x4c,0x8b,0xf2,0x48,0x8b,0xd9};
    static constexpr uint8_t outputs[]={0xf3,0x0f,0x11,0x07,0xf3,0x0f,0x11,0x06};
    return p && n>=0x57+sizeof(outputs) && !std::memcmp(p,start,sizeof(start)) && !std::memcmp(p+0x57,outputs,sizeof(outputs));
}
// 重量级打手：原生伤害 = 武器强度 × 角色攻击（攻击已含影塔攻击% 倍率 atkMul）。
// 用户定（2026-11-13，N9l 撤回"先加再乘"）：先 手持武器 × 攻击% 加成，再 + 15% 生命，合起来算总攻击（否则后期太高）。
// 引擎按 atkMul = 1 调用：强度' = 强度 + 15% 生命 / 当前攻击 → 伤害 = 强度 × 当前攻击 + 15% 生命。atkMul 参数只给测试 / 以后调整用
inline bool AddHealthPower(float power,float attack,float maxHealth,float* out,double atkMul=1.0,double share=0.15) {  // 3.6.0 重量级打手 Lv2 22% / Lv3 30%
    if(!out || !std::isfinite(power) || !std::isfinite(attack) || !std::isfinite(maxHealth) || !std::isfinite(atkMul) ||
       power<0 || attack<=0 || maxHealth<=0)return false;
    atkMul=std::clamp(atkMul,0.01,1000.0);
    const double value=static_cast<double>(power)+static_cast<double>(maxHealth)*std::clamp(share,0.0,1.0)*atkMul/attack;
    if(!std::isfinite(value) || value>10000)return false;
    *out=static_cast<float>(value);return true;
}
}
