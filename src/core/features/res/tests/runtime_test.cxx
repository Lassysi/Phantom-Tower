// Compile the actual feature implementations against an isolated fake UE/PE.
// This file is .cxx deliberately: the production build only collects *.cpp.
#include "../res.cpp"
#include "../../world/world.cpp"
#include "../../cam/cam.cpp"
#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace fake {
using namespace pbz;
std::vector<std::unique_ptr<uint8_t[]>> memory;
std::vector<ue::UObject*> objects;
std::vector<std::string> names{"None"};
std::map<std::string, ue::UObject*> classes;
std::map<std::pair<ue::UObject*, std::string>, ue::UObject*> functions;
std::map<std::pair<ue::UObject*, std::string>, int> properties;
std::unordered_map<ue::UObject*, pe::Interceptor> interceptors;
std::unordered_map<ue::UObject*, std::function<void(ue::UObject*, void*)>> native;
std::vector<attr::Target> targets;
struct Wish { attr::Kind kind; float multiplier; };
std::vector<Wish> wishes;
int checks = 0, nativeAdds = 0, lastAdd = 0, lastRemove = 0, lastDust = 0, capacity = 1000, inventory = 20;
bool raiseOnAdd = false, nestedAdd = false;
template<class T> void put(void* base, int offset, T value) { std::memcpy(static_cast<uint8_t*>(base) + offset, &value, sizeof value); }
uint8_t* alloc() { memory.push_back(std::make_unique<uint8_t[]>(8192)); std::memset(memory.back().get(), 0, 8192); return memory.back().get(); }
uint32_t name(const std::string& s) {
    auto it = std::find(names.begin(), names.end(), s);
    if (it != names.end()) return static_cast<uint32_t>(it - names.begin());
    names.push_back(s); return static_cast<uint32_t>(names.size() - 1);
}
ue::UObject* object(const std::string& n, ue::UObject* cls = nullptr, ue::UObject* outer = nullptr) {
    auto* p = alloc(); put(p, ue::Layout::ObjIndex, static_cast<int>(objects.size()));
    put(p, ue::Layout::ObjName, name(n)); put(p, ue::Layout::ObjClass, cls); put(p, ue::Layout::ObjOuter, outer);
    objects.push_back(p); return p;
}
ue::UObject* cls(const std::string& n, ue::UObject* super = nullptr) {
    auto* p = object(n); put(p, ue::Layout::StructSuper, super); classes[n] = p; return p;
}
ue::UObject* function(ue::UObject* owner, const std::string& n, std::initializer_list<runtime::Param> params) {
    auto* f = object(n); int offset = 0; uint8_t* previous = nullptr;
    for (const auto& param : params) {
        const int align = (std::min)(param.size, 8); offset = (offset + align - 1) / align * align;
        auto* field = alloc(); auto* type = alloc();
        put(type, ue::Layout::FieldClassName, name(param.type)); put(field, ue::Layout::FieldClass, type);
        put(field, ue::Layout::FieldName, name(param.name)); put(field, ue::Layout::PropOffset, offset);
        put(field, 0x34, param.size); put(field, 0x38, uint64_t(0x80 | (param.result ? 0x400 : 0)));
        if (previous) put(previous, ue::Layout::FieldNext, field); else put(f, ue::Layout::StructChildProps, field);
        previous = field; properties[{f, param.name}] = offset; offset += param.size;
    }
    put(f, ue::Layout::StructSize, offset); functions[{owner, n}] = f; return f;
}
void dispatch(ue::UObject* o, ue::UObject* f, void* p) {
    if (auto it = interceptors.find(f); it != interceptors.end() && it->second(o, p)) return;
    pe::Call(o, f, p);
}
void check(bool condition, const char* message) {
    ++checks; if (!condition) throw std::runtime_error(message);
}
template<class T> T arg(ue::UObject* fn, void* params, const char* n) { return ue::Get<T>(static_cast<uint8_t*>(params) + properties.at({fn, n})); }
template<class T> void argPut(ue::UObject* fn, void* params, const char* n, T v) { put(params, properties.at({fn, n}), v); }
}  // namespace fake

namespace pbz {
std::vector<std::unique_ptr<IFeature>>& Features() { static std::vector<std::unique_ptr<IFeature>> v; return v; }
IFeature* FindFeature(const std::string& id) { for (auto& f : Features()) if (id == f->Id()) return f.get(); return nullptr; }
namespace ue {
bool Read(const void* p, void* out, size_t n) { if (!p) return false; std::memcpy(out, p, n); return true; }
bool Write(void* p, const void* in, size_t n) { if (!p) return false; std::memcpy(p, in, n); return true; }
std::string Name(uint32_t index) { return index < fake::names.size() ? fake::names[index] : ""; }
std::string ObjName(UObject* o) { return o ? Name(Get<uint32_t>(o + Layout::ObjName)) : ""; }
UObject* ClassOf(UObject* o) { return o ? Get<UObject*>(o + Layout::ObjClass) : nullptr; }
UObject* FindClass(std::string_view n) { auto it = fake::classes.find(std::string(n)); return it == fake::classes.end() ? nullptr : it->second; }
bool IsA(UObject* o, UObject* wanted) { for (auto* c = ClassOf(o); c; c = Get<UObject*>(c + Layout::StructSuper)) if (c == wanted) return true; return false; }
bool IsDefault(UObject* o) { return ObjName(o).starts_with("Default__"); }
UObject* Obj(int i) { return i >= 0 && i < static_cast<int>(fake::objects.size()) ? fake::objects[i] : nullptr; }
int PropOffset(UObject* owner, std::string_view name) {
    for (; owner; owner = Get<UObject*>(owner + Layout::StructSuper))
        if (auto it = fake::properties.find({owner, std::string(name)}); it != fake::properties.end()) return it->second;
    return -1;
}
}
namespace attr {
bool Resolve(std::string*) { return true; }
int AttrOffset(const std::string& n) { return n == "MindDustAdditiveMultiplier" ? 0 : n == "WalkSpeed" ? 32 : n == "JogSpeed" ? 64 : n == "SprintSpeed" ? 96 : -1; }
int ActorOffset(const std::string&) { return -1; }
int WorldOffset(const std::string&) { return -1; }
float* AttrCurrent(const Target& t, int off) { return reinterpret_cast<float*>(t.set + off + 12); }
float* AttrBase(const Target& t, int off) { return reinterpret_cast<float*>(t.set + off + 8); }
const std::vector<Target>& Targets() { return fake::targets; }
bool Alive(const Target& t) { return runtime::Identity(t.obj).Alive(); }
void Want(const Target&, float*, Kind kind, float multiplier) { fake::wishes.push_back({kind, multiplier}); }
void Hold(const Target&, float*, float) {}
}
namespace pe {
ue::UObject* FindFunction(ue::UObject* owner, std::string_view name) {
    for (; owner; owner = ue::Get<ue::UObject*>(owner + ue::Layout::StructSuper))
        if (auto it = fake::functions.find({owner, std::string(name)}); it != fake::functions.end()) return it->second;
    return nullptr;
}
void Call(ue::UObject* o, ue::UObject* f, void* p) { if (auto it = fake::native.find(f); it != fake::native.end()) it->second(o, p); }
void Intercept(ue::UObject* f, Interceptor h) { fake::interceptors[f] = std::move(h); }
void RemoveIntercept(ue::UObject* f) { fake::interceptors.erase(f); }
}
}  // namespace pbz

int main() try {
    using namespace pbz; using namespace fake;
    int32_t out = -1;
    check(res::ScaleCount(3, .5, false, out) && out == 1, "floor reward");
    check(res::ScaleCount(3, .5, true, out) && out == 2, "ceil cost");
    check(res::ScaleCount(3, 0, true, out) && out == 0, "explicit free cost");
    check(!res::ScaleCount(INT32_MAX, 10, false, out), "overflow rejected");
    check(!res::ScaleCount(-1, 2, false, out), "signed delta rejected");
    check(!res::ScaleCount(1, NAN, false, out) && !res::ScaleCount(1, INFINITY, false, out), "nonfinite rejected");
    check(res::StackableMaterial("Weapon_Material_01") && !res::StackableMaterial("Item_001") && !res::StackableMaterial("I_060002"), "material exclusions");

    auto* worldClass = cls("World"); auto* world = object("World_1", worldClass);
    auto* pawnClass = cls("Pawn"); auto* playerClass = cls("BP_RenewalPlayer_C", pawnClass);
    auto* player = object("Player", playerClass, world); auto* backpackClass = cls("PBZBackPackSubsystem");
    auto* backpack = object("Backpack", backpackClass, world);
    targets.push_back({player, alloc(), attr::Who::Player, playerClass});
    auto* npcClass = cls("BP_NPCLootComp_C"); auto* npc = object("NPC", npcClass, world);
    auto* flowBase = cls("LootItems_C"); auto* flowClass = cls("FromActor_LootItem_C", flowBase);
    auto* flow = object("Pickup", flowClass, world); auto* quest = object("Quest", flowBase, world);
    auto* add = function(backpackClass, "AddPropsToBackPack", {{"Num","IntProperty",4},{"ItemId","NameProperty",8},{"bPushWidget","BoolProperty",1}});
    auto* info = function(backpackClass, "GetItemNumInfo", {{"PropID","NameProperty",8},{"CurrentNum","IntProperty",4},{"MaxNum","IntProperty",4}});
    auto* remove = function(backpackClass, "RemovePropsFromBackPack", {{"Num","IntProperty",4},{"ItemId","NameProperty",8},{"bSendBroadcast","BoolProperty",1}});
    auto* dust = function(playerClass, "ChangeDustNum", {{"bIsAdd","BoolProperty",1},{"Num","IntProperty",4}});
    auto* loot = function(npcClass, "Loot Items", {{"ActorClass","ClassProperty",8},{"NPCLevel","DoubleProperty",8}});
    auto* pickup = function(flowBase, "CollectItems", {});
    auto* buy = function(backpackClass, "BuyItem", {{"ItemName","NameProperty",8},{"ItemNum","IntProperty",4}});
    auto* upgrade = function(backpackClass, "UpgradeWeapon", {{"WeaponID","NameProperty",8},{"IsNormalWeapon","BoolProperty",1},{"UpgradeLevel","IntProperty",4}});
    std::array<uint8_t,128> quantity{};
    argPut(add, quantity.data(), "Num", 2); argPut(add, quantity.data(), "ItemId", std::array<uint32_t,2>{name("Weapon_Material_01"),0});
    native[info] = [=](auto*, void* p) { argPut(info,p,"CurrentNum",inventory); argPut(info,p,"MaxNum",capacity); };
    native[add] = [=](auto* o, void* p) {
        ++nativeAdds; lastAdd = arg<int>(add,p,"Num");
        if (raiseOnAdd) throw std::runtime_error("native failure");
        if (nestedAdd) { nestedAdd=false; dispatch(o,add,p); }
    };
    native[loot] = native[pickup] = [&,backpack,add](auto*, void*) { dispatch(backpack,add,quantity.data()); };
    native[dust] = [=](auto*, void* p) { lastDust = arg<int>(dust,p,"Num"); };
    native[remove] = [=](auto*, void* p) { lastRemove = arg<int>(remove,p,"Num"); };
    auto spend = [=](auto*, void*) {
        std::array<uint8_t,128> p{}; argPut(dust,p.data(),"Num",100); dispatch(player,dust,p.data());
        check(arg<int>(dust,p.data(),"Num")==100,"debit input restored");
        argPut(remove,p.data(),"Num",3); dispatch(backpack,remove,p.data());
        check(arg<int>(remove,p.data(),"Num")==3,"material input restored");
    };
    native[buy]=native[upgrade]=spend;
    std::string why;
    RegisterResFeatures(); RegisterWorldFeatures(); RegisterCamFeatures();
    auto* money=FindFeature("res.money_mul"); auto* move=FindFeature("world.move_speed");
    check(money->Resolve(&why) && move->Resolve(&why),"attribute resolve");
    money->SetValue(2); money->Enable(); money->Tick(0);
    check(wishes.size()==1 && wishes[0].kind==attr::Kind::Additive && wishes[0].multiplier==2,"money additive effects delegation");
    wishes.clear(); move->SetValue(2); move->Enable(); move->Tick(0);
    check(wishes.size()==3 && std::all_of(wishes.begin(),wishes.end(),[](auto w){return w.kind==attr::Kind::Scale && w.multiplier==2;}),"all movement attributes");
    move->Disable(); wishes.clear(); move->Tick(0); check(wishes.empty(),"disabled effects issue no wishes");
    // The shared slider range must not constrain the runtime multiplier.
    for (double requested : {.01, .25, 8.0, 25.0, 1000.0}) {
        money->SetValue(requested); money->Enable(); wishes.clear(); money->Tick(0);
        check(money->value==requested && wishes.size()==1 && wishes[0].multiplier==static_cast<float>(requested),"money accepts values outside slider hints");
        move->SetValue(requested); move->Enable(); wishes.clear(); move->Tick(0);
        check(move->value==requested && wishes.size()==3 && wishes[0].multiplier==static_cast<float>(requested),"movement accepts values outside slider hints");
    }
    int rejects=0;
    log::g_sink=[&](const std::string& message) { if(message.find("rejected value")!=std::string::npos) ++rejects; };
    for (double invalid : {0.0, -1.0, double(NAN), double(INFINITY), -double(INFINITY)}) {
        const double previous=move->value; move->SetValue(invalid); wishes.clear(); move->Tick(0);
        check(move->value==previous && move->enabled && wishes.size()==3 && wishes[0].multiplier==static_cast<float>(previous),"invalid request preserves active effect");
        money->Disable(); money->SetValue(invalid); money->Enable();
        check(!money->enabled,"rejected request cannot enable disabled effect");
    }
    log::g_sink={}; check(rejects==10,"rejected numeric requests logged");
    money->SetValue(1); money->Disable(); move->SetValue(1); move->Disable();
    for (auto id : res::kIds) check(!FindFeature(id)->Resolve(&why),"unproven production source must fail closed");
    // Enable directly only in this isolated fixture to exercise the dormant implementation.
    auto* drop=FindFeature("res.drop_mul"); auto* gather=FindFeature("res.gather_mul");
    auto* price=FindFeature("res.price_mul"); auto* cost=FindFeature("res.cost_mul");
    drop->SetValue(2); drop->Enable(); gather->SetValue(3); gather->Enable();
    dispatch(npc,loot,nullptr); check(lastAdd==4 && nativeAdds==1,"scoped combat loot, native once");
    check(arg<int>(add,quantity.data(),"Num")==2,"reward input restored");
    dispatch(flow,pickup,nullptr); check(lastAdd==6,"actor pickup separate multiplier");
    dispatch(quest,pickup,nullptr); check(lastAdd==2,"quest source excluded");
    dispatch(backpack,add,quantity.data()); check(lastAdd==2,"unknown grant excluded");
    std::thread other([&]{dispatch(npc,loot,nullptr);}); other.join(); check(lastAdd==2,"wrong-thread unchanged");
    auto* otherWorld=object("OtherWorld",worldClass); auto* foreign=object("ForeignNPC",npcClass,otherWorld);
    dispatch(foreign,loot,nullptr); check(lastAdd==2,"foreign world excluded");
    capacity=22; dispatch(npc,loot,nullptr); check(lastAdd==2,"remaining capacity clamp"); capacity=1000;
    nestedAdd=true; dispatch(npc,loot,nullptr); check(lastAdd==4,"nested leaf not multiplied twice");
    raiseOnAdd=true; try { dispatch(npc,loot,nullptr); } catch(const std::runtime_error&) {}
    raiseOnAdd=false; check(arg<int>(add,quantity.data(),"Num")==2 && res::g_context.itemDepth==0 && res::g_context.source==res::Source::Unknown,"exception restores parameters and source");
    price->SetValue(.5); price->Enable(); cost->SetValue(.25); cost->Enable();
    dispatch(backpack,buy,nullptr); check(lastDust==50 && lastRemove==2,"shop discount and material ceil");
    dispatch(backpack,upgrade,nullptr); check(lastDust==25 && lastRemove==1,"upgrade separate cost");
    cost->SetValue(0); dispatch(backpack,upgrade,nullptr); check(lastDust==0 && lastRemove==0,"free cost explicit");
    price->SetValue(10); check(price->value==1,"unvalidated upcharge rejected to neutral");
    gather->Disable(); dispatch(npc,loot,nullptr); check(lastAdd==4,"disabling one owner preserves other hooks");
    drop->Disable(); price->Disable(); cost->Disable(); check(interceptors.empty(),"all owned interceptors removed");

    runtime::Function signature;
    check(signature.Resolve(playerClass,"ChangeDustNum",{{"bIsAdd","BoolProperty",1},{"Num","IntProperty",4}},&why),"real signature checker accepts fixture");
    const auto oldOffsets=signature.offsets;
    check(!signature.Resolve(playerClass,"ChangeDustNum",{{"Num","FloatProperty",4}},&why) && signature.offsets==oldOffsets,"failed resolve is atomic");
    put(dust,ue::Layout::StructSize,1);
    check(!signature.Resolve(playerClass,"ChangeDustNum",{{"bIsAdd","BoolProperty",1},{"Num","IntProperty",4}},&why),"out-of-buffer field rejected");

    auto* controllerClass=cls("PlayerController"); auto* managerClass=cls("PBZPlayerCameraManager"); auto* cameraClass=cls("CameraComponent");
    properties[{pawnClass,"Controller"}]=0x300; properties[{controllerClass,"PlayerCameraManager"}]=0x308; properties[{cameraClass,"FieldOfView"}]=0x310;
    auto* controller=object("Controller",controllerClass,world); auto* manager=object("Manager",managerClass,world);
    auto* camera=object("Camera",cameraClass,world); put(player,0x300,controller); put(controller,0x308,manager); put(camera,0x310,80.f);
    auto* getCamera=function(managerClass,"GetCurrentActivateCameraComponent",{{"ReturnValue","ObjectProperty",8,true}});
    auto* setFov=function(cameraClass,"SetFieldOfView",{{"InFieldOfView","FloatProperty",4}});
    native[getCamera]=[&](auto*,void* p){argPut(getCamera,p,"ReturnValue",camera);};
    native[setFov]=[=](auto* o,void* p){put(o,0x310,arg<float>(setFov,p,"InFieldOfView"));};
    auto* fov=FindFeature("cam.fov"); check(fov->Resolve(&why),"camera setter signature");
    fov->SetValue(110); fov->Enable(); fov->Tick(0); check(ue::Get<float>(camera+0x310)==110,"current player camera setter");
    fov->Disable(); check(ue::Get<float>(camera+0x310)==80,"FOV restores original");
    fov->Enable(); fov->Tick(0); put(camera,0x310,75.f); fov->Tick(0); fov->Disable();
    check(ue::Get<float>(camera+0x310)==75,"FOV preserves game-originated changes");
    fov->Enable(); fov->Tick(0); auto* previous=camera; camera=object("Camera2",cameraClass,world); put(camera,0x310,85.f); fov->Tick(0);
    check(ue::Get<float>(previous+0x310)==75 && ue::Get<float>(camera+0x310)==110,"camera switch restores old target");
    fov->Disable(); check(ue::Get<float>(camera+0x310)==85,"new camera restored");
    std::cout << "RUNTIME_TEST_OK " << checks << " checks (synthetic, no game process)\n";
    return 0;
} catch(const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
