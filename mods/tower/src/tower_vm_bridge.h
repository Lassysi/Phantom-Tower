#pragma once
#include <windows.h>
#include <array>
#include <cstring>
#include "../../../src/core/services/abshook.h"

namespace towervm::bridge {
// Build26266538: observed shared ProcessInternal wrapper reads FFrame.Node+10,
// then calls the script interpreter. Match instructions, never a sampled RVA.
// 3.0.0 兼容：入口被别的 Mod（UE4SS 等）写成了跳转
inline bool ForeignJump(const uint8_t* p) {
    return p && (p[0] == 0xE9 || p[0] == 0xEB || (p[0] == 0xFF && p[1] == 0x25) || (p[0] == 0x48 && p[1] == 0xB8 && p[10] == 0xFF && p[11] == 0xE0) ||
                 (p[0] == 0x49 && p[1] == 0xBB && p[10] == 0x41 && p[11] == 0xFF && p[12] == 0xE3));
}
inline void* Interpreter(const uint8_t* wrapper) {
    static constexpr uint8_t prefix[]={0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x30,0x4c,0x8b,0x72,0x10};
    static constexpr uint8_t callSite[]={0x4c,0x8b,0xc7,0x48,0x8b,0xd5,0x48,0x8b,0xcb,0xe8};
    // 包装入口被挂（UE4SS 挂 ProcessInternal）时前 15 字节（三条 5 字节 mov）已被改写，只核对其后的指令
    const size_t skip = ForeignJump(wrapper) ? 15 : 0;
    if(!wrapper || std::memcmp(wrapper+skip,prefix+skip,sizeof prefix-skip) || std::memcmp(wrapper+0x66,callSite,sizeof callSite))return nullptr;
    int32_t rel=0;std::memcpy(&rel,wrapper+0x70,4);
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(wrapper)+0x74+rel);
}
inline void Jump(uint8_t* out,const void* address) {
    const uint8_t op[]={0xff,0x25,0,0,0,0};std::memcpy(out,op,6);std::memcpy(out+6,&address,8);
}
inline bool Signature(const uint8_t* p) {
    static constexpr uint8_t prefix[]={0x48,0x89,0x5c,0x24,0x20,0x56,0x57,0x41,0x56,0x48,0x83,0xec,0x70,0x48,0x8b,0x05};
    static constexpr uint8_t after[]={0x48,0x33,0xc4,0x48,0x89,0x44,0x24,0x60,0x48,0x8b,0x42,0x20};
    return p && !std::memcmp(p,prefix,sizeof prefix) && !std::memcmp(p+20,after,sizeof after);
}
// The first 20 bytes contain one RIP-relative MOV RAX,[cookie]. Relocate it to
// MOV RAX,moffs64: same destination, flags and value, no scratch register.
// Other instructions keep their offsets; suspended threads at their boundaries
// can use the SDK's existing original/trampoline IP translation.
inline bool Prepare(pbz::abshook::Hook& h,uint8_t* target,void* detour,std::string* why) {
    if(!Signature(target)){
        // 解释入口已被其他 Mod 挂接：交给核心通用钩子（顺着跳转叠加，或只换跳转槽），"原函数"就是它们的入口
        if(ForeignJump(target)){
            if(!pbz::abshook::Prepare(h,target,detour,why))return false;
            if(why)*why="";
            return true;
        }
        if(why)*why="蓝图解释入口指令特征不匹配";return false;
    }
    int32_t displacement=0;std::memcpy(&displacement,target+16,4);
    const void* cookie=target+20+displacement;
    auto* code=static_cast<uint8_t*>(VirtualAlloc(nullptr,64,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if(!code){if(why)*why="蓝图解释入口跳板分配失败";return false;}
    std::memcpy(code,target,13);code[13]=0x48;code[14]=0xa1;std::memcpy(code+15,&cookie,8);
    Jump(code+23,target+20);
    DWORD old=0;
    if(!VirtualProtect(code,64,PAGE_EXECUTE_READ,&old)){VirtualFree(code,0,MEM_RELEASE);if(why)*why="蓝图解释入口跳板保护失败";return false;}
    FlushInstructionCache(GetCurrentProcess(),code,64);
    h.target=target;h.tramp=code;h.len=20;h.detour=detour;std::memcpy(h.orig,target,20);
    return true;
}
inline bool OwnedBytes(const pbz::abshook::Hook& h) {
    if(h.target && h.slot)return *reinterpret_cast<void* const volatile*>(h.slot)==(h.on?h.detour:h.prev);  // 叠加模式：只看跳转槽
    if(h.target && h.len>=14 && h.len!=20 && h.len<=32){  // 通用远跳（叠加在别的 Mod 代码上）
        if(!h.on)return std::memcmp(h.target,h.orig,h.len)==0;
        uint8_t expected[32];std::memset(expected,0x90,sizeof expected);Jump(expected,h.detour);
        return !std::memcmp(h.target,expected,h.len);
    }
    if(!h.target || h.len!=20)return false;
    if(!h.on)return std::memcmp(h.target,h.orig,20)==0;
    std::array<uint8_t,20> expected;expected.fill(0x90);Jump(expected.data(),h.detour);
    return !std::memcmp(h.target,expected.data(),20);
}
}
