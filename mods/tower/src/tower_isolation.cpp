#include "tower_isolation.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../../src/core/host/log.h"
#include "../../../src/core/ipc/pipe.h"
#include "../../../src/core/features/feature.h"
#include "../../../src/core/features/world/enemy_spawn.h"
#include "../../../src/core/sdk/pbz_module.h"
#include "../../../src/core/services/process_event.h"
#include "../../../src/core/services/ue.h"
#include "boss_pool.generated.h"
#include "tower_interactions.h"
#include "tower_widgets.h"
#include "tower_travel_api.h"
#include "tower_perf.h"
#include "tower_storygate.h"
#include "tower_weapon_grant.h"
#include "tower_replay_group.h"

namespace towerisolate {
namespace {

using pbz::ue::UObject;
namespace ue = pbz::ue;
namespace pe = pbz::pe;
namespace nx = pbz::nx;

// 已知会触发原剧情、尚无法隔离的场地（按 Boss 身份）。依据：Codex 2026-10-31 3c501e6 实机第 2 层，
// 断魂崖力者和锯者开打一会后触发原剧情。场地级隔离验证通过之前一律不抽
// F34-VENUE-001：原写死 {"PowerMan"}（1de3577 ABI5 旧剧情冻结失败），之后会话隔离 / 剧情门已重做，写死项让 34 层每局都退去风雪刀客场地。
// 去掉写死；断魂场地若实机再冻结失败，会按 MarkUnsafe 写进 venue-unsafe.txt（本机证据），不再按旧版本猜。

struct Fn { UObject* fn = nullptr; int ret = -1; std::vector<std::pair<std::string, int>> args; int size = 0x100; };

Fn Resolve(UObject* cls, const char* name, std::initializer_list<const char*> args) {
    Fn f;
    f.fn = cls ? pe::FindFunction(cls, name) : nullptr;
    if (!f.fn) return f;
    f.ret = ue::PropOffset(f.fn, "ReturnValue");
    for (auto* a : args) f.args.push_back({a, ue::PropOffset(f.fn, a)});
    return f;
}

std::wstring TowerFile(const wchar_t* leaf) {
    wchar_t base[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return {};
    return std::wstring(base) + L"\\PBZTower\\" + leaf;
}

// Actor 所在的世界。世界分区（WP）流式单元里的 Actor：Level 的外层是单元包自己的世界对象
// （/_Generated_/<单元>.PBZ_Xigu_WP），不是玩家所在的主世界；真正所属的世界在反射属性 Level.OwningWorld。
// 8071424 第 4 层：追命一阶段刷怪点在流式单元里，外层比较把它数成 0 个 →"阶段通知所需的原生刷怪点不唯一"，开层前作废
UObject* WorldOf(UObject* actor) {
    auto* level = actor ? ue::Get<UObject*>(actor + ue::Layout::ObjOuter) : nullptr;
    if (!level) return nullptr;
    static UObject* levelCls = nullptr;
    static int owning = -1;
    if (auto* cls = ue::ClassOf(level); cls && cls != levelCls && ue::ClassName(level) == "Level") {
        levelCls = cls;
        owning = ue::PropOffset(cls, "OwningWorld");
    }
    if (owning >= 0 && ue::ClassOf(level) == levelCls)
        if (auto* world = ue::Get<UObject*>(level + owning); world && ue::Live(world)) return world;
    return ue::Get<UObject*>(level + ue::Layout::ObjOuter);
}

// 去场地途中（钟传送的原生传送演出、点传送悬停、跨图读档）：传送流程自己的过场与读档恢复不碰
bool Travelling() { return towertravel::Busy() || pbz::TeleportBusy(); }
const tower::replay::Contract* g_replay = nullptr;  // 本层重放契约（见 SetReplay）
std::unordered_set<UObject*> g_nativeGroups;  // 重放：整组按原生运行的阶段组（会话层登记，场地清怪 / 阻断都跳过）
// 出塔 / 本层结束清理里的原生调用。R6 第 1 层出塔：会话结束撤清组阻断时 RemoveClearBlocker 在游戏代码里 0xC0000005——
// 组对象随换图销毁、旧地址被别的对象复用后 ue::Live 仍为真（它只核对象表槽位）。异常冒到模块队列会让出塔事务停在半路
// （出塔状态没存、回标题后不再自动继续 → 一直黑屏）。调用前另核类与名字；万一仍出错只跳过这一项，清理照样走完
bool GuardedCall(UObject* obj, UObject* fn, void* params) {
    __try { pe::Call(obj, fn, params); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReplaySpawner(UObject* sp) { return g_replay && sp && tower::replay::In(g_replay->spawners, g_replay->spawnerN, ue::ObjName(sp)); }
unsigned long long g_cinematicAt = 0;               // 最近一次有流程把玩家切进电影模式的时刻

// ---- 会话层（见 tower_isolation.h）。1–32 层实测的证据：
//   14 毕远 / 15 碧海 / 17 炎茹 / 29 玉璧：到场静默后、读秒期间就出"斩"和原生兵器 / 道具获取页——旧 Silence 先 CancelPendingSpawnCompletely
//   再给组加阻断，组看到"没有待刷、没有活着的成员"判定清空，组脚本 MobGroupScript_AddItemAfterCombat_C.OnGroupCleared 发兵器；
//   10 白赤练：过场跳到结尾后补发 OnPlaybackFinished，Flow 往下走到发兵器 / 换图；
//   12 / 19 / 18：读秒和第二轮出现场地外的原生 Boss（白赤练 / 沐小葵 / 万钧），它们的刷怪点不在出生点 80 米内；
//   13 / 26：入场取消后 Isolation.End → Interactions.End 撤了交互过滤，宝箱 / E 又出来（塔档会话还在 Inside）。
class SessionGuard {
public:
    int granting = 0;

    void Set(bool inside, int floor, std::string_view run) {
        floor_ = floor;
        if (run_ != run) run_ = std::string(run);
        if (inside && !on_) Begin();
        else if (!inside && on_) End();
        if (on_) Tick();
    }
    bool On() const { return on_; }

    bool ReplayGroup(UObject* sp,std::string* why) {
        if(!g_replay || !ReplaySpawner(sp))return true;
        int phase=-1;
        for(int i=0;i<g_replay->spawnerN;++i)if(g_replay->spawners[i]==ue::ObjName(sp)){phase=i;break;}
        if(phase<0 || g_replay->clearTags[phase].empty())return true;
        if(WorldOf(sp)!=WorldOf(towerwidgets::Pawn()))return true;
        auto* group=spGroupOff_>=0?ue::Get<UObject*>(sp+spGroupOff_):nullptr;
        if(!group || !ue::Live(group) || !ue::IsA(group,groupCls_)){*why="原生阶段刷怪点尚无战斗组："+std::string(ue::ObjName(sp));return false;}
        // 28 层剑阵：阶段脚本自己数组员死亡，按原生运行；只在组里每个脚本都属于已核验的无奖励清单时放行
        auto goNative=[&](const char* how)->bool{
            std::string bad;
            if(!NativeGroupSafe(group,&bad)){*why=std::string(how)+"；原生阶段组含未核验脚本："+bad+"（组脚本："+GroupScripts(group)+"）";return false;}
            if(g_nativeGroups.insert(group).second){
                Unmute(group);
                if(blocked_.count(group) && removeBlocker_ && rmOwner_>=0 && rmEval_>=0){
                    uint8_t args[64]{};auto* owner=groupCdo_;std::memcpy(args+rmOwner_,&owner,8);args[rmEval_]=0;
                    if(GuardedCall(group,removeBlocker_,args))blocked_.erase(group);
                }
                Note("重放：原生阶段组按原生运行", ue::ObjName(group), group, how);
            }
            return true;
        };
        if(g_replay->nativeGroups)return goNative("契约声明整组原生");
        Mute(group); // save original bindings, mute every unrelated event first
        if(blocked_.count(group)){
            if(!removeBlocker_ || rmOwner_<0 || rmEval_<0){*why="阶段组阻断无法安全解除";return false;}
            uint8_t args[64]{};auto* owner=groupCdo_;std::memcpy(args+rmOwner_,&owner,8);args[rmEval_]=0;
            if(!GuardedCall(group,removeBlocker_,args)){*why="阶段组解除阻断异常";return false;}
            blocked_.erase(group); // no EvaluateClear and no fabricated group completion
        }
        towergroup::List originalDead{};
        if(auto saved=muted_.find(group);saved!=muted_.end())std::memcpy(&originalDead,saved->second.list[1],sizeof originalDead);
        if(!towergroup::Bind(*g_replay,phase,sp,group,scriptsOff_,delegateOff_[0],delegateOff_[1],originalDead,why)){
            // 同类兜底：组里没有通用 ClearGroup 阶段脚本（阶段标签可能由 Boss 自身死亡脚本或组自定义脚本写），组脚本全在无奖励清单时按原生运行
            if(why->rfind("未找到唯一匹配阶段标签",0)==0)return goNative("组内无通用 ClearGroup 阶段脚本");
            return false;
        }
        // Read-only boundary evidence for the next live acceptance. A bound
        // listener is not proof that the actual native death/clear path ran.
        auto* deadFn=pe::FindFunction(groupCls_,"OnMobGroupMemberDeadEvent");
        if(deadFn!=replayDeadFn_){
            if(replayDeadObs_)pe::Unobserve(replayDeadObs_);
            replayDeadFn_=deadFn;replayDeadObs_=0;
            const int input=deadFn?ue::PropOffset(deadFn,"AISpawner"):-1;
            if(deadFn && input>=0)replayDeadObs_=pe::Observe(deadFn,[this,input](UObject* g,void* raw){
                const auto it=towergroup::bindings.find(g);
                if(!on_ || it==towergroup::bindings.end() || !towergroup::Same(it->second))return;
                auto* sp=raw?ue::Get<UObject*>(static_cast<uint8_t*>(raw)+input):nullptr;
                Note("原生阶段成员死亡输入",std::string(it->second.contract->id)+" phase="+std::to_string(it->second.phase+1)+" spawner="+(sp && ue::Live(sp)?ue::ObjName(sp):"-")+" expected="+ue::ObjName(it->second.spawner),g,"只观察；由原生函数检查存活/待刷/清组阻断");
            });
        }
        auto* clearFn=towergroup::functions[1];
        if(clearFn!=replayClearFn_){
            if(replayClearObs_)pe::Unobserve(replayClearObs_);
            replayClearFn_=clearFn;replayClearObs_=clearFn?pe::Observe(clearFn,[this](UObject* script,void*){
                if(!on_)return;
                for(const auto& [g,b]:towergroup::bindings)if(b.script==script && towergroup::Same(b)){
                    Note("原生阶段清组输出",std::string(b.contract->id)+" phase="+std::to_string(b.phase+1)+" tag="+std::string(b.contract->clearTags[b.phase]),g,"只观察；原生ClearGroup发送阶段GameplayEvent");break;
                }
            }):0;
        }
        if(!replayDeadObs_ || !replayClearObs_){*why="原生阶段通知观察器未就绪";return false;}
        return true;
    }
    void ClearReplayCache() { replaySp_.clear(); contractGroups_.clear(); contractGroupsNext_ = 0; g_nativeGroups.clear(); }
    bool ReplayReady(std::string* why,int requiredPhase=0) {
        if(!g_replay || requiredPhase<0 || requiredPhase>=g_replay->phaseN)return true;
        if(g_replay->clearTags[requiredPhase].empty())return true;
        Resolve();int found=0;bool ready=true;
        UObject* world=WorldOf(towerwidgets::Pawn());
        // Later phase spawners may stream in only during a native transition.
        // Require phase one before intro; Tick registers later ones as they arrive.
        auto visit=[&](UObject* sp){
            if(!ue::Live(sp) || ue::IsDefault(sp) || !spawnerCls_ || !ue::IsA(sp,spawnerCls_) || WorldOf(sp)!=world)return;
            int phase=-1;const auto name=ue::ObjName(sp);
            for(int i=0;i<g_replay->spawnerN;++i)if(name==g_replay->spawners[i]){phase=i;break;}
            if(phase<0)return;
            replaySp_.insert(sp);
            if(phase==requiredPhase)++found;
            std::string detail;
            if(!ReplayGroup(sp,&detail) && phase==requiredPhase){*why=detail;ready=false;}
        };
        // 先查已知的契约刷怪点（会话巡检 / 上次整表扫描记下的）。8071424–23161a7 实机：等开遭遇时每帧整表扫约 15 万对象，
        // 单帧 100–270 ms 的卡顿；转阶段接管时再一次。缓存不够唯一时才整表扫一次并补缓存
        if(!replaySp_.empty()){
            std::vector<UObject*> known(replaySp_.begin(),replaySp_.end());
            for(auto* sp:known)visit(sp);
            if(found==1)return ready;
        }
        found=0;ready=true;
        ue::ForEach([&](int,UObject* sp){visit(sp);return true;});
        if(found!=1){*why="阶段通知所需的原生刷怪点不唯一：第"+std::to_string(requiredPhase+1)+"阶段";return false;}
        return ready;
    }

    // 静默一个原生刷怪点：先收组（委托 + 脚本 + 清组阻断），再停刷 / 取消待刷，最后停用组
    void Quiet(UObject* sp) {
        if (!sp || !ue::Live(sp) || quieted_.count(sp)) return;
        if (ReplaySpawner(sp)) { Note("重放：保留原生刷怪点", ue::ObjName(sp), sp, "本层遭遇契约"); return; }
        Resolve();
        // 28 层大师兄：剑阵弟子的刷怪点和大师兄同在 MobGroup_0，场地静默把它们一起停掉 / 停用组 → 契约刷怪点失去战斗组、
        // 原生"全组阵亡才转阶段"也没了。重放层里与契约刷怪点同组的刷怪点（原生遭遇自己的组员）不静默
        if (g_replay && spGroupOff_ >= 0) {
            if (contractGroups_.empty() && GetTickCount64() >= contractGroupsNext_) {
                contractGroupsNext_ = GetTickCount64() + 1000;  // 契约刷怪点的组可能还没挂上：最多每秒整表找一次
                ue::ForEach([&](int, UObject* o) {
                    if (!ue::IsDefault(o) && ue::Live(o) && ReplaySpawner(o))
                        if (auto* cg = ue::Get<UObject*>(o + spGroupOff_); cg && ue::Live(cg)) contractGroups_.insert(cg);
                    return true;
                });
            }
            auto* og = ue::Get<UObject*>(sp + spGroupOff_);
            if (og && contractGroups_.count(og)) { Note("重放：保留契约同组刷怪点", ue::ObjName(sp), sp, "原生遭遇组员"); return; }
        }
        if (!on_ && offSessionLogs_++ < 3) pbz::log::Info("影塔会话隔离：会话外收到静默请求（{}），照做；出塔时随会话一并放回", ue::ObjName(sp));
        UObject* g = spGroupOff_ >= 0 ? ue::Get<UObject*>(sp + spGroupOff_) : nullptr;
        if (g && !ue::Live(g)) g = nullptr;
        if (g) { Mute(g); Block(g); }
        if (shouldSpawnOff_ >= 0) { const uint8_t no = 0; ue::Write(sp + shouldSpawnOff_, &no, 1); }
        if (cancel_) { uint8_t buf[16] = {}; pe::Call(sp, cancel_, buf); }
        if (g && deactivate_ && deactivated_.insert(g).second) { uint8_t buf[16] = {}; pe::Call(g, deactivate_, buf); }
        quieted_.insert(sp);
    }
    // 塔自己的生成兜底（运行库落点全被拒，改由 Boss 原生刷怪点刷）：这个刷怪点照样静默（组委托 / 脚本 / 清组阻断都保留），
    // 只放行它的过场 / 异步生成入口，并把 ShouldSpawn 放回 true。42 层蛊鹤 71ad9ff：场地静默后兜底的 ClaimSeqSpawn 被本拦截挡下
    void AllowTowerSpawn(UObject* sp) {
        if (!sp || !ue::Live(sp)) return;
        towerSpawn_.insert(sp);
        if (shouldSpawnOff_ >= 0) { const uint8_t yes = 1; ue::Write(sp + shouldSpawnOff_, &yes, 1); }
        pbz::log::Info("影塔会话隔离：塔兜底放行刷怪点 {}（ShouldSpawn 恢复，组隔离保留）", ue::ObjName(sp));
    }
    void EndTowerSpawn() {
        for (auto* sp : towerSpawn_) if (ue::Live(sp) && shouldSpawnOff_ >= 0 && quieted_.count(sp)) { const uint8_t no = 0; ue::Write(sp + shouldSpawnOff_, &no, 1); }
        towerSpawn_.clear();
    }
    void Block(UObject* g) {
        if (!g || !ue::Live(g) || blocked_.count(g) || !addBlocker_) return;
        uint8_t b[64] = {};
        UObject* owner = groupCdo_;
        if (addBlockerOwner_ >= 0) std::memcpy(b + addBlockerOwner_, &owner, 8);
        pe::Call(g, addBlocker_, b);
        blocked_.insert(g);
    }
    // 收组：清空"清组 / 成员死亡 / 成员退场 / 停用"四个多播委托（存下原表，出塔前放回），挂住它的组脚本副作用事件
    // 已核验的无奖励组脚本（剑阵这类原生阶段组整组放行时用）
    bool NativeGroupSafe(UObject* g, std::string* bad) {
        static constexpr std::string_view kSafe[] = {"MobGroupScript_CombatNotify_C", "MobGroupScript_LinkFogDoor_C", "MobGroupScript_PlayerCombatTags_C",
                                                     "GroupScript_Level_C", "MobGroupScript_BossCombat_JianZhen_Phase1_C", "MobGroupScript_BossCombat_JianZhen_Phase2_C"};
        if (scriptsOff_ < 0) { *bad = "脚本表偏移未知"; return false; }
        struct Arr { UObject** data; int32_t num, max; };
        const Arr list = ue::Get<Arr>(g + scriptsOff_);
        if (!list.data || list.num <= 0 || list.num > 32) { *bad = "脚本表异常"; return false; }
        for (int i = 0; i < list.num; ++i) {
            UObject* sc = ue::Get<UObject*>(list.data + i);
            if (!sc || !ue::Live(sc)) continue;
            const auto cn = ue::ClassName(sc);
            if (std::find(std::begin(kSafe), std::end(kSafe), cn) == std::end(kSafe)) { *bad = std::string(cn); return false; }
        }
        return true;
    }
    std::string GroupScripts(UObject* g) {
        if (scriptsOff_ < 0) return "?";
        struct Arr { UObject** data; int32_t num, max; };
        const Arr list = ue::Get<Arr>(g + scriptsOff_);
        std::string out;
        for (int i = 0; list.data && i < list.num && i < 32; ++i)
            if (UObject* sc = ue::Get<UObject*>(list.data + i); sc && ue::Live(sc)) out += (out.empty() ? "" : "/") + std::string(ue::ClassName(sc));
        return out;
    }
    void Unmute(UObject* g) {
        auto it = muted_.find(g);
        if (it == muted_.end() || !ue::Live(g)) return;
        for (int i = 0; i < kDelegates; ++i) if (delegateOff_[i] >= 0) ue::Write(g + delegateOff_[i], it->second.list[i], 16);
        muted_.erase(it);
    }
    void Mute(UObject* g) {
        if (!g || !ue::Live(g)) return;
        if (g_nativeGroups.count(g)) return;  // 原生阶段组整组放行（重放 nativeGroups）
        Resolve();
        if (!muted_.count(g)) {
            Saved s{};
            for (int i = 0; i < kDelegates; ++i) {
                if (delegateOff_[i] < 0) continue;
                std::memcpy(s.list[i], g + delegateOff_[i], 16);
                static const uint8_t zero[16] = {};
                ue::Write(g + delegateOff_[i], zero, 16);
            }
            s.name = ue::ObjName(g);
            muted_.emplace(g, s);
        }
        if (scriptsOff_ < 0) return;
        struct Arr { UObject** data; int32_t num, max; };
        const Arr list = ue::Get<Arr>(g + scriptsOff_);
        for (int i = 0; list.data && i < list.num && i < 32; ++i) {
            UObject* sc = ue::Get<UObject*>(list.data + i);
            if (sc && ue::Live(sc)) HookScriptClass(ue::ClassOf(sc));
        }
    }
    void Park(UObject* node) { if (node) parked_.insert(node); }
    void Note(const char* kind, const std::string& what, UObject* src, const char* verdict) {
        const std::string key = std::string(kind) + "|" + what;
        const int n = ++counts_[key];
        ++total_[kind];
        if (n <= 3)
            pbz::log::Info("影塔会话隔离：{} {}（{}）run={} floor={} src={} class={}{}", kind, what, verdict, run_.empty() ? "-" : run_, floor_,
                           src && ue::Live(src) ? ue::ObjName(src) : "-", src && ue::Live(src) ? ue::ClassName(src) : "-", n == 3 ? "；同类后续只计数" : "");
    }

    std::string Status() const {
        std::string t;
        for (auto& [k, v] : total_) t += std::format("{}\"{}\":{}", t.empty() ? "" : ",", pbz::ipc::Esc(k), v);
        return std::format(R"("session":{{"on":{},"muted_groups":{},"blocked_groups":{},"quiet_spawners":{},"hooked_scripts":{},"parked_flow":{},"refused":{{{}}}}})",
                           on_ ? "true" : "false", muted_.size(), blocked_.size(), quieted_.size(), hookedScripts_.size(), parked_.size(), t);
    }

private:
    static constexpr int kDelegates = 4;
    struct Saved { uint8_t list[kDelegates][16]; std::string name; };  // name：收组时的对象名，出塔放回前核同一个组

    void Resolve() {
        if (resolved_) return;
        resolved_ = true;
        groupCls_ = towerperf::ClassNamed("MobGroup");
        scriptCls_ = towerperf::ClassNamed("MobGroupScript");
        spawnerCls_ = towerperf::ClassNamed("AISpawner");
        if (groupCls_) {
            const char* names[kDelegates] = {"OnMobGroupCleared", "OnMobGroupMemberDead", "OnMobGroupMemberDespawn", "OnMobGroupDeactivatedDelegate"};
            for (int i = 0; i < kDelegates; ++i) delegateOff_[i] = ue::PropOffset(groupCls_, names[i]);
            scriptsOff_ = ue::PropOffset(groupCls_, "Scripts");
            addBlocker_ = pe::FindFunction(groupCls_, "AddClearBlocker");
            removeBlocker_ = pe::FindFunction(groupCls_, "RemoveClearBlocker");
            deactivate_ = pe::FindFunction(groupCls_, "Deactivate");
            if (addBlocker_) addBlockerOwner_ = ue::PropOffset(addBlocker_, "Owner");
            if (removeBlocker_) { rmOwner_ = ue::PropOffset(removeBlocker_, "Owner"); rmEval_ = ue::PropOffset(removeBlocker_, "bEvaluateClear"); }
            ue::ForEach([&](int, UObject* o) {
                if (ue::ClassOf(o) == groupCls_ && ue::IsDefault(o)) { groupCdo_ = o; return false; }
                return true;
            });
        }
        if (spawnerCls_) {
            spGroupOff_ = ue::PropOffset(spawnerCls_, "MobGroup");
            spClassOff_ = ue::PropOffset(spawnerCls_, "Class");
            spSpawnedOff_ = ue::PropOffset(spawnerCls_, "SpawnedAI");
            shouldSpawnOff_ = ue::PropOffset(spawnerCls_, "ShouldSpawn");
            cancel_ = pe::FindFunction(spawnerCls_, "CancelPendingSpawnCompletely");
        }
        for (auto& b : tower::kBossPool) {
            const auto dot = b.path.rfind('.');
            if (dot != std::string_view::npos) bossClasses_.insert(std::string(b.path.substr(dot + 1)));
        }
        pbz::log::Info("影塔会话隔离：战斗组 {}（委托 {}/{}/{}/{} 脚本表 {} 阻断 {}/{}），组脚本基类 {}，刷怪点 {}（组 {} 类 {} 停刷 {} 取消 {}），Boss 类 {} 个",
                       !!groupCls_, delegateOff_[0], delegateOff_[1], delegateOff_[2], delegateOff_[3], scriptsOff_, !!addBlocker_, !!removeBlocker_, !!scriptCls_,
                       !!spawnerCls_, spGroupOff_, spClassOff_, shouldSpawnOff_, !!cancel_, bossClasses_.size());
    }

    void Begin() {
        Resolve();
        on_ = true;
        cursor_ = 0;
        firstPass_ = true;
        towerinteractions::Begin();
        if (scriptCls_) HookScriptClass(scriptCls_);  // 原生 C++ 组脚本（PBZMobGroupScript_KillLoot 等）走基类的事件函数
        // 原生背包入账
        if (auto* bag = towerperf::ClassNamed("PBZBackPackSubsystem")) {
            struct Hook { const char* fn; const char* id; };
            for (const Hook h : {Hook{"AddWeaponItem", "WeaponID"}, Hook{"AddPropsToBackPack", "ItemId"}, Hook{"AddPropsByMap", nullptr}, Hook{"AddPropsSilently", nullptr},
                                 Hook{"AddSkin", "SkinID"}, Hook{"AddCompleteAddItemMobDomainName", "DomainName"}}) {
                UObject* f = pe::FindFunction(bag, h.fn);
                if (!f) continue;
                const int idOff = h.id ? ue::PropOffset(f, h.id) : -1, loadOff = ue::PropOffset(f, "bIsAddByLoadData");
                const std::string name = h.fn;
                auto judge = [this, name, idOff, loadOff](UObject* self, uint8_t* p, const char* route) {
                    const bool load = loadOff >= 0 && p && p[loadOff] != 0;
                    if (JudgeGrant(on_, granting > 0, load, Travelling()) == Grant::Allow) return false;
                    const std::string id = idOff >= 0 && p ? ue::Name(ue::Get<uint32_t>(p + idOff)) : std::string("（表）");
                    Note("拒绝原生入账", name + " " + id + "（" + route + "）", self, "非塔发放");
                    return true;
                };
                pe::Intercept(f, [judge](UObject* self, void* raw) { return judge(self, static_cast<uint8_t*>(raw), "ProcessEvent"); });
                grantHooks_.push_back(f);
                // 原生执行入口（33-38：脚本 / 反射直接调用不经 ProcessEvent）：只挂前三个，原生执行入口槽位有限（共 24，点数已用 15）
                if (name == "AddWeaponItem" || name == "AddPropsToBackPack" || name == "AddPropsByMap") NxHook(f, [judge](UObject* self, uint8_t* p, void*) { return judge(self, p, "原生执行入口"); });
            }
        }
        // 原生刷怪点的过场 / 异步生成入口（33 层：刷怪点已停刷 10 秒后，独眼的出场过场 Seq082 照样经 SpawnFromSequence / ClaimSeqSpawn
        // 刷出原生独眼——第二条血条 + 运镜；ShouldSpawn 只管普通刷怪）：会话中，已静默的、或刷 Boss 池类的刷怪点一律不生成
        if (spawnerCls_) {
            for (const char* fn : {"SpawnFromSequence", "ClaimSeqSpawn", "RequestSeqPrekickSpawn", "BeginAsyncSpawn"}) {
                UObject* f = pe::FindFunction(spawnerCls_, fn);
                if (!f) continue;
                const std::string name = fn;
                auto judge = [this, name](UObject* sp, const char* route) {
                    if (!on_ || !sp || !ue::Live(sp)) return false;
                    if (ReplaySpawner(sp)) { Note("重放：放行原生刷怪点生成", name + "（" + route + "）", sp, "本层遭遇契约"); return false; }
                    if (towerSpawn_.count(sp)) { Note("塔兜底：放行原生刷怪点生成", name + "（" + route + "）", sp, "塔自己请求的 Boss 生成"); return false; }
                    if (!quieted_.count(sp)) {
                        const std::string c = spClassOff_ >= 0 ? ue::Name(ue::Get<uint32_t>(sp + spClassOff_ + 0x10)) : std::string();
                        if (!bossClasses_.count(c)) return false;  // 普通杂兵 / 场地机制用的刷怪点不动
                        Quiet(sp);
                    }
                    Note("拦下原生刷怪点生成", name + "（" + route + "）", sp, "已静默的刷怪点：过场 / 异步生成也不放");
                    return true;
                };
                pe::Intercept(f, [judge](UObject* sp, void*) { return judge(sp, "ProcessEvent"); });
                grantHooks_.push_back(f);
                if (name != "RequestSeqPrekickSpawn") NxHook(f, [judge](UObject* sp, uint8_t*, void*) { return judge(sp, "原生执行入口"); });
            }
        }
        // 已静默战斗组的"玩家进入领域"：原生遭遇从这里开（出场过场、组激活）；塔 Boss 的仇恨由塔自己的入场流程给，不靠它
        if (groupCls_) {
            for (const char* fn : {"OnActorEnterDomain", "OnActorExitDomain"}) {
                UObject* f = pe::FindFunction(groupCls_, fn);
                if (!f) continue;
                const std::string name = fn;
                pe::Intercept(f, [this, name](UObject* g, void*) {
                    if (!on_ || !deactivated_.count(g)) return false;
                    Note("拦下已静默战斗组的领域事件", name, g, "原生遭遇不再开");
                    return true;
                });
                grantHooks_.push_back(f);
            }
        }
        // Flow 过场的完成回调：被冻结的过场不让 Flow 往下走
        if (auto* node = towerperf::ClassNamed("FlowNode_PlayLevelSequence")) {
            if ((finished_ = pe::FindFunction(node, "OnPlaybackFinished")))
                pe::Intercept(finished_, [this](UObject* n, void*) {
                    if (!on_ || !parked_.count(n)) return false;
                    Note("吞掉过场完成回调", ue::ObjName(n), n, "Flow 停在本节点");
                    return true;
                });
        }
        if (auto* fs = towerperf::ClassNamed("FlowSubsystem")) {
            if (auto* f = pe::FindFunction(fs, "StartRootFlow")) {
                const int own = ue::PropOffset(f, "Owner"), asset = ue::PropOffset(f, "FlowAsset");
                flowObs_ = pe::Observe(f, [this, own, asset](UObject*, void* raw) {
                    auto* p = static_cast<uint8_t*>(raw);
                    UObject* o = own >= 0 ? ue::Get<UObject*>(p + own) : nullptr;
                    UObject* a = asset >= 0 ? ue::Get<UObject*>(p + asset) : nullptr;
                    Note("Flow 图启动（只记录）", a && ue::Live(a) ? ue::ObjName(a) : "?", o, Travelling() ? "传送途中" : "放行：原生组副作用另由组隔离收住");
                });
            }
        }
        pbz::log::Info("影塔会话隔离：开始（run={}），背包入账拦截 {} 个，过场完成回调 {}，组脚本基类 {}", run_.empty() ? "-" : run_, grantHooks_.size(), !!finished_, !!scriptCls_);
    }

    void End() {
        on_ = false;
        replaySp_.clear();
        if(replayDeadObs_)pe::Unobserve(replayDeadObs_);
        if(replayClearObs_)pe::Unobserve(replayClearObs_);
        replayDeadObs_=replayClearObs_=0;replayDeadFn_=replayClearFn_=nullptr;
        towergroup::Disarm(); // original group bindings below remain the restoration source
        towerinteractions::End();
        for (auto* f : grantHooks_) pe::RemoveIntercept(f);
        grantHooks_.clear();
        for (auto* f : nxHooks_) nx::Unhook(f);
        nxHooks_.clear();
        if (finished_) { pe::RemoveIntercept(finished_); finished_ = nullptr; }
        if (flowObs_) { pe::Unobserve(flowObs_); flowObs_ = 0; }
        for (auto* f : hookedScripts_) pe::RemoveIntercept(f);
        hookedScripts_.clear();
        // 放回委托：会话中没有新绑定才放回原表（有新绑定就保留新的、记日志；塔档世界出塔时整个重载）
        int restored = 0, kept = 0, stale = 0, faulted = 0;
        auto same = [this](UObject* g) {  // 还是收组时那个组：活着、是组类、名字没变（地址复用的对象不碰）
            if (!ue::Live(g) || !groupCls_ || !ue::IsA(g, groupCls_)) return false;
            const auto it = muted_.find(g);
            return it == muted_.end() || it->second.name == ue::ObjName(g);
        };
        for (auto& [g, s] : muted_) {
            if (!ue::Live(g)) continue;
            if (!same(g)) { ++stale; continue; }
            for (int i = 0; i < kDelegates; ++i) {
                if (delegateOff_[i] < 0) continue;
                uint8_t now[16];
                std::memcpy(now, g + delegateOff_[i], 16);
                static const uint8_t zero[16] = {};
                if (std::memcmp(now, zero, 16) == 0) { ue::Write(g + delegateOff_[i], s.list[i], 16); ++restored; }
                else ++kept;
            }
        }
        int unblocked = 0;
        for (auto* g : blocked_) {
            if (!ue::Live(g) || !removeBlocker_) continue;
            if (!same(g)) { ++stale; continue; }
            uint8_t b[64] = {};
            UObject* owner = groupCdo_;
            if (rmOwner_ >= 0) std::memcpy(b + rmOwner_, &owner, 8);
            if (rmEval_ >= 0) b[rmEval_] = 0;  // 不重新判定清空：不补发清组结果
            if (GuardedCall(g, removeBlocker_, b)) ++unblocked;
            else { ++faulted; pbz::log::Info("影塔会话隔离：撤清组阻断时原生异常（{}），跳过这一项、继续出塔", ue::ObjName(g)); }
        }
        if (stale || faulted) pbz::log::Info("影塔会话隔离：出塔清理跳过 {} 个已失效 / 地址复用的组，{} 个原生调用异常", stale, faulted);
        std::string t;
        for (auto& [k, v] : total_) t += std::format("{}{} {}", t.empty() ? "" : "，", k, v);
        pbz::log::Info("影塔会话隔离：结束（run={}），放回委托 {} 个（{} 个有新绑定保留），撤清组阻断 {} 个，静默刷怪点 {} 个；{}", run_.empty() ? "-" : run_, restored, kept,
                       unblocked, quieted_.size(), t.empty() ? "没有拦下原生副作用" : t);
        towerSpawn_.clear();
        muted_.clear(); blocked_.clear(); quieted_.clear(); deactivated_.clear(); parked_.clear(); counts_.clear(); total_.clear(); seenCls_.clear(); hookedCls_.clear();
    }

    void NxHook(UObject* f, nx::Handler h) {
        std::string why;
        if (nx::Hook(f, std::move(h), &why)) nxHooks_.push_back(f);
        else pbz::log::Info("影塔会话隔离：原生执行入口 {} 没挂上（{}），只靠 ProcessEvent 拦截", ue::ObjName(f), why);
    }
    void HookScriptClass(UObject* cls) {
        if (!cls || !hookedCls_.insert(cls).second) return;
        for (const char* name : {"OnGroupCleared", "OnGroupMemberDead", "OnGroupMemberDespawn", "OnGroupDeactivated"}) {
            UObject* f = pe::FindFunction(cls, name);
            if (!f || std::find(hookedScripts_.begin(), hookedScripts_.end(), f) != hookedScripts_.end()) continue;
            const std::string fn = name;
            pe::Intercept(f, [this, fn](UObject* script, void*) {
                if (!on_ || !SideEffectEvent(fn)) return false;
                if (!g_nativeGroups.empty() && script && ue::Live(script)) {  // 原生阶段组的脚本照原生执行
                    const int mg = ue::PropOffset(ue::ClassOf(script), "MobGroup");
                    if (mg >= 0 && g_nativeGroups.count(ue::Get<UObject*>(script + mg))) return false;
                }
                Note("吞掉原生组脚本", fn, script, "组副作用（发兵器 / 掉落 / 清组剧情）");
                return true;
            });
            hookedScripts_.push_back(f);
        }
    }

    // 增量巡检（每帧 4096 个对象）：新加载的战斗组先收；刷 Boss 池类的刷怪点静默（迟发的原生 Boss）
    void Tick() {
        towerperf::Section _("会话隔离巡检");
        towerinteractions::Tick();
        std::string milestoneWhy;
        if(!towergroup::Refresh(&milestoneWhy) && groupWhy_!=milestoneWhy){groupWhy_=milestoneWhy;Note("重放阶段通知已撤回",milestoneWhy,nullptr,"阶段标签/绑定校验失败");}
        if (!groupCls_ && !spawnerCls_) return;
        // 预算（33 层实测稳定战斗约 100 ms/秒）：第一整轮每帧 4096 个对象（尽快收住新加载的组 / 刷怪点），之后每帧 1024 个；
        // 刷怪点的资产名按 FName 下标缓存是否 Boss 类，不再每轮查字符串
        static UObject* batch[4096];
        const int n = ue::Num();
        const int step = firstPass_ ? 4096 : 1024;
        const int got = ue::ObjBatch(cursor_, step, batch);
        int processed=0;const auto scanBegin=towerperf::Qpc();
        for (int i = 0; i < got; ++i) {
            if(i && !(i%128) && towerperf::Ms(towerperf::Qpc()-scanBegin)>(firstPass_?0.35:0.2))break;  // 首轮快收，之后每帧最多 0.2 ms
            ++processed;
            UObject* o = batch[i];
            if (!o || ue::IsDefault(o)) continue;
            UObject* cls = ue::ClassOf(o);
            auto [it, fresh] = seenCls_.try_emplace(cls, 0);
            if (fresh) it->second = groupCls_ && ue::IsA(o, groupCls_) ? 1 : spawnerCls_ && ue::IsA(o, spawnerCls_) ? 2 : 0;
            if (it->second == 1 && !muted_.count(o) && ue::Live(o)) Mute(o);
            else if (it->second == 2 && !quieted_.count(o) && spClassOff_ >= 0 && ue::Live(o)) {
                // Quiet 对契约刷怪点会直接返回；调用方也必须结束本次扫描，不能再记成“已静默”。
                if (ReplaySpawner(o)) {
                    replaySp_.insert(o);
                    Quiet(o);
                    std::string why;if(!ReplayGroup(o,&why) && groupWhy_!=why){groupWhy_=why;Note("重放阶段通知未就绪",why,o,"当前阶段准入将拒绝开战");}
                    continue;
                }
                const uint32_t idx = ue::Get<uint32_t>(o + spClassOff_ + 0x10);  // 软类引用的资产名（BP_X_C），见 NativeGroupFor
                auto [b, newName] = bossName_.try_emplace(idx, false);
                if (newName) b->second = bossClasses_.count(ue::Name(idx)) > 0;
                if (!b->second) continue;
                const std::string c = ue::Name(idx);
                UObject* ai = spSpawnedOff_ >= 0 ? ue::Get<UObject*>(o + spSpawnedOff_) : nullptr;
                Quiet(o);
                Note("静默原生 Boss 刷怪点", c, o, ai && ue::Live(ai) ? "已刷出的原生 Boss 交给清场巡检" : "停刷 / 取消待刷");
            }
        }
        cursor_ += got?processed:step;
        if (cursor_ >= n) { cursor_ = 0; seenCls_.clear(); firstPass_ = false; }
    }

    bool resolved_ = false, on_ = false, firstPass_ = true;
    std::unordered_map<uint32_t, bool> bossName_;  // 刷怪点资产名 FName 下标 → 是否 Boss 池类
    int floor_ = 0, cursor_ = 0, flowObs_ = 0, offSessionLogs_ = 0;
    std::string run_;
    UObject *groupCls_ = nullptr, *scriptCls_ = nullptr, *spawnerCls_ = nullptr, *groupCdo_ = nullptr;
    UObject *addBlocker_ = nullptr, *removeBlocker_ = nullptr, *deactivate_ = nullptr, *cancel_ = nullptr, *finished_ = nullptr;
    int delegateOff_[kDelegates] = {-1, -1, -1, -1};
    int scriptsOff_ = -1, addBlockerOwner_ = -1, rmOwner_ = -1, rmEval_ = -1, spGroupOff_ = -1, spClassOff_ = -1, spSpawnedOff_ = -1, shouldSpawnOff_ = -1;
    std::unordered_map<UObject*, Saved> muted_;
    std::unordered_set<UObject*> towerSpawn_;  // 塔兜底放行的刷怪点（本层）
    std::unordered_set<UObject*> blocked_, quieted_, deactivated_, parked_, hookedCls_;
    std::vector<UObject*> hookedScripts_, grantHooks_, nxHooks_;
    std::unordered_map<UObject*, int> seenCls_;
    std::set<std::string> bossClasses_;
    std::unordered_map<std::string, int> counts_;
    std::string groupWhy_;
    std::unordered_set<UObject*> replaySp_;  // 本契约的原生刷怪点（准入缓存，换契约 / 会话结束清空）
    std::unordered_set<UObject*> contractGroups_;  // 契约刷怪点所在的原生战斗组（同组刷怪点不静默）
    unsigned long long contractGroupsNext_ = 0;
    int replayDeadObs_=0,replayClearObs_=0;
    UObject* replayDeadFn_=nullptr;UObject* replayClearFn_=nullptr;
    std::map<std::string, int> total_;
};

SessionGuard g_session;

// 剧情冻结（用户 2026-11-01：有剧情的场地和 Boss 一律冻结剧情，开挑战后倒计时直接开打；Boss 依赖场地、
// 双 / 三 Boss 配合的原生逻辑要保留）。313987b 白赤练反例分析（静态）：
//   - 旧判据靠播放设置的猜测位 + 名字（cg / seq_ …），未经标定；场地 Flow 图（FlowNode_PlayLevelSequence）在 C++ 里直接 Play，
//     不经 ProcessEvent，名字也不像剧情 → 巡检判为"不像剧情"，没日志、没拦。
//   - 旧做法拦下 Play 等于让 Flow 图永远等"Completed"：Boss 的阶段逻辑若在等这段剧情结束，会卡住（白赤练浮空、混沌停手）。
// 新规则：
//   1. Flow 图放的过场（FlowLevelSequencePlayer）= 剧情；本层期间开始播的非循环过场、且不属于塔生成角色 = 剧情。
//      循环播放的环境过场、本层开始前就在播的普通过场不碰。塔 Boss 自己的演出（Outer / Owner / Instigator 是塔生成的）放行。
//   2. 入口冻结优先：实际游戏蓝图 PlayLevelSequence_C 由 tower_storygate 在 K2_ExecuteInput 的“播放”输入前拦下，
//      不让镜头/输入/异步生成先启动。播放器 Play/巡检仍作兜底；不补完成回调，不推到原生奖励/任务/换图。
//   3. 塔 Boss 归入场地的原生战斗组（同类 AISpawner 所在的 MobGroup）：场地按组驱动的机制、双 / 三 Boss 的组内配合
//      （MetaAI、GetOtherAliveMembersInGroup）照常；组加清组阻断，打完不触发清组剧情 / 掉落脚本；本层结束移出。
class Isolation {
public:
    void Begin(std::string_view venue) {
        End();
        venue_ = venue;
        active_ = true;
        tookStory_ = false;
        startedAt_ = GetTickCount64();
        players_.clear();
        skipped_.clear();
        cursor_ = 0;
        firstPass_ = true;
        skips_ = 0;
        ResolveOnce();
        for (auto* f : {play_.fn, playTo_.fn})
            if (f) pe::Intercept(f, [this](UObject* obj, void*) { return OnPlay(obj); });
        // 33 层 Seq082 是"巡检发现正在播放"——Play 没经 ProcessEvent。原生执行入口也挂上（蓝图字节码直调仍只能靠巡检）
        if (play_.fn && !playNx_) {
            std::string why;
            playNx_ = nx::Hook(play_.fn, [this](UObject* obj, uint8_t*, void*) { return OnPlay(obj); }, &why);
            if (!playNx_) pbz::log::Info("影塔隔离：过场 Play 的原生执行入口没挂上（{}）", why);
        }
        if (cinematic_.fn)
            cineObs_ = pe::Observe(cinematic_.fn, [this](UObject* pc, void* params) { OnCinematic(pc, params); });
        pbz::log::Info("影塔隔离：本层开始（场地 {}），剧情跳过 {}，Flow 过场 {}，清组阻断 {}，战斗组归属 {}", venue_.empty() ? "原地" : venue_,
                       skip_.fn ? "可用" : "不可用", flowPlayerCls_ ? "可识别" : "不可识别", addBlocker_.fn ? "可用" : "不可用", addMember_.fn ? "可用" : "不可用");
    }

    void End() {
        if (!active_) return;
        active_ = false;
        for (auto* f : {play_.fn, playTo_.fn})
            if (f) pe::RemoveIntercept(f);
        if (playNx_) { nx::Unhook(play_.fn); playNx_ = false; }
        if (cineObs_) { pe::Unobserve(cineObs_); cineObs_ = 0; }
        // 塔 Boss 移出原生战斗组（还活着的）
        for (auto& [pawn, group] : members_)
            if (ue::Live(pawn) && ue::Live(group) && removeMember_.fn && groupCls_ && ue::IsA(group, groupCls_)) {
                std::vector<uint8_t> a(removeMember_.size);
                if (removeMember_.args[0].second >= 0) std::memcpy(a.data() + removeMember_.args[0].second, &pawn, 8);
                if (!GuardedCall(group, removeMember_.fn, a.data())) pbz::log::Info("影塔隔离：塔 Boss 移出原生战斗组时原生异常（{}），跳过", ue::ObjName(group));
            }
        if (!members_.empty()) pbz::log::Info("影塔隔离：本层结束，{} 个塔 Boss 移出原生战斗组", members_.size());
        members_.clear(); nativeMembers_.clear();
        // 清组阻断、委托、组脚本留在会话层，到出塔事务开始才撤（旧做法本层结束就撤，取消后原生清组结果可能补发）
        if (skips_) pbz::log::Info("影塔隔离：本层共跳过剧情过场 {} 段", skips_);
        blocked_.clear();
        players_.clear();
        skipped_.clear();
    }

    void Tick(float) {
        towerperf::Section _("剧情隔离");
        if (!active_ || !seqPlayerCls_ || statusOff_ < 0) return;
        const bool travelling = Travelling();
        // 增量扫对象表找过场播放器（每帧 16384 个），每 0.1 秒看一次它们的播放状态
        static UObject* batch[8192];
        const int n = ue::Num();
        const int step = firstPass_ ? 8192 : 2048;  // 预算：本层第一整轮快扫，之后放慢（33 层稳定战斗约 40 ms/秒）
        const int got = ue::ObjBatch(cursor_, step, batch);
        int processed=0;const auto scanBegin=towerperf::Qpc();
        for (int i = 0; i < got; ++i) {
            if(i && !(i%128) && towerperf::Ms(towerperf::Qpc()-scanBegin)>(firstPass_?0.35:0.2))break;  // 首轮快收，之后每帧最多 0.2 ms
            ++processed;
            if (!batch[i]) continue;
            UObject* cls = ue::ClassOf(batch[i]);
            auto [it, fresh] = isPlayerCls_.try_emplace(cls, false);
            if (fresh) it->second = cls && ue::IsA(batch[i], seqPlayerCls_);  // 每个类只走一次父类链
            if (it->second && !ue::IsDefault(batch[i]) && !players_.count(batch[i])) {
                // 第一次看到：记下当时是否已在播（非 Flow 的、本层开始前就在播的算环境过场）
                players_[batch[i]] = {Playing(batch[i]), false};
            }
        }
        cursor_ += got?processed:step;
        if (cursor_ >= n) { cursor_ = 0; isPlayerCls_.clear(); firstPass_ = false; }  // 类可能随关卡卸载、地址复用：每轮重判
        const auto now = GetTickCount64();
        if (now - lastCheck_ < 100) return;
        lastCheck_ = now;
        for (auto it = players_.begin(); it != players_.end();) {
            UObject* p = it->first;
            if (!ue::Live(p)) { it = players_.erase(it); continue; }
            auto& seen = it->second;
            ++it;
            const bool playing = Playing(p);
            if (!playing) { seen.ambient = false; continue; }  // 停过：之后再开始播就按本层新开始的判
            if (auto s = skipped_.find(p); s != skipped_.end()) {  // 跳过后仍在播：这里的原生剧情没能冻结
                if (now - s->second > 1000 && !seen.failed) { seen.failed = true; Failed(p); }
                continue;
            }
            if (travelling || !Narrative(p, seen.ambient)) continue;
            if (tower::replay::AllowSequence(g_replay, SeqName(p))) continue;  // 重放层放行的过场
            Skip(p, "巡检发现正在播放");
        }
    }

    // 重放：这个 Actor 是否属于契约的原生阶段组（剑阵弟子这类原生遭遇组员）
    bool NativeMember(UObject* actor) {
        if (g_nativeGroups.empty() || !active_ || !resolveGroup_.fn || !groupCdo_) return false;
        if (resolveGroup_.args[0].second < 0 || resolveGroup_.ret < 0) return false;
        std::vector<uint8_t> a(resolveGroup_.size);
        std::memcpy(a.data() + resolveGroup_.args[0].second, &actor, 8);
        pe::Call(groupCdo_, resolveGroup_.fn, a.data());
        return g_nativeGroups.count(ue::Get<UObject*>(a.data() + resolveGroup_.ret)) > 0;
    }
    void BlockGroupOf(UObject* actor) {
        if (!active_ || !resolveGroup_.fn || !addBlocker_.fn || !groupCdo_) return;
        std::vector<uint8_t> a(resolveGroup_.size);
        if (resolveGroup_.args[0].second < 0 || resolveGroup_.ret < 0) return;
        std::memcpy(a.data() + resolveGroup_.args[0].second, &actor, 8);
        pe::Call(groupCdo_, resolveGroup_.fn, a.data());
        auto* g = ue::Get<UObject*>(a.data() + resolveGroup_.ret);
        if (g_nativeGroups.count(g)) return;  // 28 层：清掉红衣女剑客时把剑阵一阶段组也加了清组阻断；原生阶段组不阻断
        Block(g);
    }

    // 塔 Boss 归入场地原生战斗组：先找同类 AISpawner（本世界、离 Boss 150 米内）挂的组；
    // 找不到时并入本层已归组的第一个组（双 / 三 Boss 同组配合）。都没有就各自独立（记日志）
    void Adopt(UObject* pawn) {
        if (!active_ || !pawn || !ue::Live(pawn) || !addMember_.fn || members_.count(pawn) || nativeMembers_.count(pawn)) return;
        UObject* group = NativeGroupFor(pawn);
        const char* how = "同类原生刷怪点所在的组";
        if (!group) {
            for (auto& [p, g] : members_) if (ue::Live(g)) { group = g; how = "本层主 Boss 的组（多 Boss 配合）"; break; }
        }
        if (!group) {
            pending_.push_back(pawn);
            pbz::log::Info("影塔隔离：{} 没有对应的原生战斗组，暂不归组", ue::ClassName(pawn));
            return;
        }
        // 重放 nativeGroups（28 层大师兄）：Boss 自己的阶段组按原生运行——不收组、不加清组阻断，组清空才写 zddy.击杀.剑阵1、
        // 原生 Death Flow 才播转阶段 Seq077（2026-11-05 ui6：接管一阶段 Boss 时把 MobGroup_1 收住，一阶段死后卡住不转阶段）
        if (g_nativeGroups.count(group)) {
            if (nativeMembers_.insert(pawn).second) pbz::log::Info("影塔隔离：{} 的原生阶段组 {} 按原生运行，不收组", ue::ClassName(pawn), ue::ObjName(group));
            return;
        }
        Join(pawn, group, how);
        // 先到的没归上组的同层 Boss：并进来
        for (auto* p : pending_) if (ue::Live(p) && !members_.count(p)) Join(p, group, "本层主 Boss 的组（多 Boss 配合）");
        pending_.clear();
    }

    bool Unsafe(std::string_view venue) {
        if (venue.empty()) return false;
        LoadUnsafe();
        return unsafe_.count(std::string(venue)) > 0;
    }

    bool TookStory() const { return tookStory_; }

    std::string Status() {
        return std::format(R"("isolation":{{"active":{},"venue":"{}","blocked_groups":{},"members":{},"players":{},"skipped":{},"took_story":{},"last":"{}"}})",
                           active_ ? "true" : "false", pbz::ipc::Esc(venue_), blocked_.size(), members_.size(), players_.size(), skips_,
                           tookStory_ ? "true" : "false", pbz::ipc::Esc(last_));
    }

private:
    struct Seen { bool ambient = false, failed = false; };

    void ResolveOnce() {
        if (resolved_) return;
        resolved_ = true;
        seqPlayerCls_ = towerperf::ClassNamed("MovieSceneSequencePlayer");
        flowPlayerCls_ = towerperf::ClassNamed("FlowLevelSequencePlayer");
        if (seqPlayerCls_) {
            statusOff_ = ue::PropOffset(seqPlayerCls_, "Status");
            sequenceOff_ = ue::PropOffset(seqPlayerCls_, "Sequence");
            settingsOff_ = ue::PropOffset(seqPlayerCls_, "PlaybackSettings");
        }
        if (auto* st = towerperf::ClassNamed("MovieSceneSequencePlaybackSettings")) loopOff_ = ue::PropOffset(st, "LoopCount");
        if (flowPlayerCls_) receiverOff_ = ue::PropOffset(flowPlayerCls_, "FlowEventReceiver");
        if (auto* node = towerperf::ClassNamed("FlowNode_PlayLevelSequence")) {
            nodePlayerOff_ = ue::PropOffset(node, "SequencePlayer");
            finished_ = Resolve(node, "OnPlaybackFinished", {});
        }
        play_ = Resolve(seqPlayerCls_, "Play", {});
        playTo_ = Resolve(seqPlayerCls_, "PlayTo", {});
        skip_ = Resolve(seqPlayerCls_, "Stop", {}); // never evaluate the ending frame or its events as a fallback
        if (auto* pc = towerperf::ClassNamed("PlayerController")) cinematic_ = Resolve(pc, "SetCinematicMode", {"bCinematicMode"});
        if (auto* fs = towerperf::ClassNamed("FlowSubsystem")) startFlow_ = Resolve(fs, "StartRootFlow", {"Owner", "FlowAsset"});
        actorCls_ = towerperf::ClassNamed("Actor");
        if (actorCls_) { ownerOff_ = ue::PropOffset(actorCls_, "Owner"); instigatorOff_ = ue::PropOffset(actorCls_, "Instigator"); }
        spawnerCls_ = towerperf::ClassNamed("AISpawner");
        if (spawnerCls_) { spawnerClassOff_ = ue::PropOffset(spawnerCls_, "Class"); spawnerGroupOff_ = ue::PropOffset(spawnerCls_, "MobGroup"); }
        if (auto* actor = towerperf::ClassNamed("Actor")) where_ = Resolve(actor, "K2_GetActorLocation", {});
        groupCls_ = towerperf::ClassNamed("MobGroup");
        resolveGroup_ = Resolve(groupCls_, "ResolveMobGroupForActor", {"Actor"});
        addBlocker_ = Resolve(groupCls_, "AddClearBlocker", {"Owner"});
        removeBlocker_ = Resolve(groupCls_, "RemoveClearBlocker", {"Owner", "bEvaluateClear"});
        addMember_ = Resolve(groupCls_, "AddRuntimeMember", {"Pawn"});
        removeMember_ = Resolve(groupCls_, "RemoveRuntimeMember", {"Pawn"});
        if (groupCls_)
            ue::ForEach([&](int, UObject* o) {
                if (ue::ClassOf(o) == groupCls_ && ue::IsDefault(o)) { groupCdo_ = o; return false; }
                return true;
            });
        pbz::log::Info("影塔隔离：过场播放器 {}（状态 {} 循环 {}），Flow 过场 {}（回执 {} 完成 {}），刷怪点 {}（类 {} 组 {}），战斗组 {}（查组 {} 阻断 {}/{} 成员 {}/{}）",
                       !!seqPlayerCls_, statusOff_, loopOff_, !!flowPlayerCls_, receiverOff_, !!finished_.fn, !!spawnerCls_, spawnerClassOff_, spawnerGroupOff_,
                       !!groupCdo_, !!resolveGroup_.fn, !!addBlocker_.fn, !!removeBlocker_.fn, !!addMember_.fn, !!removeMember_.fn);
    }

    // 阻断的持有者：MobGroup 默认对象（常驻、不随关卡释放）
    UObject* Owner() const { return groupCdo_; }

    bool Playing(UObject* p) const { return ue::Get<uint8_t>(p + statusOff_) == 1; }  // EMovieScenePlayerStatus：1 = Playing
    bool Looping(UObject* p) const {
        if (settingsOff_ < 0 || loopOff_ < 0) return false;
        return ue::Get<int32_t>(p + settingsOff_ + loopOff_) != 0;  // LoopCount.Value：0 播一次，-1 无限循环
    }
    bool FlowDriven(UObject* p) const { return flowPlayerCls_ && ue::IsA(p, flowPlayerCls_); }

    std::string SeqName(UObject* player) {
        UObject* seq = sequenceOff_ >= 0 ? ue::Get<UObject*>(player + sequenceOff_) : nullptr;
        return seq && ue::Live(seq) ? ue::ObjName(seq) : std::string("?");
    }
    // 放这段过场的 Flow 图（证据：哪张场地图在推剧情）
    std::string FlowName(UObject* player) {
        if (receiverOff_ < 0 || !FlowDriven(player)) return {};
        UObject* node = ue::Get<UObject*>(player + receiverOff_);
        UObject* asset = node && ue::Live(node) ? ue::Get<UObject*>(node + ue::Layout::ObjOuter) : nullptr;
        return asset && ue::Live(asset) ? ue::ObjName(asset) : std::string("?");
    }

    // 塔生成的角色自己的演出：Outer 链上有塔生成的 Actor，或过场 Actor 的 Owner / Instigator 是它们
    bool OwnedByTower(UObject* player) {
        const auto owned = pbz::enemies::OwnedActors();
        if (owned.empty()) return false;
        auto ours = [&](UObject* o) { return o && std::find(owned.begin(), owned.end(), o) != owned.end(); };
        UObject* o = player;
        for (int depth = 0; o && depth < 8; ++depth, o = ue::Get<UObject*>(o + ue::Layout::ObjOuter)) {
            if (ours(o)) return true;
            if (ownerOff_ >= 0 && depth > 0 && actorCls_ && ue::IsA(o, actorCls_)) {
                if (ours(ue::Get<UObject*>(o + ownerOff_))) return true;
                if (instigatorOff_ >= 0 && ours(ue::Get<UObject*>(o + instigatorOff_))) return true;
            }
        }
        return false;
    }
    // 剧情判定：Flow 图放的一律算；其余要"本层开始后才开始播、非循环"
    bool Narrative(UObject* p, bool ambient) { return IsStory(FlowDriven(p), ambient, Looping(p), OwnedByTower(p), false); }

    bool OnPlay(UObject* player) {
        if (!active_ || Travelling() || OwnedByTower(player)) return false;
        if (!FlowDriven(player) && Looping(player)) return false;  // 循环环境过场
        const std::string seq = SeqName(player);
        if (tower::replay::AllowSequence(g_replay, seq)) {  // 重放层：初见 / 转阶段过场照原生播，Flow 照常往下走（刷出下一阶段）
            if (allowLogs_++ < 10) pbz::log::Info("影塔重放：放行过场 {}（{}）", seq, g_replay->name);
            return false;
        }
        if (g_replay && tower::replay::In(g_replay->freezeSeq, g_replay->freezeN, seq))
            pbz::log::Info("影塔重放：战后过场 {} 冻结（Boss 死后不播任何剧情，直接进塔结算）", seq);
        // 还没开始播：直接不播。Flow 节点停在这里（完成回调由会话层吞掉），不往下发奖 / 换图 / 推任务
        tookStory_ = true;
        ++skips_;
        ParkNode(player);
        const auto flow = FlowName(player);
        last_ = std::format("播放请求：{}{}", SeqName(player), flow.empty() ? "" : "（Flow " + flow + "）");
        if (skips_ <= 40) pbz::log::Info("影塔隔离：冻结剧情，{} 不播（场地 {}，Flow 停在本节点）", last_, venue_.empty() ? "原地" : venue_);
        return true;
    }
    void ParkNode(UObject* player) {
        if (receiverOff_ < 0 || !FlowDriven(player)) return;
        UObject* node = ue::Get<UObject*>(player + receiverOff_);
        if (node && ue::Live(node)) g_session.Park(node);
    }

    // 兜底：先把 Flow 节点停住（完成回调吞掉），直接 Stop；不评估结尾的事件。
    // 旧做法补发 OnPlaybackFinished 让 Flow 往下走——10 层白赤练过场后就是这样走到发兵器 / 自动换图
    void Skip(UObject* player, const char* how) {
        tookStory_ = true;
        ++skips_;
        const auto flow = FlowName(player);
        last_ = std::format("{}：{}{}", how, SeqName(player), flow.empty() ? "" : "（Flow " + flow + "）");
        if (skips_ <= 40) pbz::log::Info("影塔隔离：冻结剧情，{} 立即停止，不评估结尾（场地 {}）", last_, venue_.empty() ? "原地" : venue_);
        skipped_[player] = GetTickCount64();
        ParkNode(player);
        if (skip_.fn) pe::Call(player, skip_.fn, nullptr);
    }
    void Failed(UObject* player) {
        last_ = std::format("跳过后仍在播放：{}", SeqName(player));
        pbz::log::Info("影塔隔离：{}（场地 {} 记为无法冻结剧情，本机不再抽）", last_, venue_.empty() ? "原地" : venue_);
        MarkUnsafe(last_);
    }

    void OnCinematic(UObject*, void* params) {
        if (!active_ || cinematic_.args.empty() || cinematic_.args[0].second < 0) return;
        if (!static_cast<uint8_t*>(params)[cinematic_.args[0].second]) return;
        g_cinematicAt = GetTickCount64();
        // 只记录：Boss 投技 / 阶段镜头也可能走电影模式，单凭它分不出原剧情
        pbz::log::Info("影塔隔离：本层有流程把玩家切进电影模式（只记录；剧情以过场归属判断）");
    }

    void Block(UObject* g) {
        if (!g || !ue::Live(g) || blocked_.count(g)) return;
        if (g_nativeGroups.count(g)) return;  // 原生阶段组（重放 nativeGroups）从不收组 / 阻断
        g_session.Mute(g);   // 先收组的委托和脚本，再加阻断
        g_session.Block(g);  // 阻断归会话层所有，出塔事务开始才撤
        blocked_.insert(g);
        pbz::log::Info("影塔隔离：战斗组 {} 已收住（委托 / 组脚本 / 清组阻断；移除 / 击败其成员不会触发清组脚本）", ue::ObjName(g));
    }
    void Join(UObject* pawn, UObject* group, const char* how) {
        Block(group);  // 先阻断：塔 Boss 倒下不让组判定清空
        std::vector<uint8_t> a(addMember_.size);
        if (addMember_.args[0].second < 0) return;
        std::memcpy(a.data() + addMember_.args[0].second, &pawn, 8);
        pe::Call(group, addMember_.fn, a.data());
        members_[pawn] = group;
        pbz::log::Info("影塔隔离：{} 归入原生战斗组 {}（{}）", ue::ClassName(pawn), ue::ObjName(group), how);
    }
    bool Loc(UObject* a, double out[3]) {
        if (!where_.fn || where_.ret < 0) return false;
        std::vector<uint8_t> b(where_.size);
        pe::Call(a, where_.fn, b.data());
        std::memcpy(out, b.data() + where_.ret, 24);
        return true;
    }
    // 同一世界里、离 Boss 150 米内、刷的是同一个类的 AISpawner 所挂的组
    UObject* NativeGroupFor(UObject* pawn) {
        if (!spawnerCls_ || spawnerClassOff_ < 0 || spawnerGroupOff_ < 0) return nullptr;
        const std::string cls = ue::ClassName(pawn);
        UObject* world = WorldOf(pawn);
        double at[3];
        if (cls.empty() || !world || !Loc(pawn, at)) return nullptr;
        UObject* best = nullptr;
        double bestD = 15000;
        ue::ForEach([&](int, UObject* o) {
            if (ue::IsDefault(o) || !ue::IsA(o, spawnerCls_) || WorldOf(o) != world || !ue::Live(o)) return true;
            // Class 是软类引用：FWeakObjectPtr(8) + FSoftObjectPath{包名 FName, 资产名 FName, 子路径}；比资产名（BP_X_C）
            const uint32_t asset = ue::Get<uint32_t>(o + spawnerClassOff_ + 0x10);
            if (ue::Name(asset) != cls) return true;
            UObject* g = ue::Get<UObject*>(o + spawnerGroupOff_);
            double p[3];
            if (!g || !ue::Live(g) || !Loc(o, p)) return true;
            const double d = std::hypot(p[0] - at[0], p[1] - at[1], p[2] - at[2]);
            if (d < bestD) { bestD = d; best = g; }
            return true;
        });
        return best;
    }

    void LoadUnsafe() {
        if (loadedUnsafe_) return;
        loadedUnsafe_ = true;
        std::ifstream f(TowerFile(L"venue-unsafe.txt"), std::ios::binary);
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto tab = line.find('\t');
            if (!line.empty() && line[0] != '#') unsafe_.insert(line.substr(0, tab));
        }
    }

    void MarkUnsafe(const std::string& why) {
        if (venue_.empty()) return;
        LoadUnsafe();
        if (!unsafe_.insert(venue_).second) return;
        SYSTEMTIME t; GetLocalTime(&t);
        std::ofstream f(TowerFile(L"venue-unsafe.txt"), std::ios::binary | std::ios::app);
        f << std::format("{}\t{:04}-{:02}-{:02} {:02}:{:02}:{:02}\t{}\n", venue_, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, why);
    }

    bool resolved_ = false, active_ = false, tookStory_ = false, loadedUnsafe_ = false, playNx_ = false, firstPass_ = true;
    std::string venue_, last_;
    unsigned long long startedAt_ = 0, lastCheck_ = 0;
    UObject *seqPlayerCls_ = nullptr, *flowPlayerCls_ = nullptr, *actorCls_ = nullptr, *groupCls_ = nullptr, *groupCdo_ = nullptr, *spawnerCls_ = nullptr;
    int statusOff_ = -1, sequenceOff_ = -1, settingsOff_ = -1, loopOff_ = -1, receiverOff_ = -1, nodePlayerOff_ = -1, ownerOff_ = -1, instigatorOff_ = -1;
    int spawnerClassOff_ = -1, spawnerGroupOff_ = -1, cursor_ = 0, cineObs_ = 0, skips_ = 0, allowLogs_ = 0;
    Fn play_, playTo_, skip_, finished_, cinematic_, startFlow_, where_, resolveGroup_, addBlocker_, removeBlocker_, addMember_, removeMember_;
    std::unordered_map<UObject*, Seen> players_;
    std::unordered_map<UObject*, unsigned long long> skipped_;
    std::unordered_set<UObject*> blocked_;
    std::unordered_map<UObject*, UObject*> members_;  // 塔 Boss → 归入的原生战斗组
    std::unordered_set<UObject*> nativeMembers_;     // 原生阶段组里的塔 Boss（不归组、不移出）
    std::vector<UObject*> pending_;                     // 没找到组、等主 Boss 的组
    std::unordered_map<UObject*, bool> isPlayerCls_;
    std::set<std::string> unsafe_;
};

Isolation g_iso;

}  // namespace

void Session(bool inside, int floor, std::string_view run) { g_session.Set(inside, floor, run); towerstorygate::Session(inside); if(!inside)towerweapongrant::Reset(); }
void SetReplay(const tower::replay::Contract* c) {
    if(c!=g_replay){towergroup::Disarm();g_session.ClearReplayCache();}
    if (c != g_replay) pbz::log::Info("影塔重放：{}", c ? std::string("本层按遭遇契约 ") + std::string(c->name) : std::string("本层不是重放层"));
    g_replay = c;
    towerstorygate::SetReplay(c);
    std::vector<std::string> names;
    if (c) for (int i = 0; i < c->interactN; ++i) names.push_back(std::string(c->interact[i]));
    towerinteractions::AllowNames(names);
}
bool ReplayEntryReady(std::string* why) { return towerstorygate::Ready(why) && g_session.ReplayReady(why); }
bool ReplayPhaseReady(int phase,std::string* why) { return g_session.ReplayReady(why,phase); }
void RestoreControl(const char* why) {
    // 战后过场被冻结时，恢复本场曾持有的电影模式；长时间柱战不能让恢复资格过期。
    if (!g_cinematicAt && !towerstorygate::HadCinematic()) return;
    auto* pcCls = towerperf::ClassNamed("PlayerController");
    UObject* fn = pcCls ? pe::FindFunction(pcCls, "SetCinematicMode") : nullptr;
    UObject* pc = towerwidgets::Controller();
    if (!fn || !pc) return;
    std::vector<uint8_t> a(0x40);
    // 退出电影模式：bInCinematicMode=false，其余 true（恢复移动 / 转向 / HUD、取消隐藏玩家）
    for (const char* k : {"bHidePlayer", "bAffectsHUD", "bAffectsMovement", "bAffectsTurning"})
        if (const int o = ue::PropOffset(fn, k); o >= 0 && o < 0x40) a[o] = 1;
    pe::Call(pc, fn, a.data());
    g_cinematicAt = 0;
    pbz::log::Info("影塔重放：{}，已关掉电影模式、把控制权还给玩家", why);
}
bool SessionOn() { return g_session.On(); }
void Quiet(void* spawner) { g_session.Quiet(static_cast<UObject*>(spawner)); }
void AllowTowerSpawn(void* spawner) { g_session.AllowTowerSpawn(static_cast<UObject*>(spawner)); }
void EndTowerSpawn() { g_session.EndTowerSpawn(); }
GrantScope::GrantScope() { ++g_session.granting; }
GrantScope::~GrantScope() { --g_session.granting; }

void Begin(std::string_view venueKey) { g_iso.Begin(venueKey); }
void End() { g_iso.End(); g_session.EndTowerSpawn(); }  // 塔兜底放行只到本层结束
void Tick(float dt) { g_iso.Tick(dt); }
void BlockGroupOf(void* actor) { g_iso.BlockGroupOf(static_cast<UObject*>(actor)); }
bool NativeMember(void* actor) { return g_iso.NativeMember(static_cast<UObject*>(actor)); }
void Adopt(void* pawn) { g_iso.Adopt(static_cast<UObject*>(pawn)); }
bool Unsafe(std::string_view venueKey) { return g_iso.Unsafe(venueKey); }
// F37（雷当）：过场占位模板（Outer = …LeiDang_Trans_Placeholder.MovieScene_0）也是活着的 BP_Monster 对象，
// 按类名认敌会把它当成原生阶段 / 场地敌人。只认真正放在关卡里（Outer 是 Level）且与玩家同一世界的对象。
bool InPlayerWorld(void* actor) {
    auto* a = static_cast<UObject*>(actor);
    if (!a || !ue::Live(a) || ue::IsDefault(a)) return false;
    auto* outer = ue::Get<UObject*>(a + ue::Layout::ObjOuter);
    if (!outer || ue::ClassName(outer) != "Level") return false;
    auto* pawn = towerwidgets::Pawn();
    auto* world = pawn ? WorldOf(pawn) : nullptr;
    return world && WorldOf(a) == world;
}
bool TookStory() { return g_iso.TookStory(); }
std::string Status() { return g_iso.Status() + "," + g_session.Status(); }

}  // namespace towerisolate
