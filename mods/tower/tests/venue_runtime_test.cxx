// Actual venue lifecycle, with an independently observed AI-only wall fixture.
#include <windows.h>
#include <array>
#include <cstdio>
#include <map>
#define PBZ_MODULE
#include "../src/tower_venue.cpp"
namespace fixture {
std::array<uint8_t,512> sp{},door{},wall{},other{},world{},level{},spc{},dc{},wc{},oc{},getfn{},setfn{},locfn{},closefn{},openfn{};
std::vector<pbz::ue::UObject*> objects;
std::map<pbz::ue::UObject*,std::string> names;
bool wallOn=true, missingSetter=false, missingWall=false, foreignWorld=false, failSetter=false, reused=false;
int wallWrites=0, closed=0,opened=0;
template<class T>void put(uint8_t* at,T v){std::memcpy(at,&v,sizeof v);}
void reset(){
 objects={sp.data(),door.data(),wall.data()};wallOn=true;missingSetter=missingWall=foreignWorld=failSetter=reused=false;wallWrites=closed=opened=0;
 put(sp.data()+0x10,spc.data());put(door.data()+0x10,dc.data());put(wall.data()+0x10,wc.data());
 put(wall.data()+0x0c,int32_t(3));put(wall.data()+0x20,level.data());put(level.data()+0x20,world.data());
 put(world.data()+0x20,static_cast<uint8_t*>(nullptr));put(getfn.data()+0xa8,int32_t(1));put(setfn.data()+0xa8,int32_t(1));
 names={{sp.data(),"BP_AISpawner_C_UAID_60CF848A91CA9B9E02_1410718921"},{door.data(),"BP_FogDoor_C_UAID_60CF848A91CA9B9E02_1410844001"},
 {wall.data(),"BP_AIWall_C_UAID_BCFCE704F7254EEC02_1711908456"},{level.data(),"PersistentLevel"},{world.data(),"PBZ_Ximeng_WP"},
 {spc.data(),"AISpawner"},{dc.data(),"BP_FogDoor_C"},{wc.data(),"BP_AIWall_C"},{oc.data(),"OtherActor_C"}};
}
const char* boss="/PBZCombatAsset/ActorData/Mobs/YuDing_00/BP_Monster_FemaleVessel_YuDing.BP_Monster_FemaleVessel_YuDing_C";
void arrive(){towervenue::Arrive(boss);towervenue::Tick();}
}
namespace pbz::ue {
bool Read(const void* p,void* out,size_t n){if(!p)return false;std::memcpy(out,p,n);return true;}
bool Live(UObject* p){return p && !(fixture::reused&&p==fixture::wall.data());}
UObject* ClassOf(UObject* p){return Get<UObject*>(p+0x10);}
std::string ObjName(UObject* p){if(fixture::foreignWorld&&p==fixture::world.data())return "PBZ_Other_WP";return fixture::names[p];}
std::string ClassName(UObject* p){return ObjName(ClassOf(p));}
bool IsDefault(UObject*){return false;}
bool IsA(UObject* p,UObject* c){return ClassOf(p)==c;}
int Num(){return 10;}
UObject* FindClass(std::string_view n){using namespace fixture;return n=="AISpawner"?spc.data():n=="BP_FogDoor_C"?dc.data():nullptr;}
void ForEach(const std::function<bool(int,UObject*)>& f){int i=0;for(auto* p:fixture::objects){if(fixture::missingWall&&p==fixture::wall.data())continue;if(!f(i++,p))break;}}
// 1.3.0 分帧扫描：按对象表下标取一段（Num() 个槽，超出 fixture 的为空槽）
int ObjBatch(int start,int count,UObject** out){int n=0;for(int i=start;i<Num()&&n<count;++i,++n){UObject* p=i<static_cast<int>(fixture::objects.size())?fixture::objects[i]:nullptr;out[n]=fixture::missingWall&&p==fixture::wall.data()?nullptr:p;}return n;}
int PropOffset(UObject* o,std::string_view name){using namespace fixture;if(o==locfn.data()&&name=="ReturnValue")return 0;if(o==getfn.data()&&name=="ReturnValue")return 0;if(o==setfn.data()&&name=="bNewActorEnableCollision")return 0;return -1;}
}
namespace pbz::pe {
ue::UObject* FindFunction(ue::UObject*,std::string_view n){using namespace fixture;return n=="K2_GetActorLocation"?locfn.data():n=="GetActorEnableCollision"?getfn.data():n=="SetActorEnableCollision"&&!missingSetter?setfn.data():n=="Close By Mob Script"?closefn.data():n=="OpenByMobScript"?openfn.data():nullptr;}
void Call(ue::UObject* o,ue::UObject* fn,void* raw){using namespace fixture;auto* p=static_cast<uint8_t*>(raw);if(fn==locfn.data()){double at[]={-189591.5,91079.5,3866};std::memcpy(p,at,24);}else if(fn==getfn.data())p[0]=wallOn;else if(fn==setfn.data()){++wallWrites;if(!failSetter)wallOn=p[0]!=0;}else if(fn==closefn.data())++closed;else if(fn==openfn.data())++opened;}
}
namespace towerisolate {void Quiet(void*){}}
namespace pbz::ipc {std::string Esc(const std::string& s){return s;}}
namespace pbz::log {void Write(const std::string&) {}}
int main(){
 using namespace fixture;int checks=0,failed=0;auto check=[&](bool ok,const char* why){++checks;if(!ok){++failed;std::printf("FAIL %s\n",why);}};
 reset();arrive();towervenue::CloseDoors();check(!wallOn,"native inner AI cage disabled before the Tower spawn/countdown");check(closed==1,"outer native fog remains closed");
 const int first=wallWrites;towervenue::CloseDoors();check(wallWrites==first,"repeat close does not acquire or overwrite original twice");towervenue::OpenDoors();check(wallOn,"victory/cancel restores original AI collision");check(opened==1,"outer fog opens normally");towervenue::End();check(wallWrites==first+1,"End after OpenDoors does not restore twice");
 reset();wallOn=false;arrive();towervenue::CloseDoors();towervenue::End();check(!wallOn&&wallWrites==0,"already disabled native wall stays disabled without ownership");
 reset();arrive();towervenue::CloseDoors();reused=true;towervenue::End();check(wallWrites==1,"destroyed/reused wall skipped on restore");
 reset();missingSetter=true;arrive();towervenue::CloseDoors();check(wallWrites==0&&closed==0,"unresolved collision API refuses all venue changes");towervenue::End();
 reset();missingWall=true;arrive();towervenue::CloseDoors();check(wallWrites==0&&closed==0,"missing required inner wall refuses preparation");towervenue::End();
 reset();foreignWorld=true;arrive();towervenue::CloseDoors();check(wallWrites==0&&closed==0,"same actor name in another world is never modified");towervenue::End();
 reset();failSetter=true;arrive();towervenue::CloseDoors();check(closed==0,"failed collision readback cannot continue with locked cage");towervenue::End();
 reset();arrive();put(wall.data()+0x10,oc.data());check(!towervenue::CloseDoors()&&wallWrites==0,"same name with a different class is rejected");towervenue::End();
 reset();arrive();put(getfn.data()+0xa8,int32_t(0));check(!towervenue::CloseDoors()&&wallWrites==0,"invalid reflected ABI size refuses native call");towervenue::End();
 reset();arrive();towervenue::CloseDoors();names[wall.data()]="reusedActor";towervenue::End();check(wallWrites==1,"live address with changed name is never restored");
 reset();arrive();towervenue::CloseDoors();put(wall.data()+0x0c,int32_t(4));towervenue::End();check(wallWrites==1,"live address with changed object index is never restored");
 reset();arrive();towervenue::CloseDoors();put(wall.data()+0x20,other.data());towervenue::End();check(wallWrites==1,"live address with changed owner is never restored");
 reset();towervenue::Arrive("not-a-contracted-venue");towervenue::Tick();towervenue::CloseDoors();towervenue::End();check(wallWrites==0,"unrelated floors never disable world AI walls");
 reset();towervenue::Arrive("/PBZCombatAsset/ActorData/Mobs/QiShu/BP_Monster_QiShu_Bing.BP_Monster_QiShu_Bing_C");
 check(!towervenue::MissingRequired(),"Hansui waits for streaming before declaring the native venue missing");
 towervenue::g_since=GetTickCount64()-10001;towervenue::Tick();
 check(towervenue::Ready() && towervenue::MissingRequired() && !towervenue::Found(),"Hansui missing native spawner cannot fall through to a default arena after ten seconds");
 towervenue::End();check(!towervenue::MissingRequired(),"venue failure state is cleared at end");
 std::printf("TOWER_VENUE_%s %d/%d\n",failed?"FAILED":"OK",failed,checks);return failed?1:0;
}
