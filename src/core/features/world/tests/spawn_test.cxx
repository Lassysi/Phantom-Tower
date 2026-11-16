// No game, process injection, or installed files. Exercises the actual planner,
// reflection wrapper, queue fencing and owned-object cleanup code.
#include "../enemy_spawn.cpp"
#include <iostream>
#include <map>
#include <memory>
#include <thread>
#include <unordered_map>

namespace fixture {
using namespace pbz;
using namespace pbz::enemies;
std::vector<std::unique_ptr<uint8_t[]>> allocations;
std::vector<U*> objects;
std::vector<std::string> names{"None"};
std::map<std::string, U*> classes;
std::map<std::pair<U*, std::string>, U*> functions;
std::map<std::pair<U*, std::string>, int> props;
std::unordered_map<U*, std::function<void(U*, void*)>> calls;
std::unordered_map<U*, pe::Interceptor> interceptions;
std::vector<std::function<void()>> queue;
bool ready = true;
int checks = 0, destroyed = 0, engineCalls = 0;
void Check(bool value, const char* why) { ++checks; if (!value) throw std::runtime_error(why); }
template<class F> void Throws(F f, const char* why) { bool caught = false; try { f(); } catch (...) { caught = true; } Check(caught, why); }
template<class T> void Put(void* base, int off, T value) { std::memcpy(static_cast<uint8_t*>(base) + off, &value, sizeof value); }
U* Alloc() { allocations.push_back(std::make_unique<uint8_t[]>(8192)); std::memset(allocations.back().get(), 0, 8192); return allocations.back().get(); }
uint32_t Name(const std::string& value) {
    const auto it = std::find(names.begin(), names.end(), value);
    if (it != names.end()) return static_cast<uint32_t>(it - names.begin());
    names.push_back(value); return static_cast<uint32_t>(names.size() - 1);
}
U* Object(const std::string& name, U* cls = nullptr, U* outer = nullptr) {
    U* p = Alloc(); Put(p, ue::Layout::ObjIndex, static_cast<int>(objects.size()));
    Put(p, ue::Layout::ObjName, Name(name)); Put(p, ue::Layout::ObjClass, cls); Put(p, ue::Layout::ObjOuter, outer);
    objects.push_back(p); return p;
}
U* Class(const std::string& name, U* super = nullptr) {
    U* cls = Object(name); Put(cls, ue::Layout::StructSuper, super); classes[name] = cls;
    Object("Default__" + name, cls); return cls;
}
U* Function(U* owner, const char* name, std::initializer_list<Spec> specs) {
    U* fn = Object(name); int off = 0; U* previous = nullptr;
    for (const auto& spec : specs) {
        off = (off + 7) / 8 * 8;
        U* f = Alloc(), *type = Alloc();
        // Current SDK's enum<...>:u8 is raw ByteProperty, not EnumProperty.
        Put(type, ue::Layout::FieldClassName, Name(std::string_view(spec.type)=="EnumProperty" ? "ByteProperty" : spec.type));
        Put(f, ue::Layout::FieldClass, type); Put(f, ue::Layout::FieldName, Name(spec.name));
        Put(f, ue::Layout::PropOffset, off); Put(f, 0x34, spec.bytes); Put(f, 0x38, uint64_t(0x80 | (spec.result ? 0x400 : 0)));
        if (spec.structure) {
            if (!classes.contains(spec.structure)) Class(spec.structure);
            Put(f, 0x70, classes.at(spec.structure));
        } else if (std::string_view(spec.type)=="EnumProperty") {
            if (!classes.contains("FixtureEnum")) Class("FixtureEnum");
            Put(f,0x70,classes.at("FixtureEnum"));
        }
        if (previous) Put(previous, ue::Layout::FieldNext, f); else Put(fn, ue::Layout::StructChildProps, f);
        previous = f; props[{fn, spec.name}] = off; off += spec.bytes;
    }
    Put(fn, ue::Layout::StructSize, off); functions[{owner, name}] = fn; return fn;
}
template<class T> T Arg(U* fn, void* params, const char* name) { return ue::Get<T>(static_cast<uint8_t*>(params) + props.at({fn, name})); }
template<class T> void Return(U* fn, void* params, const char* name, T value) { Put(params, props.at({fn, name}), value); }
void Drain() { auto old = std::move(queue); queue.clear(); for (auto& f : old) f(); }
void Dispatch(U* object, U* function, void* params) {
    auto h=interceptions.find(function);
    if (h!=interceptions.end()&&h->second(object,params)) return;
    pe::Call(object,function,params);
}

struct Scene : SceneQuery {
    bool blocked = false, noFloor = false, ledge = false, steep = false, lowFloor = false;
    bool projection = false, badProjection = false;
    int floorCalls = 0, roomCalls = 0;
    bool Project(Vec p, Vec& out) override { out = p; if (badProjection) out.x += 10000; return projection; }
    bool Floor(Vec p, Ground& out) override {
        ++floorCalls;
        if (noFloor || (ledge && floorCalls % 5 == 3)) return false;
        out = {{p.x, p.y, lowFloor ? -1000.0 : 0.0}, {0, 0, steep ? .4 : 1}, true, false}; return true;
    }
    bool Room(Vec, Capsule) override { ++roomCalls; return !blocked; }
};

void PlannerTests() {
    Scene scene; const Vec player{0, 0, 90}, forward{1, 0, 0};
    auto plan = FindLandings(scene, player, forward, {35, 90}, {50, 100}, 8, 600);
    Check(plan.size() == 8, "eight valid grounded positions");
    Check(plan[0].center.x == 600 && plan[0].center.y == 0 && plan[0].center.z == 105, "forward first, capsule base above floor");
    for (size_t i = 0; i < plan.size(); ++i) {
        Check(Distance2D(plan[i].center, player) >= 185, "player clearance");
        for (size_t j = 0; j < i; ++j) Check(Distance2D(plan[i].center, plan[j].center) >= 140, "batch capsule separation");
    }
    scene.blocked = true; Check(FindLandings(scene, player, forward, {35,90}, {50,100}, 1,600).empty(), "walls reject all candidates");
    scene = {}; scene.noFloor = true; Check(FindLandings(scene, player, forward, {35,90}, {50,100}, 1,600).empty(), "no floor never falls back to air");
    scene = {}; scene.steep = true; Check(FindLandings(scene, player, forward, {35,90}, {50,100}, 1,600).empty(), "steep slope rejection");
    scene = {}; scene.ledge = true; Check(FindLandings(scene, player, forward, {35,90}, {50,100}, 1,600).empty(), "footprint edge support required");
    scene = {}; scene.lowFloor = true; Check(FindLandings(scene, player, forward, {35,90}, {50,100}, 1,600).empty(), "lower floor cliff rejected");
    scene = {}; scene.projection = true; scene.badProjection = true; Check(FindLandings(scene, player, forward, {35,90}, {50,100}, 1,600).empty(), "far nav snap rejected");
    scene = {}; Check(!FindLandings(scene, player, forward, {35,90}, {300,600}, 1,200).empty(), "large boss gets increased distance");
    scene = {}; Check(FindLandings(scene, player, forward, {35,90}, {50,100}, 0,600).empty(), "zero count rejected");
    Check(FindLandings(scene, player, forward, {35,90}, {50,100}, 9,600).empty(), "too many rejected");
    Check(FindLandings(scene, player, forward, {35,90}, {50,100}, 1,NAN).empty(), "NaN distance rejected");
    Check(FindLandings(scene, {NAN,0,0}, forward, {35,90}, {50,100}, 1,600).empty(), "NaN player rejected");
    Check(FindLandings(scene, player, forward, {35,90}, {0,100}, 1,600).empty(), "zero capsule rejected");
    auto reserved = FindLandings(scene, player, forward, {35,90}, {50,100}, 1,600);
    auto again = FindLandings(scene, player, forward, {35,90}, {50,100}, 1,600,reserved);
    Check(!again.empty() && Distance2D(again[0].center, reserved[0].center) >= 140, "previous spawn reservations honored");
    scene = {};
    RegionQuery region(scene, {5000, 0, 90}, 1000);
    std::vector<Landing> playerReservation{{{5800,0,90}, {185,90}, false}};
    const auto inside = FindLandings(region, {5000,0,90}, forward, {35,90}, {50,100}, 4, 600, playerReservation);
    Check(inside.size() == 4, "wave spawns use a separate stable arena center");
    for (const auto& spot : inside) {
        Check(Distance2D(spot.center, {5000,0,90}) + 55 <= 1000, "entire enemy capsule stays inside conservative region");
        Check(Distance2D(spot.center, {5800,0,90}) >= 275, "player near arena edge remains separated from wave spawns");
    }
    RegionQuery tiny(scene, {5000,0,90}, 100);
    Check(FindLandings(tiny, {5000,0,90}, forward, {35,90}, {50,100}, 1, 600).empty(), "an undersized region fails without spawning outside it");
    Check(scene.floorCalls <= 1000, "bounded floor work");
    scene = {}; reserved.clear();
    for (int batch = 0; batch < 4; ++batch) {
        auto next = FindLandings(scene, player, forward, {35,90}, {50,100}, 8,600,reserved);
        Check(next.size() == 8, "consecutive batches pass the old sixteen-actor boundary");
        reserved.insert(reserved.end(), next.begin(), next.end());
    }
    Check(reserved.size() == 32, "thirty-two distinct grounded reservations retained");
}

void ReflectionTests() {
    U* owner = Class("TestLibrary"); U* vector = Class("Vector"); Class("Rotator");
    U* fn = Function(owner, "Get", {{"ReturnValue", "StructProperty", 24, true, "Vector"}});
    calls[fn] = [fn](U*, void* params) { Return(fn, params, "ReturnValue", Vec{1,2,3}); };
    Fn f; f.Bind(Cdo(owner), owner, "Get", {{"ReturnValue", "StructProperty", 24, true, "Vector"}});
    Buffer b; f.Call(b); Check(f.Get<Vec>(b,0).z == 3, "actual reflection buffer call");
    Throws([&] { f.Get<float>(b,0); }, "typed read size guard");
    Throws([&] { Fn bad; bad.Bind(Cdo(owner), owner, "Get", {{"ReturnValue","StructProperty",24,true,"Rotator"}}); }, "same-size wrong struct rejected");
    Throws([&] { Fn bad; bad.Bind(Cdo(owner), owner, "Get", {{"ReturnValue","StructProperty",24,false,"Vector"}}); }, "return flag mismatch rejected");
    Throws([&] { Fn bad; bad.Bind(Cdo(owner), owner, "Get", {{"Wrong","StructProperty",24,true,"Vector"}}); }, "parameter name mismatch rejected");
    U* prop = ue::Get<U*>(fn + ue::Layout::StructChildProps); Put(prop, ue::Layout::PropOffset, 4090);
    Throws([&] { Fn bad; bad.Bind(Cdo(owner), owner, "Get", {{"ReturnValue","StructProperty",24,true,"Vector"}}); }, "out-of-bounds metadata rejected");
    Put(prop, ue::Layout::PropOffset, 0);
    Identity id(Cdo(owner)); Put(id.object, ue::Layout::ObjName, Name("Recycled"));
    Throws([&] { f.Call(b); }, "recycled owner not called");
    Check(!id.Alive(), "name generation change invalidates ownership");
    U* pointers = Class("PointerLibrary");
    U* pointerFn = Function(pointers,"ClassArg",{{"Class","ObjectProperty",8}});
    U* pointerField=ue::Get<U*>(pointerFn+ue::Layout::StructChildProps);
    U* pointerType=ue::Get<U*>(pointerField+ue::Layout::FieldClass);
    Put(pointerType,ue::Layout::FieldClassName,Name("ClassProperty"));
    Fn pointerCall;pointerCall.Bind(Cdo(pointers),pointers,"ClassArg",{{"Class","ObjectProperty",8}});
    Check(pointerCall.offsets.size()==1,"raw ClassProperty matches SDK ObjectProperty<Class> alias");
    U* softFn=Function(pointers,"SoftArg",{{"Class","SoftObjectProperty",40}});
    U* softField=ue::Get<U*>(softFn+ue::Layout::StructChildProps);U* softType=ue::Get<U*>(softField+ue::Layout::FieldClass);
    Put(softType,ue::Layout::FieldClassName,Name("SoftClassProperty"));
    Fn softCall;softCall.Bind(Cdo(pointers),pointers,"SoftArg",{{"Class","SoftObjectProperty",40}});
    Check(softCall.offsets.size()==1,"raw SoftClassProperty alias accepted without accepting other sizes");
    U* enumFn=Function(pointers,"EnumArg",{{"Mode","EnumProperty",1,false,"ESpawnActorCollisionHandlingMethod"}});
    Fn enumCall;enumCall.Bind(Cdo(pointers),pointers,"EnumArg",{{"Mode","EnumProperty",1,false,"ESpawnActorCollisionHandlingMethod"}});
    Check(enumCall.offsets.size()==1,"raw ByteProperty enum with exact enum name accepted");
    U* enumField=ue::Get<U*>(enumFn+ue::Layout::StructChildProps);Put(enumField,0x70,classes.at("Rotator"));
    Throws([&]{Fn bad;bad.Bind(Cdo(pointers),pointers,"EnumArg",{{"Mode","EnumProperty",1,false,"ESpawnActorCollisionHandlingMethod"}});},"same-byte wrong enum rejected");
    (void)vector;
}

void OwnershipTests() {
    U* actorClass = Class("Actor"); U* pawnClass = Class("Pawn", actorClass);
    U* controllerClass = Class("AIController", actorClass); U* worldClass = Class("World");
    U* levelClass = Class("Level"); U* world = Object("LiveWorld", worldClass);
    U* level = Object("PersistentLevel", levelClass); props[{levelClass,"OwningWorld"}] = 0x200; Put(level,0x200,world);
    props[{actorClass,"Owner"}] = 0x200; props[{pawnClass,"Controller"}] = 0x208; props[{controllerClass,"Pawn"}] = 0x208;
    U* validClass = Class("KismetSystemLibrary"); U* validFn = Function(validClass,"IsValid",{{"Object","ObjectProperty",8},{"ReturnValue","BoolProperty",1,true}});
    std::set<U*> dead;
    calls[validFn] = [validFn,&dead](U*,void* p) { Return(validFn,p,"ReturnValue",uint8_t(!dead.contains(Arg<U*>(validFn,p,"Object")))); };
    U* destroyFn = Function(actorClass,"K2_DestroyActor",{});
    calls[destroyFn] = [&dead](U* actor,void*) { ++destroyed; dead.insert(actor); };
    Engine e; e.BindCleanup();
    U* natural = Object("NaturalEnemy",pawnClass,level);
    U* spawn = Object("ToolEnemy_1",pawnClass,level); U* ai = Object("ToolController_1",controllerClass,level);
    Put(spawn,0x208,ai); Put(ai,0x200,spawn); Put(ai,0x208,spawn);
    g_owned = {{Identity(spawn),{},Identity(world),"allowed",{}}};
    Check(e.Clear() == 0 && destroyed == 2 && !dead.contains(natural), "clear deletes owned pawn and its controller only");
    spawn = Object("ToolEnemy_2",pawnClass,level); ai = Object("LevelController",controllerClass,level);
    Put(spawn,0x208,ai); Put(ai,0x208,spawn); Put(ai,0x200,natural);
    g_owned = {{Identity(spawn),{},Identity(world),"allowed",{}}}; e.Clear();
    Check(dead.contains(spawn) && !dead.contains(ai), "borrowed level controller preserved");
    spawn = Object("ToolEnemy_3",pawnClass,level); ai = Object("ToolController_3",controllerClass,level);
    Put(spawn,0x208,ai); Put(ai,0x200,spawn); Put(ai,0x208,natural);
    g_owned = {{Identity(spawn),Identity(ai),Identity(world),"allowed",{}}}; e.Clear();
    Check(!dead.contains(ai), "controller repossessed to natural pawn preserved");
    U* recycled = Object("OldOwned",pawnClass,level); Owned old{Identity(recycled),{},Identity(world),"old",{}};
    Put(recycled,ue::Layout::ObjName,Name("NewNatural")); g_owned = {old}; const int before = destroyed; e.Clear();
    Check(destroyed == before, "recycled object never destroyed");
    // K2_DestroyActor may defer destruction: leave journal and fail cleanup.
    calls[destroyFn] = [](U*,void*) {};
    spawn = Object("PendingDestroy",pawnClass,level); g_owned = {{Identity(spawn),{},Identity(world),"allowed",{}}};
    Check(e.Clear() == 1, "failed/deferred cleanup retains journal");
    calls[destroyFn] = [&dead](U* a,void*) { dead.insert(a); };
    Check(e.Clear() == 0, "later cleanup retry succeeds");
    calls.clear(); // remove callbacks capturing local storage
}

void NativeLevelSourceTests() {
    U* wc=Class("World"), *world=Object("NativeSourceWorld",wc), *other=Object("OtherSourceWorld",wc);
    U* actor=Class("Actor"), *spawnerClass=Class("AISpawner",actor), *gc=Class("MobGroup"), *dc=Class("NativeDomain",actor);
    U* player=Object("SourcePlayer",actor,world), *domain=Object("Domain",dc,world);
    U* exact=Object("OriginalBossSpawner",spawnerClass,world), *fallback=Object("NearbySpawner",spawnerClass,world);
    U* wrongWorld=Object("WrongWorldBossSpawner",spawnerClass,other);
    U* eg=Object("ExactGroup",gc,world), *fg=Object("FallbackGroup",gc,world), *wg=Object("OtherWorldGroup",gc,other);
    props[{spawnerClass,"MobGroup"}]=0x200;props[{gc,"MobDomain"}]=0x200;
    Put(exact,0x200,eg);Put(fallback,0x200,fg);Put(wrongWorld,0x200,wg);Put(eg,0x200,domain);Put(fg,0x200,domain);
    auto* info=Function(spawnerClass,"GetClassLoadConfigAndPolicy",{{"IsLoadSync","BoolProperty",1},{"OutSoftClass","SoftObjectProperty",40}});
    const std::string approvedPath(kEnemyCatalog[0].path);const auto dot=approvedPath.rfind('.');
    bool approved=true;
    calls[info]=[&](U* o,void* b){std::array<uint8_t,40> ref{};uint64_t package=Name(o==fallback&&approved?approvedPath.substr(0,dot):"/Boss/Original"),asset=Name(o==fallback?(approved?approvedPath.substr(dot+1):"Other_C"):"Boss_C");std::memcpy(ref.data()+8,&package,8);std::memcpy(ref.data()+16,&asset,8);Return(info,b,"OutSoftClass",ref);};
    auto* contains=Function(dc,"IsActorInMobDomain",{{"ActorToCheck","ObjectProperty",8},{"ReturnValue","BoolProperty",1,true}});
    bool inside=true;calls[contains]=[&,contains](U*,void* b){Check(Arg<U*>(contains,b,"ActorToCheck")==player,"domain check uses the actual player");Return(contains,b,"ReturnValue",uint8_t(inside));};
    auto* loc=Function(actor,"K2_GetActorLocation",{{"ReturnValue","StructProperty",24,true,"Vector"}});
    bool sourceDistant=false;calls[loc]=[&](U* o,void* b){Return(loc,b,"ReturnValue",Vec{sourceDistant?20000.:o==exact?10000.:10.,0,0});};
    Engine e;e.world=Identity(world);e.player=Identity(player);e.loc.Bind(Cdo(actor),actor,"K2_GetActorLocation",{{"ReturnValue","StructProperty",24,true,"Vector"}});
    Check(e.NativeLevelGroup("/Boss/Original.Boss_C").object==eg,"same-class native encounter beats a closer unrelated group without live monsters");
    Check(e.NativeLevelGroup("/boss/original.boss_c").object==eg,"UE FName path case differences do not lose exact native encounter matching");
    Check(e.NativeLevelGroup("/Boss/Unloaded.Boss_C").object==fg,"without original encounter use a real group containing the player");
    inside=false;e.levelGroup={};Check(e.NativeLevelGroup("/Boss/Unloaded.Boss_C").object==fg&&e.levelSource=="nearby_encounter","standing just outside a domain uses nearby authored enemy level metadata");
    sourceDistant=true;e.levelGroup={};Throws([&]{e.NativeLevelGroup("/Boss/Unloaded.Boss_C");},"no nearby native context is explicit, never highest-level or zero fallback");sourceDistant=false;
    approved=false;e.levelGroup={};Throws([&]{e.NativeLevelGroup("/Boss/Unloaded.Boss_C");},"nearby non-catalog story NPC spawners cannot supply a weak fallback level");approved=true;
    e.levelGroup={};Check(e.NativeLevelGroup("/Boss/Original.Boss_C").object==eg,"cleared encounter metadata remains usable with no active domain members");
    props[{spawnerClass,"Class"}]=0x240;
    uint64_t pkg=Name("/Boss/Original"),asset=Name("Boss_C");Put(exact,0x248,pkg);Put(exact,0x250,asset);
    calls[info]=[=](U*,void* b){Return(info,b,"OutSoftClass",std::array<uint8_t,40>{});};
    e.levelGroup={};Check(e.NativeLevelGroup("/Boss/Original.Boss_C").object==eg,"authored soft class survives native policy returning empty after encounter clear");
    calls.clear();
}

void NativeAttributeTests() {
    U* worldClass=Class("World"), *world=Object("StatsWorld",worldClass);
    U* actorClass=Class("BP_RenewalNPC_C"), *groupClass=Class("MobGroup"), *compClass=Class("AttributeComponent"), *basic=Class("BasicAttributeSet");
    U* actor=Object("StatsBoss",actorClass,world), *group=Object("StoryGroup",groupClass,world), *prior=Object("PreviousGroup",groupClass,world);
    U* configClass=Class("PDA_ActorConfig_C"), *config=Object("NativeConfig",configClass);
    U* comp=Object("AttributeComponent",compClass,actor), *set=Object("Attributes",basic,actor);
    props[{actorClass,"AttributeSetBase"}]=0x200;props[{actorClass,"Attribute Component"}]=0x208;
    props[{actorClass,"MobGroup"}]=0x210;props[{actorClass,"Level"}]=0x218;props[{basic,"Health"}]=0x100;
    props[{actorClass,"Actor Config"}]=0x220;Put(actor,0x220,config);
    Put(actor,0x200,set);Put(actor,0x208,comp);Put(actor,0x210,prior);Put(actor,0x218,0.0);Put(set,0x10c,9000.f);Put(group,0x400,5.0);Put(group,0x500,7);
    g_owned={{Identity(actor),{},Identity(world),"stats-path",{}}};
    int initCalls=0,rebuildCalls=0;bool fail=false;
    U* init=Function(actorClass,"Init Level",{}), *rebuild=Function(compClass,"ReinitializeAttributes",{});
    U* get=Function(actorClass,"GetCombatLevel",{{"ReturnValue","FloatProperty",4,true}});
    calls[init]=[&](U* a,void*){++initCalls;Check(ue::Get<U*>(a+0x210)==group,"native Init Level sees only the borrowed context");Put(a,0x218,fail?99.0:ue::Get<double>(group+0x400));if(fail)throw std::runtime_error("native failure");};
    calls[get]=[get](U* a,void* p){Return(get,p,"ReturnValue",float(ue::Get<double>(a+0x218)));};
    calls[rebuild]=[&](U* c,void*){++rebuildCalls;Check(c==comp&&ue::Get<U*>(actor+0x210)==prior,"native attribute rebuild occurs after story association has been restored");for(int i=0;i<5;++i)Put(set,0x300+4*i,float(10+i)*float(ue::Get<double>(actor+0x218)));};
    Check(InitializeNativeStats(Identity(actor),Identity(group))==5,"uses native Blueprint combat level instead of BasicAttributeSet.Level");
    Check(initCalls==1&&rebuildCalls==1&&NativeStatsRevision(actor)==1,"one native level computation and one complete attribute rebuild");
    for(int i=0;i<5;++i)Check(ue::Get<float>(set+0x300+4*i)==float(10+i)*5,"all native curve consumers are rebuilt, including shachi and infliction");
    Check(ue::Get<int>(group+0x500)==7&&ue::Get<U*>(actor+0x210)==prior,"story group state untouched and actor detached");
    Put(actor,0x220,static_cast<U*>(nullptr));Throws([&]{InitializeNativeStats(Identity(actor),Identity(group));},"missing native config never falls back to generic weak attributes");Put(actor,0x220,config);
    fail=true;Throws([&]{InitializeNativeStats(Identity(actor),Identity(group));},"native init failure is propagated for owned spawn cleanup");
    Check(ue::Get<U*>(actor+0x210)==prior&&ue::Get<double>(actor+0x218)==5&&rebuildCalls==1,"failure restores temporary association and prior level");fail=false;
    Put(set,0x10c,0.f);const int n=initCalls;Throws([&]{InitializeNativeStats(Identity(actor),Identity(group));},"dead actors are never reinitialized or revived");Check(initCalls==n,"dead guard runs before any native mutation");Put(set,0x10c,100.f);
    RecheckNativeStats(Identity(actor),Identity(group),g_epoch.load()+1,0,0);Check(initCalls==n,"delayed work is fenced by generation");
    U* anotherWorld=Object("OtherWorld",worldClass), *otherGroup=Object("OtherGroup",groupClass,anotherWorld);
    Throws([&]{InitializeNativeStats(Identity(actor),Identity(otherGroup));},"another world's context cannot set level");
    g_owned.front().tag="tower-test";Throws([&]{InitializeNativeStats(Identity(actor),Identity(group));},"tower's adaptive stats remain owned by tower");
    g_owned.clear();Throws([&]{InitializeNativeStats(Identity(actor),Identity(group));},"natural actors cannot enter initialization path");
    calls.clear();
}

void NativeDeathCleanupTests() {
    U* ac=Class("Actor"), *pc=Class("Pawn",ac), *ai=Class("AIController",ac), *wc=Class("World"), *world=Object("DeathWorld",wc);
    U* nc=Class("DeathNPC",pc), *bc=Class("BasicAttributeSet"), *dc=Class("BP_DeathComponent_C");
    U* actor=Object("DeadOwnedNPC",nc,world), *controller=Object("DeadOwnedController",ai,world), *death=Object("Death",dc,actor), *set=Object("Set",bc,actor);
    props[{ac,"Owner"}]=0x200;props[{pc,"Controller"}]=0x208;props[{ai,"Pawn"}]=0x208;
    props[{nc,"AttributeSetBase"}]=0x210;props[{nc,"Death Component"}]=0x218;props[{nc,"Death Delay"}]=0x220;
    props[{dc,"Fake Death Enabled"}]=0x200;props[{bc,"Health"}]=0x100;
    Put(actor,0x208,controller);Put(controller,0x200,actor);Put(controller,0x208,actor);Put(actor,0x210,set);Put(actor,0x218,death);Put(actor,0x220,0.f);Put(set,0x10c,0.f);
    U* kc=Class("KismetSystemLibrary"),*valid=Function(kc,"IsValid",{{"Object","ObjectProperty",8},{"ReturnValue","BoolProperty",1,true}});
    std::set<U*> gone;bool isDead=false;
    calls[valid]=[&](U*,void* b){Return(valid,b,"ReturnValue",uint8_t(!gone.contains(Arg<U*>(valid,b,"Object"))));};
    U* destroy=Function(ac,"K2_DestroyActor",{});calls[destroy]=[&](U* o,void*){gone.insert(o);};
    U* dead=Function(dc,"Is Dead",{{"ReturnValue","BoolProperty",1,true}});calls[dead]=[&](U*,void* b){Return(dead,b,"ReturnValue",uint8_t(isDead));};
    g_owned={{Identity(actor),Identity(controller),Identity(world),"dead-path",{}}};
    MaintainOwnedAt(1000);MaintainOwnedAt(20000);Check(gone.empty()&&g_owned.front().deadSince==0,"zero health alone never reaps a native phase transition");
    isDead=true;Put(death,0x200,uint8_t(1));MaintainOwnedAt(21000);Check(gone.empty(),"native fake-death bodies remain owned by their phase logic");
    Put(death,0x200,uint8_t(0));MaintainOwnedAt(22000);MaintainOwnedAt(33999);Check(gone.empty(),"native death presentation keeps a full twelve-second grace");
    Put(set,0x10c,100.f);MaintainOwnedAt(34000);Check(g_owned.front().deadSince==0&&gone.empty(),"native revival cancels pending corpse cleanup");
    Put(set,0x10c,0.f);MaintainOwnedAt(35000);MaintainOwnedAt(47000);Check(gone.contains(actor)&&gone.contains(controller)&&g_owned.empty(),"confirmed corpse cleanup destroys only owned body/controller and releases capacity");
    gone.clear();g_owned={{Identity(actor),Identity(controller),Identity(world),"dead-path",{},"","tower-dead"}};MaintainOwnedAt(50000);MaintainOwnedAt(90000);Check(gone.empty(),"tower corpse lifecycle is excluded");
    g_owned.front().tag.clear();gone.insert(actor);MaintainOwnedAt(91000);Check(gone.contains(controller)&&g_owned.empty(),"controller-only remnant is cleaned after native actor EndPlay");
    calls.clear();
}

void QueueTests() {
    std::vector<std::string> replies;
    auto reply = [&](std::string s) { replies.push_back(std::move(s)); };
    ready = false; Submit({"spawn","anything","offline"},reply);
    Check(queue.empty() && replies.back().find("\"ok\":false") != std::string::npos,"not ready never enqueues engine work");
    ready = true; Submit({"invalid","","invalid"},reply); Check(queue.empty(),"unknown command rejected before engine work");
    for (int i = 0; i < 5; ++i) Submit({"status","",std::to_string(i)},reply);
    Check(queue.size() == 4 && g_pending == 4,"queue bounded at four requests");
    const int before = engineCalls; auto stopped = StopOnGameThread(); Drain();
    Check(stopped.ok && engineCalls == before && g_pending == 0,"stop cancels all queued native calls");
    Submit({"spawn","anything","after-stop"},reply); Check(queue.empty(),"stopped session cannot revive from delayed command");
    g_stopping = false;
    // Reflection is resolved only when the queued task runs, never on Submit.
    const int prior = engineCalls; Submit({"status","","post"},reply);
    Check(engineCalls == prior && queue.size() == 1,"IPC-facing Submit is mutation-free");
    ++g_epoch; Drain(); Check(engineCalls == prior,"epoch fence rejects stale work");
    Request escaped{"status","","quote\"\n"};
    auto json = Response(escaped,false,"x\ny");
    Check(json.find("quote\\\"\\u000a") != std::string::npos,"request IDs escaped as JSON");
}

void NativeDeathRecoveryTests() {
    U* ac=Class("Actor"), *pc=Class("Pawn",ac), *wc=Class("World"), *world=Object("RecoveryWorld",wc);
    U* nc=Class("RecoveryNPC",pc), *bc=Class("BasicAttributeSet"), *dc=Class("BP_DeathComponent_C"), *sc=Class("ActorStateComponent");
    U* actor=Object("OwnedRecoveryNPC",nc,world), *death=Object("Death",dc,actor), *state=Object("State",sc,actor), *set=Object("Set",bc,actor);
    props[{pc,"Controller"}]=0x208;props[{nc,"AttributeSetBase"}]=0x210;props[{nc,"Death Component"}]=0x218;props[{nc,"Death Delay"}]=0x220;
    props[{dc,"Fake Death Enabled"}]=0x200;props[{dc,"Actor State Component"}]=0x208;props[{dc,"Dead State Tag"}]=0x210;
    props[{bc,"Health"}]=0x100;props[{bc,"Health_Min"}]=0x110;
    Put(actor,0x210,set);Put(actor,0x218,death);Put(death,0x208,state);const uint64_t deathTag=Name("CharacterState.Dead");Put(death,0x210,deathTag);
    U* kc=Class("KismetSystemLibrary"), *valid=Function(kc,"IsValid",{{"Object","ObjectProperty",8},{"ReturnValue","BoolProperty",1,true}});
    std::set<U*> gone; bool deadState=false,pendingState=false;int additions=0;
    calls[valid]=[&](U*,void* b){Return(valid,b,"ReturnValue",uint8_t(!gone.contains(Arg<U*>(valid,b,"Object"))));};
    U* destroy=Function(ac,"K2_DestroyActor",{});calls[destroy]=[&](U* o,void*){gone.insert(o);};
    U* dead=Function(dc,"Is Dead",{{"ReturnValue","BoolProperty",1,true}});calls[dead]=[&](U*,void* b){Return(dead,b,"ReturnValue",uint8_t(deadState));};
    U* readyFn=Function(dc,"Is Ready To Dead",{{"ReturnValue","BoolProperty",1,true}});calls[readyFn]=[&](U*,void* b){Return(readyFn,b,"ReturnValue",uint8_t(deadState||pendingState));};
    U* add=Function(sc,"AddPendingState",{{"NewStateTag","StructProperty",8,false,"GameplayTag"}});
    calls[add]=[&](U* o,void* b){Check(o==state&&Arg<uint64_t>(add,b,"NewStateTag")==deathTag,"native pending request targets the actor's own state and configured dead tag");++additions;pendingState=true;};
    g_owned={{Identity(actor),{},Identity(world),"recovery",{}}};
    // This path uses only the generator's maintenance; no faction-bars feature
    // is registered/enabled, and default enemy faction is intentionally empty.
    MaintainOwnedAt(1000);MaintainOwnedAt(1999);Check(!additions&&gone.empty(),"real zero health remains native during its initial callback grace");
    MaintainOwnedAt(2000);MaintainOwnedAt(15000);Check(additions==1&&gone.empty(),"missed zero-health event queues once and waits for native transition without forcing death");
    Check(g_owned.front().deathQueued&&OwnedStatsJson().find("\"death_recovery_queued\":true")!=std::string::npos,"status exposes a recovery request without claiming completed death");
    Put(set,0x10c,5.f);MaintainOwnedAt(16000);Check(!g_owned.front().zeroSince&&!g_owned.front().deathQueued,"native healing cancels recovery tracking");
    Put(set,0x10c,0.f);pendingState=true;MaintainOwnedAt(17000);MaintainOwnedAt(19000);Check(additions==1,"already-pending native transition is never duplicated");
    pendingState=false;
    for(float hp:{0.01f,0.1f,0.5f,-1.f,std::numeric_limits<float>::quiet_NaN()}) {
        Put(set,0x10c,hp);MaintainOwnedAt(20000);MaintainOwnedAt(24000);
        Check(additions==1&&gone.empty(),"positive/native lock-health, underflow or unreadable health is never forced dead");
    }
    Put(set,0x10c,0.f);Put(set,0x11c,1.f);MaintainOwnedAt(25000);MaintainOwnedAt(27000);Check(additions==1,"positive native Health_Min blocks a synthetic death request");Put(set,0x11c,0.f);
    Put(death,0x200,uint8_t(1));MaintainOwnedAt(28000);MaintainOwnedAt(30000);Check(additions==1,"fake-death and phase-enabled actors remain native");Put(death,0x200,uint8_t(0));
    Put(state,ue::Layout::ObjOuter,world);MaintainOwnedAt(31000);MaintainOwnedAt(33000);Check(additions==1,"foreign state component cannot receive a death request");Put(state,ue::Layout::ObjOuter,actor);
    Put(death,0x210,uint64_t(0));MaintainOwnedAt(34000);MaintainOwnedAt(36000);Check(additions==1,"uninitialized dead-state tag is not replaced with a guessed tag");Put(death,0x210,deathTag);
    g_owned.front().tag="tower-death";MaintainOwnedAt(37000);MaintainOwnedAt(39000);Check(additions==1,"tower-managed death remains excluded");g_owned.front().tag.clear();
    deadState=true;MaintainOwnedAt(40000);MaintainOwnedAt(51999);Check(gone.empty(),"confirmed death still keeps native presentation grace after recovery");
    MaintainOwnedAt(52000);Check(gone.contains(actor)&&g_owned.empty(),"native confirmation eventually retires the owned corpse");
    calls.clear();
}

void NativeSpawnTests() {
    U* actor = Class("Actor"), *pawn = Class("Pawn",actor), *npc = Class("PBZCharacter_NPC",pawn);
    U* playerClass = Class("PBZCharacter_Player",pawn), *aiClass = Class("AIController",actor);
    U* capsule = Class("CapsuleComponent"), *worldClass = Class("World"), *levelClass = Class("Level");
    U* world = Object("SpawnWorld",worldClass), *level = Object("SpawnLevel",levelClass);
    props[{levelClass,"OwningWorld"}] = 0x200; Put(level,0x200,world);
    props[{actor,"Owner"}] = 0x220; props[{aiClass,"Pawn"}] = 0x228;
    props[{pawn,"Controller"}] = 0x230;
    U* viewport = Class("GameViewportClient"); props[{viewport,"World"}] = 0x200;
    U* viewportInstance = Object("Viewport",viewport); Put(viewportInstance,0x200,world);
    U* player = Object("Player",playerClass,level), *natural = Object("NaturalEnemy",npc,level);
    U* target = Class("BP_Monster_TestTarget_C",npc); Class("Transform"); Class("HitResult"); Class("LinearColor");
    U* kismet = Class("KismetSystemLibrary"), *gameplay = Class("GameplayStatics"), *aifunc = Class("BP_SGameAIFunctionLibrary_C");
    std::set<U*> dead; std::map<U*,Vec> positions; positions[player] = {0,0,90};
    std::map<U*,Capsule> sizes; bool blocked = false, failFinish = false, failAI = false;
    int begins = 0, activations = 0; uint8_t collision = 0, scaleMode = 0;
    U* fn = Function(kismet,"IsValid",{{"Object","ObjectProperty",8},{"ReturnValue","BoolProperty",1,true}});
    calls[fn] = [&,fn](U*,void* p){Return(fn,p,"ReturnValue",uint8_t(!dead.contains(Arg<U*>(fn,p,"Object"))));};
    fn = Function(actor,"K2_DestroyActor",{}); calls[fn] = [&](U* obj,void*){dead.insert(obj);};
    fn = Function(actor,"K2_GetActorLocation",{{"ReturnValue","StructProperty",24,true,"Vector"}});
    calls[fn] = [&,fn](U* obj,void* p){Return(fn,p,"ReturnValue",positions[obj]);};
    fn = Function(gameplay,"IsGamePaused",{{"WorldContextObject","ObjectProperty",8},{"ReturnValue","BoolProperty",1,true}});
    calls[fn] = [fn](U*,void* p){Return(fn,p,"ReturnValue",uint8_t(0));};
    fn = Function(gameplay,"GetPlayerPawn",{{"WorldContextObject","ObjectProperty",8},{"PlayerIndex","IntProperty",4},{"ReturnValue","ObjectProperty",8,true}});
    calls[fn] = [&,fn](U*,void* p){Return(fn,p,"ReturnValue",player);};
    fn = Function(capsule,"GetScaledCapsuleSize",{{"OutRadius","FloatProperty",4},{"OutHalfHeight","FloatProperty",4}});
    calls[fn] = [&,fn](U* obj,void* p){Return(fn,p,"OutRadius",float(sizes[obj].radius));Return(fn,p,"OutHalfHeight",float(sizes[obj].halfHeight));};
    fn = Function(kismet,"CapsuleTraceSingleForObjects", {
        {"WorldContextObject","ObjectProperty",8},{"Start","StructProperty",24,false,"Vector"},{"End","StructProperty",24,false,"Vector"},
        {"Radius","FloatProperty",4},{"HalfHeight","FloatProperty",4},{"ObjectTypes","ArrayProperty",16},{"bTraceComplex","BoolProperty",1},
        {"ActorsToIgnore","ArrayProperty",16},{"DrawDebugType","EnumProperty",1},{"OutHit","StructProperty",256,false,"HitResult"},
        {"bIgnoreSelf","BoolProperty",1},{"TraceColor","StructProperty",16,false,"LinearColor"},{"TraceHitColor","StructProperty",16,false,"LinearColor"},
        {"DrawTime","FloatProperty",4},{"ReturnValue","BoolProperty",1,true}});
    calls[fn] = [&,fn](U*,void* p){Return(fn,p,"ReturnValue",uint8_t(blocked));};
    fn = Function(gameplay,"BeginDeferredActorSpawnFromClass", {
        {"WorldContextObject","ObjectProperty",8},{"ActorClass","ObjectProperty",8},{"SpawnTransform","StructProperty",96,false,"Transform"},
        {"CollisionHandlingOverride","EnumProperty",1},{"Owner","ObjectProperty",8},{"TransformScaleMethod","EnumProperty",1},{"ReturnValue","ObjectProperty",8,true}});
    calls[fn] = [&,fn](U*,void* p){
        ++begins; collision=Arg<uint8_t>(fn,p,"CollisionHandlingOverride");scaleMode=Arg<uint8_t>(fn,p,"TransformScaleMethod");
        U* obj=Object("Owned_"+std::to_string(begins),target,level);U* component=Object("Capsule_"+std::to_string(begins),capsule,obj);
        Put(obj,0x238,component);sizes[component]={50,100};positions[obj]=Arg<Transform>(fn,p,"SpawnTransform").translation;
        Return(fn,p,"ReturnValue",obj);
    };
    fn = Function(gameplay,"FinishSpawningActor",{{"Actor","ObjectProperty",8},{"SpawnTransform","StructProperty",96,false,"Transform"},{"TransformScaleMethod","EnumProperty",1},{"ReturnValue","ObjectProperty",8,true}});
    calls[fn] = [&,fn](U*,void* p){U* obj=Arg<U*>(fn,p,"Actor");U* ai=Object("OwnedAI_"+std::to_string(begins),aiClass,level);
        Put(ai,0x220,obj);Put(ai,0x228,obj);Put(obj,0x230,ai);Return(fn,p,"ReturnValue",failFinish?static_cast<U*>(nullptr):obj);};
    fn = Function(pawn,"SpawnDefaultController",{}); calls[fn]=[](U*,void*){};
    fn = Function(aifunc,"ActivateAI",{{"Pawn","ObjectProperty",8},{"__WorldContext","ObjectProperty",8}});
    calls[fn] = [&](U*,void*){++activations;if(failAI)throw std::runtime_error("native AI failure");};
    Engine e; e.BindCleanup(); e.world=Identity(world);e.player=Identity(player);e.location={0,0,90};e.playerSize={35,90};e.capsuleOffset=0x238;e.capsuleClass=capsule;
    e.loc.Bind(Cdo(actor),actor,"K2_GetActorLocation",{{"ReturnValue","StructProperty",24,true,"Vector"}});
    e.capsuleSize.Bind(Cdo(capsule),capsule,"GetScaledCapsuleSize",{{"OutRadius","FloatProperty",4},{"OutHalfHeight","FloatProperty",4}});
    Library(e.paused,"GameplayStatics","IsGamePaused",{{"WorldContextObject","ObjectProperty",8},{"ReturnValue","BoolProperty",1,true}});
    Library(e.playerPawn,"GameplayStatics","GetPlayerPawn",{{"WorldContextObject","ObjectProperty",8},{"PlayerIndex","IntProperty",4},{"ReturnValue","ObjectProperty",8,true}});
    Library(e.room,"KismetSystemLibrary","CapsuleTraceSingleForObjects", {
        {"WorldContextObject","ObjectProperty",8},{"Start","StructProperty",24,false,"Vector"},{"End","StructProperty",24,false,"Vector"},
        {"Radius","FloatProperty",4},{"HalfHeight","FloatProperty",4},{"ObjectTypes","ArrayProperty",16},{"bTraceComplex","BoolProperty",1},
        {"ActorsToIgnore","ArrayProperty",16},{"DrawDebugType","EnumProperty",1},{"OutHit","StructProperty",256,false,"HitResult"},
        {"bIgnoreSelf","BoolProperty",1},{"TraceColor","StructProperty",16,false,"LinearColor"},{"TraceHitColor","StructProperty",16,false,"LinearColor"},
        {"DrawTime","FloatProperty",4},{"ReturnValue","BoolProperty",1,true}});
    Library(e.begin,"GameplayStatics","BeginDeferredActorSpawnFromClass", {
        {"WorldContextObject","ObjectProperty",8},{"ActorClass","ObjectProperty",8},{"SpawnTransform","StructProperty",96,false,"Transform"},
        {"CollisionHandlingOverride","EnumProperty",1},{"Owner","ObjectProperty",8},{"TransformScaleMethod","EnumProperty",1},{"ReturnValue","ObjectProperty",8,true}});
    Library(e.finish,"GameplayStatics","FinishSpawningActor",{{"Actor","ObjectProperty",8},{"SpawnTransform","StructProperty",96,false,"Transform"},{"TransformScaleMethod","EnumProperty",1},{"ReturnValue","ObjectProperty",8,true}});
    e.controllerSpawn.Bind(Cdo(pawn),pawn,"SpawnDefaultController",{});
    Library(e.activate,"BP_SGameAIFunctionLibrary_C","ActivateAI",{{"Pawn","ObjectProperty",8},{"__WorldContext","ObjectProperty",8}});
    g_owned.clear();Landing point{{600,0,105},{50,100},false};e.Spawn(target,"exact",point);
    Check(g_owned.size()==1 && activations==1,"native deferred spawn then AI initialization");
    Check(collision==3 && scaleMode==1,"AdjustIfPossibleButDontSpawnIfColliding (cfe67a1) and authored root scale passed to native call");
    Check(g_owned[0].landing.center.z==105,"actual spawned pose recorded");
    Check(e.Clear()==0 && !dead.contains(natural),"created native pawn/controller cleanup preserves natural enemy");
    for (int i = 0; i < 64; ++i) {
        auto next = point; next.center.x += i * 200;
        e.Spawn(target,"exact",next);
    }
    Check(g_owned.size()==64 && OwnedActors().size()==64,"sixty-four native spawns remain owned across vector reallocations");
    const auto stats = OwnedStatsJson(); size_t rows = 0;
    for (size_t pos = 0; (pos = stats.find("\"stats_revision\"",pos)) != std::string::npos; ++pos) ++rows;
    Check(rows==64,"status reports every owned spawn beyond sixteen");
    g_requests.clear();
    for (int i = 0; i < 1100; ++i) ClaimSpawnRequest({"spawn","exact","unlimited-"+std::to_string(i),8});
    Check(g_requests.size()==1100,"admission has neither owned-total nor 1024-request ceiling");
    Throws([&]{ClaimSpawnRequest({"spawn","exact","unlimited-0",8});},"old request stays rejected after more than 1024 requests");
    Throws([&]{ClaimSpawnRequest({"spawn","exact","invalid-batch",9});},"per-request game-thread work remains bounded");
    Throws([&]{ClaimSpawnRequest({"spawn","exact","",1});},"unlimited admission still requires a unique request id");
    const auto old = g_owned.front().landing;
    positions[g_owned.front().actor.object] = {10000,200,105};
    const auto reservations = e.Reservations();
    Check(reservations.size()==64 && reservations.front().center.x==10000,"landing reservations follow moved actors instead of stale spawn positions");
    Check(g_owned.front().landing.center.x==old.center.x,"querying current positions does not rewrite the ownership journal");
    dead.insert(g_owned.front().actor.object);
    Check(e.Reservations().size()==63,"controller-only remnant does not reserve an empty landing position");
    const auto retained = g_owned;
    Check(e.Clear()==0 && g_owned.empty(),"all sixty-four spawns and orphan controllers can be cleared");
    Check(std::all_of(retained.begin(),retained.end(),[&](const Owned& own){return dead.contains(own.actor.object)&&dead.contains(own.controller.object);}),"unlimited cleanup destroys every owned pawn and its own controller");
    Check(!dead.contains(natural) && !dead.contains(player),"large-session cleanup preserves natural enemy and player");
    g_requests.clear();
    blocked=true;int before=begins;Throws([&]{e.Spawn(target,"exact",point);},"last-moment collision refuses spawn");
    Check(begins==before,"collision failure never calls native Begin");blocked=false;
    failFinish=true;Throws([&]{e.Spawn(target,"exact",point);},"failed Finish reports failure");
    Check(g_owned.empty(),"Finish failure rolls back owned pawn and its AI controller");failFinish=false;
    failAI=true;Throws([&]{e.Spawn(target,"exact",point);},"AI failure reports failure");
    Check(g_owned.empty(),"AI failure rolls back owned pawn and controller");failAI=false;
    U* otherWorld=Object("OtherWorld",worldClass);Put(viewportInstance,0x200,otherWorld);before=begins;
    Throws([&]{e.Spawn(target,"exact",point);},"active viewport change cancels stale context");
    Check(begins==before,"world change makes no native spawn call");Put(viewportInstance,0x200,world);
    positions[player]={1000,0,90};before=begins;Throws([&]{e.Spawn(target,"exact",point);},"player movement cancels prepared points");
    Check(begins==before,"player movement makes no native spawn call");
    calls.clear();
}

void StandalonePhaseTests() {
    Check(StandalonePath(kLeiDang,"ui-one")==kLeiDangFinal && StandalonePath(kLeiDangJyx,"ui-two")==kLeiDangFinal,"both locked LeiDang variants select the native final-phase class");
    Check(StandalonePath(kChiLian,"ui-three")==kChiLianFinal,"story-led first snake phase can start at its native second phase");
    Check(StandalonePath(kLeiDangFinal,"ui-four")==kLeiDangFinal,"explicit final-phase request is idempotent");
    Check(StandalonePath(kLeiDang,"tower-1")==kLeiDang && StandalonePath(kChiLian,"tower-2")==kChiLian,"tower retains its own stage selection");
    Check(StandalonePath("BP_Monster_LeiDang_C","ui-short")=="BP_Monster_LeiDang_C","short names and unreviewed phase guesses are not rewritten");
    Check(StandaloneRuleFor("/Other/Boss.Boss_C").effect.empty(),"unknown boss locks are never removed by a substring rule");
    Check(StandaloneRuleFor(kLeiDangFinal).effect.empty(),"final LeiDang needs no artificial stat or effect override");
    for (const auto& entry : kEnemyCatalog) {
        const auto selected=StandalonePath(entry.path,"ui-validated");
        Check(std::any_of(std::begin(kEnemyCatalog),std::end(kEnemyCatalog),[&](const auto& e){return e.path==selected;}),"every mapped phase is itself a complete reviewed catalog identity");
    }
    auto makeClass=[](std::string_view path) {const auto dot=path.rfind('.');U* c=Class(std::string(path.substr(dot+1)));Put(c,ue::Layout::ObjOuter,Object(std::string(path.substr(0,dot))));return c;};
    const auto rule=StandaloneRuleFor(kChiLianFinal);
    U* actorClass=makeClass(kChiLianFinal), *ascClass=Class("AbilitySystemComponent"), *wc=Class("World"), *world=Object("CompatWorld",wc);
    U* actor=Object("ToolSnake",actorClass,world), *asc=Object("ASC",ascClass,actor);
    const auto dot=rule.config.rfind('.');U* config=Object(std::string(rule.config.substr(dot+1)),Class("Config"),Object(std::string(rule.config.substr(0,dot))));
    U* effect=makeClass(rule.effect), *cinematic=makeClass(rule.cinematicAbility), *ability=Object("StoryPassiveCDO",cinematic);
    props[{actorClass,"Actor Config"}]=0x200;props[{actorClass,"AbilitySystemComponent"}]=0x208;
    props[{ascClass,"AvatarActor"}]=0x200;props[{ascClass,"ActivatableAbilities"}]=0x300;
    Put(actor,0x200,config);Put(actor,0x208,asc);Put(asc,0x200,actor);
    U* container=Class("GameplayAbilitySpecContainer"), *spec=Class("GameplayAbilitySpec");
    Put(container,ue::Layout::StructSize,0x120);props[{container,"Items"}]=0x108;
    Put(spec,ue::Layout::StructSize,0xF0);props[{spec,"Handle"}]=0xC;props[{spec,"Ability"}]=0x10;
    std::array<uint8_t,0xF0> grant{};Put(grant.data(),0xC,int32_t(123));Put(grant.data(),0x10,ability);
    Put(asc,0x408,Array{grant.data(),1,1});
    int clears=0,removes=0,stacks=1;
    U* count=Function(ascClass,"GetGameplayEffectCount",{{"SourceGameplayEffect","ObjectProperty",8},{"OptionalInstigatorFilterComponent","ObjectProperty",8},{"bEnforceOnGoingCheck","BoolProperty",1},{"ReturnValue","IntProperty",4,true}});
    calls[count]=[&](U* receiver,void* b){Check(receiver==asc&&Arg<U*>(count,b,"SourceGameplayEffect")==effect&&Arg<U*>(count,b,"OptionalInstigatorFilterComponent")==nullptr,"count filter is the exact reviewed effect on this spawn only");Return(count,b,"ReturnValue",int32_t(stacks));};
    U* remove=Function(ascClass,"RemoveActiveGameplayEffectBySourceEffect",{{"GameplayEffect","ObjectProperty",8},{"InstigatorAbilitySystemComponent","ObjectProperty",8},{"StacksToRemove","IntProperty",4}});
    calls[remove]=[&](U* receiver,void* b){Check(receiver==asc&&Arg<U*>(remove,b,"GameplayEffect")==effect&&Arg<int32_t>(remove,b,"StacksToRemove")==-1,"native removal affects all stacks of only the story lock");Check(clears==1,"player-cinematic passive is retired before removing the story lock");++removes;stacks=0;};
    U* clear=Function(ascClass,"ClearAbility",{{"Handle","StructProperty",4,false,"GameplayAbilitySpecHandle"}});
    calls[clear]=[&](U* receiver,void* b){Check(receiver==asc&&Arg<int32_t>(clear,b,"Handle")==123,"native ClearAbility removes only the identified cinematic spec");++clears;Put(asc,0x408,Array{});};
    g_owned={{Identity(actor),{},Identity(world),std::string(kChiLianFinal),{}}};
    AdaptStandalone(g_owned.front());
    Check(removes==1&&clears==1&&g_owned.front().storyLocksRemoved==1,"standalone final phase sheds its external ending restriction through native consumers");
    AdaptStandalone(g_owned.front());Check(removes==1&&clears==1,"repeated maintenance is idempotent after native removal");
    g_owned.front().tag="tower-snake";stacks=1;AdaptStandalone(g_owned.front());Check(removes==1,"tower instance keeps its native lock and story control");g_owned.front().tag.clear();
    Put(asc,0x200,world);Throws([&]{AdaptStandalone(g_owned.front());},"foreign Avatar cannot receive compatibility mutations");Put(asc,0x200,actor);
    Put(asc,ue::Layout::ObjOuter,world);Throws([&]{AdaptStandalone(g_owned.front());},"borrowed ASC cannot receive compatibility mutations");Put(asc,ue::Layout::ObjOuter,actor);
    Put(actor,0x200,world);Throws([&]{AdaptStandalone(g_owned.front());},"changed actor config cannot inherit a rule by short name");Put(actor,0x200,config);
    Put(asc,0x408,Array{grant.data(),1,1});calls[clear]=[](U*,void*){};
    Throws([&]{AdaptStandalone(g_owned.front());},"silent native ClearAbility failure is detected before unlocking an ending-only body");
    Check(removes==1,"failed cinematic removal never exposes an unsafe unlocked final boss");Put(asc,0x408,Array{});
    Owned foreign=g_owned.front();Throws([&]{AdaptStandalone(foreign);},"unregistered/natural actors cannot run standalone adaptation");
    // Non-boss independent bodies use a separate exact lock; berserk, shared
    // pools and summon-boss shield effects are absent from these rules.
    Check(!StandaloneRuleFor(kChild).effect.empty()&&!StandaloneRuleFor(kYellowSummon).effect.empty(),"independently requested puppet bodies have explicit own-instance lock rules");
    Check(StandaloneRuleFor("/PBZCombatAsset/ActorData/Mobs/044_PengMeng/BP_Monster_PengMeng_Axe.BP_Monster_PengMeng_Axe_C").effect.empty(),"PengMeng berserk lock is preserved as a combat mechanic");
    Check(!StandaloneRuleFor(kLengTu).effect.empty(),"independent LengTu has an explicit terminal body-stage rule");
    g_owned.clear();calls.clear();
}

void ArenaTests() {
    g_stopping=false;RemoveArenaHooks();
    auto makeClass=[](std::string_view path,U* super=nullptr) {const auto dot=path.rfind('.');U* c=Class(std::string(path.substr(dot+1)),super);Put(c,ue::Layout::ObjOuter,Object(std::string(path.substr(0,dot))));return c;};
    U* actorClass=Class("Actor"), *wc=Class("World"), *world=Object("ArenaWorld",wc);
    U* bossClass=makeClass(kLengTu,actorClass), *boss=Object("IndependentLengTu",bossClass,world), *natural=Object("StoryLengTu",bossClass,world);
    U* motionClass=Class("MotionWarpingComponent"), *motion=Object("OwnMotion",motionClass,boss);
    U* ga=Class("GameplayAbility"), *base=Class("BP_ActionAbilityBase_Simple_C",ga), *derived=makeClass(kLengTuArenaAbilities[0],base);
    U* ability=Object("OwnArenaAbility",derived,boss), *naturalAbility=Object("StoryArenaAbility",derived,natural);
    U* avatar=Function(ga,"GetAvatarActorFromActorInfo",{{"ReturnValue","ObjectProperty",8,true}});
    calls[avatar]=[&](U* o,void* b){Return(avatar,b,"ReturnValue",o==naturalAbility?natural:boss);};
    Vec current{12000,14000,100},rotation{0,90,0},target{};int roots=0,parents=0,ended=0,raw=0;
    U* loc=Function(actorClass,"K2_GetActorLocation",{{"ReturnValue","StructProperty",24,true,"Vector"}});calls[loc]=[&](U*,void* b){Return(loc,b,"ReturnValue",current);};
    U* rot=Function(actorClass,"K2_GetActorRotation",{{"ReturnValue","StructProperty",24,true,"Rotator"}});calls[rot]=[&](U*,void* b){Return(rot,b,"ReturnValue",rotation);};
    U* warp=Function(motionClass,"AddOrUpdateWarpTargetFromLocationAndRotation",{{"WarpTargetName","NameProperty",8},{"TargetLocation","StructProperty",24,false,"Vector"},{"TargetRotation","StructProperty",24,false,"Rotator"}});
    calls[warp]=[&](U* o,void* b){Check(o==motion&&Arg<uint64_t>(warp,b,"WarpTargetName")==inv::ToFName("SnapPoint"),"only this spawn's native SnapPoint warp target is updated");target=Arg<Vec>(warp,b,"TargetLocation");++roots;};
    U* parent=Function(base,"K2_ActivateAbility",{});calls[parent]=[&](U* o,void*){Check(o==ability&&roots==parents+1,"native parent timeline starts after the local warp target is prepared");++parents;};
    U* entry=Function(derived,"K2_ActivateAbility",{});calls[entry]=[&](U*,void*){++raw;};
    U* end=Function(ga,"K2_EndAbility",{});calls[end]=[&](U* o,void*){Check(o==ability,"only the owned failed ability is ended");++ended;};
    g_owned={{Identity(boss),{},Identity(world),std::string(kLengTu),{}}};
    BindArenaAbility(derived);Check(interceptions.size()==1,"exact arena ability gets a single scoped interceptor");BindArenaAbility(derived);Check(interceptions.size()==1,"repeat spawn does not stack duplicate hooks");
    Dispatch(ability,entry,nullptr);
    Check(parents==1&&raw==0&&ended==0&&target.x==current.x&&target.y==current.y&&g_owned.front().arenaRedirects==1,"missing level marker cannot redirect a standalone boss away from its local scene");
    current={20000,25000,100};Dispatch(ability,entry,nullptr);Check(target.x==20000&&parents==2,"subsequent phase uses the actor's current position rather than an old/world-zero anchor");
    Dispatch(naturalAbility,entry,nullptr);Check(raw==1&&parents==2,"natural story boss runs the original derived blueprint unchanged");
    g_owned.front().tag="tower-boss";Dispatch(ability,entry,nullptr);Check(raw==2&&parents==2,"tower encounter retains authored arena control");g_owned.front().tag.clear();
    Put(motion,ue::Layout::ObjOuter,natural);Dispatch(ability,entry,nullptr);Check(ended==1&&parents==2&&raw==2,"missing own motion component ends the action without borrowing a natural actor's component");Put(motion,ue::Layout::ObjOuter,boss);
    current.x=NAN;Dispatch(ability,entry,nullptr);Check(ended==2&&parents==2&&roots==2,"invalid location never installs a zero/NaN teleport target");current.x=20000;
    Throws([&]{BindArenaAbility(base);},"unreviewed base/other teleport classes cannot be hooked");
    RemoveArenaHooks();Check(interceptions.empty()&&g_arenaHooks.empty(),"shutdown removes all arena interceptions");
    Dispatch(ability,entry,nullptr);Check(raw==3,"after removal the original entry is restored");
    U* toBattle=makeClass(kLengTuArenaAbilities[1],base), *back=Function(toBattle,"K2_ActivateAbility",{});
    calls[back]=[&](U*,void*){++raw;};
    U* utils=Class("AbilityUtils");std::set<uint64_t> removedTags;int setCount=0;
    U* remove=Function(utils,"RemoveLooseGameplayTag",{{"Actor","ObjectProperty",8},{"GameplayTag","StructProperty",8,false,"GameplayTag"},{"bShouldReplicate","BoolProperty",1}});
    calls[remove]=[&](U*,void* b){Check(Arg<U*>(remove,b,"Actor")==boss&&!Arg<uint8_t>(remove,b,"bShouldReplicate"),"return-to-battle clears loose guards on the owned actor via the exact native utility");removedTags.insert(Arg<uint64_t>(remove,b,"GameplayTag"));};
    U* setTag=Function(utils,"SetLooseGameplayTagCount",{{"Actor","ObjectProperty",8},{"GameplayTag","StructProperty",8,false,"GameplayTag"},{"Count","IntProperty",4}});
    calls[setTag]=[&](U*,void* b){Check(Arg<U*>(setTag,b,"Actor")==boss&&Arg<uint64_t>(setTag,b,"GameplayTag")==inv::ToFName("CharacterState.AbilityState.LengTu.EnableMetaAI")&&Arg<int32_t>(setTag,b,"Count")==1,"return-to-battle restores its native meta AI count");++setCount;};
    Put(ability,ue::Layout::ObjClass,toBattle);BindArenaAbility(toBattle);Dispatch(ability,back,nullptr);
    Check(removedTags.size()==4&&setCount==1&&raw==3&&parents==3,"local arena fix retains all five native return-to-battle phase/AI changes");
    Check(removedTags.contains(inv::ToFName("AI.Hatred.CanNotBeHatred"))&&removedTags.contains(inv::ToFName("CharacterState.AbilityState.LengTu.OffCombat")),"arena return cannot strand a boss in off-combat or unhatable state");
    RemoveArenaHooks();
    g_owned.clear();calls.clear();
}

void ColdBodyTests() {
    g_stopping=false;RemoveArenaHooks();
    auto makeClass=[](std::string_view path){auto dot=path.rfind('.');U* c=Class(std::string(path.substr(dot+1)));Put(c,ue::Layout::ObjOuter,Object(std::string(path.substr(0,dot))));return c;};
    const auto rule=StandaloneRuleFor(kLengTu);
    U* cls=makeClass(kLengTu), *world=Object("ColdBodyWorld",Class("World")), *boss=Object("ColdBody",cls,world), *natural=Object("NaturalCold",cls,world);
    U* ascClass=Class("AbilitySystemComponent"), *asc=Object("ASC",ascClass,boss);
    auto dot=rule.config.rfind('.'); U* config=Object(std::string(rule.config.substr(dot+1)),Class("Config"),Object(std::string(rule.config.substr(0,dot))));
    props[{cls,"Actor Config"}]=0x200;props[{cls,"AbilitySystemComponent"}]=0x208;props[{ascClass,"AvatarActor"}]=0x200;props[{ascClass,"ActivatableAbilities"}]=0x300;
    Put(boss,0x200,config);Put(boss,0x208,asc);Put(asc,0x200,boss);
    U* effect=makeClass(rule.effect);
    std::array<uint8_t,0xF0*3> grants{};
    for(int i=0;i<3;++i){U* c=makeClass(i<2?kLengTuStoryAbilities[i]:"/Safe/Combat.Combat_C");Put(grants.data()+0xF0*i,0xC,int32_t(500+i));Put(grants.data()+0xF0*i,0x10,Object("GrantedAbility",c));}
    Put(asc,0x408,Array{grants.data(),3,3});
    int clears=0,stacks=1,removes=0,storyWrites=0,tagWrites=0,applies=0;
    std::map<U*,int> phaseEffects;
    for(const auto path:kLengTuBodyEffects)phaseEffects[makeClass(path)]=0;
    U* clear=Function(ascClass,"ClearAbility",{{"Handle","StructProperty",4,false,"GameplayAbilitySpecHandle"}});
    calls[clear]=[&](U* o,void* b){int handle=Arg<int32_t>(clear,b,"Handle");Check(o==asc&&handle>=500&&handle<502,"cold removes only reviewed story controllers");Put(grants.data()+(handle-500)*0xF0,0x10,static_cast<U*>(nullptr));++clears;};
    U* count=Function(ascClass,"GetGameplayEffectCount",{{"SourceGameplayEffect","ObjectProperty",8},{"OptionalInstigatorFilterComponent","ObjectProperty",8},{"bEnforceOnGoingCheck","BoolProperty",1},{"ReturnValue","IntProperty",4,true}});
    calls[count]=[&](U* o,void* b){auto* cls=Arg<U*>(count,b,"SourceGameplayEffect");Check(o==asc&&(cls==effect||phaseEffects.contains(cls)),"cold counts only reviewed effects on its own ASC");Return(count,b,"ReturnValue",int32_t(cls==effect?stacks:phaseEffects.at(cls)));};
    U* apply=Function(ascClass,"BP_ApplyGameplayEffectToSelf",{{"GameplayEffectClass","ObjectProperty",8},{"Level","FloatProperty",4},{"EffectContext","StructProperty",24,false,"GameplayEffectContextHandle"},{"ReturnValue","StructProperty",8,true,"ActiveGameplayEffectHandle"}});
    calls[apply]=[&](U* o,void* b){auto* cls=Arg<U*>(apply,b,"GameplayEffectClass");Check(o==asc&&phaseEffects.contains(cls)&&Arg<float>(apply,b,"Level")==1.f,"cold retains only its two native final-stage effects");++phaseEffects[cls];++applies;};
    U* remove=Function(ascClass,"RemoveActiveGameplayEffectBySourceEffect",{{"GameplayEffect","ObjectProperty",8},{"InstigatorAbilitySystemComponent","ObjectProperty",8},{"StacksToRemove","IntProperty",4}});
    calls[remove]=[&](U* o,void* b){Check(o==asc&&Arg<U*>(remove,b,"GameplayEffect")==effect&&clears==2,"cold lock removed after story tasks retire");stacks=0;++removes;};
    U* utils=Class("AbilityUtils");std::map<uint64_t,int> tags;
    U* set=Function(utils,"SetLooseGameplayTagCount",{{"Actor","ObjectProperty",8},{"GameplayTag","StructProperty",8,false,"GameplayTag"},{"Count","IntProperty",4}});
    calls[set]=[&](U*,void* b){Check(Arg<U*>(set,b,"Actor")==boss,"cold stage tags are scoped to this body");tags[Arg<uint64_t>(set,b,"GameplayTag")]=Arg<int32_t>(set,b,"Count");++tagWrites;};
    for(auto* name:{"InitTags","UnlockTag","RemoveUnLockTag","ResetTags"}){U* fn=Function(cls,name,{});calls[fn]=[&](U*,void*){++storyWrites;};}
    g_owned={{Identity(boss),{},Identity(world),std::string(kLengTu),{}}};
    BindColdStoryIsolation(cls);BindColdStoryIsolation(cls);Check(g_coldStoryHooks.size()==4,"cold story hooks do not accumulate across repeated summons");
    for(auto* name:{"InitTags","UnlockTag","RemoveUnLockTag","ResetTags"}){auto* fn=pe::FindFunction(cls,name);Dispatch(boss,fn,nullptr);Dispatch(natural,fn,nullptr);}
    Check(storyWrites==4,"independent cold cannot read or write natural story progression");
    AdaptStandalone(g_owned.front());
    Check(clears==2&&removes==1&&ue::Get<U*>(grants.data()+0x1E0+0x10),"combat ability retained while stage controllers and lock retire");
    Check(tags[inv::ToFName("CharacterState.AbilityState.LengTu.Phase.Final")]==1&&tags[inv::ToFName("CharacterState.AbilityState.LengTu.EnableMetaAI")]==1,"cold body enters terminal native combat state");
    Check(tags[inv::ToFName("CharacterState.AbilityState.LengTu.IsInvulnerable")]==0&&tags[inv::ToFName("CharacterState.AbilityState.LengTu.OffCombat")]==0,"cold body is selectable vulnerable and in combat");
    const int initTags=tagWrites;AdaptStandalone(g_owned.front());Check(clears==2&&removes==1&&tagWrites==initTags&&applies==2,"cold maintenance never resets active combat tags or duplicates phase effects");
    g_owned.front().tag="tower-cold";Dispatch(boss,pe::FindFunction(cls,"UnlockTag"),nullptr);AdaptStandalone(g_owned.front());Check(storyWrites==5&&clears==2,"tower preserves its own story and phase policy");
    g_owned.clear();Check(!OwnIndependentCold(natural),"natural cold never inherits independent-body ownership");RemoveArenaHooks();Check(interceptions.empty(),"cold cleanup removes story isolation hooks");calls.clear();
}
}  // namespace fixture

namespace pbz::ue {
bool Read(const void* p,void* out,size_t n) { if (!p) return false; std::memcpy(out,p,n);return true; }
bool Write(void* p,const void* in,size_t n) { if(!p)return false;std::memcpy(p,in,n);return true; }
int Num(){return static_cast<int>(fixture::objects.size());}
UObject* Obj(int n){return n>=0&&n<Num()?fixture::objects[n]:nullptr;}
std::string Name(uint32_t n){return n<fixture::names.size()?fixture::names[n]:"";}
std::string ObjName(UObject* o){return o?Name(Get<uint32_t>(o+Layout::ObjName)):"";}
UObject* ClassOf(UObject* o){return o?Get<UObject*>(o+Layout::ObjClass):nullptr;}
std::string ClassName(UObject* o){return ObjName(ClassOf(o));}
UObject* FindClass(std::string_view n){auto it=fixture::classes.find(std::string(n));return it==fixture::classes.end()?nullptr:it->second;}
bool IsDefault(UObject* o){return ObjName(o).starts_with("Default__");}
bool IsA(UObject* o,UObject* cls){if(!cls)return false;for(auto* c=ClassOf(o);c;c=Get<UObject*>(c+Layout::StructSuper))if(c==cls)return true;return false;}
void ForEach(const std::function<bool(int,UObject*)>& fn){for(int n=0;n<Num();++n)if(!fn(n,Obj(n)))break;}
int PropOffset(UObject* c,std::string_view n){for(;c;c=Get<UObject*>(c+Layout::StructSuper)){auto i=fixture::props.find({c,std::string(n)});if(i!=fixture::props.end())return i->second;}return -1;}
}  // namespace pbz::ue
namespace pbz::pe {
bool Ready(){return true;}
ue::UObject* FindFunction(ue::UObject* cls,std::string_view n){for(int depth=0;cls&&depth<64;++depth,cls=ue::Get<ue::UObject*>(cls+ue::Layout::StructSuper)){auto i=fixture::functions.find({cls,std::string(n)});if(i!=fixture::functions.end())return i->second;}return nullptr;}
void Call(ue::UObject* o,ue::UObject* f,void* p){++fixture::engineCalls;auto it=fixture::calls.find(f);if(it!=fixture::calls.end())it->second(o,p);} // Like real pe::Call, bypasses the ProcessEvent detour.
void Intercept(ue::UObject* f,Interceptor fn){fixture::interceptions[f]=std::move(fn);}
void RemoveIntercept(ue::UObject* f){fixture::interceptions.erase(f);}
}  // namespace pbz::pe
namespace pbz::inv {
uint64_t ToFName(const std::string& s){return s.empty()?0:std::hash<std::string>{}(s)|1;}  // 阵营标签：测试里只需非零
}  // namespace pbz::inv
namespace pbz::gamethread {
bool Ready(){return fixture::ready;}
void Post(std::function<void()> f){fixture::queue.push_back(std::move(f));}
}  // namespace pbz::gamethread
int main(){try{fixture::PlannerTests();fixture::ReflectionTests();fixture::OwnershipTests();fixture::NativeSpawnTests();fixture::NativeLevelSourceTests();fixture::NativeAttributeTests();fixture::NativeDeathCleanupTests();fixture::NativeDeathRecoveryTests();fixture::StandalonePhaseTests();fixture::ArenaTests();fixture::ColdBodyTests();fixture::QueueTests();
std::cout<<"PASS "<<fixture::checks<<" isolated checks; no game access\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}}
