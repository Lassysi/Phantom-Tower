// Actual reach.cpp, independently recorded Build26266538 reflection offsets and
// the failed floor1's native floor sample. No game process/native engine calls.
#include <windows.h>
#include <array>
#include <cstdio>
#include <map>
#define PBZ_MODULE
#include "../src/tower_reach.cpp"
namespace fixture {
std::array<uint8_t,4096> pawn{},capsule{},movement{};
std::array<uint8_t,512> pawnCls{},capsuleCls{},movementCls{},floorCls{},hitCls{},kslCls{},navCls{},pathCls{},lineFn{},halfFn{};
std::map<int32_t,pbz::ue::UObject*> objects;
std::array<uint8_t,512> radiusFn{},sweepFn{};
std::array<uint8_t,512> stopFn{};
bool stopWorks=true;
bool clearanceBlocked=false,missingSweep=false; double surfaceNormal=1;
const double location[]={-19124.124681663652,292240.66375452897,-26374.51066789535};
const double impact[]={-19124.12451171875,292241.63076782227,-26472.840454101562};
float half=96;bool rayHit=true,explicitIgnore=false,missingIgnore=false,penetrating=false,invalidHit=false,missingHeight=false;
bool splitChannels=false,staticOverlap=false,dynamicOverlap=false,zeroTimeOnly=false;
int calls=0,unsafeQueries=0;double rayGap=2.329786206213612;
template<class T>void put(uint8_t* p,const T& v){std::memcpy(p,&v,sizeof v);}
void reset(){
    pawn.fill(0);capsule.fill(0);movement.fill(0);floorCls.fill(0);
    put(pawn.data()+0x10,pawnCls.data());put(capsule.data()+0x10,capsuleCls.data());put(movement.data()+0x10,movementCls.data());
    put(pawn.data()+64,capsule.data());put(pawn.data()+72,movement.data());put(movement.data()+0x20,pawn.data());
    put(floorCls.data()+0xa8,int32_t(272));
    movement[577]=1;std::memcpy(movement.data()+880,location,24);
    movement[1096]=0x8b;put(movement.data()+1096+4,2.3408608436584473f);
    std::memcpy(movement.data()+1096+16+40,impact,24);
    half=96;rayHit=true;explicitIgnore=false;missingIgnore=false;penetrating=false;invalidHit=false;missingHeight=false;rayGap=2.329786206213612;
    splitChannels=false;staticOverlap=false;dynamicOverlap=false;zeroTimeOnly=false;
    clearanceBlocked=missingSweep=false;surfaceNormal=1;
    stopWorks=true;
}
}
namespace pbz::ue {
bool Read(const void* p,void* d,size_t n){if(!p)return false;std::memcpy(d,p,n);return true;}
bool Live(UObject* p){return p!=nullptr;}
UObject* ClassOf(UObject* p){return Get<UObject*>(p+0x10);}
bool IsA(UObject* p,UObject* cls){return p&&ClassOf(p)==cls;}
int Num(){return 0;}
UObject* FindClass(std::string_view n){using namespace fixture;return n=="CapsuleComponent"?capsuleCls.data():n=="FindFloorResult"?floorCls.data():nullptr;}
UObject* Obj(int i){return fixture::objects[i];}
std::string ClassName(UObject*){return "GroundFixture";}
std::string Name(uint32_t){return "";} // fixture preloads independently recorded reflection layout
int PropOffset(UObject* c,std::string_view n){
    using namespace fixture;
    if(c==lineFn.data()){
        const std::map<std::string_view,int> layout={{"WorldContextObject",0},{"Start",8},{"End",32},{"ObjectTypes",56},{"bTraceComplex",72},{"ActorsToIgnore",80},{"OutHit",104},{"bIgnoreSelf",360},{"ReturnValue",400}};
        auto i=layout.find(n);return i==layout.end()||(n=="ActorsToIgnore"&&missingIgnore)?-1:i->second;
    }
    if(c==halfFn.data())return n=="ReturnValue"?0:-1;
    if(c==radiusFn.data())return n=="ReturnValue"?0:-1;
    if(c==sweepFn.data()) {
        const std::map<std::string_view,int> layout={{"WorldContextObject",0},{"Start",8},{"End",32},{"Radius",56},{"HalfHeight",60},{"ObjectTypes",64},{"ActorsToIgnore",80},{"bIgnoreSelf",360},{"ReturnValue",400}};
        auto i=layout.find(n);return i==layout.end()?-1:i->second;
    }
    if(c==pawnCls.data())return n=="CapsuleComponent"?64:n=="CharacterMovement"?72:-1;
    if(c==movementCls.data())return n=="MovementMode"?577:n=="LastUpdateLocation"?880:n=="CurrentFloor"?1096:n=="Velocity"?1600:-1;
    if(c==floorCls.data())return n=="FloorDist"?4:n=="HitResult"?16:n=="bBlockingHit"||n=="bWalkableFloor"?0:-1;
    return -1;
}
}
namespace pbz::pe {
ue::UObject* FindFunction(ue::UObject* c,std::string_view n){
    if(c==fixture::movementCls.data()&&n=="StopMovementImmediately")return fixture::stopFn.data();
    if(n=="CapsuleTraceSingleForObjects"&&!fixture::missingSweep)return fixture::sweepFn.data();
    if(c==fixture::capsuleCls.data()&&n=="GetScaledCapsuleRadius")return fixture::radiusFn.data();
    return c==fixture::capsuleCls.data()&&n=="GetScaledCapsuleHalfHeight"&&!fixture::missingHeight?fixture::halfFn.data():nullptr;
}
void Call(ue::UObject*,ue::UObject* fn,void* raw){
    using namespace fixture;auto* p=static_cast<uint8_t*>(raw);
    if(fn==halfFn.data()){put(p,half);return;}
    if(fn==radiusFn.data()){put(p,40.f);return;}
    if(fn==sweepFn.data()){p[400]=clearanceBlocked;return;}
    if(fn==stopFn.data()){if(stopWorks)std::memset(movement.data()+1600,0,24);return;}
    if(fn!=lineFn.data())return;
    ++calls;struct Arr{ue::UObject** data;int32_t num,max;};Arr ignored{};std::memcpy(&ignored,p+80,16);
    explicitIgnore=ignored.num==1&&ignored.max==1&&ignored.data&&ignored.data[0]==pawn.data();
    if(!explicitIgnore)++unsafeQueries;
    p[400]=rayHit;
    if(rayHit){
        // Missing explicit exclusion returns the old characteristic at the
        // trace start (Z+100), reproducing the -190cm normal-floor false alarm.
        double at[3]={location[0],location[1],explicitIgnore?location[2]-half-rayGap:location[2]+100};
        if(invalidHit)at[2]=NAN;
        struct Types{uint8_t* data;int32_t num,max;};Types types{};std::memcpy(&types,p+56,16);
        const bool initial=splitChannels?(types.data[0]==0?staticOverlap:dynamicOverlap):penetrating;
        std::memcpy(p+104+40,at,24);put(p+104+4,(initial||zeroTimeOnly)?0.f:0.092f);
        // Build26266538 HitResult's actual reflected bStartPenetrating bit,
        // independently recorded in Core's guarded trace implementation.
        p[104+0xad]=initial?2:0;
        put(p+104+88+16,surfaceNormal);
    }
}
}
namespace towerwidgets {pbz::ue::UObject* Default(pbz::ue::UObject* c){return c;}}
namespace pbz::log {void Write(const std::string&) {}}
int main(){
    using namespace fixture;using namespace towerreach;
    int checks=0,failed=0;auto check=[&](bool b,const char* why){++checks;if(!b){++failed;std::printf("FAIL %s\n",why);}};
    reset();g.resolved=true;g.ksl=kslCls.data();g.line=lineFn.data();g.penetrateOff=0xad;g.penetrateMask=2;
    auto measure=[](){return Measure(pawn.data(),location,nullptr,location);};
    auto normal=measure();
    check(explicitIgnore&&unsafeQueries==0,"the real trace call carries explicit WorldContext pawn exclusion and a live TArray");
    check(normal.nativeKnown&&normal.nativeSupport&&std::abs(normal.nativeGap-2.329786206213612)<0.001,"recorded floor1 native floor/capsule position proves nearby walkable support");
    check(Decide(normal)==Verdict::Pass&&normal.capsuleHalfHeight==96,"normal first floor is admitted with scaled96cm capsule, not a hardcoded90cm foot");
    movement[577]=3;rayGap=1500;
    check(Decide(measure())==Verdict::NoGround,"walking cache cannot admit a falling player above distant ground");
    movement[577]=1;double displaced=location[0]+1000;put(movement.data()+880,displaced);
    check(!measure().nativeKnown&&Decide(measure())==Verdict::NoGround,"pre-teleport LastUpdateLocation invalidates cached native support");
    reset();rayHit=false;
    check(measure().nativeSupport&&Decide(measure())==Verdict::Pass,"fresh native walkable floor supports collision channels the query does not report");
    movement[1096]=1;check(!measure().nativeSupport&&Decide(measure())==Verdict::NoGround,"blocking but unwalkable native floor does not admit an unsupported player");
    reset();rayHit=false;put(movement.data()+1096+4,NAN);
    check(!measure().nativeKnown&&Decide(measure())==Verdict::NoGround,"nonfinite native FloorDist is not support evidence");
    reset();movement[577]=3;penetrating=true;
    check(Decide(measure())==Verdict::NoGround,"a start-solid ray is not a standing surface");
    reset();missingIgnore=true;movement[577]=3;const auto before=calls;
    check(!measure().groundKnown&&calls==before,"missing ignore parameter layout refuses the trace before a native call");
    reset();missingHeight=true;
    check(!measure().groundKnown&&!measure().nativeSupport,"unknown capsule height cannot be replaced with a guessed foot location");
    reset();movement[577]=3;invalidHit=true;
    check(Decide(measure())==Verdict::NoGround,"invalid hit coordinates cannot pass ground admission");
    reset();movement[577]=3;rayGap=5;
    check(Decide(measure())==Verdict::Pass,"valid explicit ground ray remains usable without native walking support");
    reset();rayHit=false;put(floorCls.data()+0xa8,int32_t(288));
    check(!measure().nativeKnown&&Decide(measure())==Verdict::NoGround,"changed native floor struct layout fails closed");
    reset();splitChannels=true;dynamicOverlap=true;
    auto audit=[&](){return Audit(pawn.data(),location,96,800);};
    auto st=audit();
    check(st.staticHits==12&&st.dynamicOverlap==12&&st.startSolid==0&&Classify(st)==FloorVerdict::Ok,"recorded floor1: dynamic start overlap cannot hide twelve valid static supports");
    staticOverlap=true;dynamicOverlap=false;auto dy=audit();
    check(dy.dynamicHits==12&&dy.staticOverlap==12&&Classify(dy)==FloorVerdict::NotStatic,"static overlap cannot hide valid dynamic candidate surfaces");
    dynamicOverlap=true;auto covered=audit();
    check(covered.startSolid==12&&covered.nativePlayerSupport&&Classify(covered)==FloorVerdict::Unknown,"Lion plaza: both queries occluded while native standing support is valid is inconclusive, not a solid/void diagnosis");
    check(CoreWillRefuse(covered),"F42 Gu He: every candidate occluded on both channels while the player stands supported -> go straight to the native spawner");
    movement[1096]=1;auto embedded=audit();
    check(embedded.startSolid==12&&!embedded.nativePlayerSupport&&Classify(embedded)==FloorVerdict::StartInSolid,"true penetrating rays without standing support retain the solid-start classification");
    check(!CoreWillRefuse(embedded),"without native standing support the shortcut is not taken");
    reset();zeroTimeOnly=true;auto touch=audit();
    check(touch.startSolid==0&&touch.unresolved==12&&Classify(touch)==FloorVerdict::Unknown,"zero ray time without the penetration flag is never described as starting in solid");
    reset();g.penetrateMask=0;auto missing=audit();
    check(missing.unresolved==12&&Classify(missing)==FloorVerdict::Unknown,"missing reflected penetration layout cannot claim audited floor support");g.penetrateMask=2;
    reset();rayHit=false;auto empty=audit();
    check(empty.none==12&&empty.staticHits==0&&Classify(empty)==FloorVerdict::NoFloor,"standing at the center does not invent support for twelve empty candidate points");
    check(!CoreWillRefuse(empty)&&!CoreWillRefuse(st)&&!CoreWillRefuse(dy),"valid supports or empty candidates never take the native-spawner shortcut");
    reset();penetrating=true;double farBoss[]={location[0]+5000,location[1],location[2]};
    const auto obscured=Measure(pawn.data(),location,capsule.data(),farBoss);
    check(!obscured.lineKnown&&!obscured.lineBlocked,"start-overlap query is unknown sight, not evidence of a wall between player and Boss");
    reset();const auto blocked=Measure(pawn.data(),location,capsule.data(),farBoss);
    check(blocked.lineKnown&&blocked.lineBlocked,"valid distant blocking impact still reports obstruction");
    reset();g.normal=88;double landed[3];std::string why;
    auto landing=[&](){return Landing(pawn.data(),location,landed,&why);};
    check(landing()&&std::abs(landed[2]-(location[2]-rayGap+5))<.01,"remote landing uses independently traced surface and actual capsule height");
    rayHit=false;check(!landing(),"native CurrentFloor at a safe old position cannot substitute for absent destination geometry");
    reset();surfaceNormal=.1;check(!landing(),"wall-like surface is not walkable landing support");
    reset();penetrating=true;check(!landing(),"start-penetrating ray cannot authorize a teleport");
    reset();clearanceBlocked=true;check(!landing(),"floor exists but occupied capsule refuses teleport");
    reset();missingSweep=true;check(!landing(),"missing clearance API fails closed");
    reset();invalidHit=true;check(!landing(),"nonfinite remote impact fails closed");
    reset();splitChannels=true;dynamicOverlap=true;check(landing(),"valid static floor survives unrelated dynamic ray overlap when capsule itself is clear");
    reset();double wrongXY[3]={location[0]+8000,location[1],location[2]};check(!Landing(pawn.data(),wrongXY,landed,&why),"hit at a different XY cannot authorize this destination");
    reset();put(movement.data()+1600+16,1e7);check(StopMotion(pawn.data()),"native movement stop clears the runaway vertical velocity before relocation");
    stopWorks=false;put(movement.data()+1600+16,1e7);check(!StopMotion(pawn.data()),"failed velocity readback rejects relocation instead of retaining flight");
    put(movement.data()+0x20,capsule.data());check(!StopMotion(pawn.data()),"movement belonging to another actor is never stopped");
    reset();const int beforeStanding=calls;
    check(StandingAt(pawn.data(),location)&&calls==beforeStanding,"arrival uses fresh native support without an overlapping remote capsule trace");
    movement[577]=3;check(!StandingAt(pawn.data(),location),"falling with a cached walkable floor cannot resolve arrival height");
    reset();put(movement.data()+880,location[0]+1000);check(!StandingAt(pawn.data(),location),"stale pre-teleport floor cannot resolve arrival height");
    reset();movement[1096]=1;check(!StandingAt(pawn.data(),location),"unwalkable support cannot resolve arrival height");
    reset();missingHeight=true;check(!StandingAt(pawn.data(),location),"unknown scaled capsule fails arrival support closed");
    reset();double invalidAt[3]={location[0],location[1],NAN};check(!StandingAt(pawn.data(),invalidAt),"nonfinite arrival fails closed");
    reset();put(movement.data()+0x20,capsule.data());check(!StandingAt(pawn.data(),location),"other actor's floor cannot resolve this arrival");
    reset();const double f37At[]={-202720.76581217613,85683.567311403,5410.126742701718};
    const double f37Impact[]={-202720.76581088977,85683.56731987854,5311.97674200239};
    std::memcpy(movement.data()+880,f37At,24);std::memcpy(movement.data()+1096+16+40,f37Impact,24);
    movement[1096]=3;put(movement.data()+1096+4,2.149998664855957f);
    check(StandingAt(pawn.data(),f37At),"live F37 stationary native floor at Z5410.1267 is supported despite center Z5860.2");
    std::printf("TOWER_REACH_RUNTIME_%s %d checks (actual reach.cpp + recorded native floor, no game)\n",failed?"FAILED":"OK",checks);return failed?1:0;
}
