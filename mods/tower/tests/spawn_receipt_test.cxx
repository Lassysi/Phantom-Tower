#include <cstdio>
#include "../src/tower_spawn_receipt.h"
#include "../src/tower_fate.h"
#include <limits>
int main(){
 int n=0,f=0;auto check=[&](bool b,const char* s){++n;if(!b){++f;std::printf("FAIL %s\n",s);}};
 const std::string receipt=R"({"ok":true,"request_id":"tower-run-33-1-","requested_path":"/Game/Boss.Boss_C","path":"/Game/Boss.Boss_C","actors":[{"name":"user-trainer","path":"/Game/Boss.Boss_C","tag":"manual"},{"name":"native-target","path":"/Game/Boss.Boss_C","tag":"tower-run-33-1-"}]})";
 using tower::spawnreceipt::Read;
 auto r=Read(receipt,"tower-run-33-1-","/Game/Boss.Boss_C");
 check(r.ok&&r.name=="native-target","exact ownership selects requested actor, never unrelated trainer actor");
 check(!Read(receipt,"tower-run-33-10-","/Game/Boss.Boss_C").ok,"request boundary differentiates 1 from 10");
 check(!Read(receipt,"tower-run-33-1-","/Game/Other.Other_C").ok,"wrong requested identity cannot be adopted");
 auto wrong=receipt;auto pos=wrong.find("native-target");wrong.replace(pos,13,"");
 check(!Read(wrong,"tower-run-33-1-","/Game/Boss.Boss_C").ok,"empty target identity fails closed");
 check(!Read("{}","tower-run-33-1-","/Game/Boss.Boss_C").ok,"missing actor receipt is not a successful spawn");
 check(!Read("not json","tower-run-33-1-","/Game/Boss.Boss_C").ok,"malformed reply cannot advance pending lifecycle");
 check(!Read(R"({"request_id":"r","requested_path":"p","path":"p","actors":[{"tag":"r","name":"a","path":"p"},{"tag":"r","name":"b","path":"p"}]})","r","p").ok,"multiple actors for a single spawn request are rejected");
 const std::string swapped=R"({"ok":true,"request_id":"towerally-r-5-1-","requested_path":"/Game/P1.P1_C","path":"/Game/P2.P2_C","actors":[{"name":"ally","path":"/Game/P2.P2_C","tag":"towerally-r-5-1-"}]})";
 check(Read(swapped,"towerally-r-5-1-","/Game/P1.P1_C","/Game/P2.P2_C").ok,"servant receipt accepts the core standalone final-phase swap");
 check(!Read(swapped,"towerally-r-5-1-","/Game/P1.P1_C").ok,"tower receipts still reject any path swap");
 using tower::fate::DeathTrack;
 DeathTrack d;d.Sample(0,100);d.Sample(1,0);d.Sample(1.5,std::numeric_limits<float>::quiet_NaN());
 check(!d.Dead()&&d.zeroAt<0,"invalid HP breaks death confirmation instead of being counted as zero");
 check(d.Sample(2,0)==DeathTrack::Ev::Zero && !d.Dead(),"new finite zero starts a fresh confirmation interval");
 check(d.Sample(3.01f,0)==DeathTrack::Ev::Confirmed&&d.Dead(),"only a complete finite zero interval confirms death");
 check(d.Sample(4,100)==DeathTrack::Ev::Resumed&&!d.Dead(),"same-actor stage recovery remains legal");
 check(tower::fate::kSettleSec==0,"final confirmation has no arbitrary native-ending wait");
 check(tower::fate::JudgePlayer(std::numeric_limits<float>::quiet_NaN(),10,false,false)==tower::fate::Verdict::Fine,"invalid player HP cannot manufacture a loss");
 std::printf("TOWER_SPAWN_RECEIPT_%s %d checks (ownership + death evidence)\n",f?"FAILED":"OK",n);return f?1:0;
}
