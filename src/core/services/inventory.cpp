#include "inventory.h"

#include "../host/boot.h"
#include "inventory.policy.h"
#include "process_event.h"
#include "../host/log.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <format>
#include <map>
#include <set>
#include <vector>

namespace pbz::inv {
namespace {
struct ObjectRef {
    ue::UObject *object=nullptr, *cls=nullptr, *outer=nullptr;
    int index=-1;
    ObjectRef() = default;
    explicit ObjectRef(ue::UObject* p) : object(p) {
        if (p) { cls=ue::ClassOf(p); outer=ue::Get<ue::UObject*>(p+ue::Layout::ObjOuter); index=ue::Get<int>(p+ue::Layout::ObjIndex,-1); }
    }
    bool Alive() const {
        return object && cls && index>=0 && ue::Obj(index)==object && ue::ClassOf(object)==cls &&
            ue::Get<ue::UObject*>(object+ue::Layout::ObjOuter)==outer && !ue::IsDefault(object);
    }
};
struct Param { const char *name, *type; int size; bool result=false; };
struct Function {
    ObjectRef fn;
    int size=0;
    std::vector<int> offsets;
    bool Bind(ue::UObject* cls,const char* name,std::initializer_list<Param> params) {
        *this={}; auto* f=cls?pe::FindFunction(cls,name):nullptr;
        if (!f) return false;
        const int bytes=ue::Get<int>(f+ue::Layout::StructSize,-1);
        if (bytes<0 || bytes>4096) return false;
        std::vector<ue::UObject*> fields; std::set<ue::UObject*> seen;
        auto* p=ue::Get<ue::UObject*>(f+ue::Layout::StructChildProps);
        while (p && fields.size()<64 && seen.insert(p).second) {
            if (ue::Get<uint64_t>(p+0x38)&0x80) fields.push_back(p);
            p=ue::Get<ue::UObject*>(p+ue::Layout::FieldNext);
        }
        if (p || fields.size()!=params.size()) return false;
        for (const auto& expected:params) {
            auto at=std::find_if(fields.begin(),fields.end(),[&](auto* prop){return ue::Name(ue::Get<uint32_t>(prop+ue::Layout::FieldName))==expected.name;});
            if (at==fields.end()) return false;
            auto* prop=*at; auto* type=ue::Get<uint8_t*>(prop+ue::Layout::FieldClass);
            const int off=ue::Get<int>(prop+ue::Layout::PropOffset,-1);
            const auto actualType=type?ue::Name(ue::Get<uint32_t>(type+ue::Layout::FieldClassName)):std::string{};
            const bool matchingType=actualType==expected.type || (std::string(expected.type)=="EnumProperty" && actualType=="ByteProperty");
            if (!type || !matchingType ||
                ue::Get<int>(prop+0x34)!=expected.size || off<0 || off+expected.size>bytes ||
                bool(ue::Get<uint64_t>(prop+0x38)&0x400)!=expected.result) return false;
            offsets.push_back(off);
        }
        fn=ObjectRef(f); size=bytes; return fn.Alive();
    }
    bool Ready() const { return fn.Alive(); }
    template<class T> bool Put(std::vector<uint8_t>& args,size_t i,const T& value) const {
        if (i>=offsets.size() || offsets[i]+sizeof(T)>args.size()) return false;
        std::memcpy(args.data()+offsets[i],&value,sizeof(T)); return true;
    }
    template<class T> bool Get(const std::vector<uint8_t>& args,size_t i,T& value) const {
        if (i>=offsets.size() || offsets[i]+sizeof(T)>args.size()) return false;
        std::memcpy(&value,args.data()+offsets[i],sizeof(T)); return true;
    }
    bool Run(ue::UObject* object,std::vector<uint8_t>& args) const {
        if (!Ready() || !ObjectRef(object).Alive() || args.size()!=size) return false;
        pe::Call(object,fn.object,args.data()); return true;
    }
};
struct State {
    ue::UObject *bagClass=nullptr,*playerClass=nullptr,*worldClass=nullptr,*levelClass=nullptr,*controllerClass=nullptr,*entClass=nullptr;
    ObjectRef bag,player,stringCDO,entitlement;
    Function toName,info,add,remove,weapon,hasWeapon,weaponLevel,weaponMax,upgradeWeapon,accessory,medicine,skin,dust,isExtraSkin,isExtraAccessory,outfitState,accessoryState;
    int inv=-1,main=-1,shaqi=-1,acc=-1,med=-1,skins=-1,current=-1,acquired=-1,itemId=-1,balance=-1,controller=-1,pawn=-1,levelWorld=-1,worldInstance=-1;
    std::array<ue::UObject*,4> valueClasses{};
    bool resolved=false;
    uint64_t generation=0,lastList=0;
    std::map<std::string,uint64_t> names;
    std::set<std::string> observedStacks;
    std::string lastReadIssue;
} S;

std::string Escape(const std::string& s) {
    std::string out;
    for (unsigned char c:s) {
        if (c=='"' || c=='\\') { out+='\\'; out+=char(c); }
        else if (c<32) out+=std::format("\\u{:04x}",unsigned(c));
        else out+=char(c);
    }
    return out;
}
// session 令牌 = 核心实例标识 + 对象代次：热换核心后计数从 1 重来，带上 boot id 才不会与旧核心的令牌撞号
std::string SessionToken() { return BootId() + "-" + std::to_string(S.generation); }
std::string Result(bool ok,const std::string& id,const std::string& msg,int64_t actual=-1,bool pending=false,int64_t requested=-1) {
    return std::format(R"({{"op":"item","ok":{},"id":"{}","msg":"{}","actual":{},"requested":{},"pending":{},"session":"{}"}})",
        ok?"true":"false",Escape(id),Escape(msg),actual,requested,pending?"true":"false",SessionToken());
}
ue::UObject* WorldOf(ue::UObject* object) {
    for (int i=0; object && i<24; ++i) {
        if (!ObjectRef(object).Alive()) return nullptr;
        if (ue::IsA(object,S.worldClass)) return object;
        if (S.levelClass && ue::IsA(object,S.levelClass)) {
            auto* world=S.levelWorld>=0?ue::Get<ue::UObject*>(object+S.levelWorld):nullptr;
            return ObjectRef(world).Alive() && ue::IsA(world,S.worldClass)?world:nullptr;
        }
        object=ue::Get<ue::UObject*>(object+ue::Layout::ObjOuter);
    }
    return nullptr;
}
bool PlayerNow(ue::UObject* player) {
    if (!ObjectRef(player).Alive() || !ue::IsA(player,S.playerClass) || S.controller<0 || S.pawn<0) return false;
    auto* controller=ue::Get<ue::UObject*>(player+S.controller);
    return ObjectRef(controller).Alive() && ue::IsA(controller,S.controllerClass) && ue::Get<ue::UObject*>(controller+S.pawn)==player;
}
bool Resolve(std::string& why) {
    if (S.resolved && S.toName.Ready() && S.info.Ready()) return true;
    S.resolved=false; S.names.clear(); S.observedStacks.clear(); S.lastList=0;
    S.bagClass=ue::FindClass("PBZBackPackSubsystem"); S.playerClass=ue::FindClass("BP_RenewalPlayer_C");
    S.worldClass=ue::FindClass("World"); S.levelClass=ue::FindClass("Level"); S.controllerClass=ue::FindClass("PlayerController");
    auto* pawnClass=ue::FindClass("Pawn"); auto* stringClass=ue::FindClass("KismetStringLibrary");
    S.entClass=ue::FindClass("PBZEntitlementSubsystem");
    if (!S.bagClass || !S.playerClass || !S.worldClass || !S.controllerClass || !pawnClass || !stringClass) { why="玩家/背包类型尚未加载"; return false; }
    auto bind=[&](Function& f,const char* n,std::initializer_list<Param> params){f.Bind(S.bagClass,n,params);};
    S.toName.Bind(stringClass,"Conv_StringToName",{{"InString","StrProperty",16},{"ReturnValue","NameProperty",8,true}});
    bind(S.info,"GetItemNumInfo",{{"PropID","NameProperty",8},{"CurrentNum","IntProperty",4},{"MaxNum","IntProperty",4}});
    bind(S.add,"AddPropsToBackPack",{{"Num","IntProperty",4},{"ItemId","NameProperty",8},{"bPushWidget","BoolProperty",1}});
    bind(S.remove,"RemovePropsFromBackPack",{{"Num","IntProperty",4},{"ItemId","NameProperty",8},{"bSendBroadcast","BoolProperty",1}});
    bind(S.weapon,"AddWeaponItem",{{"WeaponID","NameProperty",8},{"bIsNormalWeapon","BoolProperty",1},{"bPushWidget","BoolProperty",1},{"bIsAddByLoadData","BoolProperty",1},{"bAutoEquipment","BoolProperty",1}});
    bind(S.hasWeapon,"HasWeapon",{{"WeaponID","NameProperty",8},{"bIsNormalWeapon","BoolProperty",1},{"ReturnValue","BoolProperty",1,true}});
    bind(S.weaponLevel,"GetWeaponLevel",{{"WeaponID","NameProperty",8},{"bIsNormalWeapon","BoolProperty",1},{"ReturnValue","IntProperty",4,true}});
    bind(S.weaponMax,"GetWeaponMaxLevel",{{"WeaponID","NameProperty",8},{"bIsNormalWeapon","BoolProperty",1},{"ReturnValue","IntProperty",4,true}});
    bind(S.upgradeWeapon,"UpgradeWeapon",{{"WeaponID","NameProperty",8},{"IsNormalWeapon","BoolProperty",1},{"UpgradeLevel","IntProperty",4}});
    bind(S.accessory,"PushAccessoryToInventory",{{"AccessoryID","NameProperty",8},{"bAutoEquip","BoolProperty",1}});
    bind(S.medicine,"PushMedicineToInventory",{{"MedicineID","NameProperty",8},{"bAutoEquip","BoolProperty",1},{"bShowPickupUI","BoolProperty",1}});
    bind(S.skin,"AddSkin",{{"SkinID","NameProperty",8}});
    S.dust.Bind(S.playerClass,"ChangeDustNum",{{"bIsAdd","BoolProperty",1},{"Num","IntProperty",4}});
    S.isExtraSkin.Bind(S.entClass,"IsEntitlementOutfit",{{"OutfitID","NameProperty",8},{"ReturnValue","BoolProperty",1,true}});
    S.isExtraAccessory.Bind(S.entClass,"IsEntitlementAccessory",{{"AccessoryID","NameProperty",8},{"ReturnValue","BoolProperty",1,true}});
    S.outfitState.Bind(S.entClass,"GetOutfitState",{{"OutfitID","NameProperty",8},{"ReturnValue","EnumProperty",1,true}});
    S.accessoryState.Bind(S.entClass,"GetAccessoryState",{{"AccessoryID","NameProperty",8},{"ReturnValue","EnumProperty",1,true}});
    S.inv=ue::PropOffset(S.bagClass,"Inventories"); S.main=ue::PropOffset(S.bagClass,"MainWeaponInfos"); S.shaqi=ue::PropOffset(S.bagClass,"ShaqiWeaponInfos");
    S.acc=ue::PropOffset(S.bagClass,"Accessories"); S.med=ue::PropOffset(S.bagClass,"MedicineInfos"); S.skins=ue::PropOffset(S.bagClass,"HasSkins");
    auto* infoClass=ue::FindClass("PBZInventoryInfo"); S.current=ue::PropOffset(infoClass,"CurrentAmount"); S.acquired=ue::PropOffset(infoClass,"HasMaxAmount"); S.itemId=ue::PropOffset(infoClass,"ItemId");
    S.balance=ue::PropOffset(S.playerClass,"ConsciousnessDust"); S.controller=ue::PropOffset(pawnClass,"Controller"); S.pawn=ue::PropOffset(S.controllerClass,"Pawn");
    S.levelWorld=S.levelClass?ue::PropOffset(S.levelClass,"OwningWorld"):-1;
    S.worldInstance=ue::PropOffset(S.worldClass,"OwningGameInstance");
    S.valueClasses={ue::FindClass("MainWeaponInfo"),ue::FindClass("ShaqiWeaponInfo"),ue::FindClass("PBZAccessoryInfo"),ue::FindClass("PBZMedicineInfo")};
    S.stringCDO={}; ue::ForEach([&](int,auto* o){if(ue::ObjName(o)=="Default__KismetStringLibrary"){S.stringCDO=ObjectRef(o);return false;}return true;});
    // The string library CDO is the one intentional default-object caller.
    if (!S.toName.Ready() || !S.info.Ready() || !S.stringCDO.object || S.inv<0 || S.current<0 || S.acquired<0 || S.itemId<0 || S.balance<0 || S.controller<0 || S.pawn<0) {why="背包接口/参数布局不匹配";return false;}
    S.resolved=true; return true;
}

bool FindObjects(std::string& why) {
    if (!Resolve(why)) return false;
    auto entitlement=[&](ue::UObject* world){
        auto* instance=world && S.worldInstance>=0?ue::Get<ue::UObject*>(world+S.worldInstance):nullptr;
        if(S.entitlement.Alive() && S.entitlement.outer==instance)return;
        S.entitlement={};if(!ObjectRef(instance).Alive()||!S.entClass)return;
        ue::ForEach([&](int,auto* o){if(ObjectRef(o).Alive()&&ue::IsA(o,S.entClass)&&ue::Get<ue::UObject*>(o+ue::Layout::ObjOuter)==instance){S.entitlement=ObjectRef(o);return false;}return true;});
    };
    auto valid=[&]{return S.bag.Alive() && S.player.Alive() && PlayerNow(S.player.object) && ue::IsA(S.bag.object,S.bagClass) && WorldOf(S.bag.object)==WorldOf(S.player.object) && WorldOf(S.player.object);};
    if (valid()) {entitlement(WorldOf(S.player.object));return true;}
    S.bag={};S.player={};S.entitlement={};S.observedStacks.clear();S.lastList=0;
    std::vector<ue::UObject*> players,bags;
    ue::ForEach([&](int,auto* o){
        if (!ObjectRef(o).Alive()) return true;
        if (PlayerNow(o)) players.push_back(o);
        if (ue::IsA(o,S.bagClass)) bags.push_back(o);
        return true;
    });
    if (players.size()!=1) {why="当前玩家不唯一或尚未进入存档";return false;}
    auto* world=WorldOf(players[0]);
    std::erase_if(bags,[&](auto* b){return !world || WorldOf(b)!=world;});
    if (bags.size()!=1) {why="当前World的背包子系统未就绪";return false;}
    S.player=ObjectRef(players[0]);S.bag=ObjectRef(bags[0]);++S.generation;entitlement(world);
    return true;
}
uint64_t MakeName(const std::string& id) {
    auto key=catalog::Lower(id);if(auto at=S.names.find(key);at!=S.names.end())return at->second;
    std::wstring wide(id.begin(),id.end());struct FString{const wchar_t* data;int32_t num,max;} str{wide.c_str(),int32_t(wide.size()+1),int32_t(wide.size()+1)};
    std::vector<uint8_t> args(S.toName.size);S.toName.Put(args,0,str);
    if (!S.toName.Ready() || !S.stringCDO.object || ue::Obj(S.stringCDO.index)!=S.stringCDO.object) return 0;
    pe::Call(S.stringCDO.object,S.toName.fn.object,args.data());uint64_t value=0;
    if (!S.toName.Get(args,1,value) || !uint32_t(value)) return 0;
    S.names.emplace(key,value);return value;
}
std::string NameStr(uint64_t value) {
    auto s=ue::Name(uint32_t(value));if(s.empty()||s=="None")return {};
    const auto suffix=uint32_t(value>>32);if(suffix)s+="_"+std::to_string(suffix-1);return s;
}
struct Holding {int32_t current=0,acquired=0;};
struct Snapshot {std::map<std::string,Holding> stack;std::set<std::string> owned,unread;int32_t dust=0;};
void ReadIssue(const std::string& detail) {
    if(detail==S.lastReadIssue)return;
    S.lastReadIssue=detail;
    if(!detail.empty())log::Info("物品读取：{}",detail);
}
// Current build's sparse-array allocation bits. The native inventory leaf also
// compares ArrayNum (+8) with NumFreeIndices (+0x34). Reject incoherent headers;
// never read a free slot merely because it still contains a live UObject*.
template<class F> bool EachMap(int offset,ue::UObject* valueClass,F&& consume,std::string& issue,bool allowUnreadValues=false) {
    auto fail=[&](const char* stage,int slot=-1){issue=std::format("map={:#x} stage={} slot={}",offset,stage,slot);return false;};
    if(offset<0||!valueClass)return fail("layout");
    auto* map=S.bag.object+offset;int32_t n=-1,max=-1,bits=-1,bitMax=-1,free=-1;
    if(!ue::Read(map+8,&n,4)||!ue::Read(map+12,&max,4)||!ue::Read(map+0x28,&bits,4)||!ue::Read(map+0x2c,&bitMax,4)||!ue::Read(map+0x34,&free,4)||
       n<0||n>8192||max<n||max>1048576||bits!=n||bitMax<bits||bitMax>1048576||free<0||free>n)return fail("header");
    if(!n)return true;
    auto* data=ue::Get<uint8_t*>(map);auto* flags=ue::Get<uint8_t*>(map+0x20);
    if(!flags)flags=map+0x10;
    if(!data || (!ue::Get<uint8_t*>(map+0x20) && bitMax>128))return fail("storage");
    std::vector<uint32_t> allocated((n+31)/32);if(!ue::Read(flags,allocated.data(),allocated.size()*4))return fail("bitmap");
    int used=0;
    for(int i=0;i<n;++i)if((allocated[i/32]>>(i%32))&1){
        ++used;uint64_t key=0;ue::UObject* value=nullptr;
        if(!ue::Read(data+0x18*i,&key,8)||!ue::Read(data+0x18*i+8,&value,8))return fail("entry",i);
        const bool live=ObjectRef(value).Alive()&&ue::IsA(value,valueClass);
        if((!live&&!allowUnreadValues)||!consume(key,live?value:nullptr))return fail("value",i);
    }
    return used==n-free?true:fail("allocation_count");
}
bool ReadSnapshot(Snapshot& snap,std::string& why) {
    snap={};
    const auto owner=S.bag,player=S.player;
    std::string issue,firstUnread;
    auto* infoClass=ue::FindClass("PBZInventoryInfo");
    if(!EachMap(S.inv,infoClass,[&](uint64_t key,auto* value){
        Holding h;uint64_t embeddedId=0;const auto id=NameStr(key);
        // If the key cannot be identified, absence of any item is uncertain:
        // keep the whole snapshot closed instead of inventing zero holdings.
        if(id.empty())return false;
        const auto normalized=catalog::Lower(id);
        const bool valid=value&&ue::Read(value+S.itemId,&embeddedId,8)&&embeddedId==key&&
            ue::Read(value+S.current,&h.current,4)&&ue::Read(value+S.acquired,&h.acquired,4)&&policy::Counts(h.current,h.acquired);
        if(!valid||snap.unread.count(normalized)||!snap.stack.emplace(normalized,h).second){
            snap.stack.erase(normalized);snap.unread.insert(normalized);S.observedStacks.erase(normalized);
            if(firstUnread.empty())firstUnread=std::format("id={} live={} key_match={} current={} acquired={}",id,value!=nullptr,embeddedId==key,h.current,h.acquired);
        }
        return true;
    },issue,true)){ReadIssue(issue);why="背包读取失败，请重新刷新";return false;}
    const int offsets[]={S.main,S.shaqi,S.acc,S.med};
    for(size_t i=0;i<4;++i)if(!EachMap(offsets[i],S.valueClasses[i],[&](uint64_t key,auto*){const auto id=NameStr(key);if(id.empty())return false;snap.owned.insert(catalog::Lower(id));return true;},issue)){ReadIssue(issue);why="装备读取失败，请重新刷新";return false;}
    auto* data=S.skins>=0?ue::Get<uint8_t*>(S.bag.object+S.skins):nullptr;
    int32_t count=-1,max=-1;
    if(S.skins<0||!ue::Read(S.bag.object+S.skins+8,&count,4)||!ue::Read(S.bag.object+S.skins+12,&max,4)||count<0||count>1024||max<count||max>16384||(count&&!data)){why="外观持有列表读取失败";return false;}
    for(int i=0;i<count;++i){uint64_t key=0;if(!ue::Read(data+8*i,&key,8)){why="外观读取中断";return false;}auto id=NameStr(key);if(id.empty()){why="外观名称读取失败";return false;}snap.owned.insert(catalog::Lower(id));}
    if(!ue::Read(S.player.object+S.balance,&snap.dust,4)||snap.dust<0||!owner.Alive()||!player.Alive()||!PlayerNow(player.object)){why="读取期间玩家或存档已变化";return false;}
    ReadIssue(firstUnread.empty()?std::string{}:std::format("unread={} {}",snap.unread.size(),firstUnread));
    return true;
}
bool Query(uint64_t id,Holding& h) {
    std::vector<uint8_t> args(S.info.size);return S.info.Put(args,0,id)&&S.info.Run(S.bag.object,args)&&S.info.Get(args,1,h.current)&&S.info.Get(args,2,h.acquired)&&policy::Counts(h.current,h.acquired);
}
// 0=base, 1=owned entitlement, 2=not owned, 3=unknown. Read only;
// never invoke entitlement reconciliation/override/debug-edition methods.
int ContentState(const catalog::Item& item,uint64_t id) {
    if(item.kind!=catalog::Kind::Skin && item.kind!=catalog::Kind::Accessory)return 0;
    if(!S.entitlement.Alive())return 3;
    auto& check=item.kind==catalog::Kind::Skin?S.isExtraSkin:S.isExtraAccessory;
    auto& state=item.kind==catalog::Kind::Skin?S.outfitState:S.accessoryState;
    if(!check.Ready() || !state.Ready())return 3;
    std::vector<uint8_t> args(check.size);check.Put(args,0,id);uint8_t extra=0;
    if(!check.Run(S.entitlement.object,args)||!check.Get(args,1,extra))return 3;
    if(!extra && !item.extra)return 0;
    args.assign(state.size,0);state.Put(args,0,id);uint8_t owned=0;
    if(!state.Run(S.entitlement.object,args)||!state.Get(args,1,owned))return 3;
    return owned==1?1:owned==2?2:3;
}
bool Available(const catalog::Item& item) {
    if(item.component)return false;
    switch(item.kind){
    case catalog::Kind::Weapon:case catalog::Kind::Shaqi:return S.weapon.Ready()&&S.hasWeapon.Ready();
    case catalog::Kind::Accessory:return S.accessory.Ready();case catalog::Kind::Medicine:return S.medicine.Ready();case catalog::Kind::Skin:return S.skin.Ready();
    default:return S.add.Ready()&&(!(item.quantity||item.observed)||S.remove.Ready());}
}
} // namespace

uint64_t ToFName(const std::string& id){std::string why;return Resolve(why)?MakeName(id):0;}
bool CurrentActors(ue::UObject*& player,ue::UObject*& backpack){player=backpack=nullptr;std::string why;if(!FindObjects(why))return false;player=S.player.object;backpack=S.bag.object;return true;}
std::string List(){
    std::string why;Snapshot snapshot;
    if(!FindObjects(why)||!ReadSnapshot(snapshot,why)){S.lastList=0;return std::format(R"({{"op":"items","ok":false,"msg":"{}","stack":{{}},"owned":[]}})",Escape(why));}
    std::string stack,acquired,owned,rules;
    for(const auto& item:catalog::Items){
        const auto key=catalog::Lower(item.id);const auto at=snapshot.stack.find(key);const Holding h=at!=snapshot.stack.end()?at->second:Holding{};
        if(item.observed && h.current>=2) S.observedStacks.insert(key);
        const bool quantity=item.quantity || (item.observed && S.observedStacks.count(key));
        auto append=[&](std::string& str,const std::string& value){if(!str.empty())str+=",";str+=value;};
        if(at!=snapshot.stack.end()) {append(stack,std::format(R"("{}":{})",item.id,h.current));append(acquired,std::format(R"("{}":{})",item.id,h.acquired));}
        if(snapshot.owned.count(key))append(owned,std::format(R"("{}")",item.id));
        const int content=ContentState(item,MakeName(item.id));
        const bool read=!snapshot.unread.count(key);
        std::string reason=!read?"该物品读取异常":item.component?"套装内部部件，请添加对应套装":!Available(item)?"该类原生接口未就绪":content==2?"未拥有对应额外内容":content==3?"额外内容状态尚未确认":"";
        const auto limit = policy::TargetLimit(h.current,h.acquired);
        append(rules,std::format(R"("{}":{{"read":{},"quantity":{},"quantity_mode":"{}","quantity_basis":"{}","add":{},"component":{},"content":"{}","limit":{},"add_limit":{},"reason":"{}"}})",item.id,read?"true":"false",quantity?"true":"false",quantity?"target":"none",item.quantity?"catalog":quantity?"observed_stack":"unconfirmed",reason.empty()?"true":"false",item.component?"true":"false",content==1?"owned":content==2?"not_owned":content==3?"unknown":"base",limit,policy::AdditionLimit(h.current,h.acquired),Escape(reason)));
    }
    if(!S.bag.Alive() || !S.player.Alive() || !PlayerNow(S.player.object)){S.lastList=0;return R"({"op":"items","ok":false,"msg":"读取期间玩家已变化"})";}
    S.lastList=S.generation;
    return std::format(R"({{"op":"items","ok":true,"editor_rev":3,"partial":{},"session":"{}","dust":{},"stack":{{{}}},"acquired":{{{}}},"owned":[{}],"policy":{{{}}}}})",snapshot.unread.empty()?"false":"true",SessionToken(),snapshot.dust,stack,acquired,owned,rules);
}
std::string Apply(const std::string& id,const std::string& kind,long long num,const std::string& session){
    std::string why;Snapshot before;
    if(!FindObjects(why))return Result(false,id,why);
    if(S.lastList!=S.generation || session!=SessionToken())return Result(false,id,"物品接口需要当前存档的session，请更新界面并刷新背包");
    if(!ReadSnapshot(before,why)){S.lastList=0;return Result(false,id,why);}
    if(kind=="dust"){
        if(id!="dust"||num<0||num>policy::DustLimit||!S.dust.Ready())return Result(false,id,"意识尘埃目标值无效或接口不可用");
        const int64_t delta=num-before.dust;if(!delta)return Result(true,id,"未变化",num,false,num);
        std::vector<uint8_t> args(S.dust.size);S.dust.Put(args,0,uint8_t(delta>0));S.dust.Put(args,1,int32_t(delta>0?delta:-delta));
        if(!S.dust.Run(S.player.object,args))return Result(false,id,"意识尘埃发放接口失效");
        Snapshot after;if(!ReadSnapshot(after,why))return Result(false,id,"已调用原生接口但回读失败，请刷新确认，勿重复执行");
        const bool exact=after.dust==num;log::Info("物品编辑：Dust {} -> {}，目标 {}",before.dust,after.dust,num);
        return Result(exact,id,exact?"数量已核对":"等待游戏原生更新，请刷新核对，勿重复执行",after.dust,!exact,num);
    }
    const auto* item=catalog::Find(id);
    if(!item||!policy::KindMatches(*item,kind)||!Available(*item))return Result(false,id,"物品不存在、类别不符或该操作未开放");
    if(before.unread.count(catalog::Lower(item->id)))return Result(false,id,"该物品读取异常，请重新刷新");
    if(kind=="weapon_max") {
        if(num!=0 || !S.weaponLevel.Ready() || !S.weaponMax.Ready() || !S.upgradeWeapon.Ready())return Result(false,id,"武器升级接口不可用");
        const uint64_t name=MakeName(item->id);const uint8_t main=item->kind==catalog::Kind::Weapon;
        auto read=[&](Function& fn,int32_t& out){std::vector<uint8_t> args(fn.size);return fn.Put(args,0,name)&&fn.Put(args,1,main)&&fn.Run(S.bag.object,args)&&fn.Get(args,2,out);};
        std::vector<uint8_t> has(S.hasWeapon.size);uint8_t owned=0;
        if(!name||!S.hasWeapon.Put(has,0,name)||!S.hasWeapon.Put(has,1,main)||!S.hasWeapon.Run(S.bag.object,has)||!S.hasWeapon.Get(has,2,owned)||!owned)return Result(false,id,"请先获得这把武器");
        int32_t level=0,maximum=0;
        if(!read(S.weaponLevel,level)||!read(S.weaponMax,maximum)||level<1||maximum<level||maximum>128)return Result(false,id,"武器等级或当前周目上限读取失败");
        if(level==maximum)return Result(true,id,"已达到当前周目满级",level,false,maximum);
        // The native UpgradeLevel is a delta, and UpgradeWeapon applies the
        // real stats/delegates without the smith UI's material deduction.
        std::vector<uint8_t> args(S.upgradeWeapon.size);S.upgradeWeapon.Put(args,0,name);S.upgradeWeapon.Put(args,1,main);S.upgradeWeapon.Put(args,2,maximum-level);
        if(!S.upgradeWeapon.Run(S.bag.object,args))return Result(false,id,"武器原生升级接口失效");
        int32_t actual=0;if(!read(S.weaponLevel,actual))return Result(false,id,"已调用升级，请刷新核对，勿重复执行");
        const bool exact=actual==maximum;
        log::Info("武器满级：{} {} -> {}，当前周目原生上限 {}",item->id,level,actual,maximum);
        return Result(exact,id,exact?std::format("已升至当前周目满级 {} 级",actual):"游戏未达到目标等级，请刷新核对",actual,false,maximum);
    }
    if(kind!="stack"&&kind!="add_stack"&&kind!="grant"&&num!=1)return Result(false,id,"一次性物品只能添加一件");
    const uint64_t name=MakeName(item->id);if(!name)return Result(false,id,"原生名称转换失败");
    const int content=ContentState(*item,name);if(content>=2)return Result(false,id,content==2?"未拥有对应额外内容":"额外内容状态尚未确认");
    const auto key=catalog::Lower(item->id);Holding current;
    if(!Query(name,current))return Result(false,id,"当前数量读取失败");
    if(item->observed && current.current>=2) S.observedStacks.insert(key);
    if((kind=="stack" || kind=="add_stack") && !item->quantity && !S.observedStacks.count(key)) return Result(false,id,"当前角色尚未实际持有同 ID 两件以上，不能推定可叠加");
    if(kind=="add_stack"){
        if(!policy::Addition(num,current.current,current.acquired))return Result(false,id,"本次添加数量须为正整数，且累计获得量不能溢出");
        // Read and add on this game thread. The UI's displayed count can be stale
        // after a pickup/use; never translate it into an absolute target there.
        std::vector<uint8_t> args(S.add.size);S.add.Put(args,0,int32_t(num));S.add.Put(args,1,name);S.add.Put(args,2,uint8_t(0));
        if(!S.add.Run(S.bag.object,args))return Result(false,id,"原生添加接口失效");
        Holding after;if(!Query(name,after))return Result(false,id,"已调用原生接口但回读失败，请刷新确认，勿重复执行");
        const int64_t received=int64_t(after.acquired)-current.acquired;
        const bool exact=received==num;
        log::Info("物品添加：{} 请求 +{}，持有 {} -> {}，累计 {} -> {}",item->id,num,current.current,after.current,current.acquired,after.acquired);
        return Result(exact,id,exact?"已添加，实际获得数量已核对":"实际获得数量与请求不同，请刷新核对，勿重复执行",after.current,false,num);
    }
    if(kind=="grant"){
        if(!policy::Grant(num,current.current,current.acquired))return Result(false,id,"本次获得数量须为正整数，且累计获得量不能溢出");
        std::vector<uint8_t> args(S.add.size);S.add.Put(args,0,int32_t(num));S.add.Put(args,1,name);S.add.Put(args,2,uint8_t(0));
        if(!S.add.Run(S.bag.object,args))return Result(false,id,"原生重复获得接口失效");
        Holding after;if(!Query(name,after))return Result(false,id,"已调用原生接口但回读失败，请刷新确认，勿重复执行");
        const int64_t received=int64_t(after.acquired)-current.acquired;
        const bool exact=received==num;
        log::Info("物品编辑重复获得：{} 请求 {}，累计 {} -> {}，持有 {} -> {}",item->id,num,current.acquired,after.acquired,current.current,after.current);
        return Result(exact,id,exact?"本次获得份数已核对，属性效果由游戏原生处理":"实际获得份数与请求不同，请刷新核对，勿重复执行",received,false,num);
    }
    if(kind=="stack"){
        if(!policy::Quantity(num,current.current,current.acquired))return Result(false,id,"目标数量越界或累计获得量接近整数上限");
        const int64_t delta=num-current.current;if(!delta)return Result(true,id,"未变化",num,false,num);
        auto& fn=delta>0?S.add:S.remove;std::vector<uint8_t> args(fn.size);fn.Put(args,0,int32_t(delta>0?delta:-delta));fn.Put(args,1,name);fn.Put(args,2,uint8_t(delta>0?0:1));
        if(!fn.Run(S.bag.object,args))return Result(false,id,"数量接口失效");
        Holding after;if(!Query(name,after))return Result(false,id,"已调用原生接口但回读失败，请刷新确认，勿重复执行");
        log::Info("物品编辑：{} {} -> {}，目标 {}",item->id,current.current,after.current,num);
        const bool exact=after.current==num;
        return Result(exact,id,exact?"数量已核对":"游戏实际数量与目标不同（可能已原生整合），请查看持有状态",after.current,false,num);
    }
    if(before.owned.count(key)||current.current>0||current.acquired>0)return Result(true,id,current.current>0||before.owned.count(key)?"已拥有":"曾获得或已使用，不重复授予",before.owned.count(key)?1:current.current,false,1);
    Function* fn=nullptr;
    switch(item->kind){case catalog::Kind::Weapon:case catalog::Kind::Shaqi:fn=&S.weapon;break;case catalog::Kind::Accessory:fn=&S.accessory;break;case catalog::Kind::Medicine:fn=&S.medicine;break;case catalog::Kind::Skin:fn=&S.skin;break;default:fn=&S.add;break;}
    std::vector<uint8_t> args(fn->size);
    if(item->kind==catalog::Kind::Generic){fn->Put(args,0,int32_t(1));fn->Put(args,1,name);fn->Put(args,2,uint8_t(0));}
    else{fn->Put(args,0,name);if(item->kind==catalog::Kind::Weapon||item->kind==catalog::Kind::Shaqi)fn->Put(args,1,uint8_t(item->kind==catalog::Kind::Weapon));}
    if(!fn->Run(S.bag.object,args))return Result(false,id,"物品原生授予接口失效");
    // AddWeaponItem's first call creates the inventory entry. The game's save
    // load path calls it again with bIsAddByLoadData=true, which resolves the
    // weapon's PBZ_MainWeaponConfig / runtime abilities (normal attacks and
    // the weapon's native skill). Without this second native call a weapon
    // added by the editor can exist in the backpack with only its generic
    // attack set. Keep bPushWidget/bAutoEquipment false: no pickup UI and no
    // surprise equipment change.
    bool skillInitialized = true;
    if (item->kind == catalog::Kind::Weapon || item->kind == catalog::Kind::Shaqi) {
        try {
            std::vector<uint8_t> init(fn->size);
            if (!fn->Put(init, 0, name) || !fn->Put(init, 1, uint8_t(item->kind == catalog::Kind::Weapon)) ||
                !fn->Put(init, 3, uint8_t(1)) || !fn->Run(S.bag.object, init)) skillInitialized = false;
        } catch (...) { skillInitialized = false; }
        log::Info("物品编辑：{} 原生武器数据初始化 {}", item->id, skillInitialized ? "已调用" : "调用失败");
    }
    Snapshot after;if(!ReadSnapshot(after,why))return Result(false,id,"已调用原生接口但持有回读失败，勿重复添加");
    if(after.unread.count(key))return Result(false,id,"已执行，请刷新核对，勿重复获取");
    const auto at=after.stack.find(key);const bool accepted=after.owned.count(key)||(at!=after.stack.end()&&(at->second.current>0||at->second.acquired>current.acquired));
    log::Info("物品编辑：添加 {}，真实持有回读 {}",item->id,accepted);
    const char* message = accepted
        ? (skillInitialized ? "已添加；原生武器技能数据已初始化" : "已添加；原生技能初始化未确认，请切换武器或重载存档核对")
        : "游戏未接受或尚未确认持有，不报告添加成功";
    return Result(accepted,id,message,after.owned.count(key)?1:at!=after.stack.end()?at->second.current:0,false,1);
}
std::string ApplyRequest(const std::string& id,const std::string& kind,const std::string& number,const std::string& session){
    long long value=0;
    const auto parsed=std::from_chars(number.data(),number.data()+number.size(),value);
    if(number.empty() || parsed.ec!=std::errc{} || parsed.ptr!=number.data()+number.size())return Result(false,id,"数量必须是范围内的整数，不接受小数或溢出值");
    return Apply(id,kind,value,session);
}
} // namespace pbz::inv
