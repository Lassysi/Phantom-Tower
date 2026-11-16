// Actual production bridge and native hook ledger in a child process, never
// in PBZ.exe. The fixture contains no retired transaction implementation.
#define dispatch legacy_dispatch
#include "fake_ue.h"
#undef dispatch
#include "../res.cpp"
#include "../loot.cpp"

namespace fake {
struct Observed { pbz::ue::UObject* function; pbz::pe::Observer callback; };
std::map<int, Observed> observers;
int nextObserver = 1;
void dispatch(pbz::ue::UObject* object, pbz::ue::UObject* function, void* params) {
    const auto snapshot = observers;
    for (const auto& [id, observer] : snapshot) if (observer.function == function) observer.callback(object,params);
    legacy_dispatch(object,function,params);
}
}
namespace pbz::pe {
int Observe(ue::UObject* function, Observer callback) {
    const int token = fake::nextObserver++; fake::observers.emplace(token,fake::Observed{function,std::move(callback)}); return token;
}
void Unobserve(int token) { fake::observers.erase(token); }
}

namespace fixture {
using namespace pbz;
int amount = 3, calls = 0, observed = 0;
uint64_t item = 0;
bool push = true, seenPush = false, useDust = false, recurseUnknown = false, throwNative = false;
ue::UObject *seenObject = nullptr, *bag = nullptr, *player = nullptr, *dustFunction = nullptr;
std::array<uint8_t, 128> unknownFrame{};

__declspec(noinline) void __fastcall NativeAdd(ue::UObject* object, int32_t quantity, uint64_t id, bool widget) {
    ++calls; observed = quantity; seenObject = object; seenPush = widget;
    if (id != item) throw std::runtime_error("FName argument ABI corrupted");
    if (throwNative) throw std::runtime_error("fixture native error");
    if (recurseUnknown) {
        recurseUnknown = false;
        NativeAdd(object, 1, id, widget);
    }
}
__declspec(noinline) void __fastcall NativeExec(ue::UObject* object, void*, void*) {
    if (useDust) {
        std::array<uint8_t, 128> args{};
        fake::argPut(dustFunction, args.data(), "bIsAdd", uint8_t(1));
        fake::argPut(dustFunction, args.data(), "Num", amount);
        fake::dispatch(player, dustFunction, args.data());
        fake::check(fake::arg<int>(dustFunction, args.data(), "Num") == amount, "dust caller parameters restored");
    } else NativeAdd(object, amount, item, push);
}
template<int N> __declspec(noinline) void __fastcall Exec(ue::UObject* object, void* frame, void* result) {
    volatile int unique = N; (void)unique;
    if constexpr (N == 4) NativeExec(bag, frame, result);
    else NativeExec(object, frame, result);
}
}

int main() try {
    using namespace pbz; using namespace pbz::res::loot; using namespace fake;
    // The real test executable has an unsupported PE fingerprint, just like
    // updated PBZ. Repeated shared resolves must never reach UE discovery.
    std::string fingerprintReason;
    const int lookupsBefore = classLookups;
    for (int i = 0; i < 12; ++i) {
        check(!bridge.Resolve(&fingerprintReason), "unsupported image remains unavailable on every shared feature retry");
        check(fingerprintReason == "奖励原生桥版本指纹不匹配", "early rejection keeps the original version-gate reason");
    }
    check(classLookups == lookupsBefore, "unsupported image retries perform zero UObject class scans");
    check(!bridge.resolved && !bridge.hooksCreated && !bridge.observing &&
          !bridge.active[Item] && !bridge.active[Money], "unsupported image installs no hooks or active reward state");
    std::array<uint8_t, 8192> supported{};
    IMAGE_DOS_HEADER fixtureDos{};fixtureDos.e_magic=IMAGE_DOS_SIGNATURE;fixtureDos.e_lfanew=256;
    IMAGE_NT_HEADERS64 fixtureNt{};fixtureNt.Signature=IMAGE_NT_SIGNATURE;
    fixtureNt.OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    fixtureNt.FileHeader.TimeDateStamp=signature::Timestamp;fixtureNt.OptionalHeader.SizeOfImage=signature::ImageSize;
    fixtureNt.FileHeader.NumberOfSections=4;
    std::memcpy(supported.data(),&fixtureDos,sizeof fixtureDos);std::memcpy(supported.data()+256,&fixtureNt,sizeof fixtureNt);
    IMAGE_DOS_HEADER readDos{};IMAGE_NT_HEADERS64 readNt{};
    check(ReadSupportedImage(supported.data(),readDos,readNt,&fingerprintReason), "supported PE identity still passes to normal reflection and native checks");
    supported[256]=0;
    check(!ReadSupportedImage(supported.data(),readDos,readNt,&fingerprintReason), "malformed PE identity still fails closed");
    auto policy = [&](int n, double m, int c, int a, int cap = INT32_MAX) { return ScaleGrant(n, m, c, a, cap); };
    check(policy(3, 2, 20, 20).amount == 6, "acquisition total is not capacity");
    check(!policy(3, .5, 20, 20).changed && !policy(3, 1.5, 20, 20).changed, "fractional multipliers rejected without rounding");
    check(policy(3, 0, 20, 20).amount == 0, "zero reward explicit");
    check(policy(3, 1000, 0, 0).amount == 3000, "full protocol multiplier range");
    check(policy(INT32_MAX, 1000, 0, 0).amount == INT32_MAX, "integer product cannot overflow before clamping");
    check(policy(7, 3, 0, 0).amount == 21 && policy(1, 1, 0, 0).amount == 1, "integer grants are exact and neutral one has no rounding");
    check(!policy(3, NAN, 0, 0).changed && !policy(3, INFINITY, 0, 0).changed, "nonfinite neutral");
    check(!policy(3, -1, 0, 0).changed && !policy(3, 1001, 0, 0).changed, "invalid protocol value neutral");
    check(!policy(3, 2, 10, 9).changed && !policy(3, 2, -1, 10).changed, "bad native count neutral");
    check(policy(3, 4, 90, 95, 100).amount == 10, "separately proven capacity honored");
    check(!policy(3, 2, 99, 99, 100).changed, "inconsistent limit cannot shrink native request");
    check(policy(2, 1000, 0, INT32_MAX-5).amount == 5, "acquisition counter overflow protected");
    check(policy(2, 1000, INT32_MAX-5, INT32_MAX-5).amount == 5, "both integer counters bounded");
    check(CatalogStackable("Weapon_Material_01") && CatalogStackable("Item_Piece_001"), "generated ordinary and fragment policy");
    check(FullItemName("Weapon_Material",11) == "Weapon_Material_10", "numeric FName suffix reconstructed");
    check(FullItemName("Return_ShaqiWeapon",2) == "Return_ShaqiWeapon_1", "consumable FName suffix reconstructed");
    check(FullItemName("Weapon_Material_01",0) == "Weapon_Material_01", "literal leading-zero suffix preserved");
    check(FullItemName("Item",1) == "Item_0" && FullItemName("None",2).empty(), "FName zero suffix and None handled");
    check(CatalogStackable("Return_ShaqiWeapon_1") && CatalogStackable("Item_Traversing"), "live repeatability evidence in generated runtime policy");
    check(CatalogStackable("Item_001") && CatalogStackable("Item_002"), "user-confirmed repeatable black stone and amber follow the shared catalog");
    for (const char* unique : {"Tang", "I_060002", "AC_Loot", "Hulu", "ShaqiHexin", "Hun_OutFit_Default", "Missing"})
        check(!CatalogStackable(unique), "unique/unknown item never multiplied");

    auto* worldClass = cls("World"); auto* world = object("World", worldClass);
    auto* playerClass = cls("BP_RenewalPlayer_C"); auto* pawn = object("Player", playerClass, world);
    auto* backpackClass = cls("PBZBackPackSubsystem"); auto* bag = object("Backpack", backpackClass, world);
    auto* chestClass = cls("BP_Chest_C"); auto* chest = object("Chest", chestClass, world);
    auto* rubbishClass = cls("BP_Rubbish_C"); auto* rubbish = object("Rubbish", rubbishClass, world);
    auto* npcClass = cls("BP_RenewalNPC_C"); auto* npc = object("Enemy", npcClass, world);
    auto* pickupClass = cls("PBZPickupActor"); auto* pickup = object("Pickup", pickupClass, world);
    auto* groupClass = cls("MobGroupScript_AddItemAfterCombat_C"); auto* group = object("CombatGroupReward", groupClass);
    auto* groupReward = function(groupClass,"LootItems",{});
    auto* groupInitialize = function(groupClass,"OnInitialize",{});
    auto* functionClass = cls("Function");
    targets.push_back({pawn, alloc(), attr::Who::Player, playerClass});
    auto* itemInfo = function(backpackClass,"GetItemNumInfo",{{"PropID","NameProperty",8},{"CurrentNum","IntProperty",4},{"MaxNum","IntProperty",4}});
    auto* dust = function(playerClass,"ChangeDustNum",{{"bIsAdd","BoolProperty",1},{"Num","IntProperty",4}});
    auto* buy = function(backpackClass,"BuyItem",{{"ItemName","NameProperty",8},{"ItemNum","IntProperty",4}});
    auto* load = function(backpackClass,"LoadGame",{});
    auto* endPickup = function(rubbishClass,"EndSwitchInteract",{});
    properties[{pickupClass,"Owner"}] = 0x180;
    bridge.backpackClass = backpackClass; bridge.playerClass = playerClass; bridge.worldClass = worldClass;
    bridge.pickupClass = pickupClass;
    bridge.functionClass = functionClass; bridge.dustOffset = 0x190;
    properties[{playerClass, "ConsciousnessDust"}] = 0x190;
    std::string why;
    check(bridge.itemInfo.Resolve(backpackClass,"GetItemNumInfo",{{"PropID","NameProperty",8},{"CurrentNum","IntProperty",4},{"MaxNum","IntProperty",4}},&why), "query reflection ABI");
    check(bridge.dust.Resolve(playerClass,"ChangeDustNum",{{"bIsAdd","BoolProperty",1},{"Num","IntProperty",4}},&why), "dust reflection ABI");
    native[itemInfo] = [=](auto*,void* args) { argPut(itemInfo,args,"CurrentNum",20); argPut(itemInfo,args,"MaxNum",20); };
    native[dust] = [=](auto*,void* args) { fixture::observed = arg<int>(dust,args,"Num"); ++fixture::calls; };
    fixture::bag = bag; fixture::player = pawn; fixture::dustFunction = dust;
    fixture::item = name("Weapon_Material_01");
    put(pawn, bridge.dustOffset, int32_t(100));
    auto makeFrame = [&](ue::UObject* caller, ue::UObject* owner, const char* functionName) {
        std::array<uint8_t,128> frame{};
        auto* node = function(owner,functionName,{}); put(node,ue::Layout::ObjClass,functionClass); put(node,ue::Layout::ObjOuter,owner);
        put(frame.data(),0x10,node); put(frame.data(),0x18,caller); return frame;
    };
    auto chestFrame = makeFrame(chest,chestClass,"Pickup Items");
    auto enemyFrame = makeFrame(npc,npcClass,"LootItems");
    auto groupFrame = makeFrame(group,groupClass,"LootItems");
    groupReward = pe::FindFunction(groupClass,"LootItems"); // makeFrame installs the live function identity.
    auto pickupFrame = makeFrame(pickup,pickupClass,"BoxCollisionOverlapBegin");
    auto loadedFrame = makeFrame(chest,chestClass,"OnLoadedActorData");
    auto graphFrame = makeFrame(chest,chestClass,"ExecuteUbergraph_BP_Chest");
    auto unknownFrame = makeFrame(pawn,playerClass,"QuestReward");
    auto forwardingFrame = makeFrame(bag,backpackClass,"AddPropsToBackPack");
    auto* actorInteract = function(rubbishClass,"OnInteract",{{"InteractWith","ObjectProperty",8},{"ReturnValue","BoolProperty",1,true}});
    auto* completed = function(rubbishClass,"CompletedDisplayUI_Event",{{"Guid","StructProperty",16}});
    put(rubbishClass,ue::Layout::StructChildren,completed);
    check(Bridge::GuidCallback(rubbishClass) == completed, "unique actual Guid callback selected by metadata");
    auto* duplicate = function(rubbishClass,"CompletedDisplayUI_Other",{{"Guid","StructProperty",16}});
    put(completed,ue::Layout::UFieldNext,duplicate);
    check(Bridge::GuidCallback(rubbishClass) == nullptr, "ambiguous callback rejected");
    put(completed,ue::Layout::UFieldNext,static_cast<ue::UObject*>(nullptr));
    check(bridge.Caller(chestFrame.data()).origin == Origin::Chest, "FFrame validates actual caller and function owner");
    check(bridge.Caller(loadedFrame.data()).origin == Origin::Unknown, "load explicitly excluded");
    check(bridge.Caller(graphFrame.data()).origin == Origin::Unknown && bridge.Caller(graphFrame.data()).caller.Alive(), "whole ubergraph observed but not authorized");
    check(bridge.Caller(unknownFrame.data()).origin == Origin::Unknown, "quest caller excluded");
    auto wrongFrame = makeFrame(chest,npcClass,"LootItems");
    check(bridge.Caller(wrongFrame.data()).origin == Origin::Unknown, "frame class mismatch rejected");
    auto* foreignWorld = object("ForeignWorld",worldClass); auto* foreign = object("ForeignChest",chestClass,foreignWorld);
    auto foreignFrame = makeFrame(foreign,chestClass,"Pickup Items");
    check(bridge.Caller(foreignFrame.data()).origin == Origin::Chest, "world mismatch is diagnostic under approved class policy");
    put(pickup,0x180,npc); check(bridge.Caller(pickupFrame.data()).origin == Origin::Enemy, "NPC-owned pickup keeps enemy provenance");
    put(pickup,0x180,static_cast<ue::UObject*>(nullptr));
    check(bridge.Caller(pickupFrame.data()).origin == Origin::Pickup, "unowned environment pickup");

    auto* packageClass = cls("Package"); auto* package = object("StreamingCellPackage",packageClass);
    auto* levelClass = cls("Level"); auto* streamedLevel = object("StreamingLevel",levelClass,package);
    properties[{levelClass,"OwningWorld"}] = 0x1d0;
    put(streamedLevel,0x1d0,world);
    auto* streamedRubbish = object("StreamedRubbish",rubbishClass,streamedLevel);
    check(bridge.Classify(streamedRubbish) == Origin::Pickup, "external package cannot reject live whitelisted source class");
    bridge.levelClass = levelClass; bridge.levelWorldOffset = 0x1d0;
    check(bridge.WorldOf(streamedRubbish) == world && bridge.Classify(streamedRubbish) == Origin::Pickup, "typed Level OwningWorld supports streamed actor source");
    put(streamedLevel,0x1d0,foreignWorld);
    check(bridge.Classify(streamedRubbish) == Origin::Pickup && !bridge.Local(streamedRubbish), "foreign world remains visible as diagnostic only");
    put(streamedLevel,0x1d0,package);
    check(bridge.WorldOf(streamedRubbish) == nullptr, "wrong-type Level OwningWorld rejected");
    put(streamedLevel,0x1d0,static_cast<ue::UObject*>(nullptr));
    check(bridge.WorldOf(streamedRubbish) == nullptr, "null OwningWorld cannot inherit outer context");
    put(streamedLevel,0x1d0,world);

    bridge.bindings.exec = {reinterpret_cast<void*>(fixture::Exec<0>),reinterpret_cast<void*>(fixture::Exec<1>),reinterpret_cast<void*>(fixture::Exec<2>),reinterpret_cast<void*>(fixture::Exec<3>),reinterpret_cast<void*>(fixture::Exec<4>)};
    bridge.bindings.add = reinterpret_cast<void*>(fixture::NativeAdd);
    bridge.resolved = true; bridge.validated = true; bridge.gameThread = GetCurrentThreadId();
    NativeBindings rejected;
    check(signature::ForImage(signature::Timestamp,signature::ImageSize)!=nullptr,"legacy native loot profile remains supported");
    const auto* currentProfile=signature::ForImage(1793568319u,504082432u);
    check(currentProfile&&currentProfile->execs[0].hash==0x80A898569F8F2ABDull&&currentProfile->addCallOffset==249,"updated loot profile selects audited registration and native consumer fingerprints");
    check(!signature::ForImage(1793568319u,504082431u)&&!signature::ForImage(1,504082432u),"unknown executable fingerprint remains rejected");
    check(!rejected.Resolve(&why), "unrelated child executable fingerprint fails closed");
    std::array<uint8_t, 24> before{}; std::memcpy(before.data(),bridge.bindings.add,before.size());
    check(hooks::Init(&why) && bridge.Start(), "real hook ledger installs in isolated child");
    RegisterResFeatures();
    check(FindFeature("res.item_mul") && FindFeature("res.money_mul"), "two resource acquisition multipliers registered");
    for (const char* retired : {"res.drop_mul", "res.gather_mul", "res.chest_mul", "res.price_mul", "res.cost_mul", "res.material_mul"})
        check(FindFeature(retired) == nullptr, "retired source and transaction IDs are not registered");
    bridge.values = {4, 5}; bridge.active.fill(true);
    check(bridge.Rebind(), "real hooks enabled");
    using Run = res::loot::Exec;
    auto run = [&](int slot, auto& frame) { reinterpret_cast<Run>(bridge.bindings.exec[slot])(bag,frame.data(),nullptr); };
    run(0,chestFrame); check(fixture::observed == 12 && fixture::calls == 1 && fixture::seenObject == bag && fixture::seenPush, "native count, object, FName, bool ABI preserved once");
    fixture::item = uint64_t(name("Weapon_Material")) | (uint64_t(11) << 32);
    run(0,chestFrame); check(fixture::observed == 12, "numbered FName quantity changes without changing original FName ABI");
    fixture::item = name("Weapon_Material_01");
    run(1,enemyFrame); check(fixture::observed == 12, "enemy wrapper uses the same item multiplier as chest");
    run(1,groupFrame); check(fixture::observed == 12, "observed MobGroupScript combat reward caller is identified as enemy");
    native[groupReward] = [&](auto*,void*) { fixture::NativeAdd(bag,fixture::amount,fixture::item,true); };
    dispatch(group,groupReward,nullptr); check(fixture::observed == 12, "group reward PE scope identifies direct C++ grant without FFrame");
    native[groupInitialize] = [&](auto*,void*) { dispatch(group,groupReward,nullptr); };
    dispatch(group,groupInitialize,nullptr); check(fixture::observed == 3, "combat group initialization cannot impersonate earned reward");
    run(2,pickupFrame); check(fixture::observed == 12, "silent wrapper scopes final leaf to pickup");
    reinterpret_cast<Run>(bridge.bindings.exec[4])(pickup,unknownFrame.data(),nullptr);
    check(fixture::observed == 12, "native pickup overlap carries actual actor to direct C++ grant");
    run(0,unknownFrame); check(fixture::observed == 3, "unknown origin unchanged");
    run(0,loadedFrame); check(fixture::observed == 3, "loaded rewards unchanged");
    run(0,foreignFrame); check(fixture::observed == 12, "class source does not require same-world metadata");
    native[actorInteract] = [&](auto*,void*) { fixture::NativeAdd(bag,fixture::amount,fixture::item,true); };
    dispatch(rubbish,actorInteract,nullptr); check(fixture::observed == 12, "PE actor root identifies direct C++ grant without exec entry");
    check(context.origin == Origin::Unknown && !context.actorRoot, "actor event source immediately restored after return");
    native[actorInteract] = [&](auto*,void*) { run(0,forwardingFrame); };
    dispatch(rubbish,actorInteract,nullptr); check(fixture::observed == 12, "callee forwarding preserves exact synchronous actor root");
    native[actorInteract] = [&](auto*,void*) { run(0,unknownFrame); };
    dispatch(rubbish,actorInteract,nullptr); check(fixture::observed == 12, "live synchronous actor root is authoritative over wrapper details");
    native[actorInteract] = [&](auto*,void*) { reinterpret_cast<Run>(bridge.bindings.exec[0])(bag,nullptr,nullptr); };
    dispatch(rubbish,actorInteract,nullptr); check(fixture::observed == 12, "null FFrame cannot erase actual synchronous caller provenance");
    native[actorInteract] = [&](auto*,void*) { run(0,loadedFrame); };
    dispatch(rubbish,actorInteract,nullptr); check(fixture::observed == 3, "load callback cannot borrow actor root");
    auto* foreignRubbish = object("ForeignRubbish",rubbishClass,foreignWorld);
    native[completed] = [&](auto*,void*) { fixture::NativeAdd(bag,fixture::amount,fixture::item,true); };
    native[actorInteract] = [&](auto*,void*) { dispatch(foreignRubbish,completed,nullptr); };
    dispatch(rubbish,actorInteract,nullptr); check(fixture::observed == 12, "whitelisted actor callback remains classified with different world metadata");
    native[buy] = [&](auto*,void*) { fixture::NativeAdd(bag,fixture::amount,fixture::item,true); };
    native[load] = native[buy];
    native[actorInteract] = [&](auto*,void*) { dispatch(bag,buy,nullptr); };
    dispatch(rubbish,actorInteract,nullptr); check(fixture::observed == 3, "nested shop transaction masks actor source");
    native[actorInteract] = [&](auto*,void*) { dispatch(bag,load,nullptr); };
    dispatch(rubbish,actorInteract,nullptr); check(fixture::observed == 3, "nested load transaction masks actor source");
    native[load] = [&](auto*,void*) { dispatch(rubbish,completed,nullptr); };
    native[actorInteract] = [&](auto*,void*) { dispatch(bag,load,nullptr); };
    dispatch(rubbish,actorInteract,nullptr); check(fixture::observed == 3, "actor callbacks inside restoration cannot re-authorize rewards");
    native[actorInteract] = [&](auto*,void*) { throw std::runtime_error("fixture actor error"); };
    try { dispatch(rubbish,actorInteract,nullptr); } catch (const std::runtime_error&) {}
    check(context.origin == Origin::Unknown && !context.actorRoot, "actor exception restores context");
    native[completed] = [&](auto*,void*) { fixture::useDust = true; fixture::NativeExec(bag,nullptr,nullptr); fixture::useDust = false; };
    dispatch(rubbish,completed,nullptr); check(fixture::observed == 15, "delayed UI completion establishes a fresh bounded Dust source");
    std::thread other([&] { run(0,chestFrame); }); other.join();
    check(fixture::observed == 3, "other thread original unchanged");
    for (const char* id : {"Item_001", "Item_002"}) {
        fixture::item = name(id);
        for (auto* frame : {&enemyFrame,&pickupFrame,&chestFrame}) {
            run(0,*frame); check(fixture::observed == 12, "confirmed repeated attribute follows shared stackable policy");
        }
        run(0,unknownFrame); check(fixture::observed == 3, "editor/unknown attribute grant is never scaled");
    }
    for (const char* id : {"Tang", "AC_ActHit", "BrowLashCard_Default", "I_01001", "T_02002", "ShaqiHexin", "Missing", "Dust"}) {
        fixture::item = name(id);
        for (auto* frame : {&enemyFrame,&pickupFrame,&chestFrame}) {
            run(0,*frame); check(fixture::observed == 3, "unique/null/unknown/Dust item never uses item multiplier in any source");
        }
    }
    fixture::item = name("Weapon_Material_01");
    fixture::recurseUnknown = true; run(0,chestFrame); check(fixture::observed == 1, "nested native grants not multiplied again");
    fixture::throwNative = true;
    try { run(0,chestFrame); } catch(const std::runtime_error&) {}
    fixture::throwNative = false;
    check(grantDepth == 0 && context.origin == Origin::Unknown, "native exception releases scope and depth");
    bridge.values[Item] = 0; int beforeCalls = fixture::calls; run(0,chestFrame);
    check(fixture::calls == beforeCalls, "zero quantity does not create zero stack");
    bridge.values[Item] = 4;
    fixture::useDust = true;
    for (auto* frame : {&enemyFrame,&pickupFrame,&chestFrame,&groupFrame}) {
        run(3,*frame); check(fixture::observed == 15, "all three identified Dust sources use independent money multiplier");
    }
    native[endPickup] = [&](auto*,void*) { fixture::NativeExec(bag,nullptr,nullptr); };
    dispatch(rubbish,endPickup,nullptr); check(fixture::observed == 15, "deferred pickup completion uses a fresh synchronous Dust source");
    native[load] = [&](auto*,void*) { dispatch(rubbish,endPickup,nullptr); };
    dispatch(bag,load,nullptr); check(fixture::observed == 3, "completion invoked inside restoration stays excluded");
    native[buy] = [&](auto*,void*) { fixture::NativeExec(bag,nullptr,nullptr); };
    native[actorInteract] = [&](auto*,void*) { dispatch(bag,buy,nullptr); };
    dispatch(rubbish,actorInteract,nullptr);
    check(fixture::observed == 3, "positive shop refund cannot inherit Dust reward multiplier");
    native[load] = native[buy];
    native[actorInteract] = [&](auto*,void*) { dispatch(bag,load,nullptr); };
    dispatch(rubbish,actorInteract,nullptr);
    check(fixture::observed == 3, "positive load restoration cannot inherit Dust reward multiplier");
    check(wishes.empty(), "Dust ledger does not declare additive property multiplier");
    run(3,unknownFrame); check(fixture::observed == 3, "unknown positive currency delta unchanged");
    bridge.active[Item] = false;
    fixture::useDust = false; run(0,chestFrame); check(fixture::observed == 3, "item disable immediate while money stays enabled");
    bridge.active.fill(false); check(bridge.Rebind(), "all hooks disabled");
    check(interceptors.empty(), "owned Dust interceptor removed");
    run(0,enemyFrame); check(fixture::observed == 3, "off executes native quantity");
    check(std::memcmp(before.data(),bridge.bindings.add,before.size()) == 0, "native code bytes restored");
    auto* probe = FindFeature("res.loot_probe");
    check(probe && probe->IsToggle(), "development probe is separate from production multipliers");
    int externalDustSeen = 0;
    const int externalDustToken = pe::Observe(dust,[&](auto*,void*) { ++externalDustSeen; });
    probe->Enable(); fixture::useDust = false; run(0,chestFrame);
    check(fixture::observed == 3 && bridge.active[Money] == false, "probe leaves material request unchanged");
    check(!interceptors.count(dust) && observers.size() > 1, "pure Dust probe uses multi-observer API without interception");
    fixture::useDust = true; run(3,chestFrame);
    check(fixture::observed == 3, "probe leaves Dust request unchanged");
    check(externalDustSeen == 1, "probe does not overwrite another consumer's observer");
    fixture::useDust = false; probe->Disable();
    check(observers.size() == 1 && observers.count(externalDustToken), "probe cleanup removes only owned observer tokens");
    pe::Unobserve(externalDustToken);
    check(observers.empty(), "owned observer cleanup complete");
    fixture::NativeAdd(bag,3,fixture::item,true);
    check(fixture::observed == 3 && context.origin == Origin::Unknown, "standalone item editor-style native grant remains unchanged");
    auto* itemFeature = FindFeature("res.item_mul");
    auto* moneyFeature = FindFeature("res.money_mul");
    for (auto* feature : {itemFeature, moneyFeature}) {
        const int slot = feature == itemFeature ? Item : Money;
        feature->SetValue(3); feature->Enable();
        for (double invalid : {.5, 1.5, 2.000000000000001, -1.0, 1001.0, double(NAN), double(INFINITY)}) {
            feature->SetValue(invalid); feature->Enable();
            check(feature->enabled && feature->value == 3 && bridge.values[slot] == 3 && bridge.active[slot], "invalid integer request preserves previous active value without truncation");
        }
        feature->Disable(); feature->SetValue(1.5); feature->Enable();
        check(!feature->enabled && !bridge.active[slot] && feature->value == 3, "fractional request cannot enable inactive feature");
        feature->SetValue(1000); feature->Enable();
        check(feature->enabled && feature->value == 1000, "resource integer range is not clamped to UI slider hints");
        feature->Disable(); feature->SetValue(1);
    }
    for (unsigned i = 0; i < 20; ++i) {
        itemFeature->SetValue(2); itemFeature->Enable(); run(0,enemyFrame);
        check(fixture::observed == 6, "repeat toggle uses event quantity, never accumulates multiplier");
        itemFeature->Disable(); run(0,enemyFrame);
        check(fixture::observed == 3, "repeat toggle immediate native restoration");
    }
    check(std::memcmp(before.data(),bridge.bindings.add,before.size()) == 0, "native bytes restored after twenty toggles");
    itemFeature->SetValue(2); itemFeature->Enable();
    native[actorInteract] = [&](auto*,void*) { fixture::NativeAdd(bag,fixture::amount,fixture::item,true); };
    auto* chestInteract = function(chestClass,"Pickup Items",{});
    bridge.BindActorRoots(false); bridge.BindActorRoots(true);
    int balance = 10;
    native[itemInfo] = [=,&balance](auto*,void* args) { argPut(itemInfo,args,"CurrentNum",balance); argPut(itemInfo,args,"MaxNum",balance); };
    native[chestInteract] = [&](auto*,void*) { fixture::NativeAdd(bag,3,fixture::item,true); balance += fixture::observed; };
    dispatch(chest,chestInteract,nullptr);
    check(balance == 16 && fixture::observed == 6, "chest source scales native single-event request3 to6 and authoritative fixture inventory10 to16");
    fixture::NativeAdd(bag,3,fixture::item,true);
    check(fixture::observed == 3, "item editor-style grant stays neutral even while item multiplier enabled");
    dispatch(rubbish,actorInteract,nullptr);
    check(fixture::observed == 6, "single item multiplier also scales ordinary pickup");
    itemFeature->Disable(); dispatch(chest,chestInteract,nullptr);
    check(balance == 19 && fixture::observed == 3, "disabled item multiplier restores next native grant quantity");

    // The real consumer receives the already scaled count; these observers
    // must neither change parameters nor displace any other observer.
    auto* tipsClass = cls("WB_ItemTips_C"); auto* tips = object("ItemTips",tipsClass,world);
    auto* display = function(tipsClass,"DisplayItem",{{"PropID","NameProperty",8},{"bIsAdd","BoolProperty",1},{"Num","IntProperty",4}});
    auto* applyDust = function(playerClass,"ChangeDustNumInternal",{{"ChangeDustNum","IntProperty",4}});
    auto* eventData = cls("GameplayEventData"); put(eventData,ue::Layout::StructSize,0xb0);
    properties[{eventData,"OptionalObject"}] = 0x18;
    auto* dustTipClass = cls("BP_UpdateDustTipsObj_C"); auto* dustTip = object("DustTipPayload",dustTipClass);
    properties[{dustTipClass,"CurrentDustNum"}] = 0x28; properties[{dustTipClass,"ChangeDustNum"}] = 0x2c;
    auto* dustUIClass = cls("W_Dust_Tips_C"); auto* dustUI = object("DustUI",dustUIClass,world);
    auto* dustEvent = function(dustUIClass,"EventReceived_13C4885342CE314232EF92B7E1475574",{{"Payload","StructProperty",0xb0}});
    std::vector<std::string> notices; log::g_sink = [&](const auto& s) { if (s.find("loot native-ui fn=") != std::string::npos) notices.push_back(s); };
    int displayValue = 0, dustValue = 0, displayCalls = 0;
    native[display] = [&](auto*,void* args) { displayValue = arg<int>(display,args,"Num"); ++displayCalls; };
    native[applyDust] = [&](auto*,void* args) { dustValue = arg<int>(applyDust,args,"ChangeDustNum"); };
    const int another = pe::Observe(display,[&](auto*,void*) { ++displayCalls; });
    probe->Enable(); itemFeature->SetValue(2); itemFeature->Enable(); moneyFeature->SetValue(3); moneyFeature->Enable();
    native[chestInteract] = [&](auto*,void*) {
        fixture::NativeAdd(bag,3,fixture::item,true);
        std::array<uint8_t,32> args{};
        argPut(display,args.data(),"PropID",fixture::item); argPut(display,args.data(),"bIsAdd",uint8_t(1)); argPut(display,args.data(),"Num",fixture::observed);
        dispatch(tips,display,args.data());
        check(arg<int>(display,args.data(),"Num") == 6, "notice observer preserves scaled input buffer");
    };
    dispatch(chest,chestInteract,nullptr);
    check(displayValue == 6 && displayCalls == 2, "native item formatter receives grant3x2=6 once and other observers remain active");
    std::array<uint8_t,32> applyArgs{}; argPut(applyDust,applyArgs.data(),"ChangeDustNum",9);
    dispatch(pawn,applyDust,applyArgs.data());
    check(dustValue == 9, "Dust apply observer does not multiply an already final delta again");
    put(dustTip,0x28,int32_t(109)); put(dustTip,0x2c,int32_t(9));
    std::array<uint8_t,0xb0> eventArgs{}; put(eventArgs.data(),0x18,dustTip);
    const auto argsBefore = eventArgs;
    dispatch(dustUI,dustEvent,eventArgs.data());
    check(eventArgs == argsBefore && ue::Get<int>(dustTip+0x2c)==9, "native Dust UI payload remains byte-identical");
    check(std::any_of(notices.begin(),notices.end(),[](const auto& s) { return s.find("fn=W_Dust_Tips item=Dust quantity=9 current=109")!=std::string::npos; }), "observer reads native Dust display delta and current balance");
    const auto count = notices.size();
    dispatch(dustUI,dustEvent,nullptr);
    auto* unrelated = object("UnrelatedPayload",cls("Unrelated")); put(eventArgs.data(),0x18,unrelated);
    dispatch(dustUI,dustEvent,eventArgs.data());
    check(notices.size()==count, "unreadable and wrong-class UI payloads are ignored");
    // While a multiplier stays on, the tip-follow-multiplier observers (DisplayItem + W_Dust_Tips event in this fixture) remain.
    probe->Disable(); check(observers.size()==3 && observers.count(another), "probe removes only its UI and source observers");
    dispatch(pawn,applyDust,applyArgs.data()); check(notices.size()==count, "disabled UI probe no longer logs notices");
    pe::Unobserve(another); itemFeature->Disable(); moneyFeature->Disable(); log::g_sink={};
    check(observers.empty(), "all temporary notice observers released");
    // RefreshUnavailableFeatures retries the three loot features every second
    // while their shared bridge is unavailable. Class-level resolution must
    // never scan optional source classes, and the expensive native PE
    // registration scan is deferred until a loot feature is enabled.
    bridge.resolved = false; bridge.validated = false; bridge.nextResolve_ = 0; bridge.lastWhy_.clear();
    classQueries.clear();
    std::string retryWhy;
    check(bridge.Resolve(&retryWhy), "class-level loot bridge resolves without scanning the game image");
    for (const auto& query : classQueries)
        check(query != "BP_Chest_C" && query != "BP_NPCLootComp_C" && query != "BP_RenewalNPC_C" && query != "BP_Rubbish_C",
              "Resolve retry does not globally search optional source classes");
    const auto firstRetry = classQueries.size();
    check(bridge.Resolve(&retryWhy) && classQueries.size() == firstRetry,
          "already-resolved loot bridge does not repeat reflection");
    check(!bridge.Validate(&retryWhy), "deferred native PE validation still fails closed for an unrelated executable");
    hooks::RemoveAll();
    std::cout << "NATIVE_LOOT_TEST_OK " << checks << " checks (isolated child, real hooks, no game process)\n";
    return 0;
} catch(const std::exception& error) {
    pbz::hooks::RemoveAll(); std::cerr << "FAIL: " << error.what() << '\n'; return 1;
}
