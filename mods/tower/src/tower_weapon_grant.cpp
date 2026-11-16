#include "tower_weapon_grant.h"
#include <array>
#include <cstring>
#include <set>
#include <tuple>
#include "../../../src/core/sdk/pbz_module.h"
#include "../../../src/core/host/log.h"
#include "tower_api.h"
#include "tower_isolation.h"
namespace towersb { bool Active(); }
namespace towerweapongrant {
namespace {std::set<std::tuple<pbz::ue::UObject*,pbz::ue::UObject*,pbz::ue::UObject*,uint64_t,bool>> attempted;}
void Reset(){attempted.clear();}
bool Grant(pbz::ue::UObject* bag, uint64_t name, bool normal) {
    namespace ue=pbz::ue;namespace pe=pbz::pe;
    if (!towerapi::InsideTower() || !towersb::Active() || !bag || !ue::Live(bag) || !(name&0xffffffff)) return false;
    auto* fn=pe::FindFunction(ue::ClassOf(bag),"AddWeaponItem");
    if (!fn) return false;
    const char* fields[]={"WeaponID","bIsNormalWeapon","bPushWidget","bIsAddByLoadData","bAutoEquipment"};
    const int sizes[]={8,1,1,1,1};std::array<int,5> offsets{};
    std::array<uint8_t,0x100> args{};
    for (int i=0;i<5;++i) {
        offsets[i]=ue::PropOffset(fn,fields[i]);
        if (offsets[i]<0 || offsets[i]+sizes[i]>static_cast<int>(args.size())) return false;
        for (int j=0;j<i;++j)
            if (offsets[i]<offsets[j]+sizes[j] && offsets[j]<offsets[i]+sizes[i]) return false;
    }
    std::memcpy(args.data()+offsets[0],&name,8);
    args[offsets[1]]=normal?1:0;
    args[offsets[2]]=1; // User-approved original pickup display, while Tower still owns actual rewards.
    const auto key=std::make_tuple(bag,ue::ClassOf(bag),ue::Get<ue::UObject*>(bag+ue::Layout::ObjOuter),name,normal);
    if(!attempted.insert(key).second)return false; // native call may be queued; a missing immediate readback must never grant/display twice
    // bIsAddByLoadData=false and bAutoEquipment=false remain explicit zeroes.
    { towerisolate::GrantScope grant;pe::Call(bag,fn,args.data()); }
    pbz::log::Info("影塔兵器原生展示：请求发放 {} normal={} bPushWidget=1 autoequip=0（单次原生调用，仍需持有读回）",ue::Name(static_cast<uint32_t>(name)),normal);
    return true;
}
}
