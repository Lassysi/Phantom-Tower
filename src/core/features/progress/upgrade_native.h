#pragma once
#include <algorithm>
#include <array>
#include <cstring>
#include <windows.h>
#include "../../services/ue.h"
#include "upgrade_native.generated.h"
#include "upgrade_native_26367565.generated.h"
namespace pbz::upgrade {
inline bool GameCode(const void* p,size_t size) {
    MEMORY_BASIC_INFORMATION m{};
    if(!p||!VirtualQuery(p,&m,sizeof m)||m.State!=MEM_COMMIT||m.Type!=MEM_IMAGE||m.AllocationBase!=GetModuleHandleW(nullptr)||(m.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
    const auto address=reinterpret_cast<uintptr_t>(p),begin=reinterpret_cast<uintptr_t>(m.BaseAddress);const auto access=m.Protect&0xff;
    return address>=begin&&size<=m.RegionSize&&address-begin<=m.RegionSize-size&&(access==PAGE_EXECUTE||access==PAGE_EXECUTE_READ||access==PAGE_EXECUTE_READWRITE||access==PAGE_EXECUTE_WRITECOPY);
}
inline void* ResolveEnoughItems(ue::UObject* function,bool(*code)(const void*,size_t)=GameCode) {
    auto* thunk=ue::Get<uint8_t*>(function+0x128);std::array<uint8_t,0xbe> bytes{};
    if(!code(thunk,bytes.size())||!ue::Read(thunk,bytes.data(),bytes.size())||bytes[0xb9]!=0xe8)return nullptr;
    const bool legacy=std::equal(kEnoughThunkPrefix.begin(),kEnoughThunkPrefix.end(),bytes.begin());
    const bool current=std::equal(build26367565::kEnoughThunkPrefix.begin(),build26367565::kEnoughThunkPrefix.end(),bytes.begin());
    if(!legacy&&!current)return nullptr;
    int32_t relative=0;memcpy(&relative,bytes.data()+0xba,4);
    auto* target=thunk+0xbe+relative;std::array<uint8_t,kEnoughBody.size()> body{};
    if(!code(target,body.size())||!ue::Read(target,body.data(),body.size())||
       !((legacy&&body==kEnoughBody)||(current&&body==build26367565::kEnoughBody)))return nullptr;
    return target;
}
}
