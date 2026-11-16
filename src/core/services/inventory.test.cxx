// Actual inventory.cpp with a bounded fake UE provider. No game process.
#include "inventory.cpp"
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <fstream>
#include <tuple>

namespace fake {
using namespace pbz;
std::vector<std::unique_ptr<uint8_t[]>> memory;
std::vector<ue::UObject*> objects;
std::vector<std::string> names{"None"};
std::map<std::string,ue::UObject*> classes;
std::map<std::pair<ue::UObject*,std::string>,ue::UObject*> functions;
std::map<std::pair<ue::UObject*,std::string>,int> properties;
std::map<ue::UObject*,std::function<void(ue::UObject*,void*)>> native;
ue::UObject *meta=nullptr,*functionClass=nullptr,*bag=nullptr,*player=nullptr,*controller=nullptr,*world=nullptr;
int checks=0,mutations=0,quantityQueries=0;bool rejectGrant=false,partial=false,consume=false;
std::map<uint64_t,std::pair<int,int>> counts;
std::array<std::set<uint64_t>,4> owned;
std::set<uint64_t> skins,extra;
uint8_t entitlement=1;
template<class T> void put(void* p,int off,T value){std::memcpy(static_cast<uint8_t*>(p)+off,&value,sizeof value);}
uint8_t* alloc(){memory.push_back(std::make_unique<uint8_t[]>(16384));std::memset(memory.back().get(),0,16384);return memory.back().get();}
uint32_t name(const std::string& s){auto at=std::find(names.begin(),names.end(),s);if(at!=names.end())return uint32_t(at-names.begin());names.push_back(s);return uint32_t(names.size()-1);}
ue::UObject* object(const std::string& s,ue::UObject* cls=nullptr,ue::UObject* outer=nullptr){auto* p=alloc();put(p,ue::Layout::ObjIndex,int(objects.size()));put(p,ue::Layout::ObjName,name(s));put(p,ue::Layout::ObjClass,cls?cls:meta);put(p,ue::Layout::ObjOuter,outer);objects.push_back(p);return p;}
ue::UObject* cls(const std::string& s,ue::UObject* super=nullptr){auto* p=object(s);put(p,ue::Layout::StructSuper,super);classes[s]=p;return p;}
ue::UObject* function(ue::UObject* owner,const std::string& s,std::initializer_list<inv::Param> params){
 auto* f=object(s,functionClass,owner);uint8_t* prev=nullptr;int off=0;
 for(auto e:params){int align=(std::min)(e.size,8);off=(off+align-1)/align*align;auto* p=alloc();auto* type=alloc();put(type,ue::Layout::FieldClassName,name(e.type));put(p,ue::Layout::FieldClass,type);put(p,ue::Layout::FieldName,name(e.name));put(p,ue::Layout::PropOffset,off);put(p,0x30,int32_t(1));put(p,0x34,e.size);put(p,0x38,uint64_t(0x80|(e.result?0x400:0)));if(prev)put(prev,ue::Layout::FieldNext,p);else put(f,ue::Layout::StructChildProps,p);prev=p;properties[{f,e.name}]=off;off+=e.size;}
 put(f,ue::Layout::StructSize,off);functions[{owner,s}]=f;return f;
}
template<class T>T arg(ue::UObject* fn,void* p,const char* key){T v{};std::memcpy(&v,static_cast<uint8_t*>(p)+properties.at({fn,key}),sizeof v);return v;}
template<class T>void result(ue::UObject* fn,void* p,const char* key,T value){put(p,properties.at({fn,key}),value);}
void check(bool c,const char* msg){++checks;if(!c)throw std::runtime_error(msg);}
bool ok(const std::string& s){return s.find("\"ok\":true")!=std::string::npos;}
void map(int off,ue::UObject* valueClass,const std::vector<std::pair<uint64_t,std::pair<int,int>>>& rows){
 auto* header=bag+off;std::memset(header,0,0x50);auto* data=alloc();put(header,0,data);put(header,8,int(rows.size()));put(header,12,int(rows.size()));put(header,0x28,int(rows.size()));put(header,0x2c,128);put(header,0x30,int32_t(-1));
 auto* flags=header+0x10;if(rows.size()>128){flags=alloc();put(header,0x20,flags);put(header,0x2c,int((rows.size()+31)/32*32));}
 for(size_t i=0;i<rows.size();++i){auto* value=object("Held",valueClass,bag);put(value,0x180,rows[i].first);put(value,0x188,rows[i].second.first);put(value,0x18c,rows[i].second.second);put(data,int(0x18*i),rows[i].first);put(data,int(0x18*i+8),value);const int o=int(i/32*4);uint32_t word=0;std::memcpy(&word,flags+o,4);put(flags,o,word|(1u<<(i%32)));}
}
void sync(){
 std::vector<std::pair<uint64_t,std::pair<int,int>>> rows(counts.begin(),counts.end());map(0x200,classes["PBZInventoryInfo"],rows);
 const int offsets[]={0x250,0x2a0,0x2f0,0x340};const char* classNames[]={"MainWeaponInfo","ShaqiWeaponInfo","PBZAccessoryInfo","PBZMedicineInfo"};
 for(int i=0;i<4;++i){rows.clear();for(auto id:owned[i])rows.push_back({id,{1,1}});map(offsets[i],classes[classNames[i]],rows);}
 auto* data=alloc();int i=0;for(auto id:skins)put(data,8*i++,id);put(bag,0x390,data);put(bag,0x398,int(skins.size()));put(bag,0x39c,int(skins.size()));
}
}
namespace pbz::ue {
bool Read(const void* p,void* out,size_t n){auto a=reinterpret_cast<uintptr_t>(p);for(const auto& m:fake::memory){auto b=reinterpret_cast<uintptr_t>(m.get());if(a>=b&&n<=16384&&a-b<=16384-n){std::memcpy(out,p,n);return true;}}return false;}
bool Write(void* p,const void* v,size_t n){uint8_t dummy; if(!Read(p,&dummy,1))return false;std::memcpy(p,v,n);return true;}
std::string Name(uint32_t i){return i<fake::names.size()?fake::names[i]:"";}
std::string ObjName(UObject* p){return p?Name(Get<uint32_t>(p+Layout::ObjName)):"";}
UObject* ClassOf(UObject* p){return p?Get<UObject*>(p+Layout::ObjClass):nullptr;}
UObject* Obj(int i){return i>=0&&i<int(fake::objects.size())?fake::objects[i]:nullptr;}
bool IsDefault(UObject* p){return ObjName(p).starts_with("Default__");}
bool IsA(UObject* p,UObject* cls){for(auto* c=ClassOf(p);c;c=Get<UObject*>(c+Layout::StructSuper))if(c==cls)return true;return false;}
UObject* FindClass(std::string_view s){auto at=fake::classes.find(std::string(s));return at==fake::classes.end()?nullptr:at->second;}
int PropOffset(UObject* cls,std::string_view s){for(;cls;cls=Get<UObject*>(cls+Layout::StructSuper)){auto at=fake::properties.find({cls,std::string(s)});if(at!=fake::properties.end())return at->second;}return -1;}
void ForEach(const std::function<bool(int,UObject*)>& call){const auto snapshot=fake::objects;for(int i=0;i<int(snapshot.size());++i)if(snapshot[i]&&!call(i,snapshot[i]))break;}
}
namespace pbz::pe {
ue::UObject* FindFunction(ue::UObject* cls,std::string_view s){for(;cls;cls=ue::Get<ue::UObject*>(cls+ue::Layout::StructSuper)){auto at=fake::functions.find({cls,std::string(s)});if(at!=fake::functions.end())return at->second;}return nullptr;}
void Call(ue::UObject* o,ue::UObject* fn,void* p){if(auto at=fake::native.find(fn);at!=fake::native.end())at->second(o,p);}
}
int main(int argc,char** argv) try {
 using namespace pbz;using namespace fake;
 meta=cls("Class");put(meta,ue::Layout::ObjClass,meta);functionClass=cls("Function");
 auto* worldClass=cls("World");auto* levelClass=cls("Level");auto* pawnClass=cls("Pawn");auto* pcClass=cls("PlayerController");auto* playerClass=cls("BP_RenewalPlayer_C",pawnClass);auto* bagClass=cls("PBZBackPackSubsystem");
 auto* infoClass=cls("PBZInventoryInfo");for(auto n:{"MainWeaponInfo","ShaqiWeaponInfo","PBZAccessoryInfo","PBZMedicineInfo"})cls(n);
 auto* stringClass=cls("KismetStringLibrary");object("Default__KismetStringLibrary",stringClass);
 auto* entClass=cls("PBZEntitlementSubsystem");auto* giClass=cls("GameInstance");auto* gi=object("GameInstance",giClass);
 world=object("World",worldClass);put(world,0x180,gi);properties[{worldClass,"OwningGameInstance"}]=0x180;
 auto* level=object("Level",levelClass);put(level,0x180,world);properties[{levelClass,"OwningWorld"}]=0x180;
 player=object("Player",playerClass,level);controller=object("Controller",pcClass,world);put(player,0x180,controller);put(controller,0x180,player);properties[{pawnClass,"Controller"}]=0x180;properties[{pcClass,"Pawn"}]=0x180;
 bag=object("Backpack",bagClass,world);object("Entitlement",entClass,gi);
 for(auto [n,off]:std::initializer_list<std::pair<const char*,int>>{{"Inventories",0x200},{"MainWeaponInfos",0x250},{"ShaqiWeaponInfos",0x2a0},{"Accessories",0x2f0},{"MedicineInfos",0x340},{"HasSkins",0x390}})properties[{bagClass,n}]=off;
 properties[{infoClass,"ItemId"}]=0x180;properties[{infoClass,"CurrentAmount"}]=0x188;properties[{infoClass,"HasMaxAmount"}]=0x18c;properties[{playerClass,"ConsciousnessDust"}]=0x190;put(player,0x190,int32_t(100));
 auto* toName=function(stringClass,"Conv_StringToName",{{"InString","StrProperty",16},{"ReturnValue","NameProperty",8,true}});
 native[toName]=[=](auto*,void* p){auto* w=arg<const wchar_t*>(toName,p,"InString");std::string s;for(;*w;++w){if(*w>=128)throw std::runtime_error("non-ASCII test identity");s+=char(*w);}result(toName,p,"ReturnValue",uint64_t(name(s)));};
 auto* info=function(bagClass,"GetItemNumInfo",{{"PropID","NameProperty",8},{"CurrentNum","IntProperty",4},{"MaxNum","IntProperty",4}});
 native[info]=[=](auto*,void* p){++quantityQueries;auto id=arg<uint64_t>(info,p,"PropID");auto at=counts.find(id);result(info,p,"CurrentNum",at==counts.end()?0:at->second.first);result(info,p,"MaxNum",at==counts.end()?0:at->second.second);};
 auto bindCount=[&](const char* fn,bool add){auto* f=function(bagClass,fn,{{"Num","IntProperty",4},{"ItemId","NameProperty",8},{add?"bPushWidget":"bSendBroadcast","BoolProperty",1}});native[f]=[=](auto*,void* p){++mutations;if(rejectGrant)return;auto id=arg<uint64_t>(f,p,"ItemId");int n=arg<int>(f,p,"Num");if(partial)n=(std::min)(n,2);auto& q=counts[id];if(add){q.first+=consume?0:n;q.second+=n;}else q.first=(std::max)(0,q.first-n);sync();};};
 bindCount("AddPropsToBackPack",true);bindCount("RemovePropsFromBackPack",false);
 auto* weapon=function(bagClass,"AddWeaponItem",{{"WeaponID","NameProperty",8},{"bIsNormalWeapon","BoolProperty",1},{"bPushWidget","BoolProperty",1},{"bIsAddByLoadData","BoolProperty",1},{"bAutoEquipment","BoolProperty",1}});
 int weaponInits=0;
 native[weapon]=[&](auto*,void* p){++mutations;const bool normal=arg<uint8_t>(weapon,p,"bIsNormalWeapon");const auto id=arg<uint64_t>(weapon,p,"WeaponID");const bool push=arg<uint8_t>(weapon,p,"bPushWidget"),load=arg<uint8_t>(weapon,p,"bIsAddByLoadData"),autoeq=arg<uint8_t>(weapon,p,"bAutoEquipment");
   if(!owned[normal?0:1].count(id)){
     check(!push&&!load&&!autoeq,"first weapon acquisition call never auto-equips, loads unrelated data or displays pickup UI");
     if(!rejectGrant)owned[normal?0:1].insert(id);sync();
   } else {
     check(!push&&load&&!autoeq,"second weapon acquisition call initializes native weapon data without UI or auto-equip");++weaponInits;
   }};
 auto* hasWeapon=function(bagClass,"HasWeapon",{{"WeaponID","NameProperty",8},{"bIsNormalWeapon","BoolProperty",1},{"ReturnValue","BoolProperty",1,true}});native[hasWeapon]=[=](auto*,void* p){result(hasWeapon,p,"ReturnValue",uint8_t(owned[arg<uint8_t>(hasWeapon,p,"bIsNormalWeapon")?0:1].count(arg<uint64_t>(hasWeapon,p,"WeaponID"))));};
 std::map<uint64_t,int> weaponLevels;int nativeMaximum=30,upgrades=0;
 auto* getLevel=function(bagClass,"GetWeaponLevel",{{"WeaponID","NameProperty",8},{"bIsNormalWeapon","BoolProperty",1},{"ReturnValue","IntProperty",4,true}});
 native[getLevel]=[&](auto*,void* p){auto id=arg<uint64_t>(getLevel,p,"WeaponID");result(getLevel,p,"ReturnValue",int32_t(weaponLevels.contains(id)?weaponLevels[id]:20));};
 auto* getMax=function(bagClass,"GetWeaponMaxLevel",{{"WeaponID","NameProperty",8},{"bIsNormalWeapon","BoolProperty",1},{"ReturnValue","IntProperty",4,true}});
 native[getMax]=[&](auto*,void* p){result(getMax,p,"ReturnValue",int32_t(nativeMaximum));};
 auto* upgrade=function(bagClass,"UpgradeWeapon",{{"WeaponID","NameProperty",8},{"IsNormalWeapon","BoolProperty",1},{"UpgradeLevel","IntProperty",4}});
 native[upgrade]=[&](auto*,void* p){auto id=arg<uint64_t>(upgrade,p,"WeaponID");const int level=weaponLevels.contains(id)?weaponLevels[id]:20;check(arg<int>(upgrade,p,"UpgradeLevel")==nativeMaximum-level,"native upgrade receives a delta to the current cycle limit");if(!rejectGrant)weaponLevels[id]=nativeMaximum;++upgrades;};
 for(auto [n,param,index]:std::initializer_list<std::tuple<const char*,const char*,int>>{{"PushAccessoryToInventory","AccessoryID",2},{"PushMedicineToInventory","MedicineID",3}}){auto* f=index==2?function(bagClass,n,{{param,"NameProperty",8},{"bAutoEquip","BoolProperty",1}}):function(bagClass,n,{{param,"NameProperty",8},{"bAutoEquip","BoolProperty",1},{"bShowPickupUI","BoolProperty",1}});native[f]=[=](auto*,void* p){++mutations;check(!arg<uint8_t>(f,p,"bAutoEquip"),"equipment never auto-equips");if(!rejectGrant)owned[index].insert(arg<uint64_t>(f,p,param));sync();};}
 auto* skin=function(bagClass,"AddSkin",{{"SkinID","NameProperty",8}});native[skin]=[=](auto*,void* p){++mutations;if(!rejectGrant)skins.insert(arg<uint64_t>(skin,p,"SkinID"));sync();};
 auto* dust=function(playerClass,"ChangeDustNum",{{"bIsAdd","BoolProperty",1},{"Num","IntProperty",4}});native[dust]=[=](auto*,void* p){++mutations;if(!rejectGrant){int now=ue::Get<int>(player+0x190);int n=arg<int>(dust,p,"Num");put(player,0x190,now+(arg<uint8_t>(dust,p,"bIsAdd")?n:-n));}};
 for(auto [checkName,stateName,param]:std::initializer_list<std::tuple<const char*,const char*,const char*>>{{"IsEntitlementOutfit","GetOutfitState","OutfitID"},{"IsEntitlementAccessory","GetAccessoryState","AccessoryID"}}){auto* c=function(entClass,checkName,{{param,"NameProperty",8},{"ReturnValue","BoolProperty",1,true}});native[c]=[=](auto*,void* p){result(c,p,"ReturnValue",uint8_t(extra.count(arg<uint64_t>(c,p,param))));};auto* f=function(entClass,stateName,{{param,"NameProperty",8},{"ReturnValue","EnumProperty",1,true}});native[f]=[=](auto*,void* p){result(f,p,"ReturnValue",entitlement);};}
 sync();check(ok(inv::List()),"complete initial snapshot resolves real reflection signatures");
 std::string session=pbz::BootId()+"-1";  // session 令牌 = boot id + 代次（2e332eb）
 auto run=[&](const char* id,const char* kind,long long num){return inv::Apply(id,kind,num,session);};
 check(!ok(inv::Apply("Weapon_Material_01","stack",5)),"legacy missing session fails closed");
 check(!ok(inv::Apply("Weapon_Material_01","stack",5,"stale")),"mismatched session cannot mutate current inventory");
 for(const auto* token:{"1.5","2e2","NaN","","9223372036854775808","1junk"})check(!ok(inv::ApplyRequest("Weapon_Material_01","stack",token,session)),"IPC quantities must parse completely as bounded integers");
 check(ok(run("Weapon_Material_01","stack",10)),"new repeatable stack can be created at target10");check(counts[inv::ToFName("Weapon_Material_01")].first==10,"authoritative count10");
 check(ok(run("Weapon_Material_01","stack",3)),"target decrease uses native remove");check(counts[inv::ToFName("Weapon_Material_01")].second==10,"acquired total is not capacity or current count");
 const auto material=inv::ToFName("Weapon_Material_01");
 check(inv::List().find("\"editor_rev\":3")!=std::string::npos,"inventory advertises target-count editor capability");
 int calls=mutations;
 check(ok(run("Weapon_Material_01","add_stack",7))&&counts[material].first==10&&mutations==calls+1,"adding7 to current3 acquires7 with one native call");
 check(ok(run("Weapon_Material_01","add_stack",7))&&counts[material].first==17,"repeating the same addition acquires another7");
 counts[material]={20,50};sync();
 check(ok(run("Weapon_Material_01","add_stack",5))&&counts[material].first==25,"addition uses current game-thread count after an intervening pickup");
 calls=mutations;
 for(const auto n:{-1LL,0LL,100000LL,9223372036854775807LL})check(!ok(run("Weapon_Material_01","add_stack",n)),"invalid addition rejected before native mutation");
 check(!ok(run("Tang","add_stack",2))&&!ok(run("I_060002","add_stack",2))&&!ok(run("Item_001","add_stack",2))&&mutations==calls,"unique, unconfirmed and permanent-effect items cannot use ordinary additive route");
 check(!ok(inv::ApplyRequest("Weapon_Material_01","add_stack","2.5",session))&&mutations==calls,"decimal additive request never reaches native interface");
 counts[material]={INT32_MAX-2,INT32_MAX-2};sync();
 check(!ok(run("Weapon_Material_01","add_stack",3))&&mutations==calls,"addition cannot overflow current or cumulative int32 count");
 check(ok(run("Weapon_Material_01","add_stack",2))&&counts[material].first==INT32_MAX,"exact remaining int32 room is valid");
 counts[material]={3,10};sync();
 const int unchanged=mutations;check(!ok(run("Tang","stack",99))&&!ok(run("I_060002","stack",3))&&!ok(run("Missing","stack",1)),"unique, unknown-repeatability and unknown IDs cannot use quantity route");check(mutations==unchanged,"invalid requests cause no native mutation");
 check(!ok(run("Tang","weapon",2))&&!ok(run("Weapon_Material_01","stack",-1))&&!ok(run("Weapon_Material_01","stack",100000)),"one-shot and quantity boundaries rejected");
 for(auto [id,kind]:std::initializer_list<std::pair<const char*,const char*>>{{"Tang","weapon"},{"HuoLiuXing","shaqi_weapon"},{"AC_ActHit","accessory"},{"Hulu","medicine"},{"Hun_OutFit_Default","skin"},{"T_02002","item"}}){check(ok(run(id,kind,1)),"native family-specific acquisition accepted and read back");const int n=mutations;check(ok(run(id,kind,1))&&mutations==n,"duplicate unique item is idempotent");}
 check(weaponInits>=2,"weapon and shaqi grants run the native load-data initialization without duplicate ownership");
 rejectGrant=true;check(!ok(run("AC_ActHitShachi","accessory",1)),"native rejection never reports unnamed accessory success");rejectGrant=false;
 extra.insert(inv::ToFName("Hun_OutFit_YaYu"));entitlement=2;const int noAccess=mutations;check(!ok(run("Hun_OutFit_YaYu","skin",1))&&mutations==noAccess,"unowned extra content keeps native entitlement restriction");entitlement=0;check(!ok(run("Hun_OutFit_YaYu","skin",1)),"unknown extra status does not claim ownership");entitlement=1;check(ok(run("Hun_OutFit_YaYu","skin",1)),"already licensed extra outfit uses native grant and verified holder");
 check(!ok(run("HunBody","skin",1)),"outfit component is not a standalone selectable skin");
 consume=true;check(ok(run("Item_001","grant",3)),"repeatable black stone grants3 using native acquisition counter");
 check(counts[inv::ToFName("Item_001")].first==0 && counts[inv::ToFName("Item_001")].second==3,"immediate native effect does not fake carried quantity");
 check(ok(run("Item_001","grant",2)) && counts[inv::ToFName("Item_001")].second==5,"consumed repeatable attribute can be acquired again");consume=false;
 check(ok(run("Item_002","grant",4)) && ok(run("Item_002","grant",2)) && counts[inv::ToFName("Item_002")].first==6,"repeatable amber can also remain in carried inventory");
 check(ok(run("Item_002","stack",2))&&counts[inv::ToFName("Item_002")].first==2,"amber surplus can be reduced through native removal");
 check(ok(run("Item_002","stack",0))&&counts[inv::ToFName("Item_002")].first==0&&counts[inv::ToFName("Item_002")].second==6,"amber can be cleared without falsifying acquisition history");
 const int noGrant=mutations;check(!ok(run("Item_001","grant",0))&&!ok(run("Tang","grant",3))&&mutations==noGrant,"invalid grants and unique family mismatches fail before mutation");
 check(ok(run("Tang","weapon_max",0))&&weaponLevels[inv::ToFName("Tang")]==30&&upgrades==1,"owned level20 weapon reaches native base-cycle level30 without materials");
 check(ok(run("Tang","weapon_max",0))&&upgrades==1,"repeated max upgrade is idempotent");
 nativeMaximum=50;check(ok(run("Tang","weapon_max",0))&&weaponLevels[inv::ToFName("Tang")]==50,"higher-cycle native limit is queried rather than hardcoded30");
 check(ok(run("HuoLiuXing","weapon_max",0))&&weaponLevels[inv::ToFName("HuoLiuXing")]==50,"shaqi weapon uses its own native upgrade path");
 const int noUpgrade=upgrades;check(!ok(run("Weapon_Material_01","weapon_max",0))&&!ok(run("Tang","weapon_max",1))&&upgrades==noUpgrade,"invalid max-upgrade requests cannot mutate inventory");
 nativeMaximum=60;rejectGrant=true;check(!ok(run("Tang","weapon_max",0)),"native upgrade rejection cannot report a false maximum");rejectGrant=false;
 const auto tree=inv::ToFName("I_060002");counts[tree]={2,2};sync();auto treeList=inv::List();
 check(treeList.find("\"quantity_basis\":\"observed_stack\"")!=std::string::npos && ok(run("I_060002","stack",5)),"native current stack of2 confirms eligible unknown material");
 check(ok(run("I_060002","stack",1)) && ok(run("I_060002","stack",3)),"confirmed stack remains editable within current owner after decrease");
 check(ok(run("I_060002","add_stack",4))&&counts[tree].first==7,"observed stack proof also authorizes additive acquisition");
 calls=mutations;partial=true;auto added=run("Weapon_Material_02","add_stack",10);partial=false;
 check(!ok(added)&&counts[inv::ToFName("Weapon_Material_02")].first==2&&mutations==calls+1,"partial addition reports actual outcome and never repeats the native call");
 partial=true;check(!ok(run("Weapon_Material_02","stack",10)),"partial acceptance is reported as actual outcome, not requested success");partial=false;
 check(ok(run("dust","dust",250))&&ue::Get<int>(player+0x190)==250,"Dust setter calls native signed gain");check(ok(run("dust","dust",50))&&ue::Get<int>(player+0x190)==50,"Dust setter calls native debit");
 rejectGrant=true;auto pending=run("dust","dust",500);check(!ok(pending)&&pending.find("\"pending\":true")!=std::string::npos,"unconfirmed or delayed Dust is not false success");rejectGrant=false;
 const auto traversing=inv::ToFName("Item_Traversing");counts[traversing]={0,2};sync();calls=mutations;rejectGrant=true;
 const auto refusedTraversing=run("Item_Traversing","stack",11);rejectGrant=false;
 check(!ok(refusedTraversing)&&refusedTraversing.find("\"actual\":0")!=std::string::npos&&refusedTraversing.find("\"pending\":false")!=std::string::npos&&mutations==calls+1,"current-core rejected Traversing target11 reports actual0 immediately, never starts a pending grant or retries");
 check(ok(run("Item_Traversing","stack",11))&&counts[traversing].first==11,"confirmed Traversing target11 uses a single native increase and authoritative readback");
 check(ok(run("Item_Traversing","stack",2))&&counts[traversing].first==2,"Traversing remains an absolute quantity editor rather than an additive workaround");
 // One malformed item must not hide unrelated, independently verified stock.
 const auto healthy=inv::ToFName("Weapon_Material_02");counts[healthy]={12,40};counts[material]={5,2};sync();
 check(ok(inv::List()),"one inconsistent item must not invalidate the entire backpack");
 std::string why;inv::Snapshot read;
 check(inv::ReadSnapshot(read,why)&&read.unread.count("weapon_material_01")&&!read.stack.count("weapon_material_01")&&read.stack.at("weapon_material_02").current==12,"bad item is unread, never zero or stale; healthy quantity is retained");
 const auto partialList=inv::List();
 check(partialList.find("\"partial\":true")!=std::string::npos&&partialList.find("\"Weapon_Material_01\":{\"read\":false")!=std::string::npos,"IPC marks per-item unread state and partial snapshot");
 calls=mutations;check(!ok(run("Weapon_Material_01","stack",1))&&!ok(run("Weapon_Material_01","add_stack",1))&&mutations==calls,"unread target cannot reach native add or remove");
 check(ok(run("Weapon_Material_02","stack",11))&&counts[healthy].first==11&&mutations==calls+1,"verified unrelated item remains editable with exactly one native operation");
 for(auto bad:std::initializer_list<std::pair<int,int>>{{-1,5},{0,-1},{20,2}}){counts[material]=bad;sync();check(inv::ReadSnapshot(read,why)&&read.unread.count("weapon_material_01"),"negative or inconsistent counts isolate the affected item");}
 counts[material]={3,10};sync();
 auto rowFor=[&](uint64_t id){auto* data=ue::Get<uint8_t*>(bag+0x200);for(int i=0;i<int(counts.size());++i)if(ue::Get<uint64_t>(data+24*i)==id)return data+24*i;throw std::runtime_error("missing fixture row");};
 auto* row=rowFor(material);auto* rowValue=ue::Get<ue::UObject*>(row+8);
 put(rowValue,0x180,healthy);
 check(inv::ReadSnapshot(read,why)&&read.unread.count("weapon_material_01")&&read.stack.count("weapon_material_02"),"mismatched ItemId does not poison unrelated stock or acquire the wrong identity");
 sync();row=rowFor(material);rowValue=ue::Get<ue::UObject*>(row+8);put(row,8,static_cast<ue::UObject*>(nullptr));
 check(inv::ReadSnapshot(read,why)&&read.unread.count("weapon_material_01"),"allocated null value is unread without dereferencing it");
 calls=mutations;const int queryCount=quantityQueries;check(!ok(run("Weapon_Material_01","stack",4))&&mutations==calls&&quantityQueries==queryCount,"null value never reaches an unsafe native quantity query");
 sync();row=rowFor(material);put(row,8,player);
 check(inv::ReadSnapshot(read,why)&&read.unread.count("weapon_material_01"),"live object with wrong type is isolated");
 sync();row=rowFor(material);rowValue=ue::Get<ue::UObject*>(row+8);objects[ue::Get<int>(rowValue+ue::Layout::ObjIndex)]=nullptr;
 check(inv::ReadSnapshot(read,why)&&read.unread.count("weapon_material_01"),"retired UObject does not masquerade as held stock");
 sync();row=rowFor(material);put(row,8,reinterpret_cast<ue::UObject*>(uintptr_t(1)));
 check(inv::ReadSnapshot(read,why)&&read.unread.count("weapon_material_01"),"unreadable value pointer is never followed outside guarded reads");
 sync();row=rowFor(material);put(row,0,uint64_t(0));
 check(!ok(inv::List())&&!ok(run("Weapon_Material_02","stack",5)),"unidentifiable key keeps collection closed: missing holdings cannot be assumed zero");
 sync();row=rowFor(material);put(row,0,healthy);rowValue=ue::Get<ue::UObject*>(row+8);put(rowValue,0x180,healthy);
 check(inv::ReadSnapshot(read,why)&&read.unread.count("weapon_material_02")&&!read.stack.count("weapon_material_02"),"duplicate normalized ID invalidates both entries rather than reporting arbitrary stock");
 sync();row=rowFor(material);put(row,8,static_cast<ue::UObject*>(nullptr));
 int readLogs=0;log::g_sink=[&](const auto&){++readLogs;};inv::S.lastReadIssue.clear();inv::List();inv::List();inv::List();log::g_sink={};
 check(readLogs==1,"identical refresh failures produce one bounded diagnostic instead of log spam");
 sync();check(inv::ReadSnapshot(read,why)&&read.unread.empty()&&read.stack.at("weapon_material_01").current==3,"fresh verified row recovers without retaining unread or stale state");
 check(inv::List().find("\"partial\":false")!=std::string::npos,"complete snapshot clears partial marker");
 const auto savedCounts=counts;
 for(int i=0;i<400;++i)counts[uint64_t(name("FixtureItem_"+std::to_string(i)))]={i,i+1};sync();
 check(inv::ReadSnapshot(read,why)&&read.unread.empty()&&read.stack.size()==counts.size(),"large real-layout backpack uses heap bitmap after128 slots");
 counts[material]={9,2};sync();
 check(inv::ReadSnapshot(read,why)&&read.unread.size()==1&&read.stack.size()==counts.size()-1,"large backpack isolates one malformed row without dropping the remaining hundreds");
 counts=savedCounts;sync();inv::List();
 // A live stale value in a free sparse slot must not appear in ownership.
 auto* data=alloc();auto* stale=object("Stale",infoClass,bag);put(stale,0x180,uint64_t(name("Weapon_Material_03")));put(stale,0x188,7);put(stale,0x18c,7);put(data,0,uint64_t(name("Weapon_Material_03")));put(data,8,stale);
 auto* valid=object("Live",infoClass,bag);put(valid,0x180,uint64_t(name("Weapon_Material_04")));put(valid,0x188,2);put(valid,0x18c,2);put(data,0x18,uint64_t(name("Weapon_Material_04")));put(data,0x20,valid);
 std::memset(bag+0x200,0,0x50);put(bag,0x200,data);put(bag,0x208,2);put(bag,0x20c,2);put(bag,0x210,uint32_t(2));put(bag,0x228,2);put(bag,0x22c,128);put(bag,0x234,1);
 auto sparse=inv::List();check(ok(sparse)&&sparse.find("\"Weapon_Material_03\":7")==std::string::npos&&sparse.find("\"Weapon_Material_04\":2")!=std::string::npos,"sparse allocation bits ignore stale free slot while preserving live row");
 put(bag,0x228,3);check(!ok(inv::List()),"incoherent allocation bits fail closed");check(!ok(run("Weapon_Material_01","stack",4)),"failed read invalidates mutation permission");sync();check(ok(inv::List()),"fresh read recovers after structural error");
 auto* heapData=alloc();auto* heapFlags=alloc();auto* heapValue=object("HeapHeld",infoClass,bag);const auto heapId=uint64_t(name("Weapon_Material_01"));put(heapValue,0x180,heapId);put(heapValue,0x188,5);put(heapValue,0x18c,7);put(heapData,128*0x18,heapId);put(heapData,128*0x18+8,heapValue);put(heapFlags,16,uint32_t(1));
 std::memset(bag+0x200,0,0x50);put(bag,0x200,heapData);put(bag,0x208,129);put(bag,0x20c,160);put(bag,0x220,heapFlags);put(bag,0x228,129);put(bag,0x22c,160);put(bag,0x234,128);
 auto heapSnapshot=inv::List();check(ok(heapSnapshot)&&heapSnapshot.find("\"Weapon_Material_01\":5")!=std::string::npos,"heap allocation bitmap handles inventory beyond128 sparse slots");
 put(bag,0x220,static_cast<uint8_t*>(nullptr));check(!ok(inv::List()),"missing heap bitmap cannot fall back to a too-small inline bitmap");sync();inv::List();
 auto* oldPlayer=player;auto* oldBag=bag;auto* newWorld=object("NextWorld",worldClass);put(newWorld,0x180,gi);world=newWorld;player=object("NextPlayer",playerClass,world);controller=object("NextController",pcClass,world);put(player,0x180,controller);put(controller,0x180,player);put(player,0x190,50);put(oldPlayer,0x180,static_cast<ue::UObject*>(nullptr));bag=object("NextBag",bagClass,world);counts.clear();for(auto& set:owned)set.clear();skins.clear();sync();
 const int safe=mutations;check(!ok(run("Weapon_Material_01","stack",2))&&mutations==safe,"owner/world change rejects stale edit before native call");
 check(ok(inv::List())&&!ok(run("Weapon_Material_01","stack",2))&&mutations==safe,"another client refresh cannot authorize an old-session write");
 session=pbz::BootId()+"-2";check(ok(run("Weapon_Material_01","stack",2)),"new session requires fresh current-world snapshot and matching token");
 check(!ok(run("I_060002","stack",3)),"observed stacking proof does not cross owner/save generation");
 for(const auto& it:inv::catalog::Items){check(inv::catalog::Find(it.id)==&it,"generated native index has exact case-insensitive identity");if(it.component)check(!inv::policy::KindMatches(it,"skin"),"all appearance components excluded from standalone grant");if(!it.quantity&&!it.observed)check(!inv::policy::KindMatches(it,"stack"),"all unique and non-observable rows excluded from quantity path");}
 if(argc>1){std::ofstream out(argv[1]);out<<inv::List();}
 std::cout<<"INVENTORY_TEST_OK "<<checks<<" checks (isolated reflection provider; no game process)\n";return 0;
} catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
