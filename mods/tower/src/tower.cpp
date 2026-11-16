// 影塔模块（M1）：层管理器 + 战力归一 + 强化 3 选 1。运行库加载本模块（管道 op "module"），之后用 op "tower" 控制：
//   {"op":"tower","cmd":"start","floors":3,"seed":0,"hp0":25000}  开一局（不传 seed 用时间）
//   {"op":"tower","cmd":"next"}                                     开下一层（在当前位置附近生成本层 Boss）
//   {"op":"tower","cmd":"pick","slot":0}                            3 选 1
//   {"op":"tower","cmd":"stop"} / {"op":"tower","cmd":"state"}
//   {"op":"tower","cmd":"probe"}  读玩家强度相关属性（标定统一起点用），回 {"op":"tower","event":"probe",…}
//   {"op":"tower","cmd":"use","item":"heal|freeze|slow|sunder|shachi"}   用道具
//   {"op":"tower","cmd":"reroll"}                                   刷新令：三选一重抽
//   {"op":"tower","cmd":"st","slot":0}                              用神通（本层 / 下一层有效，用完消失）
//   {"op":"tower","cmd":"resume"}                                   读局存档继续（%LOCALAPPDATA%\PBZTower\run.txt）
//   {"op":"tower","cmd":"grant","backup_ok":true,"session":"…"}     把账本里待领取的兵器写进存档（前端须先备份存档；session 用于防换档）
//   {"op":"tower","cmd":"suspend"}                                  暂离：保存后结束本次，之后 resume
// 所有命令可带 request_id（回 {"event":"ack"}）、client（控制端标识）、expect_run / expect_rev（过期拒绝）；种子用十进制字符串
// 每次状态变化广播 {"op":"tower","event":"state",…}，界面（Codex 做的覆盖层）只靠这个渲染。
// 所有加成都只作用于玩家和本塔生成的 Boss，不碰地图上原有的敌人；停止 / 失败 / 运行库停用时自动还原（停止声明即还原）。
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <format>
#include <map>
#include <limits>
#include <fstream>
#include <sstream>
#include <mutex>
#include <unordered_map>
#include <tuple>
#include <unordered_set>
#include <utility>

#include "tower_verbose.h"
#include "boss_drops.generated.h"
#include "venues.generated.h"
#include "../../../src/core/host/log.h"
#include "../../../src/core/ipc/pipe.h"
#include "../../../src/core/sdk/pbz_module.h"
#include "../../../src/core/services/diag.h"
#include "../../../src/core/services/gamethread.h"
#include "boss_pool.generated.h"
#include "tower_api.h"
#include "tower_arsenal_selection.h"
#include "tower_entry.h"
#include "tower_widgets.h"
#include "tower_diagmode.h"
#include "tower_admission.h"
#include "tower_arena.h"
#include "tower_floorplay.h"
#include "tower_mobs.h"
#include "tower_wave_guard.h"
#include "tower_venue.h"
#include "tower_venue_fixed.h"
#include "tower_endless.h"
#include "tower_endless_share.h"
#include "tower_endless_venues.h"
#include "tower_venue_real.h"
#include "tower_wall.h"
#include "tower_growth_run.h"
#include "tower_plan.h"
#include "tower_trip.h"
#include "tower_native.h"
#include "tower_fate.h"
#include "tower_reach.h"
#include "tower_travel.h"
#include "tower_mods.h"
#include "tower_combatlog.h"
#include "tower_burn.h"
#include "tower_arcane_probe.h"
#include "tower_weapon_cap.h"
#include "tower_replay.h"
#include "encounter_dependencies.generated.h"
#include "tower_interactions.h"
#include "tower_isolation.h"
#include "tower_music.h"
#include "tower_storygate.h"
#include "tower_vm_gate.h"
#include "tower_spawn_receipt.h"
#include "tower_servants.h"
#include "tower_playstyle.h"
#include "tower_special.h"
#include "tower_logic.h"
#include "tower_failed_stage.h"
#include "tower_summon_permit.h"
#include "tower_travel_api.h"
#include "tower_perf.h"
#include "tower_potionboost.h"
#include "tower_leech.h"
#include "tower_trade.h"
#include "tower_weapon_config.generated.h"
#include "tower_attack_speed.h"
#include "tower_hit_probe.h"
#include "tower_weapon_power_probe.h"
#include "tower_seed.h"

// 场地门面：本层是固定场地（审计计划 @rongshu）走 towerfixed，否则走 Boss 原生场地 towervenue（v1.3.5 契约，基线锁定不改）
namespace tvenue {
using tower::arena::P3;
inline std::string g_path;  // 本层场地键（Boundary 收紧小场地用）
inline void Arrive(std::string_view p) { g_path = std::string(p); if (towerfixed::Is(p)) { towervenue::End(); towerfixed::Arrive(p); } else { towerfixed::End(); towervenue::Arrive(p); } }
// 3.0.1（玩家反馈：断肠 Boss 卡在雾门外不拉回）：没有雾门凸包的原生场地按「出生点到门 × 1.2、最少 25 米」画圆；
// 断肠 / 铁面杀手 / 白衣鬼差变体的雾墙只有 11–19 米，Boss 在雾墙外 20 米仍算圈内。门距不到 21 米的改成门距 × 1.15（最少 12 米）
inline double TightRadius(double radius) {
    for (const auto& v : tower::kVenues)
        if (v.path == g_path && v.hullN < 3 && v.reach > 0 && v.reach * 1.2 < 2500) return std::min(radius, std::max(1200.0, v.reach * 1.15));
    return radius;
}
inline void Tick() { if (towerfixed::Active()) towerfixed::Tick(); else towervenue::Tick(); }
inline bool Ready() { return towerfixed::Active() ? towerfixed::Ready() : towervenue::Ready(); }
inline bool Found() { return towerfixed::Active() ? towerfixed::Found() : towervenue::Found(); }
inline bool MissingRequired() { return towerfixed::Active() ? towerfixed::MissingRequired() : towervenue::MissingRequired(); }
inline void* Spawner() { return towerfixed::Active() ? towerfixed::Spawner() : towervenue::Spawner(); }
inline std::vector<void*> GroupMates() { return towerfixed::Active() ? std::vector<void*>{} : towervenue::GroupMates(); }
inline bool CloseDoors() { return towerfixed::Active() ? towerfixed::CloseDoors() : towervenue::CloseDoors(); }
inline bool DoorsClosed() { return towerfixed::Active() ? towerfixed::DoorsClosed() : towervenue::DoorsClosed(); }
inline void OpenDoors() { if (towerfixed::Active()) towerfixed::OpenDoors(); else towervenue::OpenDoors(); }
inline bool Boundary(std::vector<P3>* h, P3* c, double* r, double* lo, double* hi) {
    if (towerfixed::Active()) return towerfixed::Boundary(h, c, r, lo, hi);
    const bool ok = towervenue::Boundary(h, c, r, lo, hi);
    if (ok && h->empty()) *r = TightRadius(*r);
    return ok;
}
inline void End() { towerfixed::End(); towervenue::End(); g_path.clear(); }
}  // namespace tvenue

namespace towerprobe { std::string Call(const std::string& line); std::string Ach(const std::string& v); std::string Obs(const std::string& line); }
namespace towerpoints { void Enable(bool on, bool dustGate); bool Enabled(); std::string Status(); }
namespace towersb { bool Set(bool on, std::string* why, bool force); bool Active(); bool Dirty(); std::string State(); void Clear(); void BeginReal(); void EndReal(); }
unsigned long long g_lastEncounterClear = 0;  // towerapi::LastEncounterClear（1.1.1 回标题前的战斗收尾等待）
namespace towergate {  // tower_savegate.cpp：原生存档锁与进塔基准（游戏线程）
bool SaveClean(std::string* why); bool Veto(bool on, std::string* why); bool Snapshot(std::string* why); bool Restore(std::string* why); std::string State(); void OnStop(); void Touch();
void ConfirmOriginal(); void MarkForeign();
}

namespace {

using pbz::attr::Kind;
using pbz::attr::Target;
using pbz::ue::UObject;
namespace attr = pbz::attr;
namespace ue = pbz::ue;
namespace pe = pbz::pe;

constexpr int kPool = static_cast<int>(std::size(tower::kBossPool));
const char* const kDamage[] = {"Physical", "PhysicalSharp", "PhysicalBlunt", "Fire", "Ice", "Lightning", "Aqua", "Elemental", "Chaos", "Blood", "Toxin"};
const char* const kShachi[] = {"ImpactShachi", "ReflectShachi", "DefenceShachi"};

std::string J(std::string_view s) { return pbz::ipc::Esc(std::string(s)); }

// 原生召唤物可能由自己的 AIController 持有，不能只看 Owner。只读取当前角色能力系统
// 登记的实例；不按怪物类名全局豁免，避免把同类的场地敌人放进影塔。
struct SummonArray { uint8_t* data; int32_t num, max; };
static bool SummonArrayAt(const void* p, int limit, SummonArray& a) {
    return ue::Read(p, &a, sizeof(a)) && a.num >= 0 && a.num <= limit && a.max >= a.num &&
           a.max <= 65536 && (a.num == 0 || a.data);
}
static bool RegisteredSummons(UObject* avatar, std::unordered_set<UObject*>& out) {
    if (!avatar || !ue::Live(avatar)) return false;
    auto* ascClass = towerperf::ClassNamed("AbilitySystemComponent");
    auto* container = towerperf::ClassNamed("GameplayAbilitySpecContainer");
    auto* spec = towerperf::ClassNamed("GameplayAbilitySpec");
    auto* summon = towerperf::ClassNamed("GA_SummonPassive_C");
    auto* instance = towerperf::ClassNamed("AISpawnedSummonInstance");
    if (!ascClass || !container || !spec || !summon || !instance) return false;
    const int ascOff = ue::PropOffset(ue::ClassOf(avatar), "AbilitySystemComponent");
    const int avOff = ue::PropOffset(ascClass, "AvatarActor");
    const int abilityOff = ue::PropOffset(ascClass, "ActivatableAbilities");
    const int itemsOff = ue::PropOffset(container, "Items");
    const int nonrepOff = ue::PropOffset(spec, "NonReplicatedInstances");
    const int repOff = ue::PropOffset(spec, "ReplicatedInstances");
    const int summonsOff = ue::PropOffset(summon, "SpawnedSummonInstances");
    const int pawnOff = ue::PropOffset(instance, "Pawn");
    const int specSize = ue::Get<int>(spec + ue::Layout::StructSize, 0);
    const int instanceSize = ue::Get<int>(instance + ue::Layout::StructSize, 0);
    if (ascOff < 0 || avOff < 0 || abilityOff < 0 || itemsOff < 0 || summonsOff < 0 ||
        specSize < 16 || specSize > 4096 || instanceSize < 8 || instanceSize > 1024 ||
        nonrepOff < 0 || nonrepOff + int(sizeof(SummonArray)) > specSize ||
        repOff < 0 || repOff + int(sizeof(SummonArray)) > specSize || pawnOff < 0 || pawnOff + 8 > instanceSize)
        return false;
    UObject* asc = ue::Get<UObject*>(avatar + ascOff);
    if (!asc || !ue::Live(asc) || !ue::IsA(asc, ascClass) || ue::Get<UObject*>(asc + avOff) != avatar) return false;
    SummonArray specs{};
    if (!SummonArrayAt(asc + abilityOff + itemsOff, 1024, specs)) return false;
    for (int i = 0; i < specs.num; ++i) for (int off : {nonrepOff, repOff}) {
        SummonArray abilities{};
        if (!SummonArrayAt(specs.data + size_t(i) * specSize + off, 128, abilities)) return false;
        for (int j = 0; j < abilities.num; ++j) {
            UObject* ability = ue::Get<UObject*>(abilities.data + size_t(j) * sizeof(UObject*));
            if (!ability || !ue::Live(ability) || !ue::IsA(ability, summon) || ue::IsDefault(ability)) continue;
            SummonArray spawned{};
            if (!SummonArrayAt(ability + summonsOff, 512, spawned)) return false;
            for (int k = 0; k < spawned.num; ++k) {
                UObject* pawn = ue::Get<UObject*>(spawned.data + size_t(k) * instanceSize + pawnOff);
                if (pawn && ue::Live(pawn)) out.insert(pawn);
            }
        }
    }
    return true;
}

class SummonPermit {
    tower::summon::Lease lease_;
    uint64_t next_ = 0;
    static bool Live(tower::summon::Identity id) {
        auto* pawn = static_cast<UObject*>(id.pawn); auto* asc = static_cast<UObject*>(id.asc);
        if (!id.Valid() || !ue::Live(pawn) || !ue::Live(asc) || ue::Obj(id.pawnIndex) != pawn || ue::Obj(id.ascIndex) != asc) return false;
        const int off = ue::PropOffset(ue::ClassOf(pawn), "AbilitySystemComponent");
        auto* ascClass = towerperf::ClassNamed("AbilitySystemComponent");
        const int avatar = ascClass ? ue::PropOffset(ascClass, "AvatarActor") : -1;
        return off >= 0 && ue::Get<UObject*>(pawn + off) == asc && ascClass && ue::IsA(asc, ascClass) && avatar >= 0 && ue::Get<UObject*>(asc + avatar) == pawn;
    }
    static uint64_t Tag() { return towerwidgets::Name("CharacterState.Summon.SummonAvailable"); }
    static int Count(tower::summon::Identity id, uint64_t tag) {
        auto* asc = static_cast<UObject*>(id.asc);
        auto* fn = asc ? pe::FindFunction(ue::ClassOf(asc), "GetGameplayTagCount") : nullptr;
        const int param = fn ? ue::PropOffset(fn, "GameplayTag") : -1;
        const int result = fn ? ue::PropOffset(fn, "ReturnValue") : -1;
        if (!tag || param < 0 || param + 8 > 64 || result < 0 || result + 4 > 64 || !(param + 8 <= result || result + 4 <= param)) return -1;
        uint8_t args[64] = {}; std::memcpy(args + param, &tag, 8); pe::Call(asc, fn, args);
        const int n = ue::Get<int32_t>(args + result, -1);
        return n >= 0 && n < 65536 ? n : -1;
    }
    static bool Change(tower::summon::Identity id, uint64_t tag, const char* name) {
        auto* cls = towerperf::ClassNamed("AbilityUtils");
        auto* cdo = cls ? towerwidgets::Default(cls) : nullptr;
        auto* fn = cls ? pe::FindFunction(cls, name) : nullptr;
        const int actor = fn ? ue::PropOffset(fn, "Actor") : -1;
        const int value = fn ? ue::PropOffset(fn, "GameplayTag") : -1;
        const int rep = fn ? ue::PropOffset(fn, "bShouldReplicate") : -1;
        if (!cdo || !fn || !tag || actor < 0 || actor + 8 > 64 || value < 0 || value + 8 > 64 || rep < 0 || rep >= 64 ||
            !(actor + 8 <= value || value + 8 <= actor) || (rep >= actor && rep < actor + 8) || (rep >= value && rep < value + 8)) return false;
        uint8_t args[64] = {}; std::memcpy(args + actor, &id.pawn, 8); std::memcpy(args + value, &tag, 8);
        pe::Call(cdo, fn, args); return true; // bShouldReplicate=false（本地单人）
    }
    static bool Add(tower::summon::Identity id) {
        const auto tag = Tag(); const int before = Count(id, tag);
        if (before != 0 || !Change(id, tag, "AddLooseGameplayTag")) return false;
        const int after = Count(id, tag);
        if (after == before + 1) { pbz::log::Info("影塔召唤许可：已临时增加一份（{}→{}）", before, after); return true; }
        if (after < 0) { pbz::log::Info("影塔召唤许可：添加后暂不能读回，保留撤回责任，不重复添加"); return true; }
        if (after > before) Change(id, tag, "RemoveLooseGameplayTag");
        pbz::log::Info("影塔召唤许可：读回未核验（{}→{}）", before, after);
        return false;
    }
    static void Remove(tower::summon::Identity id) {
        const auto tag = Tag(); const int before = Count(id, tag);
        if (!Change(id, tag, "RemoveLooseGameplayTag")) { pbz::log::Info("影塔召唤许可：无法撤回临时标签"); return; }
        pbz::log::Info("影塔召唤许可：已撤回自己的一份（{}→{}）", before, Count(id, tag));
    }
  public:
    void Reset() { lease_.Reset(Live, Remove); next_ = 0; }
    void Tick(bool active) {
        if (!active) { if (lease_.held) Reset(); return; }
        const auto now = GetTickCount64(); if (now < next_) return; next_ = now + 500;
        auto* pawn = towerwidgets::Pawn();
        const int off = pawn && ue::Live(pawn) ? ue::PropOffset(ue::ClassOf(pawn), "AbilitySystemComponent") : -1;
        auto* asc = off >= 0 ? ue::Get<UObject*>(pawn + off) : nullptr;
        tower::summon::Identity id;
        if (pawn && asc && ue::Live(pawn) && ue::Live(asc)) id = {pawn, asc, ue::Get<int>(pawn + ue::Layout::ObjIndex, -1), ue::Get<int>(asc + ue::Layout::ObjIndex, -1)};
        lease_.Update(true, id, Live, Add, Remove);
    }
};

class Tower : public pbz::IFeature {
public:
    const char* Id() const override { return "tower.run"; }
    bool IsToggle() const override { return true; }

    bool Resolve(std::string* why) override {
        hp_ = attr::AttrOffset("Health"); hpMax_ = attr::AttrOffset("Health_Max"); hpMin_ = attr::AttrOffset("Health_Min");
        shachi_ = attr::AttrOffset("ShachiRegen"); dilation_ = attr::ActorOffset("CustomTimeDilation");
        attack_ = attr::AttrOffset("Attack"); shachiAttack_ = attr::AttrOffset("ShachiAttack");
        shachiMax_ = attr::AttrOffset("Shachi_Max"); infliction_ = attr::AttrOffset("Infliction_Max");
        shachiNow_ = attr::AttrOffset("Shachi"); worldDilation_ = attr::WorldOffset("TimeDilation");
        spirit_ = attr::AttrOffset("SpiritShadow"); spiritMax_ = attr::AttrOffset("SpiritShadow_Max");  // 3.4.0 暗源
        walkSpeed_ = attr::AttrOffset("WalkSpeed"); jogSpeed_ = attr::AttrOffset("JogSpeed"); sprintSpeed_ = attr::AttrOffset("SprintSpeed");
        pbz::log::Info("影塔海克斯：移动属性 Walk={} Jog={} Sprint={}", walkSpeed_, jogSpeed_, sprintSpeed_);
        taken_.clear(); dealt_.clear(); posture_.clear();
        for (auto* d : kDamage) {
            if (int o = attr::AttrOffset(std::string("TakenVulnerabilityMultiplier_") + d); o >= 0) taken_.push_back(o);
            if (int o = attr::AttrOffset(std::string("DealtVulnerabilityMultiplier_") + d); o >= 0) dealt_.push_back(o);
        }
        for (auto* s : kShachi)
            if (int o = attr::AttrOffset(std::string("TakenVulnerabilityMultiplier_") + s); o >= 0) posture_.push_back(o);
        if (hp_ < 0 || hpMax_ < 0 || hpMin_ < 0 || attack_ < 0 || shachiAttack_ < 0 || shachiMax_ < 0 || shachiNow_ < 0 || taken_.size() != std::size(kDamage) || dealt_.empty()) { *why = "影塔所需的生命、攻击、破防、杀气或伤害属性接口未就绪"; return false; }
        return true;
    }
    void Enable() override { enabled = true; }
    const tower::Run& RunState() const { return run_; }
    // 用户 2026-11-08：2.0.0 改动太大，旧版的局不继续。旧局文件改名留存（不删），菜单里就只有"开始新局"；玩家自己的原存档不受影响
    static void RetireLegacyRuns() {
        static bool done = false;
        if (done) return;
        done = true;
        const auto dir = TowerDir();
        if (dir.empty()) return;
        const std::pair<const wchar_t*, const wchar_t*> sets[] = {{L"\\run.txt", L"\\growth.txt"}, {L"\\hidden-run.txt", L"\\hidden-growth.txt"}, {L"\\hidden51-run.txt", L"\\hidden51-growth.txt"}};
        for (const auto& [runLeaf, growthLeaf] : sets) {
            const std::wstring run = dir + runLeaf, growth = dir + growthLeaf;
            std::ifstream f(run, std::ios::binary);
            if (!f) continue;
            std::stringstream ss; ss << f.rdbuf(); f.close();
            tower::Run r;
            if (!r.Load(ss.str()) || !r.legacy) continue;
            const bool moved = MoveFileExW(run.c_str(), (run + L".pre2").c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
            MoveFileExW(growth.c_str(), (growth + L".pre2").c_str(), MOVEFILE_REPLACE_EXISTING);
            pbz::log::Info("影塔：发现 2.0.0 之前的爬塔局，{}", moved ? "已改名留存，需开新局" : "改名失败，本局不能继续");
        }
    }
    static bool SaveExists() { RetireLegacyRuns(); const auto f = RunFile(); return !f.empty() && GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES; }
    static int ImportedSeed(uint64_t* seed, int* difficulty, int* shuffle = nullptr) {
        const auto d = TowerDir(); if (d.empty()) return 0;
        const auto path = d + L"\\pending-seed.txt";
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return 0;
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f || f.tellg() < 1 || f.tellg() > 512) return -1;
        f.seekg(0); std::stringstream ss; ss << f.rdbuf(); tower::seedshare::Seed imported;
        if (!tower::seedshare::Parse(ss.str(), &imported) || tower::seedshare::kPlan != tower::kPlanVersion) return -1;
        *seed = imported.value; *difficulty = imported.difficulty; if (shuffle) *shuffle = imported.shuffle; return 1;
    }
    static bool BackupForSeed() {
        const auto d = TowerDir(); if (d.empty()) return false;
        const std::wstring dst = d + L"\\seed-backup-" + std::to_wstring(GetTickCount64());
        if (!CreateDirectoryW(dst.c_str(), nullptr)) return false;
        for (const auto* leaf : {L"run.txt", L"growth.txt"}) {
            const auto from = d + L"\\" + leaf;
            if (GetFileAttributesW(from.c_str()) != INVALID_FILE_ATTRIBUTES && !CopyFileW(from.c_str(), (dst + L"\\" + leaf).c_str(), TRUE)) return false;
        }
        pbz::log::Info("影塔种子：旧爬塔进度已备份"); return true;
    }
    void Disable() override {  // 运行库停用（或别处强制关闭）：本局作废；自己正常结束时 ending_ 为真，只关开关
        MeltRestore();  // 3.1.0 熔铸：还原武器配置的品质倍率
#ifdef PBZ_HEX_TEST
        if (qaAssist_) { for (const char* id : {"combat.dmg_mul", "player.no_stagger"}) if (auto* f = pbz::FindFeature(id)) pbz::ReleaseFeature(f, "tower-qa"); qaAssist_ = false; }
#endif
        arcaneOn_ = false;
        towerarcane::Stop();
        towerhitprobe::Stop();
        towerweaponprobe::Stop();
        towerwidgets::ResetMaxBarSync();
        towerspeed::Reset();
        summonPermit_.Reset();
        towerpotion::Clear();
        towerentry::End();
        enabled = false;
        if (ending_) return;
        towerisolate::End();
        towerisolate::Session(false, run_.floor, run_.id);  // 运行库停用：拦截不能留在卸载后的代码上
        towerpoints::Enable(false, false);
        // 运行库要停（热换 / 卸载）而还在塔里：钩子马上没了，先把改过的等级撤回去；发放的东西只能靠读档撤（沙盒基准）
        if (trip_.phase() != tower::trip::Phase::Outside)
            pbz::log::Info("影塔独立塔档：运行库停止时仍在塔档会话（{}）：游戏内存是塔内存档，原生存档锁保留；下次加载影塔后按\"恢复原进度\"处理", tower::trip::PhaseName(trip_.phase()));
        Unwear("disable");
        if (run_.phase != tower::Phase::Idle) { Clear("disable"); ReleaseShentong(); run_ = {}; actors_.clear(); controller_.clear(); Broadcast("info.stopped", "已停止"); }  // 局存档保留，可以 resume
    }

    // ---- 管道命令（游戏线程执行） ----
    void Command(const std::string& line) {
        const auto cmd = pbz::ipc::Field(line, "cmd").value_or("state");
#ifdef PBZ_HEX_TEST
        if (cmd == "qa_venue_sample") { towervenue::BeginSample(line); return; }
        if (cmd == "qa_cap") { BeginCap(line); return; }
#endif
#if defined(PBZ_HEX_TEST) || defined(PBZ_PLAYTEST)
        // 3.6.0 测试包（用户）：实时改无尽 100 层一次性抬高的四个倍数 {"cmd":"qa_endless_scale","hp":3,"atk":2.5,"brk":2,"shachi":1.5}；
        // 只给出的项会改，不写存档；下一只刷出的敌人起生效（已在场的按每帧重算的属性跟着变）。正式包没有这条命令
        if (cmd == "qa_measure") { BeginMeasure(line); return; }
        // 3.6.0 测试包（用户 2026-11-15：一次测完魂玉 / 场地 / 反震）：只在准备页、塔内；不进正式包
        if (cmd == "qa_endless_go") {  // {"to":51}：准备页直接进无尽模式第 to 层（显示 to+49），保留海克斯 / 成长 / 背包
            if (!grow_.on || trip_.phase() != tower::trip::Phase::Inside || run_.phase != tower::Phase::Ready || run_.floors < 50 || run_.hidden || !plan_.empty()) {
                Broadcast("err.qa_endless", "只能在 50 层正式局的准备页跳到无尽模式"); return;
            }
            int to = 51;
            try { to = std::max(51, std::stoi(pbz::ipc::Field(line, "to").value_or("51"))); } catch (...) {}
            run_.endless = true; run_.floors = 50;
            run_.floor = to - 1; run_.peak = std::max(run_.peak, to - 1);
            run_.mobFloor = 51; run_.mobWave = 0;
            grow_.shop = {}; settlement_.clear();
            SaveRun();
            pbz::log::Info("影塔 QA 跳层：无尽模式，下一层第 {} 层（显示 {}）", to, ShownBoss(to));
            Broadcast("info.qa_endless", std::format("测试：下一层无尽第 {} 层", ShownBoss(to)));
            return;
        }
        if (cmd == "qa_special") {  // 3.7.0 测试：{"kind":"melee|survival|duel|outnumbered|team|off"}，下一个小兵段换成指定创意层
            const auto k = pbz::ipc::Field(line, "kind").value_or("");
            using K = tower::special::Kind;
            qaSpecial_ = k == "melee" ? K::Melee : k == "survival" ? K::Survival : k == "duel" ? K::Duel : k == "outnumbered" ? K::Outnumbered : k == "team" ? K::Team : K::None;
            pbz::log::Info("影塔 QA：下一个小兵段换成创意层 {}", qaSpecial_ == K::None ? std::string("（关）") : std::string(tower::special::Name(qaSpecial_)));
            Broadcast("info.qa_special", qaSpecial_ == K::None ? std::string("测试：创意层指定已关") : "测试：下一个小兵段 = " + std::string(tower::special::Name(qaSpecial_)));
            return;
        }
        if (cmd == "qa_floor" || cmd == "qa_jade" || cmd == "qa_hex" || cmd == "qa_hex_only" || cmd == "qa_hp") {
            auto field = [&](const char* k) { return pbz::ipc::Field(line, k).value_or(""); };
            auto number = [&](const char* k, double d) { try { const auto v = field(k); return v.empty() ? d : std::stod(v); } catch (...) { return d; } };
            if (cmd == "qa_hp") {  // {"value":20000}：锁定生命上限（0 = 关），每帧生效，开着时当前生命补满一次
                qaHpMax_ = std::clamp(number("value", 0), 0.0, 1e8);
                qaHpFill_ = qaHpMax_ > 0;
                pbz::log::Info("影塔 QA：生命上限锁定 {}", qaHpMax_ > 0 ? std::format("{:.0f}", qaHpMax_) : std::string("关"));
                Broadcast("info.qa_hp", qaHpMax_ > 0 ? std::format("测试：生命上限 {:.0f}", qaHpMax_) : std::string("测试：生命上限恢复正常"));
                return;
            }
            if (!grow_.on || trip_.phase() != tower::trip::Phase::Inside || run_.phase != tower::Phase::Ready) { Broadcast("err." + cmd, "只能在塔内准备页用"); return; }
            if (cmd == "qa_jade") {  // {"value":50000}
                run_.jade = static_cast<int>(std::clamp(number("value", run_.jade), 0.0, 2e9));
                SaveRun();
                pbz::log::Info("影塔 QA：魂玉设为 {}", run_.jade);
                Broadcast("info.qa_jade", std::format("测试：魂玉 {}", run_.jade));
                return;
            }
            if (cmd == "qa_hex_only") {  // {"keep":"反震,仆从大师,钱生钱"}：只留这些海克斯，其余卸下（栏位记成已退役编号，层号 / 栏位进度不变）
                const auto keep = field("keep");
                auto kept = [&](int id) {
                    if (id < 0 || id >= tower::hex::Count) return false;
                    const std::string name(tower::hex::defs[id].name);
                    size_t a = 0;
                    while (a <= keep.size()) {
                        size_t b = keep.find(',', a); if (b == std::string::npos) b = keep.size();
                        if (keep.substr(a, b - a) == name) return true;
                        a = b + 1;
                    }
                    return false;
                };
                auto& hx = grow_.hexes;
                std::string removed;
                for (auto& p : hx.picks) if (p >= 0 && p != tower::hex::Invincible && !kept(p)) { removed += std::string(removed.empty() ? "" : "、") + std::string(tower::hex::defs[p].name); p = tower::hex::Invincible; }
                for (auto& p : hx.extra) if (p >= 0 && p != tower::hex::Invincible && !kept(p)) { removed += std::string(removed.empty() ? "" : "、") + std::string(tower::hex::defs[p].name); p = tower::hex::Invincible; }
                SaveHex();
                pbz::log::Info("影塔 QA：只留海克斯 {}；卸下 {}", keep, removed.empty() ? "无" : removed);
                Broadcast("info.qa_hex", "测试：只留 " + keep);
                return;
            }
            if (cmd == "qa_hex") {  // {"name":"反震"}：放进额外栏位（不占 10 层一个的栏位）
                const auto name = field("name");
                int id = -1;
                for (int i = 0; i < tower::hex::Count && id < 0; ++i) if (tower::hex::Playable(i) && tower::hex::defs[i].name == name) id = i;
                // 3.6.0：已有且能升级的再发一张 = 升一级（测 Lv2 / Lv3）
                if (id < 0 || (grow_.hexes.Has(static_cast<tower::hex::Id>(id)) && !grow_.hexes.CanUpgrade(static_cast<tower::hex::Id>(id)))) { Broadcast("err.qa_hex", id < 0 ? "没有这个海克斯" : "已经满级"); return; }
                grow_.hexes.extra.push_back(id); grow_.hexes.extraAt.push_back(-1); grow_.hexes.extraBase.push_back(grow_.hexes.endlessCleared);
                grow_.hexes.seen |= tower::hex::Bit(id);
                SaveHex();
                pbz::log::Info("影塔 QA：加海克斯 {}", name);
                Broadcast("info.qa_hex", "测试：已获得 " + name);
                return;
            }
            // qa_floor {"to":41} 或 {"boss":"雷婷"}：下一层改为该 Boss 层（跳过它前面的小怪段；之后的小怪段照常）
            int to = static_cast<int>(number("to", -1));
            if (const auto key = field("boss"); !key.empty()) {
                to = -1;
                for (int f = 1; f <= std::min(50, run_.floors) && to < 0; ++f) {
                    const auto fl = tower::floorplay::ScheduledFloor(run_.seed, f, run_.shuffleBosses);
                    for (int i = 0; i < fl.count && i < 3 && to < 0; ++i) {
                        const int b = FindBoss(std::string(fl.bosses[i]));
                        if (b >= 0 && (tower::kBossPool[b].name.find(key) != std::string_view::npos || fl.bosses[i].find(key) != std::string_view::npos)) to = f;
                    }
                }
                if (to < 0) { Broadcast("err.qa_floor", "这一局的 50 层里没有这个 Boss：" + key); return; }
            }
            if (to < 1 || to > run_.floors) { Broadcast("err.qa_floor", "层号无效"); return; }
            const int from = run_.floor + 1;
            run_.floor = to - 1; run_.peak = std::min(run_.peak, to - 1); run_.shopFloor = std::min(run_.shopFloor, to - 1);
            run_.mobFloor = to; run_.mobWave = tower::mobs::WaveCount(to);
            lastBossVenue_ = -1;
            SaveRun();
            pbz::log::Info("影塔 QA 跳层：下一层 {} → {}（显示 {}）", from, to, ShownBoss(to));
            Broadcast("info.qa_floor", std::format("测试：下一层第 {} 层", ShownBoss(to)));
            return;
        }
        if (cmd == "qa_endless_scale") {
            auto& st = tower::balance::g_endlessStep;
            auto take = [&](const char* key, double& v) {
                if (const auto f = pbz::ipc::Field(line, key); f && !f->empty()) {
                    try { const double x = std::stod(*f); if (std::isfinite(x)) v = std::clamp(x, 0.1, 100.0); } catch (...) {}
                }
            };
            take("hp", st.hp); take("atk", st.atk); take("brk", st.brk); take("shachi", st.shachi);
            const auto text = std::format("无尽抬高：血量 ×{:g}、攻击 ×{:g}、破防 ×{:g}、杀气上限 ×{:g}（小怪取抬高部分的 80%）", st.hp, st.atk, st.brk, st.shachi);
            pbz::log::Info("影塔 QA：{}", text);
            Broadcast("info.qa_endless_scale", text);
            return;
        }
#endif
        const auto requestId = pbz::ipc::Field(line, "request_id").value_or("");
        const auto client = pbz::ipc::Field(line, "client").value_or("");
        const bool write = cmd != "state" && cmd != "probe";
        lastCode_.clear();
#ifdef PBZ_RELEASE
        // 发布版：只接受游戏内影塔界面会发的命令；开发验收 / 实验命令（call、grant、sandbox、diagmode、equip…）一律不收
        static constexpr std::string_view kPlayerCmds[] = {"state", "start", "next", "finish", "pick", "shop", "prep", "reroll", "st",
                                                           "recover", "forget", "hidden", "resume", "suspend", "stop", "restart", "badboss", "skipfloor", "grow",
                                                           "rebirth", "rebirth_no", "hex_pick", "hex_refresh",
                                                           "trade", "checkpoint", "arsenal_set", "endless_resume", "endless_import", "endless_commit", "forge_pick"};  // 3.6.1 批量锻造确认  // 3.5.0 无尽模式入口  // 3.1.0 N7：驿站交易、无尽存档点回退、武库扩建（之前漏放，发布版点了报"不提供"）
        if (std::find(std::begin(kPlayerCmds), std::end(kPlayerCmds), cmd) == std::end(kPlayerCmds)) {
            Broadcast("err.release", "发布版不提供这个命令");
            return Ack(requestId, cmd);
        }
#endif
        // 诊断模式"只开沙盒"：只接受状态 / 沙盒 / 模式切换，其余（开局、下一层、传送、换兵器…）一律拒绝
        if (cmd == "diagmode") {
            const bool on = pbz::ipc::Field(line, "value").value_or("state") == "on";
            if (pbz::ipc::Field(line, "value").value_or("state") != "state") {
                if (on && run_.phase != tower::Phase::Idle) Broadcast("err.diag_mode", "有进行中的局：先暂离或停止，再开诊断模式");
                else if (!towerdiag::SetSandboxOnly(on)) Broadcast("err.diag_mode", "诊断模式开关写入失败");
                else Broadcast(on ? "info.diag_on" : "info.diag_off", on ? "诊断模式：只开沙盒，影塔战斗 / 界面 / 传送 / 清场均不活动" : "已退出诊断模式");
                pbz::log::Info("影塔：诊断模式（只开沙盒）{}", towerdiag::SandboxOnly() ? "开" : "关");
            } else Broadcast("info.state", "");
            return Ack(requestId, cmd);
        }
        if (towerdiag::SandboxOnly() && write && cmd != "sandbox") {
            Broadcast("err.diag_mode", "诊断模式（只开沙盒）中，影塔玩法不活动；先关诊断模式");
            return Ack(requestId, cmd);
        }
        if (write && !controller_.empty() && !client.empty() && client != controller_ && pbz::ipc::Field(line, "takeover").value_or("") != "true" && cmd != "start" && cmd != "resume") {
            Broadcast("err.not_controller", "另一个界面正在控制这一局");
            return Ack(requestId, cmd);
        }
        if (auto er = pbz::ipc::Field(line, "expect_run"); write && er && *er != run_.id) { Broadcast("err.stale_run", "这一局已经变了"); return Ack(requestId, cmd); }
        if (auto ev = pbz::ipc::Field(line, "expect_rev"); write && ev && *ev != std::to_string(rev_)) { Broadcast("err.stale_revision", "界面状态过期，请按最新状态操作"); return Ack(requestId, cmd); }
        resumeClient_ = client;
        Dispatch(cmd, line, client);
        if (!requestId.empty()) Ack(requestId, cmd);
    }
    // 回执：本次命令最后一条状态消息的代码 err.* = 失败
    void Ack(const std::string& requestId, const std::string& cmd) {
        if (requestId.empty()) return;
        const bool ok = lastCode_.rfind("err.", 0) != 0;
        pbz::ipc::Send(std::format(R"({{"op":"tower","event":"ack","request_id":"{}","cmd":"{}","ok":{},"code":"{}","run_id":"{}","revision":{}}})",
                                   J(requestId), J(cmd), ok, J(lastCode_), J(run_.id), rev_));
    }

    void Dispatch(const std::string& cmd, const std::string& line, const std::string& client) {
        auto num = [&](const char* k, double def) { try { return std::stod(pbz::ipc::Field(line, k).value_or("")); } catch (...) { return def; } };
        // 1.0.1：殒命后还没选重生符就开新局 / 重开 / 出塔 = 不用（不留下可续的局存档）
        if (rebirthAsk_ && (cmd == "start" || cmd == "restart" || cmd == "stop")) RebirthDecide(false);
        if (cmd == "start") {
            if (!available) { Broadcast("err.not_ready", "影塔还没就绪：" + reason); return; }
            // 跳层编辑器（用户 2026-11-06：录视频演示后面层的 Boss）：带 editor:"floor" 的开局在发布版也接受起始层 / 补记挑战次数 /
            // 补发跳过层的固定点数；在塔档里时放弃当前局、塔内重开。其余开发参数（血量 / 场地 / 方案）发布版仍然忽略
            const bool editorStart = pbz::ipc::Field(line, "editor").value_or("") == "floor";
            const bool importSeed = pbz::ipc::Field(line, "seed_import").value_or("") == "1";
            uint64_t importedValue = 0; int importedDifficulty = 0, importedShuffle = 0;
            if (importSeed && (editorStart || trip_.phase() != tower::trip::Phase::Outside || ImportedSeed(&importedValue, &importedDifficulty, &importedShuffle) != 1 || !BackupForSeed())) {
                Broadcast("err.seed_import", "种子无效、影塔尚未退出或旧塔档备份失败；请退出影塔并重新导入"); return;
            }
            // 塔内重开：已在塔档里、上一局已结束（死亡 / 通关结算页）或编辑器开局 → 不回原档，直接用新塔档重生成（Trip::Rebuild）
            const bool rebuildInTower = trip_.phase() == tower::trip::Phase::Inside && (run_.phase == tower::Phase::Lost || run_.phase == tower::Phase::Won || editorStart);
            if (trip_.phase() != tower::trip::Phase::Outside && !rebuildInTower) { Broadcast("err.exit_pending", TripBusyText()); return; }
            // 3.5.0（用户：100 层爬塔放开重置）：开新爬塔前，停在当前位置的无尽存档挪到无尽存档位（不丢）；停放的旧爬塔局作废
            if (run_.phase == tower::Phase::Idle && trip_.phase() == tower::trip::Phase::Outside && !importSeed) {
                if (ActiveKind() == 2 && !ParkActive()) { Broadcast("err.slot", "无尽存档挪位失败，未开新局"); return; }
                DeleteFileW(SlotPath(L"climb-run.txt").c_str()); DeleteFileW(SlotPath(L"climb-growth.txt").c_str());
            }
            const tower::Run endedRun = run_; const std::string endedSettlement = settlement_;  // 塔内重开失败时退回结算页
            if (run_.phase != tower::Phase::Idle) { Clear("restart"); ReleaseShentong(); Unwear("restart"); }
            uint64_t seed = 0;  // 种子按十进制字符串传，避免经 double 丢精度
            try { seed = std::stoull(pbz::ipc::Field(line, "seed").value_or("0")); } catch (...) { seed = 0; }
            if (importSeed) seed = importedValue;
            s_hidden = false;
            towermusic::Stop("开新局");
            run_.Start(seed ? seed : GetTickCount64(), importSeed ? 50 : static_cast<int>(num("floors", 3)));
#ifdef PBZ_RELEASE
            const bool devStart = false;  // 发布版：忽略开局的开发参数（from 跳层、hp0、schedule/replay/arena 开关）
#else
            const bool devStart = true;
#endif
            if (devStart) run_.tuning.hp0 = std::clamp(num("hp0", run_.tuning.hp0), 1000.0, 10000000.0);
            run_.difficulty = pbz::ipc::Field(line, "difficulty").value_or("") == "easy" ? 1 : 0;  // 1.0.1 难度：只改 Boss 数值
            if (importSeed) run_.difficulty = importedDifficulty;
            if (importSeed && importedShuffle >= 1 && importedShuffle <= 4) run_.shuffleBosses = importedShuffle;  // 3.1.0：按导出局的 Boss 排序方案
            s_lastDiff = run_.difficulty;
            deathShown_ = 0; rebirthReload_ = false; lastBossVenue_ = -1;
            mob_ = {}; mobBad_.clear(); waveRecovery_ = {}; waveAvoid_.clear(); waveBadVenues_.clear(); mobOff_ = editorStart && pbz::ipc::Field(line, "nomobs").value_or("") == "1";
            directFloor_ = -1; autoNextAt_ = 0;  // 新局：重放层先照常走原生入场
            nativeRetryPending_ = false; nativeRetryFloor_ = nativeRetryBoss_ = -1;
            pbz::log::Info("影塔开局难度：{}", tower::floorplay::DifficultyName(run_.difficulty));
            ForeignMods();  // 兼容检查：开局记一次日志
            chal_ = tower::floorplay::Challenges(run_.seed, run_.floors, run_.difficulty, run_.shuffleBosses);  // 随机遭遇确定后按实际层型分配挑战
            if (editorStart) {  // 跳层工具（自用验收）：背包放道具、指定某层的附加挑战；只认编辑器开局，玩家的开局一律不收
#ifndef PBZ_RELEASE
                run_.jade = std::clamp(static_cast<int>(num("test_jade", 0)), 0, 100000);
#endif
                const std::string give = pbz::ipc::Field(line, "give").value_or("");  // "revive:1,rebirth:1"
                for (size_t a = 0; a < give.size();) {
                    size_t b = give.find(',', a); if (b == std::string::npos) b = give.size();
                    const std::string kv = give.substr(a, b - a); a = b + 1;
                    const size_t c = kv.find(':'); if (c == std::string::npos) continue;
                    const std::string id = kv.substr(0, c); int n = 0;
                    try { n = std::stoi(kv.substr(c + 1)); } catch (...) { continue; }
                    for (int i = 0; i < tower::kItemCount; ++i)
                        if (tower::kItems[i].id == id) { run_.items[i] = std::clamp(run_.items[i] + n, 0, tower::rewards::kItemStackCap); pbz::log::Info("影塔跳层编辑器：背包放入 {} ×{}", tower::kItems[i].name, n); }
                }
                const std::string fc = pbz::ipc::Field(line, "chal").value_or("");  // "16:timed"（不存档，只本次运行）
                if (const size_t c = fc.find(':'); c != std::string::npos) {
                    int f = 0; try { f = std::stoi(fc.substr(0, c)); } catch (...) { f = 0; }
                    const std::string k = fc.substr(c + 1);
                    using C = tower::floorplay::Challenge;
                    const C kind = k == "timed" ? C::Timed : k == "nohit" ? C::NoHit : k == "potion" ? C::NoPotion : k == "berserk" ? C::Berserk : C::None;
                    if (f >= 1 && f <= 50) { chal_[f] = kind; pbz::log::Info("影塔跳层编辑器：第 {} 层附加挑战指定为 {}（只本次运行）", f, tower::floorplay::ChallengeName(kind)); }
                }
            }
            if (const int from = (devStart || editorStart) ? static_cast<int>(num("from", 1)) : 1; from > 1 && from <= run_.floors) {  // 开发验收：直接从第 N 层开（方案第 9 节逐层型验收）
                run_.floor = from - 1;
                // 开发验收：从第 50 层连打第 51 层时补记前面完成的额外·限时 / 禁药次数（只在显式 from 开局时生效）
                run_.doneTimed = std::clamp(static_cast<int>(num("timed", 0)), 0, 99);
                run_.donePotion = std::clamp(static_cast<int>(num("potion", 0)), 0, 99);
                pbz::log::Info("影塔开发验收：从第 {} 层开局（跳过前面的层，奖励 / 成长不补；补记额外·限时 {}、禁药 {}）", from, run_.doneTimed, run_.donePotion);
            }
            actors_.clear();
            run_.peak = run_.floor;  // 跳层开局：起始层之前不算"打过"
            run_.id = std::format("{:016x}", run_.seed ^ (GetTickCount64() * 0x9E3779B97F4A7C15ull));
            RunLog(std::format("start\t{}\t{}\t{}\t{}", run_.id, static_cast<long long>(std::time(nullptr)), run_.floors, run_.difficulty ? "easy" : "hard"), true);
            controller_ = client;
            settlement_.clear();
            clampHp_ = true;
            backedUp_ = false;
            arenaOn_ = !devStart || pbz::ipc::Field(line, "arena").value_or("on") != "off";
            replayOn_ = !devStart || pbz::ipc::Field(line, "replay").value_or("on") != "off";  // 原生遭遇重放层（关掉 = 重放层按 1v1 打第一阶段）
            schedOff_ = devStart && pbz::ipc::Field(line, "schedule").value_or("on") == "off";  // 50 层方案逐层表（off = 旧的随机抽取，开发对照用）
            travelThenSpawn_ = false; spawnAt_ = 0; repicks_ = 0; badBosses_.clear(); venueBad_.clear(); stayVenue_ = stayFloor_ = -1;
            // 原生成长（v2，默认）：备份真档 → 存档沙盒（含进塔基准）→ 停成就 → 套用塔档（兵器 / 能力 / 药 / 饰品）。
            // 任何一步失败都不开局、游戏不被改动。"growth":"off" 走旧的统一起点（离线对照用）
            grow_ = {};
            grow_.difficulty = run_.difficulty;
            if (run_.floors >= 50 && !s_hidden) grow_.hexes.Init(run_.seed);
            gains_.clear();
            baseAtk_ = baseHp_ = 0;
            growHpComp_ = growDmgComp_ = 1;
            // Boss 准入：正式池只放实机验证过的（pool:"audit" 开发审计用全静态池）；boss:<类路径或身份> 固定每层 Boss（开发验收）
            // v14：玩家从游戏内菜单开 50 层局时也按完整逐层表（逐层表本身就是挑好的内容）；只有显式 pool:"verified" 才走旧的实机白名单。禁用名单照样生效
            poolAudit_ = pbz::ipc::Field(line, "pool").value_or(num("floors", 3) >= 50 ? "audit" : "verified") == "audit";
            run_.audit = poolAudit_;  // 存进局存档：续局沿用同一个池（33-38 静态发现：Resume 不恢复 poolAudit_，新模块默认 false）
            nativePoints_ = pbz::ipc::Field(line, "native_points").value_or("on") != "off";  // 塔内原生升级 / 能力面板用点数结算
            towerentry::SetFullState(pbz::ipc::Field(line, "full_state").value_or("on") != "off");  // 开层满状态（用户 2026-11-01 定为免费塔规则）
            dustGate_ = pbz::ipc::Field(line, "dust_gate").value_or("on") != "off";          // 塔内只读尘预算（用户 2026-10-31 同意；绝不写入、不带出塔，见 tower_points.cpp）
            fixedBoss_ = FindBoss(pbz::ipc::Field(line, "boss").value_or(""));
            LoadAdmission();
            if (const auto b = pbz::ipc::Field(line, "boss"); b && !b->empty() && fixedBoss_ < 0) { run_ = {}; Broadcast("err.unknown_boss", "固定 Boss 不在池里：" + *b); return; }
            plan_.clear();
            if (const auto p = pbz::ipc::Field(line, "plan"); p && !p->empty()) {
                if (fixedBoss_ >= 0) { run_ = {}; Broadcast("err.plan", "plan 和 boss 不能同时用"); return; }
                std::string why;
                if (!tower::plan::Parse(*p, Pool(), &plan_, &why)) { run_ = {}; plan_.clear(); Broadcast("err.plan", "审计计划被拒绝：" + why); return; }
                run_.floors = static_cast<int>(plan_.size());
                pbz::log::Info("影塔审计计划：{} 层（{}）——开发验收用，不改正式编排", plan_.size(), *p);
                // 计划只在内存里：保存退出后续局会丢，退成随机层（实机 B 批：续局后第 1 层变成玉面郎君原生场地）。按局号另存一份
                if (!AtomicTowerFile(PlanFile(), run_.id + "\n" + *p)) pbz::log::Info("影塔审计计划：计划文件写入失败，续局后不会恢复计划");
            } else DeleteFileW(PlanFile().c_str());
            if (fixedBoss_ >= 0 && !Admitted(fixedBoss_)) {  // 固定入口同样服从禁用表（Hun 内置 / 本机 boss-denied.txt）与准入
                run_ = {};
                Broadcast("err.boss_denied", std::string("固定 Boss 被禁用或未准入：") + std::string(tower::kBossPool[fixedBoss_].name));
                fixedBoss_ = -1;
                return;
            }
            if (!devStart || pbz::ipc::Field(line, "growth").value_or("on") != "off") {  // 发布版一律走独立塔档
                // 独立塔档：原游戏新档起手（tower_growth.h Kit），生成游戏格式的塔内存档放进内存，玩家回标题继续载入
                auto prof = tower::growth::Profile::Kit();
                if (run_.floors < 10) prof.TrialKit();
                if (editorStart && run_.floor > 0 && pbz::ipc::Field(line, "catchup").value_or("") == "1") {  // 编辑器：补发跳过各层的固定收获（点数 / 药瓶等级）
                    for (int f = 1; f <= run_.floor; ++f) {
                        const auto plan = tower::growth::PlanFor(f, run_.floors);
                        prof.Earn(plan.temper, plan.cultivate);
                        if (plan.medLevel > prof.medLevel) prof.medLevel = std::min(plan.medLevel, tower::growth::kMedLevelMax);
                    }
                    pbz::log::Info("影塔跳层编辑器：补发第 1–{} 层固定收获（修为 {}、药瓶 {} 级）", run_.floor, prof.cultivate, prof.medLevel);
                }
                if (editorStart) {  // 编辑器（自用验收）：塔档里放兵器 "weapons":"HeiMoShang,Double"（主兵器 id）、铸魂令 "soul":"1"
                    const std::string ws = pbz::ipc::Field(line, "weapons").value_or("");
                    for (size_t a = 0; a < ws.size();) {
                        size_t b = ws.find(',', a); if (b == std::string::npos) b = ws.size();
                        const std::string id = ws.substr(a, b - a); a = b + 1;
                        int w = tower::growth::WeaponIndex(id, 0);
                        if (w < 0) w = tower::growth::WeaponIndex(id, 1);
                        if (w >= 0 && prof.AddWeapon(w)) pbz::log::Info("影塔跳层编辑器：塔档放入兵器 {}", tower::growth::kWeapons[w].name);
                        else pbz::log::Info("影塔跳层编辑器：兵器 {} 没放进去（不认识或已有）", id);
                    }
                    if (const int t = static_cast<int>(num("temper", 0)); t > 0) { prof.Earn(std::min(t, 200), 0); pbz::log::Info("影塔跳层编辑器：塔档放入淬炼点 ×{}", std::min(t, 200)); }
                    if (const int s = static_cast<int>(num("soul", 0)); s > 0) { prof.soulTokens += std::min(s, 9); pbz::log::Info("影塔跳层编辑器：塔档放入铸魂令 ×{}", std::min(s, 9)); }
                }
                trip_.SetAuto(pbz::ipc::Field(line, "auto_trip").value_or("on") != "off");  // 自动回标题 / 继续（off = 旧的手动流程）
                if (!(rebuildInTower ? trip_.Rebuild(Io(), prof, tower::save::DefaultEquip(prof), Stamp()) : trip_.Enter(Io(), prof, tower::save::DefaultEquip(prof), Stamp()))) {
                    importedSeedInFlight_ = 0;
                    if (rebuildInTower) { run_ = endedRun; settlement_ = endedSettlement; } else run_ = {};
                    Broadcast("err.growth_enter", (rebuildInTower ? "塔内重开失败，可以再试或退出影塔：" : "进塔失败，游戏未改动：") + trip_.error());
                    return;
                }
                if (rebuildInTower) pbz::log::Info("影塔独立塔档：塔内重开，新塔档已放进内存（不回原档）");
                pbz::log::Info("影塔独立塔档：世界 / 剧情 / 地图 / 角色状态用{}", trip_.usedTemplate() ? "随包世界模板（与玩家进度、周目无关）" : "玩家原档（没有可用的世界模板）");
                if (!trip_.droppedKeys().empty()) {
                    std::string k; for (auto& x : trip_.droppedKeys()) k += (k.empty() ? "" : "、") + x;
                    pbz::log::Info("影塔独立塔档：玩家原档独有的分区没有带进塔档（二周目 / 通关后才有的系统）：{}", k);
                }
                grow_.profile = prof;
                importedSeedInFlight_ = importSeed ? importedValue : 0;
                importedSeedDifficulty_ = importedDifficulty;
                FreshRewards();
                backedUp_ = true;  // 进塔时已备份真档
                SaveTrip();
                pbz::log::Info("影塔独立塔档：塔内存档已放进内存（{} 主 {} 影 {} 药），等玩家回标题继续载入", tower::save::DefaultEquip(prof).main.size(),
                               tower::save::DefaultEquip(prof).shaqi.size(), prof.medicines.size());
            }
            pbz::HoldFeature(this, "tower");
            towergate::Touch();
            SaveRun();
            if (grow_.on) pbz::log::Info("影塔开局：{} 层，种子 {}（原生成长）", run_.floors, run_.seed);
            else pbz::log::Info("影塔开局：{} 层，种子 {}（统一起点：攻击 {} 生命 {}）", run_.floors, run_.seed, run_.standard.attack, run_.standard.maxHp);
            if (trip_.phase() == tower::trip::Phase::Entering) Broadcast("info.enter_reload", EnterText());
            else Broadcast("info.started", "开局");
        } else if (cmd == "next") {
            Next();
        }
#ifdef PBZ_HEX_TEST
        else if (cmd == "hex_test_temper") {
            // Explicit user-authorized QA grant; not compiled into production.
            if (!grow_.on || trip_.phase() != tower::trip::Phase::Inside || run_.phase != tower::Phase::Ready ||
                pbz::ipc::Field(line, "expect_run").value_or("") != run_.id || num("amount", 0) != 40) {
                Broadcast("err.hex_test_temper", "测试发放要求当前测试局的准备阶段及明确的 40 点"); return;
            }
            if (!BackupForSeed()) { Broadcast("err.hex_test_temper", "测试塔档备份失败，未发放点数"); return; }
            const auto previous = grow_.profile;
            grow_.profile.Earn(40, 0);
            if (!SaveHex()) { grow_.profile = previous; Broadcast("err.hex_test_temper", "测试塔档保存失败，未发放点数"); return; }
            pbz::log::Info("影塔测试局 {}：按玩家要求发放 40 淬炼点，当前 {}", run_.id, grow_.profile.temper);
            Broadcast("info.hex_test_temper", "测试局已获得 40 淬炼点");
        }
        else if (cmd == "hex_test_heavy_grant") {
            // Old continuous QA run passed the last test selection before this
            // augment existed; use its one empty QA slot, never a release save.
            auto& hex=grow_.hexes; constexpr int slot=23; // original QA slot, stable across schema expansion
            if (!grow_.on || trip_.phase()!=tower::trip::Phase::Inside || run_.phase!=tower::Phase::Ready ||
                pbz::ipc::Field(line,"expect_run").value_or("")!=run_.id || hex.pending || hex.Has(tower::hex::Heavy) ||
                hex.picks[slot]>=0 || (hex.completed & (1u<<slot)) || !towerweaponprobe::Supported()) {
                Broadcast("err.hex_test_heavy","当前测试局无法安全添加重量级打手"); return;
            }
            if(!BackupForSeed()){Broadcast("err.hex_test_heavy","测试塔档备份失败");return;}
            const auto previous=hex;
            hex.picks[slot]=tower::hex::Heavy; hex.pickedAt[slot]=tower::hex::SlotFloor(slot);
            hex.completed|=1u<<slot; hex.seen|=tower::hex::Bit(tower::hex::Heavy);
            if(!SaveHex()){hex=previous;Broadcast("err.hex_test_heavy","测试塔档保存失败");return;}
            pbz::log::Info("影塔测试局 {}：补入重量级打手（仅旧测试局槽位）",run_.id);
            Broadcast("info.hex_test_heavy","测试局已获得重量级打手");
        }
        else if (cmd == "qa_hex") {  // QA（3.0.1 复测）：准备页把指定海克斯放进第一个空栏位 {"name":"火上浇油"}；只在测试版
            auto& hex = grow_.hexes;
            const auto name = pbz::ipc::Field(line, "name").value_or("");
            int id = -1;
            for (int i = 0; i < tower::hex::Count && id < 0; ++i) if (tower::hex::Playable(i) && tower::hex::defs[i].name == name) id = i;
            int slot = -1;
            for (int i = 0; i < tower::hex::kSlots && slot < 0; ++i) if (hex.picks[i] < 0 && !(hex.completed & (1u << i))) slot = i;
            if (!grow_.on || !hex.enabled || trip_.phase() != tower::trip::Phase::Inside || run_.phase != tower::Phase::Ready || hex.pending || id < 0 ||
                hex.Has(static_cast<tower::hex::Id>(id)) || slot < 0) {
                Broadcast("err.qa_hex", "现在不能添加（要在塔内准备页、名称正确、未持有、有空栏位）"); return;
            }
            if (!BackupForSeed()) { Broadcast("err.qa_hex", "测试塔档备份失败"); return; }
            const auto previous = hex;
            hex.picks[slot] = id; hex.pickedAt[slot] = tower::hex::SlotFloor(slot);
            hex.completed |= 1u << slot; hex.seen |= tower::hex::Bit(id);
            if (!SaveHex()) { hex = previous; Broadcast("err.qa_hex", "测试塔档保存失败"); return; }
            pbz::log::Info("影塔测试局 {}：QA 添加海克斯 {}（栏位 {}）", run_.id, name, slot);
            Broadcast("info.qa_hex", std::format("已添加 {}", name));
        }
        else if (cmd == "hex_hit_probe") {
            if (pbz::ipc::Field(line, "value").value_or("") == "off") { towerhitprobe::Stop(); Broadcast("info.hex_probe", "攻击来源观察已停止"); }
            else {
                void* pawn = nullptr;
                for (const auto& t : attr::Targets()) if (t.who == attr::Who::Player && attr::Alive(t)) { pawn = t.obj; break; }
                const bool ok = run_.phase == tower::Phase::Fighting && pawn && towerhitprobe::Start(pawn);
                Broadcast(ok ? "info.hex_probe" : "err.hex_probe", ok ? "攻击来源观察已开启" : "攻击来源观察不可用");
            }
        }
        else if (cmd == "hex_arcane_test") {
            const bool on = pbz::ipc::Field(line,"value").value_or("")=="on";
            if (!on) { arcaneTest_=false; towerarcane::Stop(); Broadcast("info.hex_arcane", "秘术冲拳测试已关闭"); return; }
            const bool countMode = pbz::ipc::Field(line,"mode").value_or("")=="count";
            const double rate=num("rate",1.2);
            if (!std::isfinite(rate) || rate < 1.0 || rate > 20.0) { Broadcast("err.hex_arcane", "充能测试倍率须在 1 到 20 之间"); return; }
            if (arcaneTest_ && towerarcane::Tick(Player() ? Player()->obj : nullptr)) {
                arcaneChargeMul_=countMode ? 1.0f : static_cast<float>(rate);
                if (!towerarcane::SetCountCandidate(countMode)) { arcaneTest_=false; towerarcane::Stop(); Broadcast("err.hex_arcane", "原生命中接口校验失败，充能测试未开启"); return; }
                Broadcast("info.hex_arcane", "秘术冲拳测试充能倍率已更新"); return;
            }
            const Target* p=Player();
            const int charge=attr::AttrOffset("UltimateChargeAdditiveRate");
            const bool ok=run_.phase==tower::Phase::Fighting && grow_.on && p && attr::Alive(*p) && charge>=0 && towerarcane::Start(p->obj);
            arcaneTest_=ok && towerarcane::SetCountCandidate(countMode);
            if (!arcaneTest_) towerarcane::Stop();
            arcaneChargeMul_=countMode ? 1.0f : static_cast<float>(rate);
            arcaneLastCharge_=-1; arcaneLastCount_=-1; arcaneSamples_=0;
            Broadcast(arcaneTest_?"info.hex_arcane":"err.hex_arcane",arcaneTest_?(countMode?"秘术冲拳：普通攻击计数充能测试已开启":std::format("秘术冲拳测试已开启，充能倍率暂为 {:g}",rate)):"秘术冲拳原生接口未就绪");
        }
        else if (cmd == "hex_heavy_test") {
            const bool on=pbz::ipc::Field(line,"value").value_or("")=="on";
            const bool ok=run_.phase==tower::Phase::Fighting && towerweaponprobe::SetTestBonus(on);
            Broadcast(ok?"info.hex_heavy_test":"err.hex_heavy_test",ok?(on?"重量级换算测试开启":"重量级换算测试关闭"):"重量级测试接口尚未就绪");
        }
        else if (cmd == "hex_speed_test") {
            const float multiplier = static_cast<float>(num("value", -1));
            if (!towerspeed::SetMultiplier(multiplier)) { Broadcast("err.hex_speed_test", "攻速测试倍率须在 1 到 4 之间"); return; }
            Broadcast("info.hex_speed_test", "攻速测试倍率已更新");
        }
        else if (cmd == "hex_test_hp") {
            const double fraction = num("fraction", -1);
            if (!grow_.on || run_.phase != tower::Phase::Fighting || fightSince_ <= 0 || hp_ < 0 || hpMax_ < 0 || !(fraction > 0 && fraction <= 1)) {
                Broadcast("err.hex_test_hp", "只能在测试战斗中设置有效生命比例"); return;
            }
            bool done = false;
            for (const auto& t : attr::Targets()) if (t.who == attr::Who::Player && attr::Alive(t)) {
                const float max = ue::Get<float>(attr::AttrCurrent(t, hpMax_), 0.0f);
                const float old = ue::Get<float>(attr::AttrCurrent(t, hp_), 0.0f);
                if (max > 0 && std::isfinite(max)) {
                    const float value = static_cast<float>(max * fraction);
                    done = ue::Write(attr::AttrBase(t, hp_), &value, 4) && ue::Write(attr::AttrCurrent(t, hp_), &value, 4);
                    if (done) towerwidgets::RefreshHpBarFast(old, value);
                }
                break;
            }
            Broadcast(done ? "info.hex_test_hp" : "err.hex_test_hp", done ? "测试生命值已设置" : "测试生命值设置失败");
        }
        else if (cmd == "qa_floor") {  // QA：准备页把下一层改成第 to 层（保留本局海克斯 / 成长 / 背包；跳过该层的小怪段）
            const int to = static_cast<int>(num("to", -1));
            if (!grow_.on || run_.phase != tower::Phase::Ready || to < 2 || to > run_.floors) { Broadcast("err.qa_floor", "只能在准备页把下一层设为 2 到最终层"); return; }
            const int from = run_.floor + 1;
            run_.floor = to - 1;
            run_.mobFloor = to; run_.mobWave = tower::mobs::WaveCount(to);
            SaveRun();
            pbz::log::Info("影塔 QA 跳层：下一层 {} → {}（海克斯、成长、背包保留，该层小怪段跳过）", from, to);
            Broadcast("info.qa_floor", std::format("下一层改为第 {} 层", to));
        }
        else if (cmd == "qa_auto") {
            auto_.on = pbz::ipc::Field(line, "value").value_or("on") != "off";
            auto_.to = static_cast<int>(num("to", 130));
            try { auto_.seed = std::stoull(pbz::ipc::Field(line, "seed").value_or("0")); } catch (...) { auto_.seed = 0; }
            // fresh=1：塔里已有局也从第 1 层重开（编辑器开局，塔内重建，不回原档）
            auto_.started = run_.phase != tower::Phase::Idle && pbz::ipc::Field(line, "fresh").value_or("0") != "1";
            auto_.next = 0; auto_.phaseSince = GetTickCount64(); auto_.grantedAll = false; auto_.shopDoneFloor = -1; auto_.buys = 0;
            if (!auto_.on) qaGod_ = false;
            AutoSkipSequences(auto_.on);
            AutoEvent(auto_.on ? std::format("开始，目标显示第 {} 层", auto_.to) : "手动停止");
            Broadcast("info.qa_auto", auto_.on ? "QA：自动爬塔开始" : "QA：自动爬塔停止");
            return;
        }
        else if (cmd == "qa_god") {  // QA：塔内无敌 + 秒杀 {"value":"on|off"}
            qaGod_ = pbz::ipc::Field(line, "value").value_or("on") != "off";
            Broadcast("info.qa_god", qaGod_ ? "QA：无敌 + 秒杀（塔内）" : "QA：无敌 + 秒杀已关");
        }
        else if (cmd == "qa_endless") {  // QA：准备页直接跳到无尽模式第 51 层（显示 100），保留本局海克斯 / 成长 / 背包
            if (!grow_.on || run_.phase != tower::Phase::Ready || run_.floors < 50 || run_.hidden || !plan_.empty()) { Broadcast("err.qa_endless", "只能在 50 层正式局的准备页跳到无尽模式"); return; }
            const int to = std::max(51, static_cast<int>(num("to", 51)));
            run_.endless = true; run_.floors = 50;
            run_.floor = to - 1; run_.peak = std::max(run_.peak, to - 1);
            run_.mobFloor = 51; run_.mobWave = 0;
            grow_.shop = {}; settlement_.clear();
            SaveRun();
            pbz::log::Info("影塔 QA 跳层：无尽模式，下一层第 {} 层（显示 {}）", to, ShownBoss(to));
            Broadcast("info.qa_endless", std::format("下一层：无尽第 {} 层", ShownBoss(to)));
        }
        else if (cmd == "qa_venue_warp") {  // QA：塔外、同一世界内点传送到场地采样点（核心点传送，悬停等加载；不开剧情、不写档）
            const auto world = pbz::ipc::Field(line, "world").value_or("");
            const double x = num("x", NAN), y = num("y", NAN), z = num("z", NAN), yaw = num("yaw", 0);
            double at[4]; std::string here, msg;
            auto reply = [&](bool ok, const std::string& m) {
                pbz::ipc::Send(std::format(R"({{"op":"tower","event":"qa_venue_warp","ok":{},"msg":"{}","world":"{}","to":[{:.1f},{:.1f},{:.1f}]}})",
                    ok, pbz::ipc::Esc(m), pbz::ipc::Esc(here), x, y, z));
            };
            if (run_.phase != tower::Phase::Idle || trip_.phase() != tower::trip::Phase::Outside) { reply(false, "only_outside_tower"); return; }
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(yaw)) { reply(false, "bad_point"); return; }
            if (!towerentry::LoadingClear() || pbz::TeleportBusy() || towertravel::Busy() || !pbz::PlayerWhere(at, &here)) { reply(false, "player_or_level_not_ready"); return; }
            if (world.empty() || world != here) { reply(false, "different_world"); return; }
            const bool ok = pbz::PointTeleport(x, y, z, yaw, &msg);
            pbz::log::Info("影塔 QA 场地传送：{} ({:.0f}, {:.0f}, {:.0f}) {}", here, x, y, z, msg);
            reply(ok, msg);
        }
        else if (cmd == "hex_drop") {  // QA：移除一个已拥有的海克斯（id = 枚举序号），后面的往前挪，写档
            const int id = static_cast<int>(num("id", -1));
            auto& h = grow_.hexes;
            if (!grow_.on || id < 0 || id >= tower::hex::Count || !h.Has(static_cast<tower::hex::Id>(id))) { Broadcast("err.hex_drop", "没有这个海克斯"); return; }
            const auto previous = h;
            std::vector<std::pair<int, int>> keep;
            for (size_t i = 0; i < h.picks.size(); ++i) if (h.picks[i] >= 0 && h.picks[i] != id) keep.emplace_back(h.picks[i], h.pickedAt[i]);
            h.picks.fill(-1); h.pickedAt.fill(0);
            for (size_t i = 0; i < keep.size(); ++i) { h.picks[i] = keep[i].first; h.pickedAt[i] = keep[i].second; }
            if (!SaveHex()) { h = previous; Broadcast("err.hex_save", "海克斯保存失败"); return; }
            if (id == tower::hex::Arsenal) { armMain_ = {}; armShaqi_ = {}; nativePick_.slot = -1; }
            pbz::log::Info("影塔海克斯：QA 移除 {}", tower::hex::defs[static_cast<size_t>(id)].name);
            Broadcast("info.hex_drop", std::format("已移除：{}", tower::hex::defs[static_cast<size_t>(id)].name));
        }
#endif
        else if (cmd == "hex_pick") {
            const int slot = static_cast<int>(num("slot", -1));
            if (slot < 0 || slot >= 3 || pbz::ipc::Field(line, "hex_floor").value_or("") != std::to_string(grow_.hexes.pending) || pbz::ipc::Field(line, "hex_id").value_or("") != std::to_string(grow_.hexes.offers[slot])) { Broadcast("err.hex_pick", "海克斯选项已变化，请重新选择"); return; }
            if (slot >= 0 && slot < 3 && grow_.hexes.offers[slot] >= 0 && grow_.hexes.offers[slot] < tower::hex::Count && !(HexAvailable() & (tower::hex::Bit(grow_.hexes.offers[slot]))))  // 3.3.4：接口暂不可用也照常发放（之前拒绝选择 → 卡片留着点不动），效果在接口就绪后生效
                pbz::log::Info("影塔海克斯：{} 的游戏接口暂不可用，照常发放，接口就绪后生效", tower::hex::defs[grow_.hexes.offers[slot]].name);
            const auto previous = grow_.hexes;
            const auto previousProfile = grow_.profile;
            const auto previousAttributes = grow_.hexAttributes;
            if (run_.phase != tower::Phase::Ready || !grow_.on || !grow_.hexes.Pick(slot)) { Broadcast("err.hex_pick", "当前海克斯选择无效"); return; }
            grow_.GrantHexChoice(static_cast<tower::hex::Id>(previous.offers[slot]));
            if (previous.offers[slot] == tower::hex::Transmutation) {  // 质变：立即随机拿 2 个
                const auto got = grow_.hexes.GrantRandom(tower::hex::kTransmuteCount, HexDrawMask());
                std::string names; for (int id : got) { grow_.GrantHexChoice(static_cast<tower::hex::Id>(id)); names += (names.empty() ? "" : "、") + std::string(tower::hex::defs[id].name); }
                pbz::log::Info("影塔海克斯：质变获得 {}", names.empty() ? "（没有可拿的）" : names);
                Broadcast("info.hex_transmute", "质变：获得 " + (names.empty() ? std::string("无") : names));
            }
            if (previous.offers[slot] == tower::hex::Arsenal) grow_.session.Sync(Game(), grow_.profile);  // QA 补的兵器写进原生背包
            if (previous.offers[slot] == tower::hex::AttributeStack)
                grow_.hexAttributes.BeginMul(run_.seed, grow_.hexes.ThrowerMul(), grow_.Ctx(run_.floor, run_.floors, run_.items));
            if (!SaveHex()) { grow_.hexes = previous; grow_.profile = previousProfile; grow_.hexAttributes = previousAttributes; Broadcast("err.hex_save", "海克斯保存失败，请重试"); return; }
            pbz::log::Info("影塔海克斯：第 {} 层选择 {}（种子 {}）", previous.pending, tower::hex::NameAt(previous.offers[slot], previous.OfferLevel(previous.offers[slot])), run_.seed);
            Broadcast("info.hex_pick", "已选择海克斯");
        } else if (cmd == "arsenal_set") {  // 武库扩建：装备页第 3、4 格选兵器（slot 0–1 主武器、2–3 影之武装；weapon = kWeapons 下标）
            const int slot = static_cast<int>(num("slot", -1)), w = static_cast<int>(num("weapon", -1));
            if (!grow_.on || !grow_.hexes.Has(tower::hex::Arsenal) || slot < 0 || slot > 3 || w < 0 || w >= tower::growth::kWeaponCount ||
                tower::growth::kWeapons[w].kind != slot / 2 || grow_.profile.Find(w) < 0) { Broadcast("err.arsenal", "这件兵器不能放进这一格"); return; }
            for (int k = 0; k < 4; ++k) if (k != slot && grow_.arsenal[static_cast<size_t>(k)] == w) grow_.arsenal[static_cast<size_t>(k)] = -1;
            grow_.arsenal[static_cast<size_t>(slot)] = w;
            SaveRun();
            armMain_ = {}; armShaqi_ = {};  // 轮转按新选择重排
            pbz::log::Info("影塔武库扩建：{}第 {} 格 = {}", slot < 2 ? "主武器" : "影之武装", slot % 2 + 3, tower::growth::kWeapons[w].name);
            Broadcast("info.arsenal", std::format("已放入：{}", tower::growth::kWeapons[w].name));
        } else if (cmd == "hex_refresh") {
            if (pbz::ipc::Field(line, "hex_floor").value_or("") != std::to_string(grow_.hexes.pending) || pbz::ipc::Field(line, "hex_rng").value_or("") != std::to_string(grow_.hexes.rng)) { Broadcast("err.hex_refresh", "海克斯选项已变化，请重新查看"); return; }
            const auto previous = grow_.hexes;
            const int slot = static_cast<int>(num("slot", -1));
            if (run_.phase != tower::Phase::Ready || !grow_.on || !grow_.hexes.Refresh(slot, HexDrawMask())) { Broadcast("err.hex_refresh", "本次无法刷新该海克斯"); return; }
            if (!SaveHex()) { grow_.hexes = previous; Broadcast("err.hex_save", "海克斯保存失败，请重试"); return; }
            Broadcast("info.hex_refresh", "海克斯已刷新");
        } else if (cmd == "checkpoint") {  // 无尽模式：殒命后回到最近的百层存档点
            CheckpointRestore();
        } else if (cmd == "rebirth" || cmd == "rebirth_no") {  // 1.0.1：殒命后用 / 不用重生符
            RebirthDecide(cmd == "rebirth");
        } else if (cmd == "finish") {  // 第 50 层通关结算页（已开第 51 层）点退出：本轮按通关结束
            if (run_.phase == tower::Phase::Ready && run_.floor >= 50) Win();
            else Broadcast("err.finish", "现在不能结算");
        } else if (cmd == "pick" && grow_.on) {  // 新奖励模型：复核 → 发放 → 结束奖励阶段；驿站层后开店
            if (run_.phase == tower::Phase::Reward && !grow_.OffersFor(run_.floor)) { grow_.DrawRewards(run_.floor, run_.floors, run_.seed, run_.items); SaveRun(); Broadcast("err.cannot_pick", "奖励已更新，请重新选择"); return; }
            tower::rewards::Offer got;
            const auto err = grow_.Claim(Game(), static_cast<int>(num("slot", -1)), run_.floor, run_.floors, run_.items, run_.jade, &got);
            if (err != tower::growth::Err::Ok) { Broadcast("err.cannot_pick", err == tower::growth::Err::AtCap ? "这一项已经无效，请刷新或选别的" : "现在不能选"); return; }
            if (got.kind == tower::rewards::Kind::Jade) { if (run_.bill.open) run_.bill.picked += got.amount; else InvestorBonus(got.amount); }  // 3.6.0 奖励页魂玉进本层账单
            if (grow_.hexRewardSecond < grow_.hexes.ThrowerExtraPages()) {  // 3.6.0 投掷狂人 Lv2：再多一次
                ++grow_.hexRewardSecond;
                grow_.DrawRewards(run_.floor, run_.floors, run_.seed, run_.items);
                pbz::log::Info("影塔投掷狂人：普通奖励第 {} 次已领取，显示第 {} 组", grow_.hexRewardSecond, grow_.hexRewardSecond + 1);
                SaveRun(); Broadcast("info.picked", std::format("已领取：{}；请选择第 {} 次奖励", tower::rewards::Label(got), grow_.hexRewardSecond + 1)); return;
            }
            grow_.hexRewardSecond = 0;
            run_.FinishReward();
            pbz::log::Info("影塔领奖：{}", tower::rewards::Label(got));
            SettleBill("奖励页选完");
            HiddenUnlockCheck();
            if (TowerCleared()) return;
            SaveRun();
            Broadcast("info.picked", "已领取：" + tower::rewards::Label(got));
        } else if (cmd == "forge_pick" && grow_.on) {  // 3.6.1 批量锻造确认 {"picks":"0,2,1,…"}（每组选第几个；-1 = 没选，按第一个）
            std::vector<int> picks;
            { const auto s = pbz::ipc::Field(line, "picks").value_or(""); size_t at = 0;
              while (at <= s.size()) { size_t e = s.find(',', at); if (e == std::string::npos) e = s.size(); if (e > at) { try { picks.push_back(std::stoi(s.substr(at, e - at))); } catch (...) { picks.push_back(-1); } } at = e + 1; } }
            ForgePick(picks);
        } else if (cmd == "shop" && grow_.on) {  // {"value":"buy|refresh|leave","slot":…,"ver":…}
            Shop(pbz::ipc::Field(line, "value").value_or(""), static_cast<int>(num("slot", -1)), static_cast<uint32_t>(num("ver", -1)));
        } else if (cmd == "trade" && grow_.on) {  // 3.1.0 驿站交易 {"kind":"recycle|exchange|melt","a":兵器,"b":兵器,"target":货位,"ver":货架版本}
            std::vector<int> list;  // 3.1.0 N8：回收 / 熔铸多选 {"list":"3,7,12"}
            { const auto s = pbz::ipc::Field(line, "list").value_or(""); size_t at = 0;
              while (at < s.size() && list.size() < 64) { const size_t e = s.find(',', at); try { list.push_back(std::stoi(s.substr(at, e - at))); } catch (...) {} if (e == std::string::npos) break; at = e + 1; } }
            // 3.1.0 实机（用户：回收 / 熔铸一直"未知交易"）：管道外层已占用 "op":"tower"，交易种类改用 "kind"
            Trade(pbz::ipc::Field(line, "kind").value_or(""), static_cast<int>(num("a", -1)), static_cast<int>(num("b", -1)), static_cast<int>(num("target", -1)), static_cast<uint32_t>(num("ver", -1)), list);
        } else if (cmd == "prep" && grow_.on) {  // {"value":"arm|disarm","item":"sunder"}
            Prep(pbz::ipc::Field(line, "value").value_or(""), pbz::ipc::Field(line, "item").value_or(""));
        } else if (cmd == "pick") {
            const int slot = static_cast<int>(num("slot", -1));
            const auto chosen = slot >= 0 && slot < 3 ? run_.offer[slot] : tower::Offer{};
            if (!run_.Pick(slot)) { Broadcast("err.cannot_pick", "现在不能选"); return; }
            pbz::log::Info("影塔三选一：{}", OfferText(chosen));
            SaveRun();
            Broadcast("info.picked", "已选择：" + OfferText(chosen), OfferArgs(chosen));
        } else if (cmd == "skipfloor") {  // 1.3.0：战斗中玩家手动跳过本层（任何没被自动识别的问题都能走到下一层）
            if (run_.phase == tower::Phase::Ready && FailedStageAvailable()) { SkipFailedStage(); return; }
            if (run_.phase != tower::Phase::Fighting) { Broadcast("err.cannot_skip", "只能在战斗中跳过本层"); return; }
            forceSkip_ = true;
            CancelEntry("玩家选择跳过本层");
            forceSkip_ = false;
        } else if (cmd == "badboss") {  // 战斗中玩家报告"Boss 有问题"（无血条 / 不能锁定）：本机禁用、本层作废重抽
            BadBoss(pbz::ipc::Field(line, "reason").value_or("玩家报告：无血条或不能锁定"));
        } else if (cmd == "recover") {  // 上次塔档会话没走完：把进塔基准放回内存，玩家回标题继续
            if (trip_.busy() && trip_.failed()) {  // 自动切档停在失败：保护一直开着，重新计时再试
                if (!trip_.Retry(Io())) { Broadcast("err.recover", "重试失败：" + trip_.error()); return; }
                lastStep_ = tower::trip::Step::None; lastTripError_.clear();
                Broadcast("info.trip_retry", std::format("重新尝试{}（事务 {}）", trip_.phase() == tower::trip::Phase::Exiting ? "恢复原进度" : "进入塔档", trip_.txn()));
                return;
            }
            if (trip_.phase() != tower::trip::Phase::Stale) { Broadcast("err.recover", "没有需要恢复的会话"); return; }
            trip_.SetAuto(pbz::ipc::Field(line, "auto_trip").value_or("on") != "off");
            if (!trip_.Exit(Io())) { Broadcast("err.recover", "恢复失败：" + trip_.error()); return; }
            SaveTrip();
            Broadcast(trip_.pending() ? "info.exit_pending" : "info.exit_reload", ExitText());
        } else if (cmd == "forget") {  // 玩家确认现在已是原进度（例如游戏已重启）
            trip_.Forget(); SaveTrip(); grow_.Release();
            towergate::ConfirmOriginal();
            Broadcast("info.exit_done", "已清除上次会话标记");
        } else if (cmd == "grow") {  // 原生成长：{"action":"upgrade|downgrade|node|reset_nodes|snap|med|acc|reconcile|state|character","index":…,"to":…}
            Grow(pbz::ipc::Field(line, "action").value_or("state"), static_cast<int>(num("index", -1)), static_cast<int>(num("to", 1)));
        } else if (cmd == "use") {
            UseItem(pbz::ipc::Field(line, "item").value_or(""));
        } else if (cmd == "arena") {  // 擂台：list / mark / unmark / go（"id" 擂台 id，go 可用 "home"）
            const auto v = pbz::ipc::Field(line, "value").value_or("list");
            pbz::ipc::Send(std::format(R"({{"op":"tower","event":"arena","value":"{}",{}}})", pbz::ipc::Esc(v), towertravel::Command(v, pbz::ipc::Field(line, "id").value_or(""))));
        } else if (cmd == "reroll" && grow_.on) {  // v14：刷新令是奖励界面第四位，选了立即重抽，不占本层领奖机会
            if (run_.phase != tower::Phase::Reward || !grow_.UseRefresh(run_.floor, run_.floors, run_.seed, run_.items)) { Broadcast("err.cannot_reroll", "现在没有刷新令"); return; }
            pbz::log::Info("影塔奖励：第 {} 层使用刷新令，重抽一组（第 {} 次抽取）{}", run_.floor, grow_.draws, grow_.refresh ? "，又出现刷新令" : "");
            SaveRun();
            Broadcast("info.rerolled", "已刷新");
        } else if (cmd == "reroll") {
            if (!run_.Reroll()) { Broadcast("err.cannot_reroll", "没有刷新令，或现在不能刷新"); return; }
            SaveRun();
            Broadcast("info.rerolled", "已刷新");
        } else if (cmd == "st") {
            UseShentong(static_cast<int>(num("slot", -1)));
        } else if (cmd == "call") {  // 实验：按名字调游戏函数（tower_probe.cpp）
            pbz::ipc::Send(towerprobe::Call(line));
        } else if (cmd == "obs") {
            pbz::ipc::Send(towerprobe::Obs(line));
        } else if (cmd == "ach") {
            pbz::ipc::Send(towerprobe::Ach(pbz::ipc::Field(line, "value").value_or("state")));
        } else if (cmd == "sandbox") {  // 实验：存档沙盒 {"value":"on|off|clear|state"}
            const auto v = pbz::ipc::Field(line, "value").value_or("state");
            std::string why;
            const bool force = pbz::ipc::Field(line, "force").value_or("") == "true";
            // 开：文件沙盒 → 原生存档锁 → 记下进塔基准（三层，任何一层失败都不算开好）
            if (v == "on") {
                if (!towersb::Set(true, &why, false) || !towergate::Veto(true, &why)) { Broadcast("err.sandbox", "存档沙盒失败：" + why); return; }
                // 标题处还没载入存档，取不到基准：文件沙盒和存档锁先生效，进档后再发一次 on 记基准
                if (!towergate::Snapshot(&why)) Broadcast("info.sandbox_no_baseline", "文件沙盒与存档锁已开；" + why);
            }
            // 还原：把进塔基准放回游戏内存（同进程出塔；之后回标题 / 继续游戏读到的是真进度）
            if (v == "restore" && !towergate::Restore(&why)) { Broadcast("err.sandbox", "还原失败：" + why); return; }
            // 关：基准没放回时拒绝（除非 force）；关了才放开原生存档锁
            if (v == "off") {
                if (!towersb::Set(false, &why, force)) { Broadcast("err.sandbox", why); return; }
                towergate::Veto(false, &why);
            }
            if (v == "clear") towersb::Clear();
            if (v == "reload") {  // 出塔还原实验：读回磁盘上的真存档（沙盒必须先关，否则会读到沙盒里的塔内存档）
                if (towersb::Active()) { Broadcast("err.sandbox", "先关沙盒再读档"); return; }
                UObject* gi = nullptr;
                if (auto* cls = towerperf::ClassNamed("PBZGameMode_Build"))  // ContinueGame 在游戏模式上（第一次实验找成了 GameInstance）
                    ue::ForEach([&](int, UObject* o) { if (ue::IsA(o, cls) && !ue::IsDefault(o)) { gi = o; return false; } return true; });
                auto* fn = gi ? pe::FindFunction(ue::ClassOf(gi), "ContinueGame") : nullptr;
                if (!fn) { Broadcast("err.sandbox", "找不到 PBZGameMode_Build.ContinueGame"); return; }
                pbz::log::Info("影塔存档沙盒：调用 {}.ContinueGame 读回真存档", ue::ObjName(gi));
                uint8_t none[16] = {};
                pe::Call(gi, fn, none);
            }
            pbz::ipc::Send("{\"op\":\"tower\",\"event\":\"sandbox\"," + towersb::State() + "," + towergate::State() + "}");
        } else if (cmd == "equip") {
            Wear(pbz::ipc::Field(line, "weapon").value_or(""));
        } else if (cmd == "unequip") {
            if (!pushed_) { Broadcast("err.not_wearing", "现在用的就是自己的兵器"); return; }
            Unwear("unequip");
            Broadcast("info.weapon_off", "已换回自己的兵器");
        } else if (cmd == "hidden" && false) {  // 第 51 层入口已撤（无尽模式取代）
            resumeAuto_ = pbz::ipc::Field(line, "auto_trip").value_or("on") != "off";
            resumePool_.clear();
            EnterHidden();
        } else if (cmd == "endless_resume") {  // 3.5.0：开局菜单「无尽模式」→ 继续无尽存档
            if (run_.phase != tower::Phase::Idle || trip_.phase() != tower::trip::Phase::Outside) { Broadcast("err.exit_pending", TripBusyText()); return; }
            s_hidden = false;
            if (!SwapIn(true)) { Broadcast("err.no_endless", "暂未开启无尽模式"); return; }
            resumeAuto_ = true; resumePool_.clear();
            Resume();
        } else if (cmd == "endless_import") {  // 3.5.0：用安装器导入的无尽档开局（覆盖现有无尽存档）
            if (run_.phase != tower::Phase::Idle || trip_.phase() != tower::trip::Phase::Outside) { Broadcast("err.exit_pending", TripBusyText()); return; }
            s_hidden = false;
            std::string why;
            if (!StartFromEndlessImport(&why)) { Broadcast("err.endless_import", "无尽档无法使用：" + why); return; }
            resumeAuto_ = true; resumePool_.clear();
            Resume();
        } else if (cmd == "endless_commit") {  // 3.5.0：第 99 层打完点下一层 → 确认这局成为唯一的无尽存档，再开第 100 层
            if (!grow_.on || run_.phase != tower::Phase::Ready || !run_.endless || run_.endlessSave || s_hidden) { Next(); return; }
            for (const wchar_t* n : {L"endless-run.txt", L"endless-growth.txt", L"endless-import.txt"}) DeleteFileW(SlotPath(n).c_str());
            run_.endlessSave = true;
            SaveRun();
            pbz::log::Info("影塔无尽模式：本局 {} 成为无尽存档（覆盖旧的无尽存档 / 导入档）", run_.id);
            Next();
        } else if (cmd == "resume") {  // 继续上一局 = 100 层爬塔；当前位置是无尽存档就先挪开，再把停放的爬塔局换回来
            if (run_.phase == tower::Phase::Idle && trip_.phase() == tower::trip::Phase::Outside) SwapIn(false);
            if (run_.phase == tower::Phase::Idle) s_hidden = false;
            resumeAuto_ = pbz::ipc::Field(line, "auto_trip").value_or("on") != "off";
            resumePool_ = pbz::ipc::Field(line, "pool").value_or("");  // 显式给了就按给的；没给就沿用局存档里的
            Resume();
        } else if (cmd == "suspend") {
            Suspend();
        } else if (cmd == "grant") {
            Grant(pbz::ipc::Field(line, "backup_ok").value_or("") == "true", pbz::ipc::Field(line, "session").value_or(""));
        } else if (cmd == "probe") {
            Probe();
        } else if (cmd == "restart") {  // 死亡结算"重新开始"：在塔档里直接从第 0 层重开（不回原档）；不在塔档里才走 出塔 → 新局
            if (trip_.phase() == tower::trip::Phase::Inside) {
                Dispatch("start", std::format(R"({{"op":"tower","cmd":"start","floors":50,"client":"ingame","difficulty":"{}"}})", s_lastDiff ? "easy" : "hard"), client);  // 重开沿用本局难度
                return;
            }
            restartAfterExit_ = true;
            Dispatch("stop", line, client);
            if (trip_.phase() == tower::trip::Phase::Outside) { restartAfterExit_ = false; PostRestart(); }
        } else if (cmd == "stop") {
            if (trip_.phase() == tower::trip::Phase::Exiting && !trip_.failed()) { Broadcast("info.exit_reload", ExitText()); return; }  // 双击退出：同一事务
            settlement_.clear();
            // 进塔（含继续上一局）还没完成就取消：只回原进度，不删局存档（2026-11-05：继续上一局核验失败后点"取消，回到原进度"把第 6 层的局删了）
            const bool keepRun = trip_.phase() == tower::trip::Phase::Entering;
            Clear("stop");
            ReleaseShentong();
            towermusic::Stop("退出影塔");
            run_ = {}; actors_.clear(); controller_.clear();
            const int left = End();
            if (keepRun) pbz::log::Info("影塔：进塔未完成就取消，局存档保留（可继续上一局）");
            else DeleteRun();
            s_hidden = false;
            if (left == 0) Broadcast("info.stopped", "已停止");  // 塔档会话：End 已广播出塔受理 / 失败，不覆盖（313987b 反例：ack 掩盖 err.exit_failed）
        } else {
            Broadcast("info.state", "");
        }
    }

    // 异常现场的影塔段（每秒一次）：局 / 层 / 阶段 / 本层 Boss / 旅行 / 沙盒 / 隔离 / 诊断模式
    void PublishDiag() {
        const auto t = GetTickCount64();
        if (t - diagAt_ < 1000) return;
        diagAt_ = t;
        if (towerdev::kVerbose && run_.phase == tower::Phase::Fighting)
            pbz::log::Info("影塔战斗状态：floor={} actors={} owned={} pending={} queued={} clear_at={:.3f} entry={} native_panel={} wall_ms={}", run_.floor, actors_.size(), pbz::enemies::OwnedActors().size(), pending_, spawnQueue_.size(), clearAt_, towerentry::State(), towerwidgets::PanelActive(), t);
        std::string bosses;
        for (int b : run_.bosses) bosses += std::string(bosses.empty() ? "" : "+") + std::string(tower::kBossPool[b].identity);
        pbz::diag::Section("tower", std::format("run={} floor={}/{} phase={} growth={} bosses={} owned={} travel={} spawnAt={:.1f} sweptTotal={} sandbox={} sandboxOnly={} {}",
            run_.id.empty() ? "-" : run_.id, run_.floor, run_.floors, tower::PhaseName(run_.phase),
            std::string(tower::trip::PhaseName(trip_.phase())) + (grow_.on ? "+growth" : ""), bosses.empty() ? "-" : bosses, actors_.size(),
            towertravel::Busy() ? "busy" : towertravel::Failed() ? "failed" : "idle", spawnAt_, swept_, towersb::Active() ? "on" : "off",
            towerdiag::SandboxOnly() ? "on" : "off", std::string("entry=") + towerentry::State() + " " + towerisolate::Status()));
    }

    // 用户 2026-11-12：买到 / 领到兵器、饰品、药时影塔菜单还开着，原生"获得"界面和菜单冲突。菜单（影塔对话框 + 原生面板 / 模态）
    // 开着时只记账，全部关掉并稳定 1.2 秒后一次同步到原生，获得界面一起弹
    unsigned long long menuIdleSince_ = 0;
    void DeferredGrantTick() {
        // 3.1.0（用户 2026-11-13）：回到原来的做法——获得装备当场同步、当场弹原生获得界面（即使和菜单叠在一起）。
        // 延后弹出时很多玩家直接点下一层，获得界面到下一层才出来。这里只把旧会话欠下的补发一次
        grow_.holdSync = false;
        if (grow_.on && grow_.syncOwed) { grow_.syncOwed = false; grow_.session.Sync(Game(), grow_.profile); }
        return;
        const bool busy = towerwidgets::MenuBusy();
        grow_.holdSync = grow_.on && (busy || (menuIdleSince_ && GetTickCount64() - menuIdleSince_ < 1200) || !menuIdleSince_);
        if (busy) { menuIdleSince_ = 0; return; }
        if (!menuIdleSince_) { menuIdleSince_ = GetTickCount64(); return; }
        if (!grow_.syncOwed || GetTickCount64() - menuIdleSince_ < 1200) return;
        grow_.syncOwed = false; grow_.holdSync = false;
        pbz::log::Info("影塔原生成长：菜单已关，补同步本批获得的装备 / 药（原生获得界面一起弹）");
        grow_.session.Sync(Game(), grow_.profile);
    }
    void Tick(float dt) override {
        // 卡顿记录（用户 2026-11-05：几乎固定几秒一顿）：两次 Tick 之间超过 80 ms 就记一行，附影塔自己上一帧用了多久。
        // 影塔这一帧用时很小、间隔却很大 → 停顿在游戏 / 钩子 / 加载，不在影塔每帧逻辑里
        const uint64_t tickStart = towerperf::Qpc();
        if (lastTickQpc_ && run_.phase != tower::Phase::Idle) {
            const double gap = towerperf::Ms(tickStart - lastTickQpc_);
            const uint64_t wall = GetTickCount64();
            if (wall - hitchWindow_ > 60000) { hitchWindow_ = wall; hitchLogs_ = 0; }
            if (towerdev::kPerfLog && gap > 80 && hitchLogs_ < 40) {
                ++hitchLogs_;
                pbz::log::Info("影塔卡顿帧：两帧间隔 {:.0f} ms，影塔上一帧自身 {:.1f} ms（{}；本分钟第 {} 条）", gap, lastTickMs_, tower::PhaseName(run_.phase), hitchLogs_);
            }
        }
        lastTickQpc_ = tickStart;
#ifdef PBZ_HEX_TEST
        AutoClimbTick(dt);
#endif
        struct TickClock { double& out; uint64_t start; ~TickClock() { out = towerperf::Ms(towerperf::Qpc() - start); } } clock{lastTickMs_, tickStart};
        towerperf::Section _("影塔引擎");
        towerperf::Flush(run_.phase != tower::Phase::Fighting ? tower::PhaseName(run_.phase) : towerentry::Holding() ? std::string_view(towerentry::State()) : "fighting");
        { towerperf::Section s("实例索引"); towerperf::IndexStep(); }  // 4.0.0：血条等按类查找改为分摊扫描
        PublishDiag();
        QaAssistTick();
        { towerperf::Section s("武库扩建"); ArsenalTick(); ArsenalPickWatch(); }
        { towerperf::Section s("切档推进"); TripTick(); RebirthReloadTick(); }
        // 会话隔离跟塔档会话走（进塔 Inside → 出塔事务开始），不跟单层：取消 / 失败 / 奖励 / 商店期间原生副作用照样收住
        towerisolate::Session(trip_.phase() == tower::trip::Phase::Inside, run_.floor, run_.id);
        summonPermit_.Tick(enabled && trip_.phase() == tower::trip::Phase::Inside && run_.phase == tower::Phase::Fighting && towersb::Active() && !towerdiag::SandboxOnly());
        if (!enabled || run_.phase == tower::Phase::Idle || towerdiag::SandboxOnly()) return;
        { towerperf::Section s("发放与文件"); DeferredGrantTick(); ForesightFile(); MeltTick(); }
        if(towervm::Faulted() && run_.phase==tower::Phase::Fighting){CancelEntry("剧情执行门异常，已停止本层并保留保护；请保留现场，不再开层", false);return;}
        // 用户 2026-11-07：战斗中游戏暂停、ESC / 原生菜单或影塔菜单开着时不算在战斗——本层计时、限时挑战、各种兜底
        // （刷不出 Boss、下一阶段没出现、无交互等）全部停走，关掉后接着算。塔的时钟 now_ 与本帧 dt 一起冻住
        const bool fightPause = run_.phase == tower::Phase::Fighting && fightSince_ > 0 && !towerentry::Holding() && deathAt_ < 0 &&
                                (towerwidgets::MenuBusy() || towerentry::GamePaused());
        if (fightPause != fightPaused_) {
            fightPaused_ = fightPause;
            pbz::log::Info("影塔第 {} 层：{}（本层用时 {:.1f} 秒{}）", run_.floor, fightPause ? "暂停 / 菜单打开，计时与兜底停走" : "继续，计时与兜底接着算",
                           FightSeconds(), fightPause ? "，原因 " + (towerentry::GamePaused() ? std::string("游戏暂停") : towerwidgets::NativeModalName()) : std::string());
        }
        if (fightPause) dt = 0;
        now_ += dt;
        if (mob_.on && run_.phase == tower::Phase::Ready) mob_ = {};
        if (autoNextAt_ > 0 && now_ >= autoNextAt_) {
            autoNextAt_ = 0;
            if (run_.phase == tower::Phase::Ready && trip_.phase() == tower::trip::Phase::Inside) { pbz::log::Info("影塔：自动重开第 {} 层（直接开打）", run_.floor + 1); Next(); }
        }
        if (run_.phase == tower::Phase::Fighting && fightSince_ > 0) {
            const bool movie = towerstorygate::Cinematic();
            if (movie) {  // 过场里定身 / 缓时不计时（入场 / 转阶段过场播完再接着算）
                if (freezeUntil_ > now_) freezeUntil_ += dt;
                if (slowUntil_ > now_) slowUntil_ += dt;
            } else if (prepPending_) FirePrep();
        }
        if(replay_ && fightSince_>0 && towerstorygate::Cinematic()){
            cinematicSeconds_+=dt;
            if(rp_.Between())rp_.betweenSince+=dt; // a real transition movie is not a missing-next-phase timeout
        }
        if(replay_ && replayEntryStarted_ && !rp_.Started() && replay_->entryAsset!=std::array<uint32_t,4>{} && !towerstorygate::Cinematic())replayEntryWait_+=dt;
        { towerperf::Section s("剧情隔离逐帧"); towerisolate::Tick(dt); }
        const Target* player = nullptr;
        auto& byObj = byObj_;  // 4.0.0：每帧复用（clear 保留桶，不再每帧重新分配）
        byObj.clear();
        {
            towerperf::Section s("目标表");
            for (auto& t : attr::Targets()) {
                if (t.who == attr::Who::World || !attr::Alive(t)) continue;  // 4.0.0：世界设置对象用不到，不进表
                if (t.who == attr::Who::Player) player = &t;
                else byObj[t.obj] = &t;
            }
        }
        { towerperf::Section s("玩家属性"); if (player) ApplyPlayer(*player); }
#ifdef PBZ_HEX_TEST
        if (arcaneTest_) {
            if (!player || run_.phase != tower::Phase::Fighting || !towerarcane::Tick(player->obj)) { arcaneTest_ = false; towerarcane::Stop(); }
            else {
                if (const int charge=attr::AttrOffset("UltimateChargeAdditiveRate"); charge>=0)
                    attr::Want(*player, attr::AttrCurrent(*player,charge), Kind::Additive, arcaneChargeMul_);
                const int curOff=attr::AttrOffset("UltimateCharge"), maxOff=attr::AttrOffset("UltimateCharge_Max");
                const int countOff=attr::AttrOffset("UltimateChargeCount"), countMaxOff=attr::AttrOffset("UltimateChargeCount_Max");
                if (curOff>=0 && maxOff>=0 && countOff>=0 && countMaxOff>=0) {
                    const float cur=ue::Get<float>(attr::AttrCurrent(*player,curOff)), mx=ue::Get<float>(attr::AttrCurrent(*player,maxOff));
                    const float count=ue::Get<float>(attr::AttrCurrent(*player,countOff)), cm=ue::Get<float>(attr::AttrCurrent(*player,countMaxOff));
                    if (std::isfinite(cur) && std::isfinite(mx) && std::isfinite(count) && std::isfinite(cm) && arcaneSamples_<50 &&
                        (arcaneLastCharge_<0 || std::abs(cur-arcaneLastCharge_)>0.1f || std::abs(count-arcaneLastCount_)>0.1f)) {
                        ++arcaneSamples_; arcaneLastCharge_=cur; arcaneLastCount_=count;
                        pbz::log::Info("秘术冲拳测试：绝技能量 {:.1f}/{:.1f}，层数 {:.1f}/{:.1f}，候选充能倍率 {:.2f}",cur,mx,count,cm,arcaneChargeMul_);
                    }
                }
            }
        }
#endif
        const bool speedFight = player && grow_.on && run_.phase == tower::Phase::Fighting && fightSince_ > 0 && !towerentry::Holding();
        double speedRate = 1.0;
        // v3：攻速合计超过 +100% 的部分只算一半，绝对上限 ×2.5（QA 测试倍率另加，不受限）
        if (speedFight) speedRate = tower::balance::SoftSpeed(grow_.hexes.ScientistBoost(ShownNow(), run_.seed).attackSpeed + grow_.hexes.AttackSpeed(), tower::balance::kAttackSpeedKnee);  // 3.4.5 缩小引擎攻速
        towerspeed::Tick(player ? player->obj : nullptr, speedFight && speedRate > 1.0, static_cast<float>(speedRate));
        if (run_.phase == tower::Phase::Fighting && (travelThenSpawn_ || spawnAt_>0)) {
            auto* modal=towerwidgets::NativeModal();
            if (modal && ue::ClassName(modal)=="W_PlayerDead_C") {
                RecordFailure("entry_death",venueBoss_,"入场期间出现原生死亡界面");
                CancelEntry("入场期间死亡，已取消本层；不会在复活地点生成 Boss"); return;
            }
        }
        if (travelThenSpawn_ && run_.phase == tower::Phase::Fighting && !towertravel::Busy()) {
            travelThenSpawn_ = false;
            if (towertravel::Stopped()) {
                // 原生异常 / 加载不结束（7c2668e 第三层黑屏）：不重抽、不再跨图；取消本层入场，保留塔档与存档保护，等玩家暂离或出塔
                const auto why = towertravel::Why();
                if (run_.inMobWave) { RecordFailure("wave_travel_stop", venueBoss_, why); CancelWave("小怪场地入场未就绪：" + why, tower::wave::Failure::Outstanding); return; }
                venueBad_.insert(venueBoss_);
                RecordFailure("travel_stop", venueBoss_, why);
                CancelEntry("场地入场检查未通过，已停下：" + why);
            } else if (towertravel::Failed()) {
                // 去不了：不在错的地方开打（用户 / Codex 2026-10-31）。这只 Boss 的场地本局不再用，重抽
                const auto why = towertravel::Why();
                lastTravelWhy_ = why;
                pbz::log::Info("影塔：去 {} 的场地失败（{}），本层作废、换 Boss", tower::kBossPool[venueBoss_].name, why);
                if (run_.inMobWave) { RecordFailure("wave_travel", venueBoss_, why); CancelWave("小怪场地不可用：" + why, tower::wave::Failure::Venue); return; }
                venueBad_.insert(venueBoss_);
                RecordFailure("travel", venueBoss_, why);
                run_.UndoFloor();
                Repick("去不了场地：" + why);
            } else {
                spawnAt_ = now_ + 0.01; // LoadingClear gates the real destination cover; landing probes still validate the ground.
                spawnTries_ = 0; transientTries_ = 0; audited_ = false; floorAudit_ = {}; gateSince_ = -1;  // 到场：重新跑生成前就绪与地面审计
            }
        }
        if (run_.inMobWave && !mobQueue_.empty() && now_ >= mobNextAt_) PumpMobs();
        if (EndlessOn() && run_.phase == tower::Phase::Fighting && !endlessMobQ_.empty() && endlessMobFlight_ == 0 && now_ >= endlessMobAt_) SubmitEndlessMob();
        if (run_.inMobWave && run_.phase == tower::Phase::Fighting &&
            mobRequestWatch_.Tick(dt, towerentry::GamePaused() || towerwidgets::MenuBusy())) {
            const auto why = std::format("第 {} 层第 {}/{} 波生成等待超时，未结算；点下一层重试当前波",
                tower::floorplay::ShownMobs(mob_.floor, run_.floors), mob_.wave + 1, mob_.total);
            pbz::log::Info("影塔小怪：{}（待回执 {}，队列 {}，已登记 {}）", why, pending_, mobQueue_.size(), actors_.size());
            RecordFailure("wave_spawn_timeout", -1, why);
            CancelWave(why, tower::wave::Failure::Outstanding);
            return;
        }
        if (!run_.inMobWave && run_.phase == tower::Phase::Fighting &&
            relayRequestWatch_.Tick(dt, towerentry::GamePaused() || towerwidgets::MenuBusy())) {
            const auto why = std::format("第 {} 层车轮第 {}/{} 位生成回执超时，本层不结算；可重试本层",
                                         ShownBoss(run_.floor), relay_.index, relay_.total);
            RecordFailure("relay_spawn_timeout", lastSubmitBoss_, why);
            CancelEntry(why);
            return;
        }
        if (spawnAt_ > 0 && now_ >= spawnAt_ && run_.phase == tower::Phase::Fighting && towerentry::LoadingClear()) {
            if (!venueArrived_ && travelled_) { tvenue::Arrive(venuePath_.empty() ? tower::kBossPool[venueBoss_].path : std::string_view(venuePath_)); venueArrived_ = true; }  // 场地：找出生点 / 雾门、静默原生
            tvenue::Tick();
            if (tvenue::MissingRequired()) {
                RecordFailure("venue_missing",venueBoss_,"高空场地原生出生点未找到");
                if (run_.inMobWave) { CancelWave("小怪场地出生点未找到", tower::wave::Failure::Venue); return; }
                CancelEntry("高空场地尚未就绪，已取消本层；不会改用复活地点开战"); return;
            }
            if (tvenue::Ready() && SpawnGate()) {
                if (replay_) { // 补满/读回沿用塔规则，但不冻结、不读秒；准备好才提示交互。
                    std::string storyWhy;
                    if (!towerisolate::ReplayEntryReady(&storyWhy)) {
                        // 28 层大师兄（4ee41ec）：到场 3 秒就因"原生阶段刷怪点尚无战斗组"作废——刚流送进来的单元里刷怪点还没挂上 MobGroup。
                        // 入场时的"还没就绪"先等最多 15 秒（每 0.5 秒复查），仍不行才作废
                        if (replayWaitSince_ < 0) { replayWaitSince_ = now_; pbz::log::Info("影塔重放：入场条件未就绪（{}），最多等 15 秒", storyWhy); }
                        if (now_ - replayWaitSince_ < 15) { spawnAt_ = now_ + 0.5; return; }
                        CancelEntry(storyWhy + "（等了 15 秒）"); return;
                    }
                    if (replayWaitSince_ >= 0) { pbz::log::Info("影塔重放：入场条件已就绪（等了 {:.1f} 秒）", now_ - replayWaitSince_); replayWaitSince_ = -1; }
                    if (tower::replay::UsesCountdown(*replay_)) {
                        if (!towerentry::Hold(towerwidgets::Pawn())) { CancelEntry(towerentry::Why()); return; }
                        spawnAt_=0; ReplayArm();
                    } else {
                    const auto prepared = towerentry::PrepareReplay();
                    if (prepared == tower::entry::State::Failed) { CancelEntry(towerentry::Why()); return; }
                    if (prepared == tower::entry::State::Running) { spawnAt_ = 0; ReplayArm(); }
                    }
                }
                else {
                    if (!towerentry::Hold(towerwidgets::Pawn())) { CancelEntry(towerentry::Why()); return; }
                    spawnAt_ = 0; Spawn();
                }
            }
        }
        if (run_.phase == tower::Phase::Fighting) { towerperf::Section s("战斗日志"); towercombat::Tick(FightSeconds()); }
        if (run_.phase == tower::Phase::Fighting && replay_ && replayArmedAt_ >= 0) ReplayTick();  // 先接管原生阶段角色，再清场（清场不会把它们当原生替身清掉）
        if (run_.phase == tower::Phase::Fighting && nativeAdoptUntil_ > 0) NativeAdoptTick();  // 原生刷怪点兜底刷出的 Boss：同样先接管再清场
        // 原生刷怪点兜底会连带刷出同组原生同伙（30 层玉面郎君 ui7：金面具组带出 2 只，读秒时还能打）：入场冻结期间每帧清场，不等 0.5 秒巡检
        if (run_.phase == tower::Phase::Fighting && nativeTried_ && towerentry::Holding() && !actors_.empty()) { towerperf::Section s("清场巡检"); Sweep(); }
        if (run_.phase == tower::Phase::Fighting && !travelThenSpawn_ && now_ >= nextSweep_) {
            nextSweep_ = now_ + 0.5;
            towerperf::Section s("清场巡检");
            Sweep();
            if (!towerfixed::Active()) towervenue::KeepDoorsClosed();
            if (fightSince_ > 0 && !towerentry::Holding() && Leash()) return;
        }
        if (run_.phase == tower::Phase::Fighting) LionMinionTick();  // 3.4.1 舞狮二阶段小怪打不死
        if (run_.phase != tower::Phase::Fighting) return;
        // Only the exact clone class with player ownership evidence is affected.
        // The ownership list is refreshed by Sweep every half second.
        for (const auto& [obj, index] : playerSummons_) {
            const auto it = byObj.find(obj);
            if (it == byObj.end() || !ue::Live(obj) || ue::Get<int>(obj + ue::Layout::ObjIndex, -1) != index) continue;
            const auto mul = tower::floorplay::PlayerCloneDmg(ue::ClassName(obj), true);
            if (mul == 1.0) continue;
            for (int o : dealt_) attr::Want(*it->second, attr::AttrCurrent(*it->second, o), Kind::Scale, static_cast<float>(mul));
        }

        // 本层 Boss：战力归一 + 玩家加成
        const auto owned = pbz::enemies::OwnedActors();
        const std::unordered_set<UObject*> alive(owned.begin(), owned.end());
        { towerperf::Section s("仆从"); TickServants(player, alive, fightPause); }
        { towerperf::Section s("创意层"); SpecialTick(fightPause); }
        WatchNativeDeaths();
        towerperf::Section enemyLoop("敌方逐个与收尾");  // 到函数结束
        bool anyAlive = false, vanished = false;
        // 51 层 1v3 错峰（ui12 实机卡死）：入场门要求错峰队列为空，而队列只在开战后才处理 → 永远不读秒。
        // 错峰的第二 / 三只本来就是开战后进场：入场只看已生成的那只，开战时把队列时间按开战时刻重排
        bool entryReady = !actors_.empty() && pending_ <= 0 && endlessMobPending_ <= 0 && !travelThenSpawn_ && spawnAt_ <= 0 && replaySupportsReady_;
        // 用户 2026-11-06：51 层魔渊开打后有原生开场台词，期间玩家被剧情镜头锁成只能慢走，另外两只却已经能打人。
        // 多 Boss 层开战后玩家处于剧情镜头锁时：除魔渊（台词本身由它的能力驱动，不能停）外的 Boss 定身且不受伤，锁解除即放开；
        // 最多 30 秒，超时放开并记日志，不会永远定住
        bool storyHold = false;
        if (stateLogUntil_ > 0 && now_ >= stateLogNext_ && fightSince_ > 0) {  // 诊断：多 Boss 层开战后 20 秒内每秒记一次玩家状态
            stateLogNext_ = now_ + 1.0;
            pbz::log::Info("影塔第 {} 层：开战 {:.0f} 秒 玩家状态 {}", run_.floor, now_ - fightSince_, towernative::PlayerStates());
            if (now_ >= stateLogUntil_) stateLogUntil_ = 0;
        }
        if ((count_ > 1 || EndlessOn()) && fightSince_ > 0 && !towerentry::Holding() && now_ >= storyNextAsk_) {
            storyNextAsk_ = now_ + 0.1;
            storyLocked_ = towernative::StoryLocked() == 1;
        }
        if (storyLocked_ && (count_ > 1 || EndlessOn()) && fightSince_ > 0) {
            if (storySince_ <= 0) { storySince_ = now_; pbz::log::Info("影塔第 {} 层：玩家进入原生剧情镜头锁（开场台词），其余 Boss 定身", run_.floor); }
            storyHold = now_ - storySince_ < 30.0;
            if (!storyHold && !storyTimedOut_) { storyTimedOut_ = true; pbz::log::Info("影塔第 {} 层：剧情镜头锁超过 30 秒，放开定身", run_.floor); }
        } else if (storySince_ > 0) {
            pbz::log::Info("影塔第 {} 层：剧情镜头锁解除（{:.1f} 秒），Boss 放开", run_.floor, now_ - storySince_);
            storySince_ = 0; storyTimedOut_ = false;
        }
        for (auto& a : actors_) {
            if (MobActor(a) && a.dead) continue;
            // 接上的原生换体不在塔的生成登记里：按属性目标是否还在判断
            const bool present = a.adopted ? (byObj.count(a.obj) > 0 && ue::Live(a.obj)) : alive.count(a.obj) > 0;
            if (!present) {
                if (!a.gone) { a.gone = true; a.goneAt = now_; pbz::log::Info("影塔战斗观测：floor={} actor={} owned=0 dead_seen={} wall_ms={}", run_.floor, a.name, a.dead, GetTickCount64()); }
                // 重放层：已归零后被原生转阶段清掉（Seq063 刚播放时清一阶段刷怪点）= 这一阶段死了
                if (replay_ && !a.dead && a.death.zeroAt >= 0) {
                    a.dead = true;
                    pbz::log::Info("影塔重放：第 {} 阶段 {} 归零后被原生流程清掉，按死亡算", a.phase + 1, a.name);
                    PhaseMemberDead(a.phase, a.obj);
                }
                // 32ffb7d 静态缺口：不在登记里就 continue，anyAlive=false → 当成打完发奖（烟千岁"异常消失却发奖"）。
                // 没见到死亡的消失不是击杀：先等 5 秒看有没有同身份的原生换体（多阶段合法换 Actor），没有就作废本层
                if (tower::wave::MissingFinished(MobActor(a), a.dead, a.death.zeroAt >= 0, DiedNatively(a))) {
                    FinishMissingWaveMob(a); continue;
                }
                entryReady = false;
                if (!a.dead) { anyAlive = true; vanished = true; }
                continue;
            }
            if (MobActor(a) && a.gone && !a.dead) {
                pbz::log::Info("影塔战斗观测：floor={} actor={} 重新出现在登记表，清除消失计时", run_.floor, a.name);
                a.gone = false; a.goneAt = 0;
            }
            auto it = byObj.find(a.obj);
            // Refresh may drop a dying actor before ownership cleanup. Preserve the last
            // validated GAS target, rather than treating it as alive for minutes.
            if (it != byObj.end()) { a.target = *it->second; a.hasTarget = true; }
            // 用户 2026-11-06（51 层魔渊）：死亡已确认的 Boss，原生战斗目标随后读不到 / 不再"存活"，原先仍算活着，
            // 一直等到 Actor 被移除（约 40 秒）才弹结算。已确认死亡的不再算活着：Boss 全死就结算
            // 4.1.0（用户：无尽层秒杀 Boss 后经常几分钟才弹结算）：原生死亡流程可能在塔"归零 1 秒"确认前就让属性集读不到，
            // 而模型（尸体）还在 → Boss 一直算活着，等核心回收尸体（几十秒到几分钟）才结算。先从模型上重新取一次属性（同仆从取法）
            if (a.mobHp <= 0 && (!a.hasTarget || !attr::Alive(a.target)) && ue::Live(a.obj)) {
                const Target again = ServantTarget(a.obj);
                if (again.set && attr::Alive(again)) { a.target = again; a.hasTarget = true; }
            }
            if (!a.hasTarget || !attr::Alive(a.target)) {
                // 4.1.0：仍读不到——只认死亡证据，不按计时判死（兜底不白送击杀）：原生死亡组件已记录，或无尽 / 创意层（单阶段直刷）此前已见归零
                if (!a.dead && a.mobHp <= 0 && fightSince_ > 0 && (DiedNatively(a) || (EndlessOn() && !replay_ && a.zeroSeen))) {
                    a.dead = true; ++warKills_;
                    pbz::log::Info("影塔第 {} 层：{} 战斗属性已读不到，{}，按击杀算（不等尸体回收）", run_.floor, a.name, DiedNatively(a) ? "原生死亡已确认" : "此前已归零");
                    continue;
                }
                if (MobActor(a) && tower::wave::MissingFinished(true, a.dead, a.death.zeroAt >= 0,
                        DiedNatively(a) || (a.filled && !a.dead && a.targetWatch.CheckNativeDeath(now_) && NativeWaveDeath(a)))) {
                    FinishMissingWaveMob(a);
                    continue;
                }
                if (!a.dead) {
                    anyAlive = true; entryReady = false;
                    // Owned alone is not death evidence. If its combat target
                    // stops being readable, replace this slot without redoing prior kills.
                    if (MobActor(a) && fightSince_ > 0) {
                        const bool lastUnfinished = pending_ <= 0 && endlessMobPending_ <= 0 && spawnQueue_.empty() &&
                            std::none_of(actors_.begin(), actors_.end(), [&](const Actor& other) { return &other != &a && !other.dead; });
                        if (a.targetWatch.Missing(now_, lastUnfinished)) {
                            const auto why = std::format("{} 的战斗状态持续不可读取", a.name);
                            RecordFailure("wave_target_missing", -1, why);
                            if (!run_.inMobWave) { EndlessUnresolvedMob(a, why); continue; }
                            ReplaceUnresolvedWaveMob(a, why);
                            return;
                        }
                    }
                }
                else entryReady = false;
                continue;
            }
            a.targetWatch.Found();
            const Target& t = a.target;
            float* max = attr::AttrCurrent(t, hpMax_);
            const float initialMax=ue::Get<float>(max),initialHp=ue::Get<float>(attr::AttrCurrent(t,hp_));
            if(!std::isfinite(initialHp) || !std::isfinite(initialMax)) {
                // 4.1.0：无尽 / 创意层 Boss 已见归零后属性读成非法值 = 死亡后属性集回收中，按击杀算（不再无限等）
                if (!a.dead && EndlessOn() && !replay_ && a.mobHp <= 0 && a.zeroSeen && fightSince_ > 0) {
                    a.dead = true; ++warKills_;
                    pbz::log::Info("影塔第 {} 层：{} 已归零、属性读数失效，按击杀算", run_.floor, a.name);
                    continue;
                }
                a.death.Sample(now_,std::numeric_limits<float>::quiet_NaN());anyAlive=true;entryReady=false;continue;
            }
            if(a.scale<=0 && (initialMax<=0 || initialHp<=0.5f)) {
                anyAlive=true;entryReady=false;continue; // native phase may be registered before GAS initializes; HP0 here is not death
            }
            if (a.scale <= 0) {  // 第一次看到：按实机最大生命算缩放
                a.native = ue::Get<float>(max);
                // v14：逐层固定配置（floors.generated.h hpTarget × 个别 Boss 修正）；不跟玩家属性同步，读档 / 重试 / 换装备都不变
                if (a.mobHp > 0) a.scale = tower::HpScale(a.native, a.mobHp);
                else a.scale = tower::floorplay::BossHpScale(a.native, (sched_ ? tower::floorplay::FloorHp(plan_f_, a.boss >= 0 ? a.boss : (run_.bosses.empty() ? -1 : run_.bosses[0])) : EndlessFormal() ? EndlessHpTarget() : SpecialMainDef() ? SpecialMainHp(a.boss) * BossHpShare() : run_.BossHpTarget(1) * BossHpShare()) *
                                                       tower::floorplay::DiffHp(run_.difficulty, StatFloor(), run_.floors)  // 4.1.0 主线创意层按所在 Boss 层
                                                       * (SpecialOn() && special_.kind == tower::special::Kind::Survival ? tower::special::kSurvivalEnemyHp : 1.0));  // 3.7.0 生存战：近乎无限生命
                if (a.mobHp <= 0) pbz::log::Info("影塔第 {} 层：{} 原生血量 {:.0f} → 目标 {:.0f}（×{:.2f}）", run_.floor, a.name, a.native, a.native * a.scale, a.scale);
            }
            const float targetMax=static_cast<float>(a.native*a.scale);
            attr::Want(t, max, Kind::Set, targetMax);
            attr::Want(t, attr::AttrBase(t, hpMax_), Kind::Set, targetMax);
            // 原生"最低血量"（Health_Min，遭遇用它让 Boss 停在一定血量等处决 / 转阶段）按同一比例缩：
            // 2026-11-05 第 7 层荆野（原生刷怪点刷出）Health_Min 8816.6 = 原生 44083 的 20%，塔把上限压到 5075 后
            // 最低血量高过上限，血被钉住一点都打不掉。Scale 每帧按原生值相乘，原生改最低血量（转阶段）照样跟着走
            if (EndlessOn() && !replay_) {  // 3.2.1（用户：无尽层 Boss 残血不死卡关）：直刷单阶段，没有转阶段 / 处决流程接住最低血量
                attr::Want(t, attr::AttrCurrent(t, hpMin_), Kind::Set, 0.0f);
                attr::Want(t, attr::AttrBase(t, hpMin_), Kind::Set, 0.0f);
            } else {
                attr::Want(t, attr::AttrCurrent(t, hpMin_), Kind::Scale, static_cast<float>(a.scale));
                attr::Want(t, attr::AttrBase(t, hpMin_), Kind::Scale, static_cast<float>(a.scale));
            }
            const float cap=initialMax>0?std::min(targetMax,initialMax):targetMax;
            if(a.filled && initialHp>cap+0.5f && cap>0){
                ue::Write(attr::AttrBase(t,hp_),&cap,4);ue::Write(attr::AttrCurrent(t,hp_),&cap,4);
                towercombat::Adjusted(a.obj,cap,"原生阶段回血超过塔上限");
                a.leechHp = cap;  // 塔主动压回上限不是玩家造成的伤害
            }
            if (sunder_) for (int o : taken_) attr::Want(t, attr::AttrCurrent(t, o), Kind::Scale, 1.3f);  // 破甲符（淬锋改为抬攻击，见 ApplyPlayer）
            if (ItemOn(tower::Item::Calm) && shachi_ >= 0) attr::Want(t, attr::AttrCurrent(t, shachi_), Kind::Scale, 0.5f);  // 3.1.0 定魂香：敌人杀气恢复 -50%
            if (grow_.on && grow_.hexes.Has(tower::hex::Finisher) && hp_ >= 0 && hpMax_ >= 0) {  // 3.1.0 斩草除根：敌人生命低于 15% 受到伤害 ×2
                const float eh = ue::Get<float>(attr::AttrCurrent(t, hp_), -1.0f), em = ue::Get<float>(attr::AttrCurrent(t, hpMax_), -1.0f);
                if (em > 0 && eh > 0 && eh < em * static_cast<float>(grow_.hexes.FinisherLine()))
                    for (int o : taken_) attr::Want(t, attr::AttrCurrent(t, o), Kind::Scale, static_cast<float>(tower::hex::kFinisherTaken));
            }
            if (grow_.on && (grow_.hexes.Has(tower::hex::Decapitate) || grow_.hexes.Has(tower::hex::Sweep))) {
                // 斩首行动（Boss）/ 横扫千军（杂兵）。用户 2026-11-11：有屏幕 Boss 血条的是 Boss，血条在模型上的是杂兵；
                // 还没出现原生血条时按塔的分类（小怪段 = 杂兵）
                const int bar = towerwidgets::NativeBarKind(a.obj);
                const bool mob = bar == 2 ? true : bar == 1 ? false : a.mobHp > 0;
                const double m = grow_.hexes.EnemyTaken(mob);
                if (m != 1.0) for (int o : taken_) attr::Want(t, attr::AttrCurrent(t, o), Kind::Scale, static_cast<float>(m));
                const int kind = (mob ? 1 : 0) * 3 + bar;
                if (kind != a.hexTakenKind) {
                    a.hexTakenKind = kind;
                    pbz::log::Info("影塔海克斯：{} 判为{}（{}）受到伤害 ×{:g}", a.name, mob ? "杂兵" : "Boss", bar == 1 ? "屏幕 Boss 血条" : bar == 2 ? "头顶血条" : a.mobHp > 0 ? "小怪段，未见原生血条" : "Boss 层，未见原生血条", m);
                }
            }
            if (now_ < freezeUntil_ && dilation_ >= 0) attr::Want(t, reinterpret_cast<float*>(t.obj + dilation_), Kind::Scale, 0.01f);  // 定身符
            else if (now_ < slowUntil_ && dilation_ >= 0) attr::Want(t, reinterpret_cast<float*>(t.obj + dilation_), Kind::Scale, 0.6f);  // 缓时符：只放慢 Boss
            // 削杀气：旧模式的卡；成长模式（v14）的破防能力加在玩家 ShachiAttack 上（ApplyGrowth），这里只剩破势符
            const double posture = (grow_.on ? 1.0 : std::min(1.6, run_.mods.PostureMul())) * (ItemOn(tower::Item::Break) ? 1.3 : 1.0);
            for (int o : posture_) attr::Want(t, attr::AttrCurrent(t, o), Kind::Scale, static_cast<float>(posture));
            // 限时挑战超时：Boss 狂暴（困难 3:00 攻击 ×2、杀气上限再 ×1.5、回复 1/3；简单 5:00 攻击 ×1.5、杀气 ×1.5、回复 25%；只触发一次）
            namespace fp = tower::floorplay;
            const auto& bz = fp::Berserking(run_.difficulty);
            const bool berserk = sched_ && fp::Berserk(Chal(), FightSeconds(), run_.difficulty);
            if (berserk && !berserkFired_) {
                berserkFired_ = true;
                const float mx = ue::Get<float>(max), now = ue::Get<float>(attr::AttrCurrent(t, hp_));
                const float healed = std::min(mx, now + static_cast<float>(bz.heal) * mx);
                if (mx > 0 && healed > now) { ue::Write(attr::AttrBase(t, hp_), &healed, 4); ue::Write(attr::AttrCurrent(t, hp_), &healed, 4); towercombat::Adjusted(a.obj, healed, "限时挑战狂暴回复"); }
                pbz::log::Info("影塔第 {} 层：限时挑战超时（{}），{} 狂暴（攻击 ×{}、杀气上限 ×{}、回复 {:.0f}% 生命一次）", run_.floor, fp::DifficultyName(run_.difficulty), a.name, bz.attack, bz.shachi, bz.heal * 100);
                Broadcast("info.berserk", "Boss 狂暴");
                a.shachiFilled = false;  // 杀气上限变了：补满一次
            }
            // Boss 杀气上限（困难 1–15 层 ×1.5、16 层起 ×2；简单原生；护盾 Shield 不动）；狂暴再乘
            if (shachiMax_ >= 0) {
                // v3：小怪杀气上限也随 E(s)^0.7 上涨（原来不变）
                const double sm = a.mobHp > 0 ? tower::balance::MobShachiGrow(tower::balance::ShownOfWave(MobScaleFloor()))
                                              : fp::BossShachiMul(StatFloor(), run_.difficulty, run_.floors) * (berserk ? bz.shachi : 1.0);  // 4.1.0 主线创意层按所在 Boss 层
                if (sm != 1.0) {
                    attr::Want(t, attr::AttrCurrent(t, shachiMax_), Kind::Scale, static_cast<float>(sm));
                    attr::Want(t, attr::AttrBase(t, shachiMax_), Kind::Scale, static_cast<float>(sm));
                }
                // 上限生效后（下一帧）把当前杀气补到新上限一次，开场不是半条
                if (!a.shachiFilled && shachiNow_ >= 0 && a.frames >= 2) {
                    const float full = ue::Get<float>(attr::AttrCurrent(t, shachiMax_)), cur = ue::Get<float>(attr::AttrCurrent(t, shachiNow_));
                    if (sm != 1.0 && full > 0 && cur < full) { ue::Write(attr::AttrBase(t, shachiNow_), &full, 4); ue::Write(attr::AttrCurrent(t, shachiNow_), &full, 4); }
                    a.shachiFilled = true;
                }
            }
            const double bossDmg = a.mobHp > 0 ? tower::mobs::DmgMul(MobScaleFloor(), tower::floorplay::IsEasy(run_.difficulty)) : (sched_ ? fp::FloorDmg(plan_f_, a.boss >= 0 ? a.boss : (run_.bosses.empty() ? -1 : run_.bosses[0])) : EndlessFormal() ? EndlessDmgMul() : SpecialMainDef() ? SpecialMainDmg(a.boss) : run_.BossDmg(count_)) * fp::DiffDmg(run_.difficulty, StatFloor(), run_.floors) * fp::EncounterDmg(fp::ShownBoss(StatFloor(), run_.floors), replay_ && replay_->phaseN > 1, ue::ClassName(a.obj))
                                 * fp::LateBossDmg(ue::ClassName(a.obj))
                                 * (SpecialOn() && special_.kind == tower::special::Kind::Survival ? SurvivalEnemyAtk() : 1.0);  // 3.7.0 生存战：攻击 ×2（4.1.0 前 50 层 ×0.3）
            for (int o : dealt_) attr::Want(t, attr::AttrCurrent(t, o), Kind::Scale, static_cast<float>(bossDmg * (berserk ? bz.attack : 1.0)));
            if (!a.dmgLogged && a.mobHp <= 0) {
                a.dmgLogged = true;
                pbz::log::Info("影塔第 {} 层：{} 攻击 ×{:.2f}（{}，按第 {} 层 Boss 曲线）", run_.floor, a.name, bossDmg,
                               SpecialOn() ? tower::special::Name(special_.kind) : std::string_view("Boss 层"), ShownBoss(StatFloor()));
            }
            // v3（用户 2026-11-12）：敌方破防能力（ShachiAttack：削你架势 / 杀气的能力）以前不随层放大，现在随 E(s) 上涨
            if (shachiAttack_ >= 0 && run_.floors >= 50) {
                const double brk = a.mobHp > 0 ? tower::balance::MobBreakGrow(tower::balance::ShownOfWave(MobScaleFloor()))
                                                : tower::balance::BossBreakGrow(fp::ShownBoss(StatFloor(), run_.floors));  // 3.0.0 分段；4.1.0 主线创意层按所在 Boss 层
                if (brk != 1.0) attr::Want(t, attr::AttrCurrent(t, shachiAttack_), Kind::Scale, static_cast<float>(brk));
            }
            if (deathAt_ >= 0) {  // 殒命表现：Boss 定住、不受伤
                if (dilation_ >= 0) attr::Want(t, reinterpret_cast<float*>(t.obj + dilation_), Kind::Scale, 0.01f);
                for (int o : taken_) attr::Want(t, attr::AttrCurrent(t, o), Kind::Scale, 0.0f);
            }
            // 3.6.0：原「300 层起杀气 5 秒回复」并入下面的无尽层回复（EnemyRegen）
            // 用户 2026-11-12：魔渊出场会和主角对话（短剧情，主角不能高速移动），这期间其他怪出手玩家挡不了：
            // 魔渊到场后固定 2 秒、以及原生剧情镜头锁期间，除魔渊外全部定身、不受伤（小怪同样）
            if ((storyHold || now_ < motianHoldUntil_) && !(a.boss >= 0 && tower::kBossPool[a.boss].identity == std::string_view("MoTian"))) {  // 开场台词期间定身、不受伤
                if (dilation_ >= 0) attr::Want(t, reinterpret_cast<float*>(t.obj + dilation_), Kind::Scale, 0.01f);
                for (int o : taken_) attr::Want(t, attr::AttrCurrent(t, o), Kind::Scale, 0.0f);
            }
            const bool waveCountdown = EndlessOn() && endlessWave_ > 0 && fightSince_ > 0 && now_ < endlessWaveStart_;
            if (waveCountdown) {  // 3.0.1：无尽层第 2 / 3 波到齐后读秒 3 秒再开战（玩家反馈 Boss 突然跳出来就打），期间定身、不受伤
                if (dilation_ >= 0) attr::Want(t, reinterpret_cast<float*>(t.obj + dilation_), Kind::Scale, 0.01f);
                for (int o : taken_) attr::Want(t, attr::AttrCurrent(t, o), Kind::Scale, 0.0f);
            }
            // 3.0.1（玩家反馈：杂兵层有的怪对主角没有仇恨、不攻击）：塔静默了原生战斗组的领域事件，只靠视觉的怪会发呆。
            // 开战后（读秒 / 换波读秒结束）给每只敌人发一次「玩家进入领域」，4 秒后再补一次；原来只在多 Boss 层发
            if (fightSince_ > 0 && !towerentry::Holding() && !waveCountdown && !a.dead && a.woke < 2 && now_ >= a.wakeAt) {
                const int r = towernative::EnterDomain(a.obj);
                if (a.woke == 0 && r != 1) pbz::log::Info("影塔第 {} 层：{} 唤醒失败（{}）", run_.floor, a.name, r == 0 ? "没找到 AI 组件" : "接口不可用");
                ++a.woke; a.wakeAt = now_ + 4.0;
            }
            if (relay_.Guarded(now_)) {  // 车轮出场保护 1 秒：Boss 不动、不受伤
                if (dilation_ >= 0) attr::Want(t, reinterpret_cast<float*>(t.obj + dilation_), Kind::Scale, 0.01f);
                for (int o : taken_) attr::Want(t, attr::AttrCurrent(t, o), Kind::Scale, 0.0f);
            }
            float hp = ue::Get<float>(attr::AttrCurrent(t, hp_));
            towercombat::Sample(a.obj, hp);
            if (grow_.on && a.filled && fightSince_ > 0 && !towerentry::Holding())
                leech_.Damage(a.leechHp, hp, LeechRate());
            if (a.leechHp >= 0 && hp < a.leechHp - 0.5f && fightSince_ > 0) enemyHitSeen_ = true;  // 3.1.0 以伤换伤：玩家打中了
            // 3.6.0 飞身踢：Boss 进了斩杀线后被打中 → 削到只剩 1 点（有原生锁血线的，削到锁血线上方 1 点）。
            // 3.6.2 紧急修复（用户：二阶段 Boss 被踢后空血、不进转阶段动画、留在场上打人且打不死）：原来直接写 0 / 写到锁血线，
            // 绕过了游戏的伤害结算——死亡、转阶段、处决都在伤害结算里触发，写成 0 的 Boss 永远不死。现在只留 1 点，
            // 下一刀由游戏自己结算，死亡 / 转阶段 / 处决全走原生流程
            if (grow_.on && grow_.hexes.Has(tower::hex::FlyingKick) && a.mobHp <= 0 && a.filled && fightSince_ > 0 && deathAt_ < 0 && !fightPause &&
                a.leechHp >= 0 && hp < a.leechHp - 0.5f && tower::hex::KickLine(hp, ue::Get<float>(attr::AttrCurrent(t, hpMax_), 0.0f), grow_.hexes.KickRate())) {
                const float mn = std::max(0.0f, ue::Get<float>(attr::AttrCurrent(t, hpMin_), 0.0f));
                const float after = tower::hex::KickLeave(mn);
                if (std::isfinite(hp) && after < hp && ue::Write(attr::AttrBase(t, hp_), &after, 4) && ue::Write(attr::AttrCurrent(t, hp_), &after, 4)) {
                    towerwidgets::RefreshEnemyHpBar(a.obj, after, ue::Get<float>(attr::AttrCurrent(t, hpMax_), 0.0f));
                    towercombat::Adjusted(a.obj, after, "飞身踢");
                    pbz::log::Info("影塔海克斯：飞身踢 {} 生命 {:.0f} → {:.0f}（斩杀线 {:.0f}% 最大生命{}；下一刀由原生结算）", a.name, hp, after, grow_.hexes.KickRate() * 100, mn > 1.0f ? "，原生锁血线上方" : "");
                    Broadcast("info.hex_kick", "飞身踢");
                    hp = after; a.burn.Applied(after);
                    a.kickAt = now_; a.kickHp = after;
                }
            }
            // 3.6.2 飞身踢结果跟踪（日志）：踢后 Boss 死亡 / 回血（转阶段）/ 30 秒仍停在残血（疑似卡住）各记一行
            if (a.kickAt > 0) {
                if (a.death.Dead() || a.dead) { pbz::log::Info("影塔飞身踢跟踪：{} 踢后 {:.1f} 秒死亡（原生结算）", a.name, now_ - a.kickAt); a.kickAt = -1; }
                else if (hp > a.kickHp + 1.0f) { pbz::log::Info("影塔飞身踢跟踪：{} 踢后 {:.1f} 秒生命回到 {:.0f}（转阶段 / 原生回血）", a.name, now_ - a.kickAt, hp); a.kickAt = -1; }
                else if (now_ - a.kickAt > 30) { pbz::log::Info("影塔飞身踢跟踪：{} 踢后 30 秒仍停在 {:.0f} 生命、未死未转阶段（疑似卡住；锁血线 {:.0f}）", a.name, hp,
                                                                 ue::Get<float>(attr::AttrCurrent(t, hpMin_), -1.0f)); a.kickAt = -1; }
            }
            // 3.0.1（玩家反馈：火上浇油打了没灼烧）：3.0.0 正式版卡池放入了火上浇油，但灼烧逻辑还包在测试版编译开关里，正式版选了也不生效。
            // 现在两个版本都编进来；每秒的灼烧日志只在测试版写
            if (grow_.on && grow_.hexes.Has(tower::hex::Burn) && a.filled && fightSince_ > 0 && !towerentry::Holding() && !fightPause && deathAt_ < 0) {
                const bool wasBurning = a.burn.active;
                if (a.burn.Observe(now_, hp) && !wasBurning && towerdev::kPerfLog) pbz::log::Info("火上浇油：{} 受伤，开始灼烧 3 秒（之后冷却 5 秒）", a.name);
                const int ticks = a.burn.Due(now_);
                const float mx = ue::Get<float>(attr::AttrCurrent(t, hpMax_));
                const float minHp = std::max(1.0f, ue::Get<float>(attr::AttrCurrent(t, hpMin_), 1.0f));
                const float after = static_cast<float>(std::max<double>(minHp, hp - tower::burn::Effect::Damage(mx, ticks, grow_.hexes.BurnRate())));
                if (ticks > 0 && std::isfinite(hp) && std::isfinite(mx) && mx > 0 && std::isfinite(minHp) && hp > minHp && after < hp) {
                    const float oldBase = ue::Get<float>(attr::AttrBase(t, hp_));
                    const bool baseOk = ue::Write(attr::AttrBase(t, hp_), &after, 4);
                    const bool currentOk = ue::Write(attr::AttrCurrent(t, hp_), &after, 4);
                    if (baseOk && currentOk) {
                        const int bars = towerwidgets::RefreshEnemyHpBar(a.obj, after, mx);
#ifdef PBZ_HEX_TEST
                        pbz::log::Info("火上浇油测试：{} 灼烧 {} 次，生命 {:.1f} → {:.1f} / {:.1f}，刷新 {} 条血条（非致死）", a.name, ticks, hp, after, mx, bars);
#else
                        (void)bars;
#endif
                        towercombat::Adjusted(a.obj, after, "火上浇油");
                        hp = after; a.burn.Applied(after);
                    } else {
                        if (baseOk) ue::Write(attr::AttrBase(t, hp_), &oldBase, 4);
                        if (currentOk) ue::Write(attr::AttrCurrent(t, hp_), &hp, 4);
                        a.burn.Clear(); pbz::log::Info("火上浇油：{} 写入未确认，已尝试回滚并停止此目标灼烧", a.name);
                    }
                }
            }
            // 3.6.0 反震：玩家受伤时排进来的伤害打到这个目标上（不穿锁血线，单次最多最大生命 1%；只比指针，不解引用旧目标）
            if (thornsFrac_ > 0 && thornsTarget_ == a.obj) {
                const double frac = std::min(thornsFrac_, tower::hex::kThornsCap);
                thornsFrac_ = 0; thornsTarget_ = nullptr;
                const float mx = ue::Get<float>(attr::AttrCurrent(t, hpMax_));
                const float minHp = std::max(1.0f, ue::Get<float>(attr::AttrCurrent(t, hpMin_), 1.0f));
                const float after = static_cast<float>(std::max<double>(minHp, hp - frac * mx));
                if (a.filled && fightSince_ > 0 && deathAt_ < 0 && !fightPause && !towerentry::Holding() && std::isfinite(hp) && std::isfinite(mx) && mx > 0 &&
                    std::isfinite(minHp) && hp > minHp && after < hp) {
                    const bool baseOk = ue::Write(attr::AttrBase(t, hp_), &after, 4);
                    const bool currentOk = ue::Write(attr::AttrCurrent(t, hp_), &after, 4);
                    if (baseOk && currentOk) {
                        towerwidgets::RefreshEnemyHpBar(a.obj, after, mx);
                        towercombat::Adjusted(a.obj, after, "反震");
                        if (thornsLogs_ < 40 && towerdev::kPerfLog) { ++thornsLogs_; pbz::log::Info("影塔海克斯：反震 {} 生命 {:.0f} → {:.0f}（{:.2f}% 最大生命）", a.name, hp, after, frac * 100); }
                        hp = after; a.burn.Applied(after);
                    }
                }
            }
            // 3.6.0（用户 2026-11-14）无尽层敌人回复：每个敌人单独计时，距上次扣血或杀气减少满 3 秒开始回复；
            // 生命每秒 2% 最大生命（每 100 层 +1%，最多 5%），杀气每秒 10% 上限（300 层起沿用 15%）；锁血线上 / 无敌 / 归零中不回复
            if (fightSince_ > 0 && !a.dead && a.filled && run_.endless && run_.floor > 50 && deathAt_ < 0 && !fightPause && !towerentry::Holding())
                EnemyRegen(a, t, hp);
            else { a.regenHp = -1; a.shachiQuiet = -1; a.shachiAt = -1; a.shachiLast = -1; }
            a.leechHp = hp;
            // 死亡确认（20 层：血量归零就派奖，没有确认）：归零连续 1 秒才算死；中途回血 = 阶段续接
            switch (a.death.Sample(now_, hp)) {
                case tower::fate::DeathTrack::Ev::Zero: if (fightSince_ > 0) a.zeroSeen = true; if (a.mobHp <= 0) pbz::log::Info("影塔战斗观测：floor={} actor={} hp={:.1f} 归零，等确认 wall_ms={}", run_.floor, a.name, hp, GetTickCount64()); break;
                case tower::fate::DeathTrack::Ev::Resumed: a.zeroSeen = false; pbz::log::Info("影塔战斗观测：floor={} actor={} 归零后回血 {:.0f}（阶段续接 #{}），不算死亡", run_.floor, a.name, hp, a.death.resumes); break;
                case tower::fate::DeathTrack::Ev::Confirmed:
                    if (a.mobHp <= 0) pbz::log::Info("影塔战斗观测：floor={} actor={} 死亡确认（归零 {:.1f} 秒，Actor {}）wall_ms={}", run_.floor, a.name, now_ - a.death.zeroAt, ue::Live(a.obj) ? "仍在" : "已销毁", GetTickCount64());
                    if (replay_) PhaseMemberDead(a.phase, a.obj);
                    break;
                default: break;
            }
            const bool zero = hp <= 0.5f;
            // 波次怪没有阶段续接：归零 0.3 秒就算死（Boss 仍是 1 秒确认），死完马上刷下一波
            const bool dead = a.death.Dead() || (a.filled && a.mobHp > 0 && a.death.zeroAt >= 0 && now_ - a.death.zeroAt >= 0.3);
            if (dead != a.dead && dead) ++warKills_;  // 3.1.0 以战养战
            if (dead != a.dead) { a.dead = dead; if (a.mobHp <= 0) pbz::log::Info("影塔战斗观测：floor={} actor={} hp={:.1f} dead={} wall_ms={}", run_.floor, a.name, hp, dead, GetTickCount64()); }
            if (dead && a.mobHp > 0 && !a.reaped) { a.reaped = true; if (grow_.on && grow_.hexes.Has(tower::hex::Reaper)) ++reapPending_; }  // 收割者：每个小怪只算一次
            // 3.2.1 无尽层（用户：Boss 死后残血模型不消失卡关）：
            //   残血兜底：Boss 血量 ≤ 上限 1% 且 3 秒没变化（被原生流程钉住）→ 归零，按正常死亡确认走；
            //   死后清场：死亡确认 1.5 秒后直接移除模型（不等原生死亡演出 / 核心回收），不留血条和尸体
            if (EndlessOn() && !replay_ && fightSince_ > 0 && a.mobHp <= 0 && a.filled) {
                const float mx = ue::Get<float>(max, 0.0f);
                if (!dead && hp > 0.5f && mx > 0 && hp <= mx * 0.01f) {
                    if (a.lowSince < 0 || std::abs(hp - a.lowHp) > mx * 0.002f) { a.lowSince = now_; a.lowHp = hp; }
                    else if (now_ - a.lowSince >= 3.0) {
                        const float zero = 0.0f;
                        ue::Write(attr::AttrBase(t, hp_), &zero, 4); ue::Write(attr::AttrCurrent(t, hp_), &zero, 4);
                        towercombat::Adjusted(a.obj, 0.0f, "无尽层残血兜底");
                        pbz::log::Info("影塔无尽混编层：{} 残血 {:.0f}/{:.0f} 持续 3 秒不死，按击杀处理", a.name, hp, mx);
                        a.lowSince = -1;
                    }
                } else a.lowSince = -1;
                if (dead && !a.swept && a.death.zeroAt >= 0 && now_ - a.death.zeroAt >= 1.5 && fnDestroy_ && ue::Live(a.obj)) {
                    a.swept = true;
                    towerisolate::BlockGroupOf(a.obj);
                    uint8_t none[8] = {};
                    pe::Call(a.obj, fnDestroy_, none);
                    pbz::log::Info("影塔无尽混编层：{} 死亡确认，已清场", a.name);
                    continue;
                }
            }
            if (dead && a.scale>0)a.filled=true; // never resurrect a legitimate early kill merely to finish entry normalization
            if (!a.filled && hp>0.5f && a.frames++ >= 2) {  // 倍率生效后把当前血量设成新上限（只做一次）
                // 32ffb7d：只在上限变高时补，缩小时当前血量留在原生值 → Health 6000 / Max 5000、7000 / 5000、11000 / …（初始状态不一致）
                const float full = ue::Get<float>(max);
                const float cur = ue::Get<float>(attr::AttrCurrent(t, hp_));
                if (full > 0 && cur != full) {
                    ue::Write(attr::AttrBase(t, hp_), &full, 4); ue::Write(attr::AttrCurrent(t, hp_), &full, 4);
                    towercombat::Adjusted(a.obj,full,"入场归一");
                    if (cur > full) pbz::log::Info("影塔第 {} 层：{} 当前血量 {:.0f} 高于上限 {:.0f}，已设为上限", run_.floor, a.name, cur, full);
                }
                a.filled = true;
                if (a.mobHp > 0) {  // 用户 2026-11-08：定身 / 缓时对波次层每一波都生效，从这一波怪就绪时算
                    if (ItemOn(tower::Item::Freeze)) freezeUntil_ = std::max(freezeUntil_, now_ + 4);
                    if (ItemOn(tower::Item::Slow)) slowUntil_ = std::max(slowUntil_, now_ + 10);
                }
            }
            if (!dead || !a.filled) anyAlive = true;
            if (MobActor(a) && dead) towerentry::ForgetEndedWaveActor(a.obj);
            entryReady = entryReady && tower::entry::ActorReady(MobActor(a), dead, a.filled, zero);
        }
        if (player && grow_.on) ApplyLeech(*player, !anyAlive);
        if (vanished && Vanished()) return;
        if (towerentry::Holding() && SpecialOn() && !spawnQueue_.empty() && pending_ - static_cast<int>(spawnQueue_.size()) < kSpecialInFlight) {
            const int b = spawnQueue_.front().boss;  // 3.7.0 创意层：入场读秒前按回执补交，读秒要等全部到齐
            spawnQueue_.erase(spawnQueue_.begin());
            SubmitBoss(b, FactionFor(b));
            if (run_.phase != tower::Phase::Fighting) return;
        }
        if (towerentry::Holding()) {
            towerperf::Section s("入场门");
            // 读秒前准入：先按真实场地武装战斗区，玩家和 Boss 都在里面（留 2 米）才读秒；3 秒内不过就取消回准备、说明原因
            if (entryReady && !admitted_) {
                if (!gateArmed_) { ArmBoundary(); gateArmed_ = true; }
                tower::arena::Admission adm;
                if (boundary_.on && fnLoc_) {
                    auto at = [&](UObject* o) { uint8_t b[32] = {}; pe::Call(o, fnLoc_, b); double v[3]; std::memcpy(v, b, 24); return tower::arena::P3{v[0], v[1], v[2]}; };
                    std::vector<std::pair<std::string, tower::arena::P3>> bosses;
                    for (auto& a : actors_) if (!(MobActor(a) && (a.dead || a.gone)) && ue::Live(a.obj)) bosses.push_back({a.name, at(a.obj)});
                    double pl[3];
                    if (PawnLoc(pl)) adm = tower::arena::Admit(boundary_, {pl[0], pl[1], pl[2]}, bosses);
                    // 落地 / 同侧可达（每 0.5 秒探一次）：脚下没地面、或射线被挡且导航不完整 → 不读秒
                    if (adm.ok && now_ >= nextReach_ && PawnLoc(pl)) {
                        nextReach_ = now_ + 0.5;
                        for (auto& a : actors_) {
                            if ((MobActor(a) && (a.dead || a.gone)) || !ue::Live(a.obj)) continue;
                            const auto b = at(a.obj);
                            const double bo[3] = {b.x, b.y, b.z};
                            const auto probe = towerreach::Measure(towerwidgets::Pawn(), pl, a.obj, bo);
                            lastReach_ = a.name + "：" + towerreach::Describe(probe);
                            if (const auto v = towerreach::Decide(probe); v != towerreach::Verdict::Pass) {
                                adm = {false, std::format("{}（{}；{}）", towerreach::Why(v), a.name, towerreach::Describe(probe))};
                                break;
                            }
                        }
                    } else if (adm.ok && !reachDone_) adm = {false, "可达探测中"};
                }
                if (adm.ok && !reachDone_) { reachDone_ = true; adm = {false, "可达探测中"}; }  // 第一次探测通过后再确认一帧
                else if (adm.ok) {
                    admitted_ = true;
                    pbz::log::Info("影塔入场准入：通过（{}，{}；{}）", boundary_.Describe(), tvenue::Found() ? "场地数据" : "默认", lastReach_.empty() ? "可达未测" : lastReach_);
                } else {
                    // 1.0.1（用户 2026-11-07 第 7 层荆野：直接生成落在世界原点 (-65,-8,186)，准入 3 秒后判失败跳层）：
                    // Boss 生成后远在场外（离战斗区中心 100 米以上）→ 先放回场地（原生出生点，拿不到就放到玩家身前 8 米），
                    // 落点按地面射线核过才放，最多 3 次，再判准入。所有层通用（同类"刷出来但不在场里"）
                    if (adm.why != "可达探测中" && admitRecalls_ < 3 && boundary_.on && fnLoc_) RecallFarBosses();
                    // 41 层雷婷（高空独立空间）：钟传送落点在雾门凸包外，等 3 秒必然作废。玩家在界外 → 先放进界内一次，再重新计时
                    // 41 层雷婷第三次实测（a9f9974）：把人放进 13 点凸包后脚下没有地面——旧导出的凸包和实际流送进来的高空平台对不上，
                    // 真正的地面就在钟传送落点、玩家与 Boss 站的地方。不再把人搬进凸包：已站稳的玩家 / Boss 在凸包外 → 战斗区改为以他们为中心的圆（只改一次，不移动任何人）
                    // 4.0.0（用户：无尽层很多 Boss 刷在雾门外）：无尽 / 创意层玩家站在场内、只是 Boss 落在雾门外 → 把 Boss 放回场内（同出界拉回，落点核地面），
                    // 不再走下面"按站位改圆"把门外的 Boss 圈进战斗区。玩家自己也在凸包外（凸包和实际场地对不上）才照旧改圆，不移动玩家
                    if (EndlessOn() && !run_.inMobWave && adm.why != "可达探测中" && bossPlaced_ < 3 && boundary_.on && !boundary_.hull.empty()) PlaceBossesInside();
                    if (!run_.inMobWave && adm.why != "可达探测中" && !admitPushed_ && boundary_.on && !boundary_.hull.empty()) {
                        double pp2[3];
                        std::vector<tower::arena::P3> pts;
                        if (PawnLoc(pp2)) pts.push_back({pp2[0], pp2[1], pp2[2]});
                        for (auto& a : actors_) {
                            if (!ue::Live(a.obj)) continue;
                            uint8_t lb[32] = {}; pe::Call(a.obj, fnLoc_, lb); double lv[3]; std::memcpy(lv, lb, 24);
                            pts.push_back({lv[0], lv[1], lv[2]});
                        }
                        const bool outside = std::any_of(pts.begin(), pts.end(), [&](const tower::arena::P3& q) { return !boundary_.Contains(q, 200); });
                        if (outside && !pts.empty()) {
                            tower::arena::P3 c{0, 0, pts[0].z};
                            for (auto& q : pts) { c.x += q.x / pts.size(); c.y += q.y / pts.size(); }
                            double farthest = 0;
                            for (auto& q : pts) farthest = std::max(farthest, std::hypot(q.x - c.x, q.y - c.y));
                            const double r = std::max({2500.0, farthest + 1500, tower::arena::Boundary::kDefaultRadius});
                            const double slack = boundary_.slack;
                            const std::string old = boundary_.Describe();
                            boundary_.Arm(c, r);
                            boundary_.slack = slack;
                            admitPushed_ = true; admitSince_ = -1; pushedAt_ = now_; lastAdmitWhy_.clear(); reachDone_ = false; nextReach_ = now_ + 0.5;
                            pbz::log::Info("影塔战斗区：玩家 / Boss 站在场地数据边界（{}）外，按实际站立位置改为圆（中心 {:.0f}/{:.0f}/{:.0f}，半径 {:.0f} 米），不移动任何人",
                                           old, c.x, c.y, c.z, r / 100);
                        }
                    }
                    // b948566 实测：放入那一帧仍用放入前算出的"界外"理由开始计时，随后可达探测一直"探测中"，3 秒到点照样作废。
                    // 放入后 1 秒内不计时（等点传送 / 位置生效）；放入过的入场给 6 秒；"探测中"不单独触发作废；每种新理由记一行便于取证
                    const bool settling = admitPushed_ && now_ - pushedAt_ < 1.0;
                    if (adm.why != lastAdmitWhy_ && adm.why != "可达探测中") {
                        lastAdmitWhy_ = adm.why;
                        double lp[3] = {};
                        PawnLoc(lp);
                        pbz::log::Info("影塔入场准入：当前未通过理由：{}（玩家 {:.0f}/{:.0f}/{:.0f}{}）", adm.why, lp[0], lp[1], lp[2], settling ? "，放入后等待生效" : "");
                    }
                    if (!settling && admitSince_ < 0 && adm.why != "可达探测中") { admitSince_ = now_; pbz::log::Info("影塔入场准入：未通过，等 {} 秒落稳：{}", admitPushed_ ? 6 : 3, adm.why); }
                    const double limit = admitPushed_ ? 6 : 3;
                    if (!settling && admitSince_ >= 0 && now_ - admitSince_ > limit) {
                        const std::string why = adm.why == "可达探测中" && !lastAdmitWhy_.empty() ? lastAdmitWhy_ + "（最后在可达探测）" : adm.why;
                        RecordFailure("admission", venueBoss_, why);
                        if (run_.inMobWave) CancelWave("小怪入场准入失败：" + why, tower::wave::Failure::Venue);
                        else CancelEntry("入场准入失败：" + why);
                        return;
                    }
                    entryReady = false;
                }
            }
            const auto state = towerentry::Tick(entryReady);
            if (state == tower::entry::State::Failed) { CancelEntry(towerentry::Why()); return; }
            if (state != tower::entry::State::Running) return;
            fightSince_ = now_; minHpFrac_ = 1.0;
            if (EndlessOn() && std::any_of(actors_.begin(), actors_.end(), [](const Actor& x) { return x.boss >= 0 && tower::kBossPool[x.boss].identity == std::string_view("MoTian"); })) {
                motianHoldUntil_ = now_ + 2.0;
                pbz::log::Info("影塔第 {} 层：魔渊在场，读秒后其他怪定身 2 秒、玩家无敌（出场对话）", run_.floor);
            }
            cinematicSeconds_=0;
            if (!spawnQueue_.empty()) {
                for (auto& q : spawnQueue_) q.at += now_ - staggerBase_;
                pbz::log::Info("影塔第 {} 层：开战，错峰的 {} 只按开战时刻进场", run_.floor, spawnQueue_.size());
            }
            if (!gateArmed_) ArmBoundary();  // 准入时已按场地武装过就沿用（同一个战斗区）
            towercombat::Begin(run_.floor, towerwidgets::Pawn());
            for (auto& a : actors_) towercombat::Watch(a.obj, a.name);
            // 多 Boss 层：开战时让每只 Boss 的 AI 收到"玩家进入领域"（塔静默了战斗组领域事件，左殇真龙之躯会一直站着等挨打）
            if (count_ > 1) for (auto& a : actors_) {
                const int r = towernative::EnterDomain(a.obj);
                a.woke = 1; a.wakeAt = now_ + 4.0;
                pbz::log::Info("影塔第 {} 层：{} 开战唤醒（玩家进入领域）{}", run_.floor, a.name, r == 1 ? "已发" : r == 0 ? "没找到 AI 组件" : "接口不可用");
            }
            stateLogUntil_ = towerdev::kVerbose && count_ > 1 ? now_ + 20.0 : 0; stateLogNext_ = now_;
            gateArmed_ = false; admitted_ = false; reachDone_ = false; admitSince_ = -1;
            fight_ = {}; lastHp_ = -1; berserkFired_ = false; medUses_ = -1;
            FirePrep();  // 准备道具：读秒结束、正式开战这一刻才扣并生效（取消 / 生成失败不会走到这里）
            floorNote_.clear();
            Broadcast("info.floor_started", std::format("第 {} 层，开战", ShownBoss(run_.floor)));
        }
        // 错峰：还有没进场 / 没回执的，不算打完
        // Submit may synchronously invoke a failure callback which edits the
        // same queue. Remove one due request before invoking it, and never
        // keep an iterator across the callback.
        if (auto it = std::find_if(spawnQueue_.begin(), spawnQueue_.end(), [&](const auto& q) { return now_ >= q.at; }); it != spawnQueue_.end()) {
            const int b = it->boss;
            spawnQueue_.erase(it);
            SubmitBoss(b, FactionFor(b));
            if (run_.phase != tower::Phase::Fighting) return;
        }
        if (replay_ && !actors_.empty() && !anyAlive && !rp_.Settle() && rp_.seen == replay_->phaseN - 1) {
            // 兜底（34 层二阶段死后既不结算也不作废、一直挂着）：最后一阶段已到场、本层角色全死 = 打完
            pbz::log::Info("影塔重放：最后一阶段已到场且角色全部死亡，按打完结算");
            rp_.Dead(rp_.seen, now_);
        }
        if (replay_ && !actors_.empty() && !anyAlive && !rp_.Settle()) {  // 重放层：中间阶段死了，等原生转阶段刷出下一阶段
            clearAt_ = 0;
            if (replay_->phaseTravel) PhaseTravel();
            else if (rp_.betweenSince >= 0) KickWhenQuiet(rp_.seen + 1, 15, "上一阶段已死、下一阶段没出现");  // 只是消失（没死）由 Vanished 处理  // 2026-11-06 玩家报第 4 层追命二阶段没出：所有重放层都兜底
            if (rp_.TimedOut(now_, replay_->phaseGap)) {
                const std::string why = std::format("原生遭遇重放：第 {} 阶段死后 {:.0f} 秒内没出现下一阶段（转阶段没跑起来）：本层作废、不发奖", rp_.seen + 1, replay_->phaseGap);
                RecordFailure("replay_phase", venueBoss_, why);
                CancelEntry(why);
            }
            return;
        }
        StragglerTick(anyAlive);  // 4.1.0：打完了却不结算——回执超时、残余敌人唤醒 / 拉近、HUD 提示剩余
        if (EndlessOn() && fightSince_ > 0 && preloadedWave_ != endlessWave_ + 1 &&
            std::count_if(actors_.begin(), actors_.end(), [](const Actor& x) { return !x.dead && !x.gone; }) <= 1) PreloadNextWave();  // 4.0.0
        // 无尽混编层：本波全部确认死亡 → 2.5 秒后刷下一波（不结算）；最后一波打完走下面的正常结算，只结算一次
        if (EndlessOn() && endlessWave_ + 1 < static_cast<int>(endless_.size()) && fightSince_ > 0 && !anyAlive && spawnQueue_.empty() &&
            pending_ <= 0 && endlessMobPending_ <= 0) {
            if (endlessNextAt_ < 0) {
                endlessNextAt_ = now_ + 1.0; clearAt_ = 0;  // 3.2.1：死者已清场，换波只留 1 秒
                pbz::log::Info("影塔无尽混编层：第 {} 层第 {}/{} 波清完，1 秒后下一波", run_.floor, endlessWave_ + 1, endless_.size());
            }
            if (now_ >= endlessNextAt_) AdvanceEndless();
            return;
        }
        if (!actors_.empty() && !anyAlive && spawnQueue_.empty() && pending_ <= 0 && relay_.Dead(now_)) {
            if (const int next = relay_.Due(now_); next >= 0) {
                clearAt_ = 0;
                // 44 层玉琮车轮：上一形态死后 Actor 仍在，原生 Boss 血条一直挂着，下一形态出场时屏幕上多一条无名血条。
                // 换下一位前移除已确认死亡的上一位（战后流程本来就冻结；先阻断其战斗组清组）
                for (auto& a : actors_) {
                    if (!a.dead || !ue::Live(a.obj) || !fnDestroy_) continue;
                    towerisolate::BlockGroupOf(a.obj);
                    uint8_t none[8] = {};
                    pe::Call(a.obj, fnDestroy_, none);
                    pbz::log::Info("影塔第 {} 层车轮：移除已阵亡的 {}（收掉残留血条）", run_.floor, a.name);
                }
                actors_.clear();
                before_ = pbz::enemies::OwnedActors();
                pending_ = 1;
                relayRetries_ = 0;
                pbz::log::Info("影塔第 {} 层车轮：下一位 {}（{}/{}）", run_.floor, tower::kBossPool[next].name, relay_.index, relay_.total);
                Broadcast("info.relay_next", std::format("下一位：{}（{}/{}）", tower::kBossPool[next].name, relay_.index, relay_.total));
                SubmitBoss(next);
            }
            return;
        }
        if ((!actors_.empty() || (EndlessOn() && fightSince_ > 0)) && !anyAlive && spawnQueue_.empty() && pending_ <= 0 && endlessMobPending_ <= 0) {
            if (clearAt_ <= 0) { clearAt_ = now_ + tower::fate::kSettleSec; pbz::log::Info("影塔战斗观测：floor={} all_dead=1 pending={} queued={} clear_at={:.3f} wall_ms={}", run_.floor, pending_, spawnQueue_.size(), clearAt_, GetTickCount64()); }
            if (run_.inMobWave) { if (now_ >= clearAt_) { clearAt_ = 0; MobWaveCleared(); } return; }
            if (specialMain_ && run_.specialStage) { if (now_ >= clearAt_) { clearAt_ = 0; FinishSpecialMain(); } return; }  // 3.7.0 主线创意层：按小兵段结算
            if (now_ >= clearAt_ && rewardedFloor_ == run_.floor && rewardedRun_ == run_.id) {  // 同一层只派一次
                pbz::log::Info("影塔战斗观测：floor={} 已派过奖，忽略重复结算", run_.floor);
                clearAt_ = 0; actors_.clear();
            } else if (now_ >= clearAt_) {
                rewardedFloor_ = run_.floor; rewardedRun_ = run_.id;
                if (replay_) { towerisolate::RestoreControl("重放层结算（战后过场已冻结）"); pbz::log::Info("影塔重放：最后一阶段确认死亡，直接结算（不播战后过场）"); }
                EndReplay();
                pbz::log::Info("影塔战斗观测：floor={} reward_dispatch=1 game_time={:.3f} wall_ms={}", run_.floor, now_, GetTickCount64());
                clearAt_ = 0;
                Clear("floor");
                actors_.clear();
                std::vector<std::string_view> paths;
                for (int b : run_.bosses) paths.push_back(tower::kBossPool[b].path);
                const std::string clearedNames = LoggedBossNames();
                // v14：难度按逐层表固定，不按上一层表现自适应（用户 2026-11-05）；这里只记录，供数值复测
                pbz::log::Info("影塔平衡记录：第 {} 层用时 {:.0f} 秒、最低生命 {:.0f}%（逐层表固定，不调整后续层）", run_.floor, FightSeconds(), minHpFrac_ * 100);
                fight_.seconds = FightSeconds();
                // 34 层力者（ui11 实机）：局存档的"已得兵器"里有断肢锯（退档 / 取消本层留下的旧记录），背包里却没有 → 当成重复不发。
                // 原生成长模式以背包实际持有为准：记录里有、背包里没有的先去掉，再按本层 Boss 判断要不要发
                if (grow_.on) std::erase_if(run_.weapons, [&](int w) {
                    const auto& d = tower::kWeaponDrops[w];
                    const int gi = tower::growth::WeaponIndex(d.weapon, d.shaqi ? 1 : 0);
                    const bool stale = gi < 0 || grow_.profile.Find(gi) < 0;
                    if (stale) pbz::log::Info("影塔Boss掉落：局记录有 {}（{}）但背包没有，按未获得处理", d.name, d.weapon);
                    return stale;
                });
                // 1.4.0 用户 2026-11-08：重生符回退后再打的层不给任何奖励（魂玉 / 点数 / 掉落 / 三选一 / 额外奖励 / 驿站），否则可以无限刷
                const bool replayed = grow_.on && run_.floor <= run_.peak;
                if (grow_.on && !replayed) { HexCharm(ShownBoss(run_.floor)); grow_.hexes.ClearFloor(ShownBoss(run_.floor)); }
                if (grow_.on && !replayed) run_.RecordClearedRoute(paths);
                const bool scavenger = grow_.on && grow_.hexes.Has(tower::hex::Scavenger);
                // Existing shop unlocks stay intact. New kills route directly to
                // the player after Scavenger is acquired, not into the shop.
                if (grow_.on && run_.shuffleBosses && !replayed && !scavenger) run_.RecordShopDrops(paths);
                lastBossVenue_ = replay_ ? -1 : venueBoss_;  // 小怪层优先沿用上一场 Boss 场地；原生遭遇层的场地带剧情 / 阶段状态，走本区经典场地
                PreloadWave(run_.floor + 1, 0);  // 奖励页期间后台加载下一段第一波的怪
                run_.FloorCleared(paths, replayed, grow_.on && !scavenger);  // 拾荒者改为走已核实的原生掉落路径
                // 3.6.0 魂玉账单：通关时只记下基础魂玉和倍率，奖励页选完再一次性结算（重打的层只发 1/4、不吃海克斯）
                if (!replayed) OpenBill(ShownBoss(run_.floor), run_.last.jade, true);
                if (grow_.on && !replayed) HexClearRewards(ShownBoss(run_.floor), run_.last.jade, true);
                FloorBonus(replayed);
                {
                    const auto c = Chal();
                    const int tenths = run_.floor >= 1 && run_.floor <= static_cast<int>(run_.times.size()) ? run_.times[run_.floor - 1] : 0;
                    RunLog(std::format("floor\t{}\t{}\t{}\t{}\t{}", run_.floor, tenths, clearedNames, tower::floorplay::ChallengeName(c),
                                       c != tower::floorplay::Challenge::None && tower::floorplay::Done(c, fight_, run_.difficulty) ? 1 : 0));
                }
                if (replayed) {
                    grow_.prep.FloorEnded();
                    gains_.clear();
                    floorNote_ = std::format("第 {} 层已经领过奖励，这次只有魂玉 +{}", ShownBoss(run_.floor), run_.last.jade);
                    pbz::log::Info("影塔第 {} 层：回退后重打（最高通过第 {} 层），只发 1/4 魂玉 +{}", run_.floor, run_.peak, run_.last.jade);
                } else if (grow_.on) {  // 原生成长：本层点数 / 药 / 饰品匣 / 铸魂令 + Boss 兵器进背包（1 级）
                    std::vector<std::pair<std::string, int>> drops;
                    for (int w : run_.last.weapons) drops.push_back({std::string(tower::kWeaponDrops[w].weapon), tower::kWeaponDrops[w].shaqi ? 1 : 0});
                    if (const int w = tower::FinalBossWeapon(run_.floor, paths); w >= 0) {
                        const auto& d = tower::kWeaponDrops[w];
                        drops.push_back({std::string(d.weapon), d.shaqi ? 1 : 0});
                    }
                    gains_ = grow_.FloorCleared(Game(), run_.floor, run_.floors, run_.seed, drops, static_cast<int>(run_.bosses.size()));
                    for (auto& id : grow_.held) pbz::log::Info("影塔Boss掉落：兵器 {} 的Boss关联尚未核实，本层未发放（独立于三选一/商店进度）", id);
                    // Boss 原生道具掉落（用户 2026-11-04：无论重放还是场地 + 生成，都要发 Boss 的原生掉落，走原生获得界面）。
                    // 数据：死亡 Flow 静态可达的 GetItems / GiveItems / LootItems（boss_drops.generated.h）；已持有不重复发
                    int itemsGot = 0;
                    std::vector<std::string_view> doneItems;
                    for (auto path : paths) {
                        const auto slash = path.rfind('/'), dot = path.rfind('.');
                        if (slash == std::string_view::npos || dot == std::string_view::npos || dot < slash) continue;
                        const auto stem = path.substr(slash + 1, dot - slash - 1);
                        for (const auto& d : tower::kBossItemDrops) {
                            if (d.stem != stem || std::find(doneItems.begin(), doneItems.end(), d.item) != doneItems.end()) continue;
                            doneItems.push_back(d.item);
                            const int have = Game().ItemCount(d.item);
                            if (have != 0) { pbz::log::Info("影塔Boss掉落：道具 {}（{}）{}，不重复发", d.item, d.flow, have > 0 ? "已持有" : "读不到持有数"); continue; }
                            if (Game().GrantItem(d.item, d.count)) ++itemsGot;
                        }
                    }
                    if (itemsGot) pbz::log::Info("影塔Boss掉落：第 {} 层发放原生道具 {} 件", run_.floor, itemsGot);
                    auto heldDrop = [&](int w) { return std::find(grow_.held.begin(), grow_.held.end(), std::string(tower::kWeaponDrops[w].weapon)) != grow_.held.end(); };
                    std::erase_if(run_.last.weapons, heldDrop);  // 结算 / 状态里不显示没发的兵器
                    std::erase_if(run_.weapons, heldDrop);
                    grow_.prep.FloorEnded();
                    BonusChallengeItem();  // 额外挑战成功：独立发一件进度内非 Boss 关联原生装备 / 药（原生获取界面），不占普通奖励
                    if (run_.phase == tower::Phase::Reward && EndlessFormal() && EndlessShopTurn()) {  // 无尽层：一层驿站、一层奖励页交替
                        run_.FinishReward();
                        SettleBill("无尽驿站层");
                        OpenStageShop(run_.floor + 1);
                        pbz::log::Info("影塔无尽模式：第 {} 层（显示 {}）打完，本层开驿站（不发三选一）", run_.floor, ShownBoss(run_.floor));
                    } else if (run_.phase == tower::Phase::Reward) { grow_.hexRewardSecond = 0; grow_.DrawRewards(run_.floor, run_.floors, run_.seed, run_.items); }  // v14 普通奖励：每层重新抽
                    pbz::log::Info("影塔原生成长：第 {} 层 {}；淬炼 {} 修为 {}", run_.floor, gains_, grow_.profile.temper, grow_.profile.cultivate);
                }
                ReleaseShentong();
                LedgerAdd();
                pbz::log::Info("影塔第 {} 层通过：魂玉 +{}，兵器 {} 件，神通 {} 个", run_.floor, run_.last.jade, run_.last.weapons.size(), run_.last.shentong.size());
                if (run_.phase != tower::Phase::Reward) SettleBill("本层没有奖励页");
                if (run_.phase == tower::Phase::Won) { RunLog(std::format("won\t{}", run_.floor)); settlement_ = Settlement(Player(), true); DeleteRun(); Broadcast("info.won", "通关"); Conclude(); }
                else { SaveRun(); Broadcast("info.floor_cleared", "本层通过", std::format(R"("floor":{})", run_.floor)); }
            }
        } else clearAt_ = 0;
    }

private:
    // boss：kBossPool 下标（-1 = 未知）；goneAt：不在登记里、又没见到死亡的时刻（等合法换体，见 Vanished）
    struct Actor { UObject* obj; std::string name; float native = 0; double scale = 0; float leechHp = -1; double mobHp = 0; bool filled = false, shachiFilled = false; int frames = 0; bool dead = false, gone = false; Target target{}; bool hasTarget = false; int boss = -1; double goneAt = 0; bool adopted = false; tower::fate::DeathTrack death{}; int phase = -1; tower::wave::TargetWatch targetWatch{}; int mobIndex = -1; double mobOutsideSince = -1; tower::burn::Effect burn{}; bool reaped = false; int hexTakenKind = -1; double kickAt = -1; float kickHp = -1;  /* 3.6.2 飞身踢后跟踪 */
        std::string mobRequest, mobActorName;
        int mobTries = 0;
        float shachiLast = -1; double shachiQuiet = -1, shachiAt = -1;  // 3.6.0 无尽层回复：上次扣血 / 杀气减少的时刻、上一帧杀气、上次结算时刻
        float regenHp = -1;                                              // 3.6.0 无尽层回复：上一帧生命
        int woke = 0; double wakeAt = 0;  // 3.0.1：开战后给每只敌人发「玩家进入领域」（最多 2 次，间隔 4 秒）
        double lowSince = -1; float lowHp = -1; bool swept = false;  // 3.2.1 无尽层：残血不死计时 / 死后已清场
        double reachAt = -1; bool reachOut = false;  // 4.0.0：雾门外可达探测（射线 + 导航）每 0.3 秒最多一次，结果沿用
        bool zeroSeen = false;  // 4.1.0：开战后见过归零、之后没回血（读到 NaN 不清除；DeathTrack 读到 NaN 会清掉归零计时）
        bool dmgLogged = false; // 4.1.1：Boss 攻击倍率写一次日志（核对创意层数值）
    };
    // ---- 仆从大师（H 批）。生成走修改器同一条「独立 Boss」路径：标签前缀 towerally-（不以 tower- 开头），
    // 于是核心会做独立阶段替换（雷荡/赤炼 → 终阶段）、移除结束剧情能力、补原生死亡、死后回收尸体——与修改器友军 Boss 相同。
    static constexpr const char* kServantPrefix = tower::servants::kPrefix.data();
    // 兜底前缀：以 tower- 开头 → 核心不做原生定级 / 独立适配（属性由我们写成玩家 50%）。只用于 ActualPath == path 且无适配规则的 Boss
    static constexpr const char* kServantTowerPrefix = "tower-sv-";
    struct Servant {
        UObject* obj = nullptr;
        int index = -1, boss = -1;
        std::string tag;
        bool initialized = false, dead = false, frozen = false, wasInhibited = false;
        double lastSeenAt = 0, nextLog = 0, deadAt = 0;
        std::array<float, 4> logged{};
        float share = tower::servants::kShare;  // 3.2.1：仆从大师 80%；Boss 助战 20% / 30%
        bool assist = false;
        float frac = -1;  // 3.6.3：上一帧的生命比例；敌方转阶段 / 车轮换人时不许回满（只降不升，小幅自然回复放行）
        bool refillLogged = false;
    };
    struct ServantFloor {
        int shown = 0, slots = 0, resolved = 0, attempts = 0;
        int masterSlots = 0;                 // 3.2.1：其中仆从大师的名额；其余是 Boss 助战
        tower::servants::Assist assist{};
        size_t cursor = 0;
        double nextAt = 0, pendingSince = 0;
        bool hudHeld = false;
        bool noLevelGroup = false, frozen = false;  // 本场地没有原生定级组：修改器独立路径会拒绝，改走影塔路径（只用不需要阶段适配的 Boss）
        int retryBoss = -1;         // 刚因定级组失败、待改走影塔路径重试的条目
        int roomTries = 0;          // 附近没落点的连续次数（不占召唤位；每次把搜索距离往外推）
        int noGround = 0;           // 整轮候选点全是「无地面」的次数（场地地面检测失败，如舞狮原生场地）
        double waitLogAt = 0;       // 有空召唤位却没在召唤时，每 10 秒记一次原因
        std::string pending;
        std::vector<int> order;
        std::vector<Servant> actors;
    } servants_;

    bool IsServant(UObject* obj) const {
        return std::any_of(servants_.actors.begin(), servants_.actors.end(), [obj](const Servant& a) {
            return a.obj == obj && ue::Live(obj) && ue::Get<int>(obj + ue::Layout::ObjIndex, -1) == a.index;
        });
    }
    bool ServantStatsReady() const {
        return hp_ >= 0 && hpMax_ >= 0 && attack_ >= 0 && shachiAttack_ >= 0 && shachiNow_ >= 0 && shachiMax_ >= 0;
    }
    Target ServantTarget(UObject* obj) const {
        auto* character = towerperf::ClassNamed("PBZCharacter");
        const int set = character ? ue::PropOffset(character, "AttributeSetBase") : -1;
        return {obj, set >= 0 ? ue::Get<uint8_t*>(obj + set) : nullptr, attr::Who::Enemies, ue::ClassOf(obj)};
    }
    // 玩家当前（含海克斯 / 装备 / 临时效果）的 生命上限、攻击、破防、杀气上限 × 50%
    bool ServantStats(const Target& player, std::array<float, 4>& out, float share = tower::servants::kShare) const {
        const std::array offsets{hpMax_, attack_, shachiAttack_, shachiMax_};
        for (size_t i = 0; i < out.size(); ++i) {
            const auto value = ue::Get<float>(attr::AttrCurrent(player, offsets[i]), -1.f);
            if (!std::isfinite(value) || value < 0 || (i == 0 && value < 2)) return false;
            out[i] = value * share;
        }
        return true;
    }
    // 生成回执当帧（游戏线程）直接写入：AI 出手前就是 50% 属性。同时记下原生值，用来核对「攻击」口径
    bool InitServantNow(UObject* obj, const std::string& name, float share = tower::servants::kShare) {
        const Target* player = Player();
        std::array<float, 4> stats{};
        if (!player || !ServantStats(*player, stats, share)) return false;
        const Target t = ServantTarget(obj);
        if (!t.set) return false;
        auto cur = [&](int off) { return off >= 0 ? ue::Get<float>(attr::AttrCurrent(t, off), -1.f) : -1.f; };
        auto put = [&](int off, float v) { if (off < 0) return; ue::Write(attr::AttrBase(t, off), &v, 4); ue::Write(attr::AttrCurrent(t, off), &v, 4); };
        pbz::log::Info("影塔仆从属性口径：{} 原生 生命上限 {:.1f} 攻击 {:.1f} 破防 {:.1f} 杀气上限 {:.1f} → 写入 {:.1f} / {:.1f} / {:.1f} / {:.1f}（玩家 {:.0f}%）",
            name, cur(hpMax_), cur(attack_), cur(shachiAttack_), cur(shachiMax_), stats[0], stats[1], stats[2], stats[3], share * 100);
        const float shachi = std::clamp(cur(shachiNow_), 0.0f, stats[3]);
        put(hpMax_, stats[0]); put(hp_, stats[0]); put(hpMin_, 0.0f);
        put(attack_, stats[1]); put(shachiAttack_, stats[2]);
        put(shachiMax_, stats[3]); put(shachiNow_, shachi);
        return true;
    }
    void SyncServantPool(const Target& t, int valueOff, int capOff, float cap, bool first, bool health) {
        auto* capCurrent = attr::AttrCurrent(t, capOff);
        auto* valueCurrent = attr::AttrCurrent(t, valueOff);
        const float oldCap = ue::Get<float>(capCurrent), value = ue::Get<float>(valueCurrent);
        if (std::isfinite(oldCap) && std::isfinite(value) && (first || std::abs(cap - oldCap) > 0.01f || value > cap)) {
            const float next = tower::servants::PoolValue(value, oldCap, cap, first, health);
            attr::Hold(t, attr::AttrBase(t, valueOff), next);
            attr::Hold(t, valueCurrent, next);
        }
        attr::Want(t, attr::AttrBase(t, capOff), Kind::Set, cap);
        attr::Want(t, capCurrent, Kind::Set, cap);
    }
    // 3.4.2（玩家：舞狮二阶段的召唤小怪打不死，进不了站桩对战）：先取证——测试包每 2 秒记一次二阶段场上小怪（NPC）的生命 / 上限 / 最低生命。
    // 用户：召唤类 Boss 必须原生击败，与仆从大师两条线分开；不改小怪原生数值
    std::unordered_map<UObject*, double> lionStuck_;
    double lionNext_ = 0, lionDiagAt_ = 0, lionBossDiagAt_ = 0; int lionDiagLogs_ = 0, lionBossDiagLogs_ = 0;
    void LionMinionTick() {
        if (now_ < lionNext_ || hp_ < 0 || hpMax_ < 0 || hpMin_ < 0) return;
        lionNext_ = now_ + 0.5;
#ifdef PBZ_PLAYTEST
        // 3.4.2 测试（用户：开修改器伤害倍率就秒杀、不开不掉血）：每秒记舞狮两阶段的生命 / 最低生命 / 受伤倍率（当前、基础）
        if (now_ >= lionBossDiagAt_ && lionBossDiagLogs_ < 300) {
            lionBossDiagAt_ = now_ + 1.0;
            for (const auto& a : actors_) {
                if (a.dead || a.gone || !a.hasTarget || !ue::Live(a.obj) || ue::ClassName(a.obj).find("LionDancer") == std::string::npos) continue;
                const auto& t = a.target;
                std::string tv;
                for (int o : taken_) tv += std::format(" {:.3f}/{:.3f}", ue::Get<float>(attr::AttrCurrent(t, o), -1.0f), ue::Get<float>(attr::AttrBase(t, o), -1.0f));
                ++lionBossDiagLogs_;
                pbz::log::Info("舞狮 Boss 诊断：{} 生命 {:.0f} / {:.0f}（基础上限 {:.0f}）最低 {:.0f}；受伤倍率 当前/基础{}",
                    ue::ClassName(a.obj), ue::Get<float>(attr::AttrCurrent(t, hp_), -1.0f), ue::Get<float>(attr::AttrCurrent(t, hpMax_), -1.0f),
                    ue::Get<float>(attr::AttrBase(t, hpMax_), -1.0f), ue::Get<float>(attr::AttrCurrent(t, hpMin_), -1.0f), tv);
            }
        }
#endif
        const bool lion = std::any_of(actors_.begin(), actors_.end(), [](const Actor& a) {
            return !a.dead && !a.gone && ue::Live(a.obj) && ue::ClassName(a.obj) == "BP_Monster_LionDancer_P2_C"; });
        if (!lion) { lionStuck_.clear(); return; }
        std::unordered_set<UObject*> ours;
        for (auto* o : pbz::enemies::OwnedActors()) ours.insert(o);
        for (const auto& a : actors_) ours.insert(a.obj);
        const bool diag = now_ >= lionDiagAt_;
        if (diag) lionDiagAt_ = now_ + 2.0;
        struct Doll { Target t; float hp, mn; };
        std::vector<Doll> dolls;
        for (const auto& t : attr::Targets()) {
            if (t.who == attr::Who::Player || !attr::Alive(t) || ours.count(t.obj) || !npcCls_ || !ue::IsA(t.obj, npcCls_) || !towerisolate::InPlayerWorld(t.obj)) continue;
            const float hp = ue::Get<float>(attr::AttrCurrent(t, hp_), -1.0f), mx = ue::Get<float>(attr::AttrCurrent(t, hpMax_), -1.0f), mn = ue::Get<float>(attr::AttrCurrent(t, hpMin_), -1.0f);
#ifdef PBZ_PLAYTEST
            if (diag && hp >= 0 && lionDiagLogs_ < 200) { ++lionDiagLogs_; pbz::log::Info("舞狮二阶段诊断：{} {} 生命 {:.1f} / {:.1f}，最低 {:.1f}", ue::ObjName(t.obj), ue::ClassName(t.obj), hp, mx, mn); }
#else
            (void)diag; (void)mx;
#endif
            if (ue::ClassName(t.obj).find("DaTouWaWa") != std::string::npos && hp > 0.5f) dolls.push_back({t, hp, mn});
        }
        // 3.4.2 实机（用户 2026-11-14 取证）：二阶段最后一只大头娃娃被原生把最低生命锁成当前血量（约 78%）等处决，
        // 处决提示不出、血打不下去 → 卡死进不了站桩。兜底：只剩最后一只、锁血超过 6 秒 → 放开它的最低生命，打死后照原生流程进站桩
        // （实测一刀把娃娃从锁血线以上打到 0 时会正常进站桩）。其它小怪 / Boss 不碰
        if (dolls.size() == 1) {
            const auto& d = dolls[0];
            if (d.mn > 0.5f && d.hp <= d.mn + 0.5f) {
                auto [it, fresh] = lionStuck_.try_emplace(d.t.obj, now_);
                if (!fresh && now_ - it->second >= 6.0) {
                    const float zero = 0.0f;
                    ue::Write(attr::AttrBase(d.t, hpMin_), &zero, 4); ue::Write(attr::AttrCurrent(d.t, hpMin_), &zero, 4);
                    pbz::log::Info("影塔舞狮二阶段：最后一只 {} 锁血 {:.1f} 超过 6 秒、处决未出现，放开锁血（打死后照原生进站桩）", ue::ClassName(d.t.obj), d.mn);
                    lionStuck_.erase(it);
                }
            } else lionStuck_.erase(d.t.obj);
        } else lionStuck_.clear();
    }
    // 仆从召唤物：保留、改为友军阵营（不计胜负、不受影塔敌方加成）。仆从阵亡 / 撤下后由清场巡检或 DismissServants 移除
    std::unordered_map<UObject*, int> servantSummons_;
    std::unordered_map<UObject*, int> servantFactionTries_;  // 3.6.0 改阵营失败的重试次数
    std::unordered_set<UObject*> unadoptedLogged_;
    std::unordered_map<UObject*, double> summonLockSince_; std::unordered_set<UObject*> summonLockLogged_;
    UObject* controllerCls_ = nullptr; int offControllerPawn_ = -1;
    void AdoptServantSummon(UObject* obj, const std::string& cls) {
        const int index = ue::Get<int>(obj + ue::Layout::ObjIndex, -1);
        if (index < 0) return;
        const auto it = servantSummons_.find(obj);
        if (it != servantSummons_.end() && it->second == index) return;
        bool ok = false;
        // 3.6.0（用户：召唤型仆从的召唤物打玩家也打仆从）：日志全是「阵营接口不可用」——召唤物类（玉璧剑影 / 碧海残躯 / 毒分身…）没导出 GetFactionComponent。
        // 同核心生成友军的做法：取不到就用 Actor.GetComponentByClass(FactionComponent) 找；失败不记下，下一轮清场巡检再试（最多 5 次）
        auto* compCls = towerperf::ClassNamed("FactionComponent");
        UObject* comp = nullptr;
        if (auto* get = pe::FindFunction(ue::ClassOf(obj), "GetFactionComponent")) {
            uint8_t g[16]{}; pe::Call(obj, get, g);
            comp = *reinterpret_cast<UObject**>(g);
        }
        if ((!comp || !ue::Live(comp)) && compCls)
            if (auto* byClass = pe::FindFunction(towerperf::ClassNamed("Actor"), "GetComponentByClass")) {
                const int arg = ue::PropOffset(byClass, "ComponentClass"), ret = ue::PropOffset(byClass, "ReturnValue");
                const int size = ue::Get<int>(byClass + ue::Layout::StructSize);
                if (arg >= 0 && ret >= 0 && size > 0 && size <= 64 && arg + 8 <= size && ret + 8 <= size) {
                    uint8_t a[64]{}; std::memcpy(a + arg, &compCls, 8);
                    pe::Call(obj, byClass, a);
                    comp = *reinterpret_cast<UObject**>(a + ret);
                }
            }
        std::string why = "找不到阵营组件";
        if (comp && compCls && ue::Live(comp) && ue::IsA(comp, compCls)) {
            why = "阵营接口缺失";
            if (auto* set = pe::FindFunction(compCls, "SetFaction")) {
                // 3.6.0 实机（荡秽的人偶全是「阵营标签无效」）：字符串转名字要等背包接口初始化，召唤物出现时常常还没就绪 → 0。
                // 改为直接读玩家自己阵营组件的当前阵营（GetCurrnetFaction，原生拼写），原样设给召唤物；读不到再退回字符串转换
                static uint64_t player = 0;
                if (!player) player = PlayerFactionTag(compCls);
                if (!player && ResolveBag() && toName_ && strCdo_) player = ToName("Faction.Player");
                uint8_t b[16]{}; std::memcpy(b, &player, 8);
                why = "阵营标签无效";
                if (player) { pe::Call(comp, set, b); ok = true; }
            }
        }
        const int tries = ++servantFactionTries_[obj];
        if (ok || tries >= 5) servantSummons_[obj] = index;
        if (ok || tries == 1 || tries == 5)
            pbz::log::Info("影塔仆从召唤物：保留 {} {}（{}，不计胜负）", ue::ObjName(obj), cls, ok ? "已设为友军阵营" : why + (tries >= 5 ? "，保持原阵营" : "，稍后重试"));
    }
    // 玩家阵营组件的当前阵营（GameplayTag = FName，8 字节）；读不到返回 0
    uint64_t PlayerFactionTag(UObject* compCls) {
        UObject* pawn = towerwidgets::Pawn();
        auto* byClass = pe::FindFunction(towerperf::ClassNamed("Actor"), "GetComponentByClass");
        auto* cur = compCls ? pe::FindFunction(compCls, "GetCurrnetFaction") : nullptr;
        if (!pawn || !ue::Live(pawn) || !byClass || !cur) return 0;
        const int arg = ue::PropOffset(byClass, "ComponentClass"), ret = ue::PropOffset(byClass, "ReturnValue");
        const int size = ue::Get<int>(byClass + ue::Layout::StructSize);
        if (arg < 0 || ret < 0 || size <= 0 || size > 64 || arg + 8 > size || ret + 8 > size) return 0;
        uint8_t a[64]{}; std::memcpy(a + arg, &compCls, 8);
        pe::Call(pawn, byClass, a);
        UObject* comp = *reinterpret_cast<UObject**>(a + ret);
        if (!comp || !ue::Live(comp)) return 0;
        const int r = ue::PropOffset(cur, "ReturnValue"), sz = ue::Get<int>(cur + ue::Layout::StructSize);
        if (r < 0 || sz <= 0 || sz > 64 || r + 8 > sz) return 0;
        uint8_t b[64]{};
        pe::Call(comp, cur, b);
        uint64_t tag = 0; std::memcpy(&tag, b + r, 8);
        if (tag) pbz::log::Info("影塔仆从召唤物：取到玩家阵营标签（来自玩家阵营组件）");
        return tag;
    }
    void DestroyServantSummons() {
        for (auto& [obj, index] : servantSummons_)
            if (fnDestroy_ && ue::Live(obj) && ue::Get<int>(obj + ue::Layout::ObjIndex, -1) == index) pe::Call(obj, fnDestroy_, nullptr);
        servantSummons_.clear(); servantFactionTries_.clear(); unadoptedLogged_.clear(); summonLockSince_.clear(); summonLockLogged_.clear();
    }
    // 用户 2026-11-12：这些场地不召唤仆从、也不扫描落点（避免卡顿），玩家独自作战：
    // ① 舞狮、左殇的整层（场地机制战）；② 场地表里已记录落地失败的场地（kLandingBad）；③ 本机以前召唤时落不了地的场地（自动记录）
    std::string ServantSkipReason() {
        for (int b : run_.bosses) {
            const auto id = tower::kBossPool[b].identity;
            // 3.7.0：无尽 / 特殊层里直接生成的真龙之躯不在原生场地，不算场地机制战
            if (id == "LionDancer" || (id == "ZuoShang" && !EndlessOn())) return "场地机制战：" + std::string(tower::kBossPool[b].name);
        }
        if (venueBoss_ >= 0 && !tower::floorplay::LandingBad(venueBoss_).empty()) return "场地已知落地问题：" + std::string(tower::kBossPool[venueBoss_].name) + " 的场地";
        LoadServantBadVenues();
        if (venueBoss_ >= 0 && servantBadVenues_.count(std::string(tower::kBossPool[venueBoss_].path))) return "本机此前在该场地召唤落地失败：" + std::string(tower::kBossPool[venueBoss_].name) + " 的场地";
        return {};
    }
    std::unordered_set<std::string> servantBadVenues_;
    bool servantBadLoaded_ = false;
    static std::wstring ServantVenueFile() { auto f = RunFile(); return f.empty() ? f : f.substr(0, f.rfind(L'\\')) + L"\\servant-venue-bad.txt"; }
    void LoadServantBadVenues() {
        if (servantBadLoaded_) return;
        servantBadLoaded_ = true;
        const auto f = ServantVenueFile();
        if (f.empty()) return;
        std::ifstream in(f);
        for (std::string line; std::getline(in, line);) if (const auto tab = line.find('\t'); tab != std::string::npos) servantBadVenues_.insert(line.substr(0, tab)); else if (!line.empty()) servantBadVenues_.insert(line);
    }
    void RememberServantBadVenue(const char* why) {
        if (venueBoss_ < 0) return;
        LoadServantBadVenues();
        const std::string path(tower::kBossPool[venueBoss_].path);
        if (!servantBadVenues_.insert(path).second) return;
        if (const auto f = ServantVenueFile(); !f.empty()) { std::ofstream out(f, std::ios::app); out << path << '\t' << why << '\n'; }
        pbz::log::Info("影塔仆从：记下 {} 的场地不能召唤仆从（{}），以后该场地直接跳过", tower::kBossPool[venueBoss_].name, why);
    }
    // 冻结：抑制能力激活 + 停止 AI 移动（同入场冻结的做法）；解冻时恢复原状态，离玩家超过 40 米的拉回身边
    void FreezeServant(Servant& a, bool on, const Target& player, int slot) {
        if (a.frozen == on) return;
        const int ascOff = ue::PropOffset(ue::ClassOf(a.obj), "AbilitySystemComponent");
        UObject* asc = ascOff >= 0 ? ue::Get<UObject*>(a.obj + ascOff) : nullptr;
        auto* getI = asc && ue::Live(asc) ? pe::FindFunction(ue::ClassOf(asc), "GetUserAbilityActivationInhibited") : nullptr;
        auto* setI = asc && ue::Live(asc) ? pe::FindFunction(ue::ClassOf(asc), "SetUserAbilityActivationInhibited") : nullptr;
        if (!getI || !setI) { a.frozen = on; return; }
        if (on) { uint8_t g[16]{}; pe::Call(asc, getI, g); a.wasInhibited = g[0] != 0; }
        { std::vector<uint8_t> b(0x20, 0); *Arg(setI, b, "NewInhibit") = on ? 1 : (a.wasInhibited ? 1 : 0); pe::Call(asc, setI, b.data()); }
        if (on) {
            const int cOff = ue::PropOffset(ue::ClassOf(a.obj), "Controller");
            UObject* ctl = cOff >= 0 ? ue::Get<UObject*>(a.obj + cOff) : nullptr;
            if (ctl && ue::Live(ctl)) if (auto* stop = pe::FindFunction(ue::ClassOf(ctl), "StopMovement")) pe::Call(ctl, stop, nullptr);
        } else if (fnLoc_) {
            double pp[4]{}, sp[4]{};
            pe::Call(player.obj, fnLoc_, pp); pe::Call(a.obj, fnLoc_, sp);
            if (std::hypot(sp[0] - pp[0], sp[1] - pp[1]) > 4000 || std::abs(sp[2] - pp[2]) > 1500) {
                if (auto* set = pe::FindFunction(ue::ClassOf(a.obj), "K2_SetActorLocation")) {
                    const double ang = 2.4 + 1.2 * slot;  // 玩家身侧错开
                    const double want[3]{pp[0] + 300 * std::cos(ang), pp[1] + 300 * std::sin(ang), pp[2] + 50};
                    std::vector<uint8_t> b(0x200, 0);
                    std::memcpy(Arg(set, b, "NewLocation"), want, sizeof want);
                    *Arg(set, b, "bSweep") = 1; *Arg(set, b, "bTeleport") = 1;
                    pe::Call(a.obj, set, b.data());
                    pbz::log::Info("影塔仆从：{} 过场后离玩家太远，拉回身边（{}）", tower::kBossPool[a.boss].name, *Arg(set, b, "ReturnValue") ? "成功" : "被地形挡住");
                }
            }
        }
        a.frozen = on;
    }
    // 用户 2026-11-12（阿瑾层）：层载入后、读秒结束前 Boss 可能已经出手。开打前玩家无敌：受伤倍率全部 ×0（同复活起身无敌）。
    // 只在战斗阶段且尚未开打（fightSince_ <= 0）时生效，开打即放开；不动修改器的无敌开关
    bool entryGuard_ = false;
    void EntryGuard(const Target& p) {
        const bool endlessGuard = run_.phase == tower::Phase::Fighting && fightSince_ > 0 &&
            (now_ < endlessGuardUntil_ || now_ < motianHoldUntil_ || (EndlessOn() && storyLocked_ && storySince_ > 0 && now_ - storySince_ < 30.0));
        const bool on = ((run_.phase == tower::Phase::Fighting && fightSince_ <= 0) || endlessGuard || CapGuardOn()) && deathAt_ < 0 && !taken_.empty();
        if (on != entryGuard_) { entryGuard_ = on; pbz::log::Info("影塔入场保护：{}", on ? "开（开打前无敌）" : "关（已开打）"); }
        if (on) for (int o : taken_) attr::Want(p, attr::AttrCurrent(p, o), Kind::Scale, 0.0f);
    }
    // 撤下本局全部仆从（换层 / 结束 / 海克斯被移除）；释放自己持有的右侧看板
    size_t DismissServants(const char* why) {
        auto& s = servants_;
        const bool had = !s.actors.empty() || !s.pending.empty() || s.hudHeld;
        DestroyServantSummons();
        const size_t left = pbz::enemies::ClearTagged(kServantPrefix) + pbz::enemies::ClearTagged(kServantTowerPrefix);
        if (s.hudHeld) if (auto* hud = pbz::FindFeature("ui.faction_bars")) pbz::ReleaseFeature(hud, "tower-servant");
        if (had || left) pbz::log::Info("影塔仆从：撤下（{}），{} 个未能销毁", why, left);
        s = {};
        return left;
    }
    std::vector<int> ServantOrder(int shown) {
        return tower::servants::Order(run_.seed, shown, [this](int n) {
            const auto& b = tower::kBossPool[n];
            if (!Admitted(n) || badBosses_.count(n)) return false;
            if (SpecialOn() && special_.kind == tower::special::Kind::Survival && tower::special::SurvivalAllyBanned(b.identity)) return false;  // 3.7.0 生存战：打不死的不当友军
            return std::none_of(run_.bosses.begin(), run_.bosses.end(), [&](int enemy) {
                return tower::kBossPool[enemy].identity == b.identity;
            });
        });
    }
    // 4.0.0 卡顿优化：每只 Boss / 仆从生成时核心同步载入蓝图类，约 100–170 ms 卡一帧（实测"上下文/载入 109–141 ms"）。
    // 到场后生成前（等就绪的那几秒）先异步预载本层要用的类：本层 Boss、无尽各波 Boss 与小怪、仆从 / 助战前几个候选；
    // 载完后生成时只剩创建本身。预载失败 / 接口不可用时照旧同步载入，不影响流程
    void PreloadFloorClasses() {
        int n = 0;
        auto boss = [&](int b) { if (b >= 0 && b < kPool) { towernative::PreloadClass(std::string(tower::kBossPool[b].path)); ++n; } };
        for (int b : run_.bosses) boss(b);
        if (EndlessOn()) for (const auto& w : endless_) {
            for (int b : w.bosses) boss(b);
            for (int m : w.mobs) { towernative::PreloadClass(std::string(tower::mobs::kMobs[m].path)); ++n; }
        }
        const bool master = grow_.on && grow_.hexes.Has(tower::hex::ServantMaster);
        const int shown = ShownNow() + (run_.phase == tower::Phase::Fighting ? 0 : 1);
        const int assist = SpecialOn() ? SpecialAssist().count : run_.endless ? tower::servants::AssistFor(run_.seed, shown).count : 0;
        const int slots = (master ? grow_.hexes.PickCount(tower::hex::ServantMaster) : 0) + assist;
        if (slots > 0) {
            const auto order = ServantOrder(shown);
            std::unordered_set<std::string_view> ids;
            for (size_t i = 0; i < order.size() && static_cast<int>(ids.size()) < slots + 1; ++i)
                if (ids.insert(tower::kBossPool[order[i]].identity).second) boss(order[i]);
        }
        if (n) pbz::log::Info("影塔预载：第 {} 层生成前异步预载 {} 个角色类", run_.floor, n);
    }
    // 无尽下一波：本波只剩最后一只时预载（离换波最近，免得预载完又被引擎回收）
    void PreloadNextWave() {
        if (!EndlessOn() || endlessWave_ + 1 >= static_cast<int>(endless_.size()) || preloadedWave_ == endlessWave_ + 1) return;
        preloadedWave_ = endlessWave_ + 1;
        const auto& w = endless_[static_cast<size_t>(preloadedWave_)];
        for (int b : w.bosses) towernative::PreloadClass(std::string(tower::kBossPool[b].path));
        for (int m : w.mobs) towernative::PreloadClass(std::string(tower::mobs::kMobs[m].path));
        pbz::log::Info("影塔预载：第 {} 层第 {} 波异步预载 {} 个角色类", run_.floor, preloadedWave_ + 1, w.bosses.size() + w.mobs.size());
    }
    int preloadedWave_ = -1;
    std::unordered_map<UObject*, const Target*> byObj_;
    void TickServants(const Target* player, const std::unordered_set<UObject*>& owned, bool paused) {
        auto& s = servants_;
        const bool master = grow_.on && grow_.hexes.Has(tower::hex::ServantMaster);
        const bool assistFloor = grow_.on && ((run_.endless && tower::servants::AssistFor(run_.seed, ShownNow()).count > 0) || SpecialAssist().count > 0);
        if (!master && !assistFloor) {
            if (!s.actors.empty() || !s.pending.empty() || s.hudHeld) DismissServants("未持有仆从大师、本层也没有助战");
            return;
        }
        if (!player || !ServantStatsReady() || !towersb::Active()) return;
        if (s.shown && s.shown != ShownNow()) DismissServants("显示层已变化");
        // 用户 2026-11-12 舞狮层：舞台原生表演序列一直算「过场」，开打 2 分钟仆从都没召唤。敌方 Boss 已经掉血 = 真在打，过场标记不再拦
        const bool bossHurt = std::any_of(actors_.begin(), actors_.end(), [this](const Actor& a) {
            if (a.dead || a.gone || !a.hasTarget || !attr::Alive(a.target)) return false;
            const float hp = ue::Get<float>(attr::AttrCurrent(a.target, hp_), -1.f), mx = ue::Get<float>(attr::AttrCurrent(a.target, hpMax_), -1.f);
            return std::isfinite(hp) && std::isfinite(mx) && mx > 0 && hp >= 0 && hp < mx * 0.995f; });
        const bool cinematic = towerstorygate::Cinematic() && !bossHurt;
        const bool activeFight = !paused && deathAt_ < 0 && fightSince_ > 0 && !towerentry::Holding() && !cinematic &&
            std::any_of(actors_.begin(), actors_.end(), [](const Actor& a) { return !a.dead && !a.gone; });
        if (!s.shown && !activeFight) return;
        std::array<float, 4> full{};
        if (!ServantStats(*player, full, 1.0f)) return;  // 玩家 100%，每个友军按自己的比例乘
        const std::array offsets{hpMax_, attack_, shachiAttack_, shachiMax_};
        if (!s.shown) {
            s.shown = ShownNow();
            s.masterSlots = master ? grow_.hexes.PickCount(tower::hex::ServantMaster) : 0;
            s.assist = SpecialOn() ? SpecialAssist() : run_.endless ? tower::servants::AssistFor(run_.seed, s.shown) : tower::servants::Assist{};  // 3.7.0 创意层助战
            s.slots = s.masterSlots + s.assist.count;
            if (s.assist.count) pbz::log::Info("影塔助战：第 {} 层有 {} 个 Boss 助战（玩家 {:.0f}% 属性，不占仆从大师名额）", s.shown, s.assist.count, s.assist.share * 100);
            s.order = ServantOrder(s.shown);
            pbz::log::Info("影塔仆从：第 {} 层，{} 个召唤位，{} 个可抽条目", s.shown, s.slots, s.order.size());
            if (const auto why = ServantSkipReason(); !why.empty()) {
                s.resolved = s.slots;  // 不召唤、不做任何落点扫描（避免卡顿）
                pbz::log::Info("影塔仆从：第 {} 层不召唤仆从（{}），玩家独自作战", s.shown, why);
            }
        }
        // 异步回执看门狗：20 秒没回就作废（清掉可能的残留），换下一个候选
        if (!s.pending.empty() && now_ - s.pendingSince > 20.0) {
            const size_t left = pbz::enemies::ClearTagged(s.pending);
            pbz::log::Info("影塔仆从：第 {} 层，生成请求 {} 超时 20 秒，已作废（残留 {}）", s.shown, s.pending, left);
            s.pending.clear(); s.nextAt = now_ + 1.0;
            if (left) s.resolved = s.slots; else if (++s.attempts >= 3) { ++s.resolved; s.attempts = 0; }
        }
        for (auto it = s.actors.begin(); it != s.actors.end();) {
            auto& a = *it;
            const bool tracked = owned.count(a.obj) && ue::Live(a.obj) && ue::Get<int>(a.obj + ue::Layout::ObjIndex, -1) == a.index;
            if (!tracked) {  // 核心已回收（死亡演出后），或被修改器清怪清掉
                if (!a.dead) pbz::log::Info("影塔仆从：第 {} 层，{} 退场（对象已不在）", s.shown, tower::kBossPool[a.boss].name);
                it = s.actors.erase(it); continue;
            }
            const Target t = ServantTarget(a.obj);
            const float hp = t.set && attr::Alive(t) ? ue::Get<float>(attr::AttrCurrent(t, hp_), -1.f) : -1.f;
            const bool ready = std::isfinite(hp) && hp > 0.5f;
            if (!a.dead && ((a.initialized && hp >= 0 && hp <= 0.5f) || (!ready && now_ - a.lastSeenAt > 3.0))) {
                a.dead = true; a.deadAt = now_;  // 阵亡：停止同步属性，交给原生死亡演出与核心回收；当层不补
                pbz::log::Info("影塔仆从：第 {} 层，{} 退场（阵亡，当层不补）", s.shown, tower::kBossPool[a.boss].name);
            }
            if (a.dead) {
                if (now_ - a.deadAt > 20.0 && pbz::enemies::ClearTagged(a.tag) == 0) { it = s.actors.erase(it); continue; }  // 兜底：20 秒没被回收就清
                ++it; continue;
            }
            ++it;
            if (!ready) continue;
            a.lastSeenAt = now_;
            std::array<float, 4> stats{};
            for (size_t i = 0; i < stats.size(); ++i) stats[i] = full[i] * a.share;
            const float capBefore = ue::Get<float>(attr::AttrCurrent(t, hpMax_), -1.f);
            SyncServantPool(t, hp_, hpMax_, stats[0], !a.initialized, true);
            SyncServantPool(t, shachiNow_, shachiMax_, stats[3], !a.initialized, false);
            {  // 3.6.3（用户）：仆从 / 助战的血量本层延续，敌方转阶段、车轮换人时不能回满
                const float nowFrac = a.initialized && std::isfinite(capBefore) && capBefore > 0 ? std::clamp(hp / capBefore, 0.0f, 1.0f) : -1.0f;
                const float keep = tower::servants::KeepFrac(a.frac, nowFrac);
                if (keep >= 0 && nowFrac >= 0 && keep < nowFrac) {
                    const float back = std::max(1.0f, keep * stats[0]);
                    attr::Hold(t, attr::AttrBase(t, hp_), back);
                    attr::Hold(t, attr::AttrCurrent(t, hp_), back);
                    if (!a.refillLogged) pbz::log::Info("影塔仆从：{} 生命被回满（{:.0f}% → {:.0f}%），按本层延续还原", tower::kBossPool[a.boss].name, keep * 100, nowFrac * 100);
                    a.refillLogged = true;
                }
                a.frac = keep >= 0 ? keep : nowFrac;
            }
            // 3.7.0 生存战友军不额外掉血（用户：固定 30 秒死太套路），但受到伤害 ×20：靠挨打自然死，肉的 / 少挨打的多撑一会
            if (SpecialOn() && special_.kind == tower::special::Kind::Survival)
                for (int o : taken_) attr::Want(t, attr::AttrCurrent(t, o), Kind::Scale, tower::special::kSurvivalAllyTaken);
            for (const size_t i : {1u, 2u}) {
                attr::Want(t, attr::AttrBase(t, offsets[i]), Kind::Set, stats[i]);
                attr::Want(t, attr::AttrCurrent(t, offsets[i]), Kind::Set, stats[i]);
            }
            if (hpMin_ >= 0) {
                attr::Want(t, attr::AttrBase(t, hpMin_), Kind::Set, 0);
                attr::Want(t, attr::AttrCurrent(t, hpMin_), Kind::Set, 0);
            }
            if (!a.initialized || (now_ >= a.nextLog && stats != a.logged)) {
                pbz::log::Info("影塔仆从属性：{} 生命上限 {:.1f} 攻击 {:.1f} 破防 {:.1f} 杀气上限 {:.1f}",
                    tower::kBossPool[a.boss].name, stats[0], stats[1], stats[2], stats[3]);
                a.logged = stats; a.nextLog = now_ + 1.0;
            }
            a.initialized = true;
        }
        // 用户 2026-11-12：敌方转阶段 / 过场时冻结仆从（不撤、不重生，血量沿用），过场结束解冻；被过场带远的拉回玩家身边
        // 用户 2026-11-12：舞狮二阶段是主角站桩的柱上对抗，整个二阶段冻结仆从
        // 同类（2026-11-12 用户确认）：左殇两段的技能按类找原生终章战场锚点（SnapPoint / DomainCenter），整段冻结
        const bool lionPoles = !EndlessOn() && std::any_of(actors_.begin(), actors_.end(), [](const Actor& a) {  // 3.7.0：直接生成的真龙之躯不冻结仆从
            if (a.dead || a.gone || !ue::Live(a.obj)) return false;
            const auto cls = ue::ClassName(a.obj);
            return cls == "BP_Monster_LionDancer_P2_C" || cls == "BP_Monster_ZuoShang_Ending_C" || cls == "BP_Monster_ZuoShang_P2_C"; });
        const bool freeze = cinematic || towerentry::Holding() || lionPoles ||
            std::any_of(actors_.begin(), actors_.end(), [](const Actor& a) { return a.gone && !a.dead; });
        if (freeze != s.frozen) {
            pbz::log::Info("影塔仆从：{}（{}）", freeze ? "冻结" : "解冻",
                cinematic ? "过场" : towerentry::Holding() ? "入场冻结" : lionPoles ? "场地机制战（舞狮二阶段 / 左殇）" : freeze ? "敌方转阶段" : "恢复战斗");
            s.frozen = freeze;
        }
        int slot = 0;
        for (auto& a : s.actors) if (!a.dead && ue::Live(a.obj)) FreezeServant(a, freeze, *player, slot++);
        if (freeze) return;
        if (s.resolved < s.slots && s.pending.empty() && !activeFight && run_.phase == tower::Phase::Fighting && now_ >= s.waitLogAt) {
            s.waitLogAt = now_ + 10.0;
            pbz::log::Info("影塔仆从：第 {} 层还有 {} 个召唤位，暂不召唤（暂停 {}、未开打 {}、入场冻结 {}、过场 {}、敌方存活 {}）", s.shown, s.slots - s.resolved,
                paused, fightSince_ <= 0, towerentry::Holding(), cinematic,
                std::any_of(actors_.begin(), actors_.end(), [](const Actor& a) { return !a.dead && !a.gone; }));
        }
        if (!activeFight || s.resolved >= s.slots || now_ < s.nextAt || !s.pending.empty() || pending_ > 0) return;
        auto* hud = pbz::FindFeature("ui.faction_bars");
        if (!hud || !hud->enabled) {
            if (!hud || !pbz::HoldFeature(hud, "tower-servant")) { s.nextAt = now_ + 1.0; return; }
            s.hudHeld = true;
        }
        if (s.retryBoss >= 0) { const int boss = std::exchange(s.retryBoss, -1); SubmitServant(boss, true); return; }
        while (s.cursor < s.order.size()) {
            const int boss = s.order[s.cursor++];
            const auto id = tower::kBossPool[boss].identity;
            if (std::any_of(s.actors.begin(), s.actors.end(), [id](const Servant& a) { return tower::kBossPool[a.boss].identity == id; })) continue;
            if (s.noLevelGroup && !tower::servants::PlainPath(boss)) continue;  // 这张图上只能走影塔路径，需要阶段适配的跳过
            SubmitServant(boss, s.noLevelGroup);
            return;
        }
        if (s.roomTries > 0 && s.roomTries < 10) {  // 是因为没落点才轮完：从头再轮，3 秒后
            s.cursor = 0; s.nextAt = now_ + 3.0;
            pbz::log::Info("影塔仆从：第 {} 层，附近一直没有落点，3 秒后从头再试", s.shown);
            return;
        }
        pbz::log::Info("影塔仆从：第 {} 层，可用候选已用完", s.shown);
        s.resolved = s.slots;
    }
    void SubmitServant(int boss, bool towerPath = false) {
        auto& s = servants_;
        const auto& b = tower::kBossPool[boss];
        const std::string tag = std::format("{}{}-{}-{}-", towerPath ? kServantTowerPrefix : kServantPrefix, run_.id, s.shown, ++spawnSeq_);
        s.pending = tag; s.pendingSince = now_;
        const double distance = std::min(1900.0, 420.0 + 110.0 * s.resolved - 70.0 * s.attempts + 250.0 * (s.roomTries % 7));
        pbz::enemies::Submit({"spawn", std::string(b.path), tag, 1, distance, "ally"}, [this, boss, tag, towerPath](std::string json) {
            auto& floor = servants_;
            if (floor.pending != tag || run_.phase != tower::Phase::Fighting) { pbz::enemies::ClearTagged(tag); return; }
            floor.pending.clear();
            floor.nextAt = now_ + 0.2;
            const auto& pick = tower::kBossPool[boss];
            const std::string actual(pbz::enemies::StandalonePath(pick.path, tag));  // 独立阶段替换后的真实身份
            const auto receipt = tower::spawnreceipt::Read(json, tag, pick.path, actual);
            if (receipt.ok) for (const auto& [obj, faction] : pbz::enemies::OwnedFactions()) {
                if (faction != "Faction.Player" || ue::ObjName(obj) != receipt.name || !towerisolate::InPlayerWorld(obj)) continue;
                const int index = ue::Get<int>(obj + ue::Layout::ObjIndex, -1);
                if (index < 0) break;
                if (boundary_.on && fnLoc_) {
                    double point[4]{};
                    pe::Call(obj, fnLoc_, point);
                    if (!boundary_.Contains({point[0], point[1], point[2]}, 100)) break;
                }
                // 3.2.1：先补仆从大师的名额（80%），剩下的是 Boss 助战（20% / 30%）
                const int masters = static_cast<int>(std::count_if(floor.actors.begin(), floor.actors.end(), [](const Servant& x) { return !x.assist; }));
                const bool assist = masters >= floor.masterSlots && floor.assist.count > 0;
                // 3.7.0（用户）：生存战里仆从大师的仆从也只有 5%，很快被打死；其他层仍 80%
                const float share = assist ? floor.assist.share : SpecialOn() && special_.kind == tower::special::Kind::Survival ? tower::special::SurvivalAllyShare(ShownNow()) : tower::servants::kShare;
                const bool init = InitServantNow(obj, std::string(pick.name), share);
                Servant entry; entry.obj = obj; entry.index = index; entry.boss = boss; entry.tag = tag; entry.initialized = init; entry.lastSeenAt = now_;
                entry.share = share; entry.assist = assist;
                floor.actors.push_back(entry);
                ++floor.resolved; floor.attempts = 0; floor.roomTries = 0; floor.noGround = 0;
                pbz::log::Info("影塔仆从：第 {} 层，{} {} 已生成（{}/{}，玩家 {:.0f}%）{}{}", floor.shown, assist ? "助战" : "友军", pick.name, floor.resolved, floor.slots, share * 100,
                    actual != pick.path ? "，按独立阶段换为终阶段" : "", init ? "，属性已当帧写入" : "，属性下一帧写入");
                return;
            }
            const size_t left = pbz::enemies::ClearTagged(tag);
            pbz::log::Info("影塔仆从：第 {} 层，{} 生成或场内落点核验失败，残留 {}：{}",
                floor.shown, pick.name, left, json.substr(0, 360));
            // 用户 2026-11-12 第 21 层：场地没有原生定级组，修改器独立路径整批拒绝 → 本层改走影塔路径，不计失败次数
            if (!left && !towerPath && json.find("原生定级组") != std::string::npos) {
                if (!floor.noLevelGroup) pbz::log::Info("影塔仆从：第 {} 层场地没有原生定级组，改用影塔生成路径（只召唤不需要阶段适配的 Boss）", floor.shown);
                floor.noLevelGroup = true;
                if (tower::servants::PlainPath(boss)) floor.retryBoss = boss;
                floor.nextAt = now_;
                return;
            }
            // 用户 2026-11-12 舞狮台：「附近没有足够可落地的位置」是场地问题不是候选问题 → 不占召唤位，往外推距离、换候选再试
            // 用户 2026-11-12：每次生成尝试会卡一帧约 100 ms，0.5 秒重试导致持续卡顿 → 1.5 秒一次、最多 10 次；
            // 整轮候选点全是「无地面」（舞狮原生场地已知问题，核心地面检测失败）试 2 次就放弃本层
            if (!left && json.find("没有足够可落地") != std::string::npos) {
                ++floor.roomTries; floor.attempts = 0;
                floor.nextAt = now_ + 1.5;
                const auto at = json.find("无地面");
                const int ground = at != std::string::npos ? std::atoi(json.c_str() + at + std::string("无地面").size()) : 0;
                if (ground >= 20) ++floor.noGround;
                if (floor.noGround >= 2 || floor.roomTries >= 10) {
                    RememberServantBadVenue(floor.noGround >= 2 ? "地面检测全部失败" : "附近 10 次没有落点");
                    pbz::log::Info("影塔仆从：第 {} 层场地{}，本层不再召唤仆从（避免反复尝试卡顿）", floor.shown,
                        floor.noGround >= 2 ? "地面检测全部失败（与舞狮原生场地的已知落地问题一致）" : "附近 10 次都没有落点");
                    floor.resolved = floor.slots;
                }
                return;
            }
            if (left) floor.resolved = floor.slots;
            else if (++floor.attempts >= 3) { ++floor.resolved; floor.attempts = 0; }
        });
    }
    // ---- 新奖励模型：进度表、驿站、准备道具
    // 装备准入：内置 Codex 进度表合同（构建时生成，待核 / 排除项禁入）。%LOCALAPPDATA%\PBZTower\progression.txt 只作开发覆盖，存在时日志明确标出
    tower::rewards::Catalog LoadCatalog() {
        int unknown = 0;
        auto cat = tower::rewards::Contract(&unknown);
        int open = 0;
        for (auto& g : cat.gates) open += g.zone >= 0 && (g.reward || g.shop);
        pbz::log::Info("影塔奖励：装备进度表合同 {} 项（sha256 {:.12}），开放 {} 项，其余待核禁入{}", tower::rewards::kProgressionItems,
                       tower::rewards::kProgressionSha256, open, unknown ? std::format("；{} 项不在影塔成长表里，未使用", unknown) : "");
#ifdef PBZ_RELEASE
        return cat;  // 3.2.0：正式版不读本机开发覆盖表，奖励 / 驿站准入只看随包合同（同版本同种子结果一致）
#endif
        wchar_t base[MAX_PATH];
        if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return cat;
        std::ifstream f(std::wstring(base) + L"\\PBZTower\\progression.txt", std::ios::binary);
        if (!f) return cat;
        std::stringstream ss; ss << f.rdbuf();
        int bad = 0;
        cat = tower::rewards::ParseCatalog(ss.str(), cat, &bad);
        pbz::log::Info("影塔奖励：注意——读到开发覆盖表 progression.txt（{} 行坏数据），本局准入不等于合同", bad);
        return cat;
    }
    void FreshRewards() {
        grow_.bless = {}; grow_.offers = {}; grow_.offered = false; grow_.seen.clear(); grow_.rewardRng = 0;
        grow_.shop = {}; grow_.prep = {}; grow_.quick = {};
        grow_.catalog = LoadCatalog();
    }
public:
    // ---- 3.1.0 驿站【交易】（用户 2026-11-13）：回收 / 以旧换新 / 熔铸，规则见 tower_trade.h。
    // 顺序：校验 → 原生删兵器（读回不再持有）→ 以旧换新原生发新兵器 → 改成长档 / 魂玉 / 货架 → 存档。原生任一步失败就把已删的补发回去（按原等级），提示未成交
    std::vector<int> TradeEquipped() const {
        std::vector<int> out;
        const auto l = towernative::ReadLoadout();
        if (l.ok) {
            for (const auto& id : l.main) if (const int w = tower::growth::WeaponIndex(id, 0); w >= 0) out.push_back(w);
            for (const auto& id : l.shaqi) if (const int w = tower::growth::WeaponIndex(id, 1); w >= 0) out.push_back(w);
        }
        for (int kind : {0, 1}) for (int w : const_cast<Tower*>(this)->ArsenalExtras(kind)) if (w >= 0) out.push_back(w);
        return out;
    }
    // 回收按本层兵器实际售价（武库扩建七折后）的 80%，避免折扣买进、原价回收套利
    int TradeWeaponPrice() const { const int p = tower::balance::Price(tower::balance::Buy::Weapon, ShownNow(), 0); return grow_.on && grow_.hexes.Has(tower::hex::Arsenal) ? std::max(1, p * tower::hex::kArsenalWeaponPricePct / 100) : p; }
    tower::trade::Prices TradePrices() const {
        namespace b = tower::balance;
        return {TradeWeaponPrice(), b::Price(b::Buy::Medicine, ShownNow(), 0), b::Price(b::Buy::Accessory, ShownNow(), 0)};
    }
    // 原生删一把；失败返回 false。removed 记下已删的（等级、铸魂）以便回滚
    struct Removed { int w; int level; bool snapped; };
    bool TradeRemoveNative(int w, std::vector<Removed>& removed) {
        const auto& d = tower::growth::kWeapons[w];
        const int s = grow_.profile.Find(w);
        const Removed r{w, s >= 0 ? grow_.profile.weapons[s].level : 1, s >= 0 && grow_.profile.weapons[s].snapped};
        if (!Game().RemoveWeapon(d.id, d.kind)) return false;
        removed.push_back(r);
        // 3.1.0 N7：正在装备的也能交出——武库扩建第 3、4 格里的清掉；原生装备栏读回记日志（实机核对删掉手持兵器后的表现）
        for (auto& x : grow_.arsenal) if (x == w) x = -1;
        armMain_ = {}; armShaqi_ = {};
        if (const auto l = towernative::ReadLoadout(); l.ok) {
            const bool still = std::find(l.main.begin(), l.main.end(), std::string(d.id)) != l.main.end() || std::find(l.shaqi.begin(), l.shaqi.end(), std::string(d.id)) != l.shaqi.end();
            pbz::log::Info("影塔交易：删除后原生装备栏 主 {} 把 / 影 {} 把，{}", l.main.size(), l.shaqi.size(), still ? "装备栏仍列着这把（待实机看表现）" : "已不在装备栏");
        }
        return true;
    }
    void TradeRollback(const std::vector<Removed>& removed) {
        for (const auto& r : removed) {
            const auto& d = tower::growth::kWeapons[r.w];
            const bool back = Game().GrantWeapon(d.id, d.kind) && Game().SetWeaponLevel(d.id, d.kind, r.level);
            if (r.snapped) Game().SnapWeapon(d.id, d.kind);
            pbz::log::Info("影塔交易：回滚补发 {} Lv{} → {}", d.name, r.level, back ? "已补回" : "补回失败，请重进塔档核对");
        }
    }
    void Trade(const std::string& op, int a, int b, int target, uint32_t ver, std::vector<int> list = {}) {
        namespace td = tower::trade;
        auto& s = grow_.shop;
        auto fail = [this](const std::string& why) { tradeNote_ = "未成交：" + why; pbz::log::Info("影塔交易：{}", tradeNote_); Broadcast("err.trade", why); };
        tradeNote_.clear();
        if (!grow_.on || !s.Open() || run_.phase != tower::Phase::Ready) return fail("现在没有开店");
        if (ver != s.version) return fail("货架已变化，请重新打开交易");
        const td::Op kind = op == "recycle" ? td::Recycle : op == "exchange" ? td::Exchange : op == "melt" ? td::Melt : td::OpCount;
        if (kind == td::OpCount) return fail("未知交易");
        if (grow_.tradeVisit.Used(s.floor, kind)) return fail(std::format("本次驿站已经{}过了", td::kOpName[kind]));
        auto& p = grow_.profile;
        p.temperMaster = grow_.hexes.Has(tower::hex::TemperMaster);
        const auto equipped = TradeEquipped();
        auto give = [&](int w) { return td::GiveBlocked(p, w, equipped); };
        std::vector<Removed> removed;
        const auto before = p; const int jadeBefore = run_.jade; const auto visitBefore = grow_.tradeVisit; const auto shopBefore = s;
        std::string note;
        (void)give;
        if (kind == td::Recycle) {
            if (list.empty()) list.push_back(a);
            if (std::any_of(list.begin(), list.end(), [](int c) { return !td::IsWeapon(c); })) return fail("只能回收兵器");  // N9k：药 / 饰品不回收
            if (const auto why = td::RecycleMixBlocked(p, list, equipped); !why.empty()) return fail("不能回收：" + why);
            std::vector<int> gone;  // 已删的药 / 饰品（失败时补回）
            auto undo = [&] { TradeRollback(removed); for (int c : gone) { if (c >= td::kAccCode) Game().GrantAccessory(tower::growth::kAccessories[c - td::kAccCode].id); else Game().GrantMedicine(tower::growth::kMedicines[c - td::kMedCode].id); } };
            for (int c : list) {
                bool ok;
                if (td::IsWeapon(c)) ok = TradeRemoveNative(c, removed);
                else { ok = c >= td::kAccCode ? Game().RemoveAccessory(tower::growth::kAccessories[c - td::kAccCode].id) : Game().RemoveMedicine(tower::growth::kMedicines[c - td::kMedCode].id); if (ok) gone.push_back(c); }
                if (!ok) { undo(); return fail(std::format("原生删除{}失败，未成交", td::CodeName(c))); }
            }
            const auto g = td::ApplyRecycleMix(p, list, TradePrices());
            run_.jade += g.jade;
            for (int c : list) {  // N9i：从下一次驿站起进随机池
                if (std::find(grow_.recycled.begin(), grow_.recycled.end(), c) == grow_.recycled.end()) grow_.recycled.push_back(c);
                if (std::find(grow_.pendingRecycle.begin(), grow_.pendingRecycle.end(), c) == grow_.pendingRecycle.end()) grow_.pendingRecycle.push_back(c);
            }
            note = std::format("回收 {} 件：淬炼点 +{}，魂玉 +{}{}", list.size(), g.temper, g.jade, g.soul ? std::format("，铸魂令 +{}", g.soul) : "");
        } else if (kind == td::Exchange) {
            if (a == b) return fail("要交出两把不同的兵器");
            if (const auto why = td::GiveManyBlocked(p, {a, b}, equipped); !why.empty()) return fail("不能交出 " + why);
            if (a >= 0 && b >= 0 && tower::growth::kWeapons[a].kind == tower::growth::kWeapons[b].kind && td::KindCount(p, tower::growth::kWeapons[a].kind) <= 2 &&
                (target < 0 || target >= static_cast<int>(s.shelf.size()) || tower::growth::kWeapons[s.shelf[target].offer.index].kind != tower::growth::kWeapons[a].kind))
                return fail("交出后这一类兵器一把不剩");
            if (target < 0 || target >= static_cast<int>(s.shelf.size()) || s.shelf[target].sold || s.shelf[target].offer.kind != tower::rewards::Kind::Weapon) return fail("货架上没有这件兵器");
            const int want = s.shelf[target].offer.index;
            if (p.Find(want) >= 0 || p.Banned(want)) return fail("已经有这把兵器");
            if (!TradeRemoveNative(a, removed) || !TradeRemoveNative(b, removed)) { TradeRollback(removed); return fail("原生删兵器失败，未成交"); }
            const auto& d = tower::growth::kWeapons[want];
            if (!Game().GrantWeapon(d.id, d.kind)) { TradeRollback(removed); return fail("原生发放新兵器失败，未成交"); }
            const auto g = td::ApplyExchange(p, a, b, want);
            s.shelf[target].sold = true; s.bought.push_back(s.shelf[target].offer); ++s.version;
            note = std::format("以旧换新：{} + {} → {}，淬炼点 +{}", tower::growth::kWeapons[a].name, tower::growth::kWeapons[b].name, d.name, g.temper);
        } else {
            // a = 被强化的兵器，b / list = 牺牲品（N8 可多选）
            if (list.empty()) list.push_back(b);
            if (const auto why = td::MeltManyBlocked(p, a, list, equipped); !why.empty()) return fail(why);
            for (int w : list) if (!TradeRemoveNative(w, removed)) { TradeRollback(removed); return fail("原生删兵器失败，未成交"); }
            const auto g = td::ApplyMeltMany(p, a, list);
            note = std::format("熔铸：{} 把熔入 {}，武器获得伤害 / 破防加成 +{:.0f}%（共 {} 次），淬炼点 +{}{}", list.size(), tower::growth::kWeapons[a].name,
                               (td::MeltMul(p.MeltCount(a)) - 1) * 100, p.MeltCount(a), g.temper, g.soul ? std::format("，铸魂令 +{}", g.soul) : "");
            meltDirty_ = true;
        }
        Game().FixCurrentSlots();  // 3.3.2：交出的是正在装备的兵器时，手持切到还在的那把
        grow_.tradeVisit.Mark(s.floor, kind);
        ++s.version;
        if (!SaveHex() && grow_.on) {  // 成长档写不进去：原生已改，不能再回滚成长档（会和原生不一致），只提示
            pbz::log::Info("影塔交易：成长档保存失败（原生已成交）");
        }
        SaveRun();
        (void)before; (void)jadeBefore; (void)visitBefore; (void)shopBefore;
        pbz::log::Info("影塔交易：{}；剩魂玉 {}", note, run_.jade);
        tradeNote_ = note;
        Broadcast("info.trade", note);
    }
    std::string tradeNote_;  // 最近一次交易的结果（界面读）
private:
    // 熔铸加成落到原生：被强化兵器的配置对象（PBZ_WeaponConfigBase）上 QualityMultiplier / ShachiQualityMultiplier ×(1 + 15% × 次数)。
    // 配置是共享数据资产：记下原值，出塔 / 本局结束 / 模块停用时还原。每秒检查一次（配置随武器加载才出现）
    std::unordered_map<UObject*, std::pair<float, float>> meltOrig_;
    std::unordered_map<std::string, UObject*> meltCfg_;
    double meltNext_ = 0, meltSearch_ = 0; bool meltDirty_ = true, meltLogged_ = false;
    std::unordered_map<UObject*, float> meltMul_;  // N9h：熔铸兵器的配置对象 → 强度倍率（武器强度钩子读）
    UObject* MeltConfig(int w) {
        const auto& d = tower::growth::kWeapons[w];
        std::string_view name;
        for (const auto& e : tower::weaponcfg::kConfigs) if (e.weapon == d.id && e.kind == d.kind) name = e.config;
        if (name.empty()) return nullptr;
        const std::string key(name);
        if (auto it = meltCfg_.find(key); it != meltCfg_.end() && it->second && ue::Live(it->second) && ue::ObjName(it->second) == key) return it->second;
        auto* base = towerperf::ClassNamed("PBZ_WeaponConfigBase");
        UObject* found = nullptr;
        if (base) ue::ForEach([&](int, UObject* o) { if (!ue::IsDefault(o) && ue::ObjName(o) == key && ue::IsA(o, base)) { found = o; return false; } return true; });
        meltCfg_[key] = found;
        return found;
    }
    void MeltRestore() {
        auto* base = towerperf::ClassNamed("PBZ_WeaponConfigBase");
        const int q = base ? ue::PropOffset(base, "QualityMultiplier") : -1, sq = base ? ue::PropOffset(base, "ShachiQualityMultiplier") : -1;
        for (auto& [o, v] : meltOrig_) if (o && ue::Live(o) && q >= 0 && sq >= 0) { ue::Write(o + q, &v.first, 4); ue::Write(o + sq, &v.second, 4); }
        meltOrig_.clear(); meltCfg_.clear(); meltDirty_ = true;
    }
    // N9h：一把兵器可能有普通 / 铸魂（_Evolution）两份配置，两份都算
    static std::vector<std::string> MeltNames(int w) {
        const auto& d = tower::growth::kWeapons[w];
        std::vector<std::string> out;
        for (const auto& e : tower::weaponcfg::kConfigs) if (e.weapon == d.id && e.kind == d.kind) {
            std::string n(e.config);
            constexpr std::string_view ev = "_Evolution";
            if (n.size() > ev.size() && n.ends_with(ev)) { out.push_back(n); out.push_back(n.substr(0, n.size() - ev.size())); }
            else { out.push_back(n); out.push_back(n + std::string(ev)); }
        }
        return out;
    }
    // ---- 3.3.4 暗源海克斯 ----
    // 药引暗源：战斗中药瓶剩余次数下降 = 真的喝了药（空按不减），每口 +1 暗源。回归基本功会把次数清零（不是喝药），持有时不算
    // 暗源充盈：本层击败数增加时，每个按（局种子, 层, 第几个）定好的 25% 回复 1 暗源
    // 3.4.0 实机：暗源是玩家属性 SpiritShadow（上限 SpiritShadow_Max，修行点 / 饰品加的上限都在里面），
    // 不是背包的影之武装弹药接口（GetCurrentShaqiWeaponBulletNum 恒为 0）。加暗源 = 当前值 +n，最多加到原生上限
    int sparkMedLast_ = -1, sparkKillSeen_ = 0, sparkFloor_ = -1; double sparkMedAt_ = 0, sparkFight_ = -1;
    void SparkAdd(const Target& p, int n, const char* why) {
        static int logs = 0;
        const bool log = ++logs <= 40;
        if (spirit_ < 0 || spiritMax_ < 0) { if (log) pbz::log::Info("影塔暗源：{} +{} 未发放（暗源属性不可用）", why, n); return; }
        const float cur = ue::Get<float>(attr::AttrCurrent(p, spirit_), -1.0f), mx = ue::Get<float>(attr::AttrCurrent(p, spiritMax_), -1.0f);
        if (cur < 0 || mx <= 0) { if (log) pbz::log::Info("影塔暗源：{} +{} 未发放（读回 {} / {}）", why, n, cur, mx); return; }
        const float next = std::min(mx, std::floor(cur + 0.5f) + static_cast<float>(n));
        if (next <= cur + 0.01f) { if (log) pbz::log::Info("影塔暗源：{} 已满（{:.0f} / 上限 {:.0f}），不加", why, cur, mx); return; }
        // 3.4.0 实机：直接写属性值界面和原生逻辑都不认（用户：喝药暗源没涨）→ 走原生效果 GE_Talent_RestoreSpiritShadow（瞬时 +1），每点施加一次
        const int want = static_cast<int>(std::lround(next - std::floor(cur + 0.5f)));
        const int done = towerentry::AddSpiritShadow(want);
        if (done < 0) { if (log) pbz::log::Info("影塔暗源：{} 原生回复暗源效果不可用", why); return; }
        if (log) pbz::log::Info("影塔暗源：{} 暗源 {:.0f} → 目标 {:.0f}（上限 {:.0f}，原生效果 ×{}），读回 {:.0f}", why, cur, next, mx, done, ue::Get<float>(attr::AttrCurrent(p, spirit_), -1.0f));
    }
    void SparkTick(const Target& p) {
        if (!grow_.on || run_.phase != tower::Phase::Fighting || fightSince_ <= 0 || towerentry::Holding()) { sparkMedLast_ = -1; return; }
        auto& hx = grow_.hexes;
        if (!hx.Has(tower::hex::MedSpark) && !hx.Has(tower::hex::SparkFlow)) return;
        if (sparkFight_ != fightSince_) {
            sparkFight_ = fightSince_;
            if (spirit_ >= 0 && spiritMax_ >= 0) pbz::log::Info("影塔暗源：第 {} 层开打，暗源 {:.0f} / 上限 {:.0f}，药瓶剩余 {}", run_.floor,
                ue::Get<float>(attr::AttrCurrent(p, spirit_), -1.0f), ue::Get<float>(attr::AttrCurrent(p, spiritMax_), -1.0f), Game().MedicineUsesLeft());
        }
        if (hx.Has(tower::hex::MedSpark) && now_ >= sparkMedAt_) {
            sparkMedAt_ = now_ + 0.1;
            const bool zeroed = hx.Has(tower::hex::BackToBasics) && !hx.Has(tower::hex::DivineAid);
            const int uses = Game().MedicineUsesLeft();
            if (uses >= 0 && sparkMedLast_ >= 0 && uses != sparkMedLast_) pbz::log::Info("影塔暗源：药瓶剩余 {} → {}", sparkMedLast_, uses);
            if (uses >= 0 && sparkMedLast_ >= 0 && uses < sparkMedLast_ && !zeroed) {
                SparkAdd(p, (sparkMedLast_ - uses) * hx.MedSparkPoints(), "喝药");
            }
            sparkMedLast_ = uses;
        }
        if (sparkFloor_ != run_.floor || warFloor_ != run_.floor) { sparkFloor_ = run_.floor; sparkKillSeen_ = warFloor_ == run_.floor ? warKills_ : 0; }
        if (hx.Has(tower::hex::SparkFlow) && warKills_ > sparkKillSeen_) {
            int gain = 0;
            for (int k = sparkKillSeen_ + 1; k <= warKills_; ++k) {
                uint64_t x = (run_.seed ^ 0x5BA7C0DEull) + static_cast<uint64_t>(run_.floor) * 0x9E3779B97F4A7C15ull + static_cast<uint64_t>(k) * 0xD6E8FEB86659FD93ull;
                x ^= x >> 31; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 29;
                if (static_cast<int>(x % 100) < hx.SparkFlowPct()) ++gain;
            }
            if (gain > 0) SparkAdd(p, gain, "击败敌人");
        }
        sparkKillSeen_ = std::max(sparkKillSeen_, warKills_);
    }
  public:
    // 影刃专精：影之武装的强度按「攻击加成合计 + 100%」算 = 现有加成倍率 (1+X) → (1+X+1)，只乘在影之武装上，面板不变
    float ShadowEdgeMul() const {
        if (!grow_.on || !grow_.hexes.Has(tower::hex::ShadowEdge)) return 1.0f;
        const double b = std::max(0.1, atkBonusMul_);
        return static_cast<float>((b + grow_.hexes.ShadowEdgeBonus()) / b);
    }
    double atkBonusMul_ = 1.0;
  private:
    double orphanNext_ = 0;
    // 3.3.1（用户：旧版回收 / 熔铸后背包里的兵器没消失，有玩家借此刷了魂玉——给一次机会）：
    // 每局一次，在准备阶段完整读一遍原生背包，回收 / 熔铸过、背包里还在、塔档里没有的兵器认回塔档（按原生等级，熔铸掉的解除禁用），
    // 魂玉不追回。之后回收 / 熔铸会把背包里的那把一起删掉。读不到背包（一把塔档兵器都读不到）就下次再试，不记完成
    void OrphanWeaponTick() {
        if (grow_.backpackSynced || !grow_.on || trip_.phase() != tower::trip::Phase::Inside || run_.phase != tower::Phase::Ready || now_ < orphanNext_) return;
        orphanNext_ = now_ + 3.0;
        auto& p = grow_.profile;
        const bool readable = std::any_of(p.weapons.begin(), p.weapons.end(), [&](const tower::growth::OwnedWeapon& o) {
            const auto& d = tower::growth::kWeapons[o.index]; return Game().WeaponLevel(d.id, d.kind) >= 0; });
        if (!readable) return;
        const int cap = std::min(grow_.hexes.Has(tower::hex::Forge) ? 60 : tower::growth::kWeaponCap, 60);
        std::string names;
        // 只认回有塔内交易记录的：回收过（recycled）或熔铸掉（banned）的。不碰其它原生兵器（避免把塔外的兵器带进塔档）
        std::vector<int> traded(grow_.recycled.begin(), grow_.recycled.end());
        traded.insert(traded.end(), p.banned.begin(), p.banned.end());
        for (int i : traded) {
            if (i < 0 || i >= tower::growth::kWeaponCount || p.Find(i) >= 0) continue;
            const auto& d = tower::growth::kWeapons[i];
            const int native = Game().WeaponLevel(d.id, d.kind);
            if (native < 0) continue;
            std::erase(p.banned, i);
            if (!p.AddWeapon(i)) continue;
            p.weapons[p.Find(i)].level = std::clamp(native, 1, std::min(cap, d.maxLevel));
            std::erase(grow_.recycled, i); std::erase(grow_.pendingRecycle, i);
            names += (names.empty() ? "" : "、") + std::string(d.name);
        }
        grow_.backpackSynced = true;
        SaveHex(); SaveRun();
        pbz::log::Info("影塔交易：已按原生背包核对兵器，认回 {}", names.empty() ? std::string("无") : names);
    }
    void MeltTick() {
        OrphanWeaponTick();
        if (!grow_.on || trip_.phase() != tower::trip::Phase::Inside || grow_.profile.melt.empty()) { if (!meltOrig_.empty()) MeltRestore(); meltMul_.clear(); return; }
        if (!meltDirty_ && now_ < meltNext_) return;
        meltNext_ = now_ + 1.0; meltDirty_ = false;
        if (!meltOrig_.empty()) MeltRestore();  // 旧版写过的配置品质倍率先还原
        // 名字 → 倍率；缺对象的一次遍历找齐（最多 5 秒一次，配置随武器加载才出现）
        std::unordered_map<std::string, float> want;
        for (const auto& [w, n] : grow_.profile.melt) for (auto& nm : MeltNames(w)) want[nm] = static_cast<float>(tower::trade::MeltMul(n));
        bool missing = false;
        for (auto& [nm, m] : want) { auto it = meltCfg_.find(nm); if (it == meltCfg_.end() || !it->second || !ue::Live(it->second)) missing = true; }
        if (missing && now_ >= meltSearch_) {
            meltSearch_ = now_ + 5.0;
            if (auto* base = towerperf::ClassNamed("PBZ_WeaponConfigBase")) ue::ForEach([&](int, UObject* o) {
                if (!o || ue::IsDefault(o) || !ue::IsA(o, base)) return true;
                if (const auto nm = ue::ObjName(o); want.count(nm)) meltCfg_[nm] = o;
                return true;
            });
        }
        std::unordered_map<UObject*, float> next;
        for (auto& [nm, m] : want) if (auto it = meltCfg_.find(nm); it != meltCfg_.end() && it->second && ue::Live(it->second)) next[it->second] = m;
        if (next != meltMul_) {
            for (auto& [o, m] : next) pbz::log::Info("影塔熔铸：{} 强度 ×{:.2f}（原生武器强度接口{}）", ue::ObjName(o), m, towerweaponprobe::Supported() ? "" : "不可用，未生效");
            meltMul_ = std::move(next);
        }
    }
    void MeltTickOldQuality() {  // 旧做法（写配置品质倍率，实机面板 / 数值都不变），保留备查不再调用
        auto* base = towerperf::ClassNamed("PBZ_WeaponConfigBase");
        const int q = base ? ue::PropOffset(base, "QualityMultiplier") : -1, sq = base ? ue::PropOffset(base, "ShachiQualityMultiplier") : -1;
        if (q < 0 || sq < 0) return;
        for (const auto& [w, n] : grow_.profile.melt) {
            auto* cfg = MeltConfig(w);
            if (!cfg) continue;
            auto it = meltOrig_.find(cfg);
            if (it == meltOrig_.end()) it = meltOrig_.emplace(cfg, std::pair<float, float>{ue::Get<float>(cfg + q, 1.0f), ue::Get<float>(cfg + sq, 1.0f)}).first;
            const float mul = static_cast<float>(tower::trade::MeltMul(n));
            const float atk = it->second.first * mul, brk = it->second.second * mul;
            if (ue::Get<float>(cfg + q, -1.0f) != atk || ue::Get<float>(cfg + sq, -1.0f) != brk) {
                ue::Write(cfg + q, &atk, 4); ue::Write(cfg + sq, &brk, 4);
                pbz::log::Info("影塔熔铸：{} 熔铸 {} 次，原生品质倍率 攻击 {:.3f} → {:.3f}、杀气 {:.3f} → {:.3f}（{}）", tower::growth::kWeapons[w].name, n,
                               it->second.first, atk, it->second.second, brk, ue::ObjName(cfg));
            }
        }
    }
    void Shop(const std::string& value, int slot, uint32_t ver) {
        auto& s = grow_.shop;
        if (!s.Open() || run_.phase != tower::Phase::Ready) { Broadcast("err.shop", "现在没有开店"); return; }
        if (grow_.hexJournal.active && !FinishHexJournal()) { Broadcast("err.shop", "正在恢复上次交易，请稍后重试"); return; }
        const auto ctx = grow_.Ctx(run_.floor, run_.floors, run_.items);
        tower::rewards::Buy r = tower::rewards::Buy::Invalid;
        if (value == "forge") {
            // 3.6.1 批量锻造：slot = 买几个（缺省 1；0 = 买得起的全部，不设上限）
            if (ver != s.version || !grow_.hexes.Has(tower::hex::AttributeStack) || grow_.hexAttributes.remaining || grow_.forgeBatch.Active()) { Broadcast("err.shop", "当前无法锻造，请重新打开驿站"); return; }
            const int fp = tower::hex::ForgePrice(run_.floor, run_.floors);
            if (run_.jade < fp) { Broadcast("err.shop", std::format("魂玉不足，锻造需要 {} 魂玉", fp)); return; }
            const int want = slot == 0 ? std::numeric_limits<int>::max() : std::max(1, slot);
            const auto beforeBatch = grow_.forgeBatch; const auto priorCount = grow_.hexForgePurchases; const int oldJade = run_.jade; const uint32_t oldVersion = s.version;
            const int bought = grow_.BeginForgeBatch(run_.seed, run_.jade, want, run_.floor, run_.floors, run_.items);
            if (bought <= 0) { Broadcast("err.shop", "当前没有可用的成长属性"); return; }
            ++s.version; // invalidate a duplicated confirmation from the previous shelf state
            if (!SaveHex()) { grow_.forgeBatch = beforeBatch; grow_.hexForgePurchases = priorCount; grow_.hexJournal = {}; run_.jade = oldJade; s.version = oldVersion; Broadcast("err.shop", "交易保存失败，没有扣除魂玉"); return; }
            if (!FinishHexJournal()) { Broadcast("err.shop", "锻造交易已记录，正在等待保存；请稍后重试"); return; }
            pbz::log::Info("影塔驿站：批量锻造 {} 个（{} 组属性三选一），花费 {} 魂玉，剩余 {}", bought, grow_.forgeBatch.rows.size(), bought * fp, run_.jade);
            Broadcast("info.shop_bought", std::format("锻造 {} 个，请选择成长属性", bought)); return;
        } else if (value == "buy") {
            std::string label = slot >= 0 && slot < static_cast<int>(s.shelf.size()) ? tower::rewards::Label(s.shelf[slot].offer) : "";
            r = tower::rewards::Purchase(s, slot, ver, run_.jade, ctx, [this](const tower::rewards::Offer& o) {
                auto& items = run_.items; int jade = 0;  // 商店不卖魂玉：发放里的魂玉分支不会走到
                return grow_.Grant(Game(), o, items, jade);
            });
            if (r == tower::rewards::Buy::Ok) { grow_.CountBuy(s.bought.back()); grow_.Restock(run_.floor, run_.floors, run_.items, ShopSupplement()); pbz::log::Info("影塔驿站：买入 {}，剩魂玉 {}", label, run_.jade); Broadcast("info.shop_bought", (tower::rewards::Mystery(s.bought.back()) ? "抽中：" : "购买成功：") + label); }  // 3.2.1 随机货买下后揭晓
        } else if (value == "refresh") {
            r = tower::rewards::Refresh(s, ver, run_.jade, ctx);
            if (r == tower::rewards::Buy::Ok) Broadcast("info.shop_refreshed", "货架已刷新");
        } else if (value == "leave") {
            // The UI hides the panel; keep this exact stock and purchase history
            // until the player starts the next encounter.
            r = tower::rewards::Buy::Ok;
            Broadcast("info.shop_left", "已离开驿站");
        }
        if (r == tower::rewards::Buy::Invalid && value == "buy" && slot >= 0 && slot < static_cast<int>(s.shelf.size())) {
            const auto why = tower::rewards::WhyNot(s.shelf[slot].offer, ctx);
            pbz::log::Info("影塔驿站：{} 不能买（{}；魂玉 {}，价格 {}）", tower::rewards::Label(s.shelf[slot].offer), why, run_.jade, s.shelf[slot].price);
            Broadcast("err.shop", "不能买：" + std::string(why.empty() ? tower::rewards::BuyText(r) : why)); return;
        }
        if (r != tower::rewards::Buy::Ok) {
            pbz::log::Info("影塔驿站：{} 未成交（{}；魂玉 {}）", value, tower::rewards::BuyText(r), run_.jade);
            Broadcast("err.shop", std::string(tower::rewards::BuyText(r))); return;
        }
        SaveRun();
    }
    // 驿站补充渠道：50 层流程里没有已核原生掉落的 Boss 关联武器，打过对应层后上架（不进三选一）
    std::vector<tower::rewards::Offer> ShopSupplement() const {
        std::vector<tower::rewards::Offer> out;
        // 3.4.0（用户：打过的 Boss 武器、非 Boss 武器、流程里没有的 Boss 的武器都要动态流入）：随机 Boss 顺序（新局默认）以前在这里直接返回，
        // 方案外 Boss 的武器（玉双环 / 峨眉剑阵等）永远进不了驿站。现在打过的 Boss 武器 + 下面的方案外补充一起返回
        if (run_.shuffleBosses) out = tower::rewards::BossDropOffers(run_.shopDrops);
        const int scavengerAt = grow_.hexes.PickedFloor(tower::hex::Scavenger);
        std::vector<std::string_view> unlockedBeforeScavenger;
        if (scavengerAt > 0) for (const auto& f : tower::floorplan::kFloors) {
            if (ShownBoss(f.floor) >= scavengerAt || f.floor > std::max(run_.floor, run_.peak)) continue;
            for (int k = 0; k < f.count; ++k) {
                const int w = tower::WeaponForPath(f.bosses[k]);
                if (w >= 0) unlockedBeforeScavenger.push_back(tower::kWeaponDrops[w].weapon);
            }
        }
        if (!run_.shuffleBosses) for (const auto& f : tower::floorplan::kFloors) {
            if (f.floor > std::max(run_.floor, run_.peak)) continue;  // 2.0.0：打败过的 Boss 的兵器全部在驿站卖
            for (int k = 0; k < f.count; ++k) {
                const int w = tower::WeaponForPath(f.bosses[k]);
                if (w < 0 || tower::NotBossWeapon(tower::kWeaponDrops[w].weapon)) continue;
                const bool wasUnlocked = std::find(unlockedBeforeScavenger.begin(), unlockedBeforeScavenger.end(), tower::kWeaponDrops[w].weapon) != unlockedBeforeScavenger.end();
                if (!tower::hex::ScavengerShopAllowed(scavengerAt, ShownBoss(f.floor), wasUnlocked)) continue;
                const int wi = tower::growth::WeaponIndex(tower::kWeaponDrops[w].weapon, tower::kWeaponDrops[w].shaqi ? 1 : 0);
                if (wi < 0) continue;
                tower::rewards::Offer o{tower::rewards::Kind::Weapon, wi, 1, 9};
                if (std::find(out.begin(), out.end(), o) == out.end()) out.push_back(o);
            }
        }
        // 方案里打不到的 Boss（例如 v14 撤掉的玉面郎君）：它们的关联武器没有掉落渠道，第 25 层起作为补充渠道上架（用户 2026-11-05）
        {
            for (const auto& d : tower::kWeaponDrops) {
                if (run_.floor < tower::ShopBossWeaponFloor(d.weapon)) continue;
                bool inPlan = false;
                for (const auto& f : tower::floorplan::kFloors)
                    for (int k = 0; k < f.count; ++k) {
                        const auto path = f.bosses[k];
                        const auto slash = path.rfind('/'), dot = path.rfind('.');
                        if (slash != std::string_view::npos && dot != std::string_view::npos && path.substr(slash + 1, dot - slash - 1) == d.stem) inPlan = true;
                    }
                if (inPlan || tower::NotBossWeapon(d.weapon)) continue;
                const int wi = tower::growth::WeaponIndex(d.weapon, d.shaqi ? 1 : 0);
                if (wi < 0) continue;
                bool earned = false;  // 同一把武器另有方案内 Boss 能掉（妖锯：玉妖 / 沐小葵）就不上架
                for (const auto& f : tower::floorplan::kFloors)
                    for (int k = 0; k < f.count; ++k) if (const int w = tower::WeaponForPath(f.bosses[k]); w >= 0 && tower::kWeaponDrops[w].weapon == d.weapon && tower::kWeaponDrops[w].verified) earned = true;
                if (earned) continue;
                tower::rewards::Offer o{tower::rewards::Kind::Weapon, wi, 1, 9};
                if (std::find(out.begin(), out.end(), o) == out.end()) out.push_back(o);
            }
        }
        return out;
    }
    void Prep(const std::string& value, const std::string& id) {
        if (run_.phase != tower::Phase::Ready) { Broadcast("err.prep", "开层前才能预装"); return; }
        int item = -1;
        for (int i = 0; i < tower::kItemCount; ++i) if (tower::kItems[i].id == id) item = i;
        if (value == "use") {  // 3.1.0 魂玉袋 / 经验丹：MOD 背包里点了才用（可以攒到高层驿站再用）
            if (item < 0 || tower::rewards::ItemTiming(tower::kItems[item].item) != tower::rewards::Timing::Use) { Broadcast("err.prep", "这个道具不能直接使用"); return; }
            if (run_.items[item] <= 0) { Broadcast("err.prep", "没有库存"); return; }
            --run_.items[item];
            std::string note;
            if (tower::kItems[item].item == tower::Item::JadeBag) {
                const int got = std::max(1, tower::balance::Price(tower::balance::Buy::Weapon, ShownNow(), 0) * 3 / 10);
                run_.jade += got; const int extra = InvestorBonus(got);
                note = std::format("魂玉袋：魂玉 +{}", got + extra);
            } else {
                grow_.profile.Earn(3, 1);
                note = "经验丹：淬炼点 +3、修为点 +1";
            }
            SaveRun();
            pbz::log::Info("影塔道具：第 {} 层使用 {}", ShownNow(), note);
            Broadcast("info.item_use", note);
            return;
        }
        if (value == "disarm") {  // item 为空 = 全部取消
            if (id.empty()) grow_.prep.DisarmAll();
            else if (!grow_.prep.Disarm(item)) { Broadcast("err.prep", "这个道具没有预装"); return; }
            SaveRun();
            Broadcast("info.prep", id.empty() ? "已取消全部预装" : "已取消预装：" + std::string(tower::kItems[item].name));
            return;
        }
        if (const auto why = grow_.prep.Why(item, run_.items); !why.empty()) { Broadcast("err.prep", std::string(why)); return; }
        grow_.prep.Arm(item, run_.items);
        SaveRun();
        Broadcast("info.prep", "已准备：" + std::string(tower::kItems[item].name));
    }
    tower::wave::Failure waveFailure_ = tower::wave::Failure::Context;
    int waveFailedMonster_ = -1;
    void CancelWave(const std::string& reason, tower::wave::Failure failure, int monster = -1) {
        waveFailure_ = failure; waveFailedMonster_ = monster;
        CancelEntry(reason);
    }
    void CancelEntry(const std::string& reason, bool allowSkip = true) {
        // 仅玩家明确选择跳过时允许推进；异常 / 超时保留本波进度并重试。
        if (forceSkip_ && allowSkip && run_.inMobWave && run_.phase == tower::Phase::Fighting) { MobSkip(reason); return; }
        const bool wasMobWave = run_.inMobWave;
        const auto failedBosses = run_.bosses;
        const int failedFloor = run_.floor, failedMobFloor = mob_.floor, failedMobTotal = mob_.total;
        const auto waveFailure = waveFailure_; const int failedMonster = waveFailedMonster_;
        waveFailure_ = tower::wave::Failure::Context; waveFailedMonster_ = -1;
        const std::string why = reason;
        // 1.0.1（玩家反馈第 26 层万钧"进不了剧情"）：原生遭遇重放靠存档里的剧情 Flow 已激活，存档状态 / 游戏版本不同就可能
        // 一直找不到入场节点、Boss 站着不动，原来 20 秒后取消并跳过本层，玩家打不到这层。现在重放层失败先在同一个原生场地
        // 改为塔直接生成开打（实测塔生成的万钧正常出招、可击杀）再试一次；直接开打也失败才按原来跳过
        // 35 层赤影骨刃·囚锋：塔直接生成会自毁（只能原生刷怪点刷出），不走直接开打，失败照旧跳过
        // 4.0.1（2026-11-16 自动测试：阿瑾层入场时原生面板一直开着，等满 90 秒被当成"重放没启动"，改成直接生成后阿瑾不出招、打不死）：
        // 只是界面没关导致的超时不算重放失败——照常退回准备页，下次仍走原生重放
        const bool panelOnly = why.find("整备面板尚未关闭") != std::string::npos;
        const bool replayRetry = !forceSkip_ && !panelOnly && replay_ != nullptr && replay_->id != std::string_view("yingmei") && allowSkip && run_.phase == tower::Phase::Fighting && directFloor_ != run_.floor && grow_.on;
        towermusic::Stop("本层取消");
        EndReplay();
        towerentry::End(); towertravel::Abort("取消入场"); travelThenSpawn_ = false; spawnAt_ = 0; pending_ = 0; spawnQueue_.clear(); relay_.Clear();
        const size_t cleanupLeft = Clear("entry_cancel"); actors_.clear(); originSet_ = false; boundary_.Disarm(); gateArmed_ = admitted_ = reachDone_ = false; admitSince_ = -1;
        if (run_.phase == tower::Phase::Fighting) Sweep();  // 32ffb7d 小雪：塔生成失败退回准备后，场上原生 Boss 还在、打死收信——先清（清组阻断还在）
        towerisolate::End(); tvenue::End();
        if (wasMobWave && !forceSkip_ && allowSkip) {
            const bool prepRestored = grow_.prep.RearmFired(run_.items);
            const auto action = waveRecovery_.Next(waveFailure, failedMonster, cleanupLeft == 0 && prepRestored);
            if (action == tower::wave::Action::ReplaceMonster) waveAvoid_.insert(failedMonster);
            if (action == tower::wave::Action::ChangeVenue && venueBoss_ >= 0) waveBadVenues_.insert(venueBoss_);
            run_.UndoFloor();
            MobLeave(!prepRestored);
            if (action != tower::wave::Action::Stop) {
                SaveRun(); floorNote_.clear(); autoNextAt_ = now_ + 1.5;
                const std::string msg = action == tower::wave::Action::ReplaceMonster ? "小怪异常，正在换怪并重试当前波" :
                                        action == tower::wave::Action::ChangeVenue ? "小怪场地异常，正在换场地并重试当前波" : "小怪入场暂不可用，正在重试当前波";
                pbz::log::Info("影塔小怪自动恢复：{}；{}（尝试 {}/6，已完成波数 {}）", why, msg, waveRecovery_.attempts, run_.mobWave);
                Broadcast("info.wave_retry", msg); return;
            }
            failedStage_ = {run_.id, run_.floor, failedFloor, failedMobFloor, failedMobTotal, true, {}};
            floorNote_ = cleanupLeft ? "小怪残留无法安全清理，未重试；请保存退出后反馈" :
                         !prepRestored ? "准备道具状态无法核对，已保留当前波；请保存退出后反馈" :
                         "小怪自动恢复未成功，当前波未结算；可重试或选择本层问题";
            SaveRun(); pbz::log::Info("影塔小怪自动恢复停止：{}；{}", why, floorNote_);
            Broadcast("err.entry_failed", floorNote_); return;
        }
        const bool nativeRetry = !forceSkip_ && nativeRetryPending_ && allowSkip && run_.phase == tower::Phase::Fighting && grow_.on;
        nativeRetryPending_ = false;
        if (nativeRetry) {
            nativeRetryFloor_ = run_.floor;
            run_.UndoFloor();
            SaveRun();
            floorNote_.clear();
            autoNextAt_ = now_ + 1.5;
            pbz::log::Info("影塔入场取消：{}（塔生成的 Boss 反复跑出场地，改由原生刷怪点刷出后重开本层）", why);
            Broadcast("info.native_retry", "Boss 反复跑出场地，改由原生刷怪点刷出，自动重开本层");
            return;
        }
        if (replayRetry) {
            directFloor_ = run_.floor;
            run_.UndoFloor();
            SaveRun();
            floorNote_.clear();
            autoNextAt_ = now_ + 1.5;  // 退回准备后自动重开本层（直接开打），不用玩家再点
            pbz::log::Info("影塔入场取消：{}（原生遭遇重放没有启动，改为在原生场地直接开打本层）", why);
            Broadcast("info.replay_direct", "原生剧情入场没有启动，改为直接开打本层");
            return;
        }
        const bool skipped = forceSkip_ && allowSkip && SkipFloor(why);
        if (!skipped) run_.UndoFloor();
        if (wasMobWave && !skipped) MobLeave();
        if (!skipped && run_.phase == tower::Phase::Ready && (wasMobWave || !failedBosses.empty()))
            failedStage_ = {run_.id, run_.floor, failedFloor, failedMobFloor, failedMobTotal, wasMobWave, failedBosses};
        SaveRun();
        if (!skipped) floorNote_ = why;
        pbz::log::Info("影塔入场取消：{}（{}；保留塔档与存档保护）", why, skipped ? "本层跳过" : "本层可重开");
        Broadcast("err.entry_failed", skipped ? floorNote_ : why);
    }
    int directFloor_ = -1;      // 本局改为直接开打的重放层（只试一次）
    tower::FailedStage failedStage_;
    bool forceSkip_ = false;    // 1.3.0：玩家手动跳过本层，不走"自动重开 / 改直接开打"
    // 1.0.1：塔生成的 Boss 反复跑出场地（荆野式：本体自己瞬移）→ 本层改由原生刷怪点刷出再试一次
    bool nativeRetryPending_ = false;
    int teleportPulls_ = 0;  // 本次入场 Boss 瞬移出场（离中心 300 米以上）的次数
    int nativeRetryFloor_ = -1, nativeRetryBoss_ = -1;
    double autoNextAt_ = 0;     // >0：到点自动重开本层

    // 坏层兜底（用户 2026-11-06）：某层因为 bug 开不起来（落不了地、不进 CG、Boss 不刷 / 消失、超时……），不退回上一层
    // （否则"下一层"又进同一个坏层，玩家一直卡住），而是记为跳过：层号保留，下一层去下下层。发本层固定成长收获（点数 / 药匣 /
    // 饰品匣），不发 Boss 兵器 / 掉落 / 三选一。只在 50 层正式局、不是最后一层、不是第 51 层时跳；返回 false = 按原来退回重开
    // 1.1.1：游戏目录里的其他 Mod（只扫一次；日志 + 坏层提示）
    static const std::vector<std::string>& ForeignMods() {
        static const std::vector<std::string> found = [] {
            wchar_t exe[MAX_PATH] = {};
            if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return std::vector<std::string>{};
            const std::filesystem::path dir = std::filesystem::path(exe).parent_path();
            auto list = tower::mods::Foreign(dir, dir.parent_path().parent_path() / L"ProjectPBZ" / L"Content" / L"Paks");
            std::string all;
            for (auto& s : list) all += (all.empty() ? "" : "、") + s;
            pbz::log::Info("影塔兼容检查：{}", list.empty() ? std::string("没有发现其他 Mod") : std::format("发现 {} 项其他 Mod：{}", list.size(), all));
            return list;
        }();
        return found;
    }
    bool SkipFloor(const std::string& why) {
        if (run_.inMobWave) {  // 小怪波次出问题：只跳过这段小怪，Boss 层不受影响
            MobSkip(why);
            return true;
        }
        if (run_.phase != tower::Phase::Fighting || !grow_.on || run_.floors < 50 || (run_.floor >= run_.floors && !run_.endless) || run_.hidden || s_hidden) return false;
        const int f = run_.floor;
        lastBossVenue_ = -1;  // 这一层的场地出过问题
        run_.bosses.clear();
        run_.phase = tower::Phase::Ready;
        if (f > run_.peak) { gains_ = grow_.FloorCleared(Game(), f, run_.floors, run_.seed, {}, 1); run_.peak = f; }  // 1.4.0：回退后重打的层被跳过也没有收获
        else gains_.clear();
        ++skippedFloors_;
        floorNote_ = std::format("第 {} 层出了问题，已跳过，点下一层继续（{}）", ShownBoss(f), why);
        if (const auto hint = tower::mods::Hint(ForeignMods()); !hint.empty()) floorNote_ += "\n" + hint;
        RecordFailure("floor_skipped", venueBoss_, why);
        pbz::log::Info("影塔坏层兜底：第 {} 层跳过（{}）；发固定收获 {}，下一层开第 {} 层", f, why, gains_.empty() ? "无" : gains_, f + 1);
        return true;
    }

    void ApplyPlayer(const Target& p) {
        if (trip_.phase() == tower::trip::Phase::Entering) return;  // 还没载入塔档：不碰原档角色
        EntryGuard(p);
        if (grow_.on) { ApplyGrowth(p); return; }
        const auto& m = run_.mods;
        const auto& st = run_.standard;
        // 统一起点：当前值直接设成标准值（生命上限再乘强化卡），游戏重算加成后下一帧会被重新钉住；出塔停止声明即还原
        attr::Want(p, attr::AttrCurrent(p, hpMax_), Kind::Set, static_cast<float>(st.maxHp * m.MaxHpMul()));
        if (attack_ >= 0) attr::Want(p, attr::AttrCurrent(p, attack_), Kind::Set, static_cast<float>(st.attack * m.DealtMul()));  // 兵器淬炼
        if (shachiAttack_ >= 0) attr::Want(p, attr::AttrCurrent(p, shachiAttack_), Kind::Set, static_cast<float>(st.shachiAttack * m.DealtMul()));
        if (shachiMax_ >= 0) attr::Want(p, attr::AttrCurrent(p, shachiMax_), Kind::Set, static_cast<float>(st.shachiMax));
        if (infliction_ >= 0) attr::Want(p, attr::AttrCurrent(p, infliction_), Kind::Set, static_cast<float>(st.infliction));
        if (clampHp_) {  // 进塔那一刻生命可能高于标准上限：压到上限（只做一次）
            clampHp_ = false;
            const float cap = static_cast<float>(st.maxHp * m.MaxHpMul());
            if (ue::Get<float>(attr::AttrCurrent(p, hp_)) > cap) { ue::Write(attr::AttrBase(p, hp_), &cap, 4); ue::Write(attr::AttrCurrent(p, hp_), &cap, 4); }
        }
        for (int o : taken_) attr::Want(p, attr::AttrCurrent(p, o), Kind::Scale, static_cast<float>(m.TakenMul()));
        if (shachi_ >= 0) attr::Want(p, attr::AttrCurrent(p, shachi_), Kind::Scale, static_cast<float>(m.ShachiMul()));
        if (dilation_ >= 0) attr::Want(p, reinterpret_cast<float*>(p.obj + dilation_), Kind::Scale, static_cast<float>(m.SpeedMul()));
        FightRules(p, m);
    }
    // 统一起点校正：全新开局（第 0 层准备阶段）属性稳定 3 秒后实测一次原生生命 / 攻击 / 杀气上限，高于标准起点 2% 以上就按比例压回，
    // 整局固定（写进局存档）。塔里升级 / 能力 / 饰品 / 奖励的成长都乘在这个系数上，照常生效。已开打过的旧局（第 1 层以后才有这个字段）不校正
    // 1.0.1（用户 2026-11-06：不管几周目、打到哪、纯新档还是满背包，进塔第一层都是同一个起点）：
    // 新局第 0 层准备阶段持续实测原生生命 / 攻击 / 杀气上限（除掉塔自己乘上去的系数），稳定 3 秒后与塔档标准起点比，
    // 偏高或偏低 2% 以上都按比例拉回标准（高：原档永久加成 / 周目 / DLC / 增益；低：加成没上齐 / 版本差异等任何没列到的情况）。
    // 准备阶段里加成晚到会再校一次；点"下一层"后锁定，整局固定写进局存档。塔里的成长都乘在这个系数上照常生效。
    // 旧局（第 1 层以后才有这个字段）不校正
    template <class Attrs> void NormalizeStart(const Target& p, const Attrs& at) {
        if (run_.phase != tower::Phase::Ready || trip_.phase() != tower::trip::Phase::Inside) { normSince_ = -1; return; }
        if (run_.floor > 0 || s_hidden || attack_ < 0 || shachiMax_ < 0) {
            if (run_.norm[0] <= 0) run_.norm[0] = run_.norm[1] = run_.norm[2] = 1;
            if (run_.normPool[0] <= 0) run_.normPool[0] = run_.normPool[1] = 1;
            return;
        }
        auto f = [](double v) { return v > 0 ? v : 1.0; };
        // 用户 2026-11-11 实机：第 0 层就选了玻璃大炮，实测值里带着海克斯的 -30% 生命 / +60% 攻击，被当成原生偏差「拉回标准」，
        // 两个效果全被抵消。实测原生值要把塔自己乘上去的全部系数（奖励属性 + 海克斯）都除掉
        const auto& hx0 = grow_.hexes;
        const double raw[3] = {ue::Get<float>(attr::AttrCurrent(p, hpMax_)) / (tower::hex::LifeScale(at.Mul(tower::rewards::AttrKind::Life), grow_.on ? hx0.Health() : 0.0) * f(run_.normPool[0])),
                               ue::Get<float>(attr::AttrCurrent(p, attack_)) / (tower::hex::Scale(at.Mul(tower::rewards::AttrKind::Attack), grow_.on ? hx0.Attack() : 0.0) * f(run_.norm[1])),
                               ue::Get<float>(attr::AttrCurrent(p, shachiMax_)) / (at.Mul(tower::rewards::AttrKind::Shachi) * f(run_.normPool[1]))};
        if (!(raw[0] > 0 && raw[1] > 0 && raw[2] > 0)) { normSince_ = -1; return; }
        bool same = normSince_ >= 0;
        for (int i = 0; i < 3; ++i) same = same && std::abs(raw[i] - normRaw_[i]) <= 0.01 * normRaw_[i];
        if (!same) { normSince_ = now_; std::copy(raw, raw + 3, normRaw_); return; }  // 刚载入 / 刚换装时原生加成可能还没上齐：等稳定
        if (now_ - normSince_ < 3) return;
        normSince_ = now_ + 1e9;  // 这一轮已判定；原生值再变（same 失败）才重新计时
        const double want[3] = {tower::growth::kPoolStartHp, tower::growth::kStartAttack, tower::growth::kPoolStartShachi};
        auto factor = [](double raw, double want) { const double r = want / raw; return std::abs(r - 1.0) > 0.02 ? std::clamp(r, 0.05, 3.0) : 1.0; };
        const double hp = factor(raw[0], want[0]), atk = factor(raw[1], want[1]), sha = factor(raw[2], want[2]);
        const bool first = run_.norm[0] <= 0 || run_.normPool[0] <= 0;
        const bool changed = first || std::abs(hp - run_.normPool[0]) > 1e-3 || std::abs(atk - run_.norm[1]) > 1e-3 || std::abs(sha - run_.normPool[1]) > 1e-3;
        run_.norm[0] = run_.norm[2] = 1;  // 旧字段（生命 / 杀气）不再使用
        run_.norm[1] = atk; run_.normPool[0] = hp; run_.normPool[1] = sha;
        if (!changed) return;
        pbz::log::Info("影塔统一起点：实测原生 生命 {:.0f} 攻击 {:.0f} 杀气上限 {:.0f} → 标准 {:.0f} / {:.0f} / {:.0f}（×{:.3f} ×{:.3f} ×{:.3f}）",
                       raw[0], raw[1], raw[2], want[0], want[1], want[2], hp, atk, sha);
        hpFillAt_ = now_ + 0.3;  // 上限变了：当前生命对齐新上限（准备阶段本来就补满）
        SaveRun();
    }
    // 原生成长：不再钉统一起点，原生攻击 / 生命由兵器等级、能力、饰品算出；本局加成（旧卡 = 祝福）只乘在原生值上并封顶（方案 5.6）
    void ApplyGrowth(const Target& p) {
        const auto& m = run_.mods;
        // v14 属性奖励（整轮封顶，tower_rewards.h kAttrs）：攻击 → 全部武器攻击；破防能力 → ShachiAttack；生命 → 生命上限；杀气 → 杀气上限。
        // 锐锋符本层 +15% 攻击 / 破防能力（MOD 道具，层结束清除）
        const auto& at = grow_.attrs;
        if (hp_ >= 0 && hpMax_ >= 0 && shachiNow_ >= 0 && shachiMax_ >= 0) {
            towerwidgets::SetBarSource(p.obj, attr::AttrCurrent(p, hp_), ue::Get<float>(attr::AttrCurrent(p, hpMax_)),
                                       attr::AttrCurrent(p, shachiNow_), ue::Get<float>(attr::AttrCurrent(p, shachiMax_)));
            towerwidgets::SyncMaxBars(ue::Get<float>(attr::AttrCurrent(p, hp_)), ue::Get<float>(attr::AttrCurrent(p, hpMax_)),
                                      ue::Get<float>(attr::AttrCurrent(p, shachiNow_)), ue::Get<float>(attr::AttrCurrent(p, shachiMax_)));
        }
        const double edge = ItemOn(tower::Item::Edge) ? 1.15 : 1.0;
        const double rage = ItemOn(tower::Item::Rage) ? 2.5 : 1.0;  // 3.1.0 狂暴药（用户）：攻击力 +150%（受伤 +25% 在下面的受伤倍率里）
        NormalizeStart(p, at);
        // 生命 / 杀气上限不做统一起点校正（用户 2026-11-06 视频：吃持续回血药疯狂加血）：这两个是"池"属性，塔每帧把上限写低，
        // 游戏回血 / 增益效果一跳就把上限重算回原生值、按比例放大当前值，下一帧塔再写回上限，当前值留在放大后，越跳越多。
        // 世界模板统一后所有玩家起点本来一致，不再需要压；旧局存档里记的生命 / 杀气系数一律忽略。攻击不是池属性，照常校正
        const auto nf = [&](int i) {
            if (i == 1) return run_.norm[1] > 0 ? run_.norm[1] : 1.0;
            const double v = run_.normPool[i == 0 ? 0 : 1];  // 旧局（没有这个字段）= 0 → 不校正
            return v > 0 ? v : 1.0;
        };
        auto& hx = grow_.hexes;
        if (hx.Has(tower::hex::Heavy) || hx.Has(tower::hex::ShadowEdge) || !meltMul_.empty()) towerweaponprobe::Ensure(p.obj);
        atkBonusMul_ = std::max(0.1, static_cast<double>(tower::hex::Scale(at.Mul(tower::rewards::AttrKind::Attack), hx.Attack())));
        SparkTick(p);  // 3.3.4 药引暗源 / 暗源充盈
        hx.UpdateEscape(run_.phase == tower::Phase::Fighting && fightSince_ > 0 && !towerentry::Holding(),
                        hp_ >= 0 ? ue::Get<float>(attr::AttrCurrent(p, hp_), -1.0f) : -1.0f,
                        hpMax_ >= 0 ? ue::Get<float>(attr::AttrCurrent(p, hpMax_), -1.0f) : -1.0f);
        const int gritBefore = hx.gritStacks;
        hx.UpdateGrit(run_.phase == tower::Phase::Fighting && fightSince_ > 0 && !towerentry::Holding(),
                      hp_ >= 0 ? ue::Get<float>(attr::AttrCurrent(p, hp_), -1.0f) : -1.0f, now_);
        if (towerdev::kPerfLog && hx.gritStacks > gritBefore)  // 3.0.1：狂徒豪气每叠一层记一行（带上一帧写入原生受伤倍率的值，核对接口）
            pbz::log::Info("影塔海克斯：狂徒豪气 {} 层（减伤 +{}%），上一帧原生受伤倍率 影塔 ×{:.3f} / 读回 {:.3f}", hx.gritStacks, hx.gritStacks, hx.liveTakenMul, hx.liveTakenNative);
        {   // E 批：绝境狂怒（生命 < 30%）、先声夺人（开战后 8 秒）
            const bool fighting = run_.phase == tower::Phase::Fighting && fightSince_ > 0 && !towerentry::Holding();
            const bool furyBefore = hx.furyActive, openingBefore = hx.openingActive;
            hx.UpdateFury(fighting, hp_ >= 0 ? ue::Get<float>(attr::AttrCurrent(p, hp_), -1.0f) : -1.0f,
                          hpMax_ >= 0 ? ue::Get<float>(attr::AttrCurrent(p, hpMax_), -1.0f) : -1.0f);
            hx.UpdateOpening(fighting, now_ - fightSince_);
            {   // 3.1.0 新海克斯：背水一战 / 以战养战 / 铁壁 / 杀意沸腾 / 以伤换伤
                const float hpNow = hp_ >= 0 ? ue::Get<float>(attr::AttrCurrent(p, hp_), -1.0f) : -1.0f, mxNow = hpMax_ >= 0 ? ue::Get<float>(attr::AttrCurrent(p, hpMax_), -1.0f) : -1.0f;
                if (mxNow > 0 && hpNow >= 0) lastHpFrac_ = hpNow / mxNow;
                if (mxNow > 1 && std::isfinite(mxNow)) playerMaxHp_ = mxNow;  // 3.6.0 飞身踢
                hx.backWaterBonus = fighting && hx.Has(tower::hex::BackWater) && mxNow > 0 && hpNow >= 0
                    ? tower::hex::BackWaterOn(hpNow / mxNow, hx.BackWaterStep(), hx.BackWaterCap()) : 0.0;
                if (fightSince_ != warFightMark_) { warFightMark_ = fightSince_; if (run_.floor != warFloor_) { warFloor_ = run_.floor; warKills_ = 0; } }
                hx.warStacks = fighting && hx.Has(tower::hex::WarFeed) ? warKills_ : 0;
                if (hx.Has(tower::hex::WarFeed) && hx.warStacks != warLogged_) {  // 3.4.5（用户：确认以战养战接进攻击）
                    warLogged_ = hx.warStacks;
                    if (hx.warStacks > 0) pbz::log::Info("影塔以战养战：本层已击败 {}，攻击加成 +{:.0f}%（攻击倍率合计 ×{:.2f}）", hx.warStacks, hx.warStacks * hx.WarFeedStep() * 100, hx.liveAtkMul);
                }
                hx.ironActive = fighting && hx.Has(tower::hex::IronWall) && now_ - fightSince_ < hx.IronSeconds();
                const float shNow = shachiNow_ >= 0 ? ue::Get<float>(attr::AttrCurrent(p, shachiNow_), -1.0f) : -1.0f, shMax = shachiMax_ >= 0 ? ue::Get<float>(attr::AttrCurrent(p, shachiMax_), -1.0f) : -1.0f;
                hx.boilActive = fighting && hx.Has(tower::hex::KillingBoil) && shMax > 0 && shNow >= shMax - 0.5f;
                if (fighting && hx.Has(tower::hex::Revenge)) {
                    if (revengeArmed_ && (enemyHitSeen_ || now_ > revengeUntil_)) revengeArmed_ = false;  // 打中一下或超时就用掉
                    // 3.1.0 压测：兵器技能自己扣血（斩马刀等）不算"受到伤害"——有原生伤害事件可看时，要求 0.5 秒内真的挨了一下
                    const bool hurt = !towercombat::PlayerDamageRoute() || towercombat::SincePlayerDamage() < 0.5;
                    if (revLastHp_ >= 0 && hpNow >= 0 && hpNow < revLastHp_ - 0.5f && hurt) { revengeArmed_ = true; revengeUntil_ = now_ + tower::hex::kRevengeSeconds; }
                } else revengeArmed_ = false;
                revLastHp_ = hpNow; enemyHitSeen_ = false;
                hx.revengeActive = revengeArmed_;
            }
            if (hx.furyActive != furyBefore) pbz::log::Info("影塔海克斯：绝境狂怒{}", hx.furyActive ? "生效（生命低于 30%）" : "结束");
            if (hx.openingActive != openingBefore) pbz::log::Info("影塔海克斯：先声夺人{}", hx.openingActive ? "生效（开战 8 秒）" : "结束");
            // 绝技余韵：判定释放了绝技。3.0.1（玩家反馈：连放两次绝技，详情页仍是未生效、日志一条也没有）：
            // 原来用 static 缓存属性偏移，第一次调用时属性表没就绪就永久是 -1、永远不触发；改为没找到就每帧重找。
            // 判定也放宽：绝技点数（UltimateChargeCount）下降，或充能条（UltimateCharge）一下掉了 30% 上限以上且点数没涨（满一格转点数时充能归零不算）
            if (ultCountOff_ < 0) ultCountOff_ = attr::AttrOffset("UltimateChargeCount");
            if (ultChargeOff_ < 0) ultChargeOff_ = attr::AttrOffset("UltimateCharge");
            if (ultMaxOff_ < 0) ultMaxOff_ = attr::AttrOffset("UltimateCharge_Max");
            const float ult = ultCountOff_ >= 0 ? ue::Get<float>(attr::AttrCurrent(p, ultCountOff_), -1.0f) : -1.0f;
            const float charge = ultChargeOff_ >= 0 ? ue::Get<float>(attr::AttrCurrent(p, ultChargeOff_), -1.0f) : -1.0f;
            const float chargeMax = ultMaxOff_ >= 0 ? ue::Get<float>(attr::AttrCurrent(p, ultMaxOff_), -1.0f) : -1.0f;
            if (towerdev::kPerfLog && hx.Has(tower::hex::Echo) && !echoProbeLogged_) {
                echoProbeLogged_ = true;
                pbz::log::Info("影塔海克斯：绝技余韵判定接口 绝技点 {}（{:.1f}）充能 {}（{:.1f}/{:.1f}）", ultCountOff_ >= 0 ? "有" : "无", ult, ultChargeOff_ >= 0 ? "有" : "无", charge, chargeMax);
            }
            // 3.0.1 实机（用户开着修改器无限绝技，绝技点 / 充能被每帧锁满）：再加一条不看数值的判定——
            // 绝技释放期间角色身上挂「CharacterState.Hitreact.Endure.Stand.Ultimate」（绝技霸体），标签从无到有 = 按下绝技
            const int ultTag = hx.Has(tower::hex::Echo) && fighting && !tagReadDead_ ? PlayerTagCount(p.obj, "CharacterState.Hitreact.Endure.Stand.Ultimate") : -1;
            if (hx.Has(tower::hex::Echo) && fighting && ultTag < 0) tagReadDead_ = true;  // 读不到就不再每帧尝试（杀气 / 绝技点判定照常）
            const bool tagRise = ultTag > 0 && echoLastTag_ == 0;
            if (towerdev::kPerfLog && hx.Has(tower::hex::Echo) && ultTag != echoLastTag_ && echoLogs_ < 200) { ++echoLogs_; pbz::log::Info("影塔海克斯：绝技余韵观察 绝技霸体标签 {} → {}", echoLastTag_, ultTag); }
            if (ultTag >= 0) echoLastTag_ = ultTag;
            const bool countDrop = ult >= 0 && echoLastUlt_ >= 0 && ult < echoLastUlt_ - 0.5f;
            const bool countRise = ult >= 0 && echoLastUlt_ >= 0 && ult > echoLastUlt_ + 0.5f;
            const bool chargeDrop = charge >= 0 && echoLastCharge_ >= 0 && chargeMax > 0 && charge < echoLastCharge_ - 0.3f * chargeMax && !countRise;
            // 3.0.1 实机（20:17）：持有绝技余韵、开战后放绝技，绝技点 1.0 / 充能 100 一次都没变。取证：任何变化都记（绝技点、充能 > 1、杀气降 > 5%），
            // 同时每 5 秒记一次快照，确认读的是不是活的数值
            // 3.0.1 实机（20:37，开着无限绝技）：绝技点 / 充能一直 1.0 / 100 不动，绝技霸体标签读不到；但每次按绝技，杀气条都是一帧内清空
            // （128→2、291→1、400→2、400→5），普通杀招每段只扣约 55。判定：一帧内杀气掉 ≥25% 上限且剩下 ≤5% = 放了绝技
            bool shachiDump = false;
            if (shachiNow_ >= 0 && shachiMax_ >= 0) {
                const float sh = ue::Get<float>(attr::AttrCurrent(p, shachiNow_), -1.0f), shm = ue::Get<float>(attr::AttrCurrent(p, shachiMax_), -1.0f);
                shachiDump = echoLastShachi_ >= 0 && shm > 0 && sh >= 0 && echoLastShachi_ - sh >= 0.25f * shm && sh <= 0.05f * shm;
                if (towerdev::kPerfLog && hx.Has(tower::hex::Echo) && fighting && echoLogs_ < 200 && echoLastShachi_ >= 0 && shm > 0 && sh < echoLastShachi_ - 0.05f * shm) {
                    ++echoLogs_; pbz::log::Info("影塔海克斯：绝技余韵观察 杀气 {:.0f} → {:.0f} / {:.0f}（绝技点 {:.1f} 充能 {:.1f}）", echoLastShachi_, sh, shm, ult, charge);
                }
                echoLastShachi_ = sh;
            }
            if (towerdev::kPerfLog && hx.Has(tower::hex::Echo) && fighting && echoLogs_ < 200 && (countDrop || countRise || (ult >= 0 && echoLastUlt_ >= 0 && ult != echoLastUlt_) || (charge >= 0 && echoLastCharge_ >= 0 && std::abs(charge - echoLastCharge_) > 1.0f))) {
                ++echoLogs_;
                pbz::log::Info("影塔海克斯：绝技余韵观察 绝技点 {:.2f} → {:.2f}，充能 {:.1f} → {:.1f} / {:.1f}{}", echoLastUlt_, ult, echoLastCharge_, charge, chargeMax, countDrop || chargeDrop ? "（判为释放）" : "");
            }
            if (towerdev::kPerfLog && hx.Has(tower::hex::Echo) && fighting && now_ >= echoSnapAt_) {
                echoSnapAt_ = now_ + 5.0;
                pbz::log::Info("影塔海克斯：绝技余韵快照 绝技点 {:.2f} 充能 {:.1f}/{:.1f} 杀气 {:.0f}", ult, charge, chargeMax, echoLastShachi_);
            }
            if (hx.Has(tower::hex::Echo) && fighting && (countDrop || chargeDrop || tagRise || shachiDump)) {
                hx.echoStacks = now_ < echoUntil_ ? hx.echoStacks + 1 : 1;  // 10 秒内连放：重新计时并叠层，无上限（3.0.1 由 3 秒改 10 秒）
                echoUntil_ = now_ + tower::hex::kEchoSeconds;
                pbz::log::Info("影塔海克斯：绝技余韵 {} 层，攻击力 +{:.0f}%（{}；绝技点 {:.0f} → {:.0f}，充能 {:.0f} → {:.0f}）", hx.echoStacks, hx.echoStacks * tower::hex::kEchoAttack * 100,
                               tagRise ? "绝技霸体标签出现" : countDrop ? "绝技点下降" : chargeDrop ? "充能下降" : "杀气一帧清空", echoLastUlt_, ult, echoLastCharge_, charge);
            }
            echoLastUlt_ = ult; echoLastCharge_ = charge;
            if (now_ >= echoUntil_ || !hx.Has(tower::hex::Echo)) hx.echoStacks = 0;
            hx.echoLeft = hx.echoStacks ? echoUntil_ - now_ : 0;
            hx.echoActive = hx.Has(tower::hex::Echo) && hx.echoStacks > 0;
            HexArcane(p, fighting && hx.Has(tower::hex::Arcane));
            PlaystyleTick(p, fighting);
            MultiJumpTick(p.obj);
            if (hx.Has(tower::hex::BackToBasics) && !hx.Has(tower::hex::DivineAid)) {  // 回归基本功：绝技点、暗源（影之武装资源）钉 0，药瓶次数清空；如有神助取消这条限制、加成保留
                for (const char* a : {"UltimateCharge", "UltimateChargeCount", "SpiritShadow"})
                    if (const int o = attr::AttrOffset(a); o >= 0) attr::Want(p, attr::AttrCurrent(p, o), Kind::Set, 0.0f);
                if (now_ >= basicsMedAt_) { basicsMedAt_ = now_ + 0.5; towernative::ZeroMedicineUses(); }
            }
        }
        const auto science = run_.phase == tower::Phase::Fighting ? hx.ScientistBoost(ShownNow(), run_.seed) : tower::hex::State::ScienceBoost{};
        const float atkMul = static_cast<float>(tower::hex::Scale(at.Mul(tower::rewards::AttrKind::Attack), hx.Attack() + science.attack) * edge * rage * nf(1)), hpMul = static_cast<float>(tower::hex::LifeScale(at.Mul(tower::rewards::AttrKind::Life), hx.Health() + science.health) * nf(0));
        towerpotion::Sync(hpMul, run_.phase != tower::Phase::Fighting || towerentry::Holding() || fightSince_ <= 0);  // 战斗中不做阻塞加载
        const float breakMul = static_cast<float>(tower::hex::Scale(at.Mul(tower::rewards::AttrKind::Break), hx.Break() + science.armorBreak) * edge), shachiCapMul = static_cast<float>(tower::hex::Scale(at.Mul(tower::rewards::AttrKind::Shachi), science.shachiCap) * nf(2));
        if (baseAtk_ <= 0 && run_.norm[0] > 0 && run_.phase == tower::Phase::Ready && attack_ >= 0) {  // 进塔后第一次：记下塔档起点的原生攻击 / 生命（Boss 补偿的基准）
            baseAtk_ = ue::Get<float>(attr::AttrCurrent(p, attack_)) / atkMul;
            baseHp_ = ue::Get<float>(attr::AttrCurrent(p, hpMax_)) / hpMul;
            pbz::log::Info("影塔原生成长：起点原生攻击 {:.0f}、生命 {:.0f}", baseAtk_, baseHp_);
        }
        // 池属性守护：上限被塔改过时，游戏重算上限会按比例缩放当前值（旧版"吃回血药疯狂加血"的根因）。
        // 识别"上限变了、百分比没变"这种纯重算，把当前值放回重算前的绝对值；真实的受伤 / 回血会改百分比，不受影响
        if (deathAt_ < 0) {
            PoolGuard(p, hpMax_, hp_, hpPool_, hpMul != 1);
            PoolGuard(p, shachiMax_, shachiNow_, shachiPool_, shachiCapMul != 1);
        }
        if (hpMul != 1) attr::Want(p, attr::AttrCurrent(p, hpMax_), Kind::Scale, hpMul);
#if defined(PBZ_HEX_TEST) || defined(PBZ_PLAYTEST)
        if (qaHpMax_ > 0) {  // 3.6.0 测试：qa_hp 锁定生命上限
            // Set 之后同字段的 Scale 仍会乘上去（实机：设 20000、生命倍率 ×4.5 → 读回 90000），所以先除掉本帧的生命倍率
            attr::Want(p, attr::AttrCurrent(p, hpMax_), Kind::Set, static_cast<float>(qaHpMax_ / std::max(0.01, static_cast<double>(hpMul))));
            if (qaHpFill_ && std::abs(ue::Get<float>(attr::AttrCurrent(p, hpMax_)) - static_cast<float>(qaHpMax_)) < 1.0f) { qaHpFill_ = false; hpFillAt_ = now_ + 0.1; }
        }
#endif
        // 生命奖励把上限抬高后，当前生命跟着补满（用户 2026-11-05：选了加生命，面板却显示 935/1262）
        if (run_.phase == tower::Phase::Ready && hpMulSeen_ > 0 && hpMul > hpMulSeen_ + 1e-4f) hpFillAt_ = now_ + 0.3;
        hpMulSeen_ = hpMul;
        if (hpFillAt_ > 0 && now_ >= hpFillAt_ && run_.phase == tower::Phase::Ready && hp_ >= 0) {
            hpFillAt_ = 0;
            const float mx = ue::Get<float>(attr::AttrCurrent(p, hpMax_));
            if (mx > 0 && std::abs(ue::Get<float>(attr::AttrCurrent(p, hp_)) - mx) > 0.5f) {  // 上限升高补满；统一起点压低上限后当前生命也对齐
                ue::Write(attr::AttrBase(p, hp_), &mx, 4); ue::Write(attr::AttrCurrent(p, hp_), &mx, 4);
                pbz::log::Info("影塔原生成长：生命上限变化，当前生命对齐 {:.0f}", mx);
                towerentry::FullHealVisible("生命上限变化");
            }
        }
        // 开层前一直满血（用户 2026-11-06："药补满了，血没补满"）：点下一层到开打之前每帧把生命写到当前上限。
        // 上限在这段时间里还会变（开层装备 / 饰品 / 生命奖励 / 统一起点校正可能晚几帧落地），只在某一刻补一次会差一截
        if (run_.phase == tower::Phase::Fighting && fightSince_ <= 0 && hp_ >= 0 && deathAt_ < 0) {
            const float mx = ue::Get<float>(attr::AttrCurrent(p, hpMax_));
            if (mx > 0 && ue::Get<float>(attr::AttrCurrent(p, hp_)) < mx - 0.5f) {
                ue::Write(attr::AttrBase(p, hp_), &mx, 4); ue::Write(attr::AttrCurrent(p, hp_), &mx, 4);
                // The cap is applied at the end of the previous frame. Refresh the
                // native bar as soon as that cap is visible, before the countdown.
                towerentry::FullHealVisible(science.health > 0 ? "科学狂人生命上限生效" : "开层生命上限生效");
            }
        }
        // The scientist can raise the Sha-Chi cap after TopUpNow has run.
        // Align it before the entrance countdown, then immediately synchronize
        // the native HUD instead of waiting until the fight starts.
        if (towerentry::FullStateEnabled() && run_.phase == tower::Phase::Fighting && fightSince_ <= 0 && shachiNow_ >= 0 && shachiMax_ >= 0 && deathAt_ < 0) {
            const float top=ue::Get<float>(attr::AttrCurrent(p,shachiMax_),0.0f);
            const float before=ue::Get<float>(attr::AttrCurrent(p,shachiNow_),0.0f);
            if (std::isfinite(top) && top>0 && std::isfinite(before) && before<top-.5f) {
                auto* base=attr::AttrBase(p,shachiNow_); auto* current=attr::AttrCurrent(p,shachiNow_);
                const float oldBase=ue::Get<float>(base,before);
                const bool baseOk=ue::Write(base,&top,4);
                const bool currentOk=ue::Write(current,&top,4);
                if(!baseOk || !currentOk){ if(baseOk)ue::Write(base,&oldBase,4); if(currentOk)ue::Write(current,&before,4); }
                if(baseOk && currentOk && hp_>=0 && hpMax_>=0)
                    towerwidgets::SyncMaxBars(ue::Get<float>(attr::AttrCurrent(p,hp_)),ue::Get<float>(attr::AttrCurrent(p,hpMax_)),top,top,true);
                if(baseOk && currentOk)pbz::log::Info("影塔入场：杀气上限更新，提前补满 {:.0f} → {:.0f}",before,top);
            }
        }
        // 保险：当前生命永远不超过上限（任何原生联动 / 写入时序导致的溢出当帧压回）
        if (hp_ >= 0 && deathAt_ < 0) {
            const float mx = ue::Get<float>(attr::AttrCurrent(p, hpMax_));
            if (mx > 0 && ue::Get<float>(attr::AttrCurrent(p, hp_)) > mx + 0.5f) { ue::Write(attr::AttrBase(p, hp_), &mx, 4); ue::Write(attr::AttrCurrent(p, hp_), &mx, 4); }
        }
        if (atkMul != 1 && attack_ >= 0) attr::Want(p, attr::AttrCurrent(p, attack_), Kind::Scale, atkMul);
        // 3.0.1（用户：加成要落到面板和原生数据上才算生效）：详情页显示从原生属性读回的当前值
        hx.liveAtkMul = atkMul; hx.liveAttack = attack_ >= 0 ? ue::Get<float>(attr::AttrCurrent(p, attack_), -1.0f) : -1.0f;
        if (breakMul != 1 && shachiAttack_ >= 0) attr::Want(p, attr::AttrCurrent(p, shachiAttack_), Kind::Scale, breakMul);
        // v3（用户 2026-11-12）：移速合计超过 +80% 的部分只算一半，绝对上限 ×2.5
        if (const float speed = static_cast<float>(tower::balance::SoftSpeed(hx.Speed() + science.move, tower::balance::kMoveKnee)); speed != 1.0f)
            for (int off : {walkSpeed_, jogSpeed_, sprintSpeed_}) if (off >= 0) attr::Want(p, attr::AttrCurrent(p, off), Kind::Scale, speed);
        if (shachiCapMul != 1 && shachiMax_ >= 0) attr::Want(p, attr::AttrCurrent(p, shachiMax_), Kind::Scale, shachiCapMul);
        if (shachiCapMul > 1.01f && shachiMax_ >= 0 && shachiNow_ >= 0 && now_ >= shachiDiagAt_) {  // 3.3.3 诊断（用户：杀气上限仍锁 1000）
            shachiDiagAt_ = now_ + 30.0;
            pbz::log::Info("影塔杀气：上限倍率 ×{:.2f}，读回上限 {:.0f}（基值 {:.0f}）、当前杀气 {:.0f}", shachiCapMul, ue::Get<float>(attr::AttrCurrent(p, shachiMax_)),
                           ue::Get<float>(attr::AttrBase(p, shachiMax_)), ue::Get<float>(attr::AttrCurrent(p, shachiNow_)));
        }
        // 伤害减免作用于原生受伤倍率（原生减伤先算，影塔减伤再乘）；生命吸取在敌人采样之后结算。
        // 用户 2026-11-11：影塔全部减伤（奖励属性、海克斯、铁甲符）乘法合计，最多 80%
        const int dr = at.pm[static_cast<int>(tower::rewards::AttrKind::Defense)];
        // 每个受伤倍率字段先算出影塔的总倍率（减伤 × 破釜沉舟 × 抗性），再一次性声明：
        // v3（用户 2026-11-12）：原生减伤（道具）× 影塔减伤 合计实际受伤最少 10%。原生值 = 读到的当前值 ÷ 上一帧影塔写入的倍率
        const double minTaken = hx.MinTaken();  // 80%（艾卡西亚 90%）
        // 3.6.0（用户 2026-11-14）：减伤相加后封顶，再扣穿透：有效减伤 = max(0, min(合计, 上限) − 穿透)；狂暴药另乘
        const double modTaken = tower::balance::TakenFactor(tower::hex::Reduction(dr, hx.Defense(), ItemOn(tower::Item::Guard)), 1.0 - minTaken,
                                                            tower::balance::DrPenetration(ShownNow())) * (ItemOn(tower::Item::Rage) ? 1.25 : 1.0);
        std::unordered_map<int, double> scale;
        for (int o : taken_) scale[o] = modTaken * hx.DamageTaken();
        hx.liveTakenMul = modTaken * hx.DamageTaken();
        hx.liveTakenNative = taken_.empty() ? -1.0 : ue::Get<float>(attr::AttrCurrent(p, taken_[0]), -1.0f);  // 原生受伤倍率（含影塔写入）
        // 用户 2026-11-11：六项抗性（奖励属性）。对应元素受到伤害 ×(1-抗性)，与上面的影塔减伤相乘后仍不低于 20%；
        // 对应异常积累 ×(1-抗性)（越难被挂上点燃 / 冰冻 / 感电 / 中毒 / 出血）
        for (const auto& r : tower::rewards::kResists) {
            const int pm = at.pm[static_cast<int>(r.kind)];
            if (pm <= 0) continue;
            const double resist = std::clamp(1.0 - pm / 1000.0, 0.0, 1.0);
            if (const int o = attr::AttrOffset(std::string("TakenVulnerabilityMultiplier_") + r.damage); o >= 0) {
                auto it = scale.find(o);
                const double before = it != scale.end() ? it->second : 1.0;
                scale[o] = before * std::max(resist, minTaken / modTaken);
            }
            for (const char* acc : r.acc) if (acc)
                if (const int o = attr::AttrOffset(std::string("TakenVulnerabilityMultiplier_") + acc + "Accumulation"); o >= 0)
                    attr::Want(p, attr::AttrCurrent(p, o), Kind::Scale, static_cast<float>(resist));
            // 用户 2026-11-12：抗性同时走防冰符同款原生接口 TakenReductionMultiplier_<元素>（冰缓等元素效果）/ _<异常>Accumulation（冰冻等异常持续时间），
            // 且与饰品加法叠加：防冰符+1 写 0.2（剩 20% = -80%），再有 20% 冰霜抗性 → 剩 0 = 免疫。值为「剩余比例」，0 / ≥1 视为未减免
            ReductionAdd(p, std::string("TakenReductionMultiplier_") + r.damage, pm / 1000.0);
            for (const char* acc : r.acc) if (acc) ReductionAdd(p, std::string("TakenReductionMultiplier_") + acc + "Accumulation", pm / 1000.0);
        }
        for (auto& [o, f] : scale) {
            const float cur = ue::Get<float>(attr::AttrCurrent(p, o), -1.f);
            const auto last = takenScale_.find(o);
            const double native = last != takenScale_.end() && last->second > 0 && std::isfinite(cur) ? cur / last->second : cur;
            double use = f;
            if (std::isfinite(native) && native > 0 && native * use < tower::balance::kMinTotalTaken) use = tower::balance::kMinTotalTaken / native;
            if (use != 1.0) attr::Want(p, attr::AttrCurrent(p, o), Kind::Scale, static_cast<float>(use));
            takenScale_[o] = use;
        }
        if (ItemOn(tower::Item::Shachi) && shachi_ >= 0) attr::Want(p, attr::AttrCurrent(p, shachi_), Kind::Scale, 1.4f);  // 聚煞丹：本层杀气恢复速度 +40%
        // 攻速奖励已移除（用户 2026-11-05）
#ifdef PBZ_PLAYTEST
        // 3.7.0 测试包（用户 2026-11-15：旧测试赠送太多——长血、十几个旧海克斯——影响测新玩法）：
        // 新局只送本批要测的玩法类海克斯，不加任何属性 / 兵器 / 魂玉，数值按正常前期。旧赠送见 git 历史（3.1.0–3.6.x 测试包）
        if (grow_.on && run_.phase == tower::Phase::Ready && run_.floor == 0 && playtestGranted_ != run_.id) {
            playtestGranted_ = run_.id;
            std::string got;
            for (const auto id : kPlaytestHexes) if (!grow_.hexes.Has(id)) {
                grow_.hexes.extra.push_back(id); grow_.hexes.extraAt.push_back(-1); grow_.hexes.extraBase.push_back(grow_.hexes.endlessCleared);
                grow_.hexes.seen |= tower::hex::Bit(id);
                got += (got.empty() ? "" : "、") + std::string(tower::hex::defs[id].name);
            }
            SaveHex();
            SaveRun();
            pbz::log::Info("影塔测试包：本局 {} 只送玩法类测试海克斯 {}（无属性加成）", run_.id, got);
            Broadcast("info.playtest", "测试包：已送 " + got);
        }
#endif
        if (run_.phase == tower::Phase::Ready && now_ >= nextReconcile_) {  // 准备阶段每 2 秒对账（玩家刚在原生界面里升级 / 点能力）
            nextReconcile_ = now_ + 2;
            if (const auto note = grow_.Reconcile(Game()); !note.empty()) { pbz::log::Info("影塔原生成长：{}", note); SaveRun(); Broadcast("info.grow_reconciled", note); }
        }
        FightRules(p, m);
    }
    double burstSince_ = -1; float burstHp_ = 0; bool burstLogged_ = false; int burstLogs_ = 0;  // 3.2.0 连击重伤取证
    tower::leech::Credit leech_;
    bool leechLogged_ = false;
    float hpTraceLast_ = -1, hpTraceMax_ = -1; int hpTraceLogs_ = 0; double leechWroteAt_ = -1;  // 3.1.0 生命轨迹取证  // 3.1.0：正式版吸血只记第一次（原来每 0.2 秒一行）
    double nextLeech_ = 0, nextLeechBar_ = 0;
    void ApplyLeech(const Target& p, bool final) {
        if (run_.phase != tower::Phase::Fighting || fightSince_ <= 0 || deathAt_ >= 0 || towerentry::Holding() || LeechRate() <= 0) { leech_.Reset(); return; }
        const float mx = ue::Get<float>(attr::AttrCurrent(p, hpMax_)), cur = ue::Get<float>(attr::AttrCurrent(p, hp_));
        // Always discard credit at full HP, even during the throttle interval.
        if (mx > 0 && cur >= mx - 0.001f) { leech_.Reset(); return; }
        if (now_ < nextLeech_ && !final) return;
        const double heal = leech_.Quote(cur, mx, final);
        if (heal <= 0) return;
        const float next = static_cast<float>(std::min<double>(mx, cur + heal));
        const bool baseOk = ue::Write(attr::AttrBase(p, hp_), &next, 4);
        const bool currentOk = ue::Write(attr::AttrCurrent(p, hp_), &next, 4);
        if (!baseOk || !currentOk) { leech_.Reset(); pbz::log::Info("影塔生命吸取：生命写入失败 base={} current={}", baseOk, currentOk); return; }
        leech_.Applied(next - cur, next >= mx - 0.001f);
        leechWroteAt_ = now_;
        nextLeech_ = now_ + 0.2;
        if (towerdev::kPerfLog || !leechLogged_) { leechLogged_ = true; pbz::log::Info("影塔生命吸取：第 {} 层 回复 {:.1f}，生命 {:.1f} → {:.1f}/{:.1f}{}", run_.floor, next - cur, cur, next, mx, towerdev::kPerfLog ? "" : "（正式版每局只记第一次）"); }
        if (now_ >= nextLeechBar_) { nextLeechBar_ = now_ + 0.2; towerwidgets::RefreshHpBarFast(cur, next); }
    }
    std::unordered_map<int, double> takenScale_;  // v3：上一帧对每个受伤倍率字段写入的影塔总倍率（反推原生值用）
    struct PoolTrack { float max = 0, cur = 0; int fixes = 0; };
    PoolTrack hpPool_, shachiPool_;
    void PoolGuard(const Target& p, int maxOff, int curOff, PoolTrack& t, bool scaled) {
        if (maxOff < 0 || curOff < 0) return;
        float* mx = attr::AttrCurrent(p, maxOff);
        float* cur = attr::AttrCurrent(p, curOff);
        const float m = ue::Get<float>(mx), c = ue::Get<float>(cur);
        if (!(m > 0) || !std::isfinite(c)) { t = {}; return; }
        if (scaled && t.max > 0 && std::abs(m - t.max) > 0.5f && t.cur > 0) {
            const float was = t.cur / t.max, now = c / m;
            if (std::abs(was - now) < 0.004f && std::abs(c - t.cur) > 0.5f) {
                const float keep = std::min(t.cur, std::max(m, t.max));
                ue::Write(attr::AttrBase(p, curOff), &keep, 4); ue::Write(cur, &keep, 4);
                if (t.fixes++ < 20) pbz::log::Info("影塔池属性守护：原生重算上限 {:.0f}→{:.0f}，当前值按比例 {:.0f}→{:.0f}，放回 {:.0f}", t.max, m, t.cur, c, keep);
                t.cur = keep; t.max = m;
                return;
            }
        }
        t.max = m; t.cur = c;
    }
    // 补发（ui12，34 层断肢锯漏发）：每次加载后第一次进准备阶段，已通过各层方案 Boss 的已核兵器若背包里没有，补进背包（1 级），只补一次
    bool weaponCatchUp_ = false;
    void CatchUpBossWeapons() {
        if (!grow_.on || run_.floor <= 0) return;
        std::vector<std::string> got;
        for (const auto& f : tower::floorplan::kFloors) {
            if (f.floor > run_.floor) continue;
            for (int i = 0; i < f.count; ++i) {
                const int w = tower::WeaponForPath(f.bosses[i]);
                if (w < 0) continue;
                const auto& d = tower::kWeaponDrops[w];
                const int kind = d.shaqi ? 1 : 0;
                if (!d.verified || !tower::EarnedBossWeapon(d.weapon, kind)) continue;
                const int gi = tower::growth::WeaponIndex(d.weapon, kind);
                if (gi < 0 || grow_.profile.Find(gi) >= 0) continue;
                if (grow_.profile.AddWeapon(gi)) {
                    got.push_back(std::format("第 {} 层 {}", ShownBoss(f.floor), d.name));
                    if (std::find(run_.weapons.begin(), run_.weapons.end(), w) == run_.weapons.end()) run_.weapons.push_back(w);
                }
            }
        }
        if (got.empty()) return;
        grow_.session.Sync(Game(), grow_.profile);
        std::string s; for (auto& x : got) s += (s.empty() ? "" : "、") + x;
        pbz::log::Info("影塔Boss掉落补发：{}（已通过的层漏发，按已核关联补进背包 1 级）", s);
        SaveRun();
        Broadcast("info.grow_reconciled", "补发 Boss 兵器：" + s);
    }
    // 本层生效中的准备道具（开战时扣的那几种；层结束清掉）
    // 准备道具生效（普通层：读秒结束；重放层：一阶段到场且入场过场播完）。定身 / 缓时从这一刻计时，过场播放期间暂停
    void FirePrep() {
        prepPending_ = false;
        if (!grow_.on) return;
        const auto fired = grow_.prep.Fire(run_.items);
        // 3.1.0（用户）：定身先机 2 秒和定身符 4 秒叠加（同时有 = 6 秒），不再取较长的
        double freezeSecs = grow_.hexes.Has(tower::hex::FirstStrike) ? grow_.hexes.FirstStrikeSeconds() : 0.0;
        std::string names;
        for (int item : fired) {
            switch (tower::kItems[item].item) {
                case tower::Item::Sunder: sunder_ = true; break;
                case tower::Item::Freeze: freezeSecs += 4; break;
                case tower::Item::Slow: slowUntil_ = now_ + 10; break;
                default: break;  // 铁甲 / 锐锋 / 破势 / 聚煞：按本层生效列表在属性里乘（ApplyGrowth / Boss 循环）
            }
            names += (names.empty() ? "" : "、") + std::string(tower::kItems[item].name);
        }
        if (freezeSecs > 0) {
            freezeUntil_ = std::max(freezeUntil_, now_ + freezeSecs);
            pbz::log::Info("影塔开战定身：{:.0f} 秒（定身先机 {} / 定身符 {}）", freezeSecs, grow_.hexes.Has(tower::hex::FirstStrike) ? "有" : "无", freezeSecs >= 4 ? "有" : "无");
        }
        if (!fired.empty()) {
            pbz::log::Info("影塔准备道具：{} 已生效（本层）", names);
            Broadcast("info.prep_fired", names + " 已生效");
        }
    }
    bool ItemOn(tower::Item it) const {
        if (!grow_.on || run_.phase != tower::Phase::Fighting || towerentry::Holding()) return false;
        for (int i : grow_.prep.active) if (tower::kItems[i].item == it) return true;
        return false;
    }
    // 钱生钱：每一笔魂玉收入入账后都调用这里，把同额再加一次；返回额外加的数。新增魂玉来源必须走这里
    // 魂玉加成（钱生钱 ×2、积少成多 +2%/段）：返回额外发放的数量并已加进魂玉
    // 3.1.0（用户 2026-11-13）：魂玉 = 基础 ×（1 + 加成合计，如聚宝盆 +50%）× 倍数（钱生钱 ×2、悬赏令 ×2）×（1 + 积少成多）
    int InvestorBonus(int earned, double additive = 0, double mul = 1) {
        if (!grow_.on || earned <= 0) return 0;
        const int total = tower::balance::JadeTotal(earned, additive, grow_.hexes.InvestorMul() * std::max(1.0, mul), grow_.hexes.SnowBallBonus());
        const int extra = std::max(0, total - earned);
        run_.jade += extra;
        return extra;
    }
    // 3.6.0（用户 2026-11-14）魂玉账单：实得 = (基础 + 奖励页魂玉) ×（1 + 积少成多）×（1 + 聚宝盆 × 生命比例）× 钱生钱 2 × 悬赏令 3；
    // 额外 = 实得 − 基础。基础与奖励页魂玉入账时按原数到账，结算只补差额。倍率在通关时定下（之后回血 / 换海克斯不影响）
    std::string jadeNote_;
    void OpenBill(int shown, int base, bool bossFloor) {
        run_.bill = {};
        jadeNote_.clear();
        if (!grow_.on || base <= 0) return;
        run_.bill = tower::BillFor(shown, base, std::max(0.0, BowlBonus()), grow_.hexes.InvestorMul() * (bossFloor ? BountyMul() : 1.0),
                                   std::max(0.0, grow_.hexes.SnowBallBonus()));
    }
    // 返回本层实得（基础 + 奖励页 + 额外）；没有待结算账单时返回 0
    int SettleBill(std::string_view why) {
        if (!run_.bill.open) return 0;
        const auto b = run_.bill;
        run_.bill = {};
        const int sum = b.base + b.picked, total = tower::BillTotal(b);
        run_.jade += total - sum;
        run_.last.jade = total;
        jadeNote_ = tower::BillText(b);
        floorNote_ = jadeNote_;
        pbz::log::Info("影塔魂玉账单：第 {} 层（{}）基础 {} + 奖励页 {} = {}；聚宝盆等 +{:g}%、积少成多 +{:g}%、倍数 ×{:g}（钱生钱 / 悬赏令）→ 实得 {}、额外 {}、补发 {}",
                       b.floor, why, b.base, b.picked, sum, b.addPm / 10.0, b.snowPm / 10.0, b.mulPm / 1000.0, total, total - b.base, total - sum);
        Broadcast("info.jade_bill", jadeNote_);
        return total;
    }
    void EnemyRegen(Actor& a, const Target& t, float& hp) {
        const int shown = ShownNow();
        const float mx = ue::Get<float>(attr::AttrCurrent(t, hpMax_), -1.f), mn = ue::Get<float>(attr::AttrCurrent(t, hpMin_), 0.f);
        float* shCur = shachiNow_ >= 0 && shachiMax_ >= 0 ? attr::AttrCurrent(t, shachiNow_) : nullptr;
        const float sh = shCur ? ue::Get<float>(shCur, -1.f) : -1.f, shMx = shCur ? ue::Get<float>(attr::AttrCurrent(t, shachiMax_), -1.f) : -1.f;
        if (!std::isfinite(hp) || !std::isfinite(mx) || mx <= 0 || hp <= 0.5f) { a.regenHp = -1; a.shachiQuiet = now_; a.shachiAt = now_; return; }
        const bool hpDrop = a.regenHp >= 0 && hp < a.regenHp - 0.5f;
        const bool shDrop = sh >= 0 && a.shachiLast >= 0 && sh < a.shachiLast - 0.5f;
        if (a.shachiQuiet < 0 || hpDrop || shDrop) a.shachiQuiet = now_;  // 受到任何伤害（含助战）或被削杀气：重新计时
        const double dt = a.shachiAt >= 0 ? std::clamp(now_ - a.shachiAt, 0.0, 0.5) : 0.0;
        a.shachiAt = now_;
        const bool invuln = !taken_.empty() && ue::Get<float>(attr::AttrCurrent(t, taken_[0]), 1.f) <= 0.001f;
        const bool locked = std::isfinite(mn) && mn > 1.f && hp <= mn + 1.f;  // 停在锁血线上（阶段 / 处决）
        if (tower::balance::RegenExempt(a.name)) { a.regenHp = hp; a.shachiLast = sh; return; }  // 蓝盾 / 虚化类 Boss 不回复
        if (now_ - a.shachiQuiet >= tower::balance::kRegenDelay && dt > 0 && !invuln && !locked && a.death.zeroAt < 0) {
            if (hp < mx) {
                const float next = std::min(mx, hp + static_cast<float>(mx * tower::balance::EnemyHpRegen(shown) * dt));
                if (next > hp && ue::Write(attr::AttrBase(t, hp_), &next, 4) && ue::Write(attr::AttrCurrent(t, hp_), &next, 4)) {
                    towerwidgets::RefreshEnemyHpBar(a.obj, next, mx);
                    towercombat::Adjusted(a.obj, next, "无尽回复");
                    hp = next; a.burn.Applied(next);
                }
            }
            if (shCur && std::isfinite(sh) && std::isfinite(shMx) && shMx > 0 && sh >= 0 && sh < shMx) {
                const float next = std::min(shMx, sh + static_cast<float>(shMx * tower::balance::EnemyShachiRegen(shown) * dt));
                // 3.6.0 实机（杀气不回）：只写当前值会被游戏按基础值重算回去，和生命一样基础值、当前值都写
                if (next > sh) { ue::Write(attr::AttrBase(t, shachiNow_), &next, 4); ue::Write(shCur, &next, 4); towerwidgets::RefreshEnemyShachiBar(a.obj, next, shMx); }  // 3.6.0 实机：值涨了但条不动，手动刷原生杀气条
                if (next > sh && shachiRegenLogs_ < 20 && towerdev::kPerfLog) {
                    ++shachiRegenLogs_;
                    pbz::log::Info("影塔无尽回复：{} 杀气 {:.0f} → {:.0f} / {:.0f}（基础值读回 {:.0f}）", a.name, sh, next, shMx, ue::Get<float>(attr::AttrBase(t, shachiNow_), -1.f));
                }
                a.shachiLast = next;
                a.regenHp = hp;
                return;
            }
        }
        a.regenHp = hp;
        a.shachiLast = sh;
    }
    // 3.6.0 反震：待打出的伤害（目标最大生命的比例）与目标；目标只做指针比较，真正写血在敌人循环里按当前帧的有效对象做
    UObject* thornsTarget_ = nullptr;
    double thornsFrac_ = 0, thornsAt_ = -1, thornsHpAt_ = -1;
    float thornsHp_ = -1;
    int thornsLogs_ = 0;
    UObject* fnActorLoc_ = nullptr;
    bool ActorLoc(UObject* o, double out[3]) {
        if (!o || !ue::Live(o)) return false;
        if (!fnActorLoc_) if (auto* actor = towerperf::ClassNamed("Actor")) fnActorLoc_ = pe::FindFunction(actor, "K2_GetActorLocation");
        if (!fnActorLoc_) return false;
        uint8_t b[32] = {};
        pe::Call(o, fnActorLoc_, b);
        std::memcpy(out, b, 24);
        return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
    }
    UObject* ThornsTarget(UObject* player) {
        auto usable = [](const Actor& a) { return !a.dead && !a.gone && a.hasTarget && a.filled && ue::Live(a.obj); };
        if (UObject* lock = static_cast<UObject*>(towernative::LockTarget()))
            for (const auto& a : actors_) if (a.obj == lock && usable(a)) return a.obj;
        double pp[3];
        if (!ActorLoc(player, pp)) return nullptr;
        UObject* best = nullptr;
        double bestD = tower::hex::kThornsRange * tower::hex::kThornsRange;
        for (const auto& a : actors_) {
            double q[3];
            if (!usable(a) || !ActorLoc(a.obj, q)) continue;
            const double d = (q[0] - pp[0]) * (q[0] - pp[0]) + (q[1] - pp[1]) * (q[1] - pp[1]) + (q[2] - pp[2]) * (q[2] - pp[2]);
            if (d < bestD) { bestD = d; best = a.obj; }
        }
        return best;
    }
    double qaHpMax_ = 0; bool qaHpFill_ = false;  // 3.6.0 测试包 qa_hp
    double playerMaxHp_ = 0;  // 3.6.0 飞身踢：本帧玩家生命上限
    double lastHpFrac_ = 1;  // 3.1.0 聚宝盆：本帧玩家生命比例
    double BowlBonus() const {
        if (!grow_.on) return 0.0;
        double b = grow_.hexes.Has(tower::hex::TreasureBowl) ? grow_.hexes.BowlRate() * std::clamp(lastHpFrac_, 0.0, 1.0) : 0.0;
        if (grow_.hexes.Has(tower::hex::Wealth)) b += tower::hex::kWealthJade;                                       // 3.3.3 财源广进
        if (grow_.hexes.Has(tower::hex::Spoils) && warFloor_ == run_.floor) b += tower::hex::kSpoilsJade * warKills_;  // 3.3.3 战利品（本层）
        return b;
    }
    double BountyMul() const { return grow_.on && grow_.prep.Active(static_cast<int>(tower::Item::Bounty)) ? tower::kBountyMul : 1.0; }
    // F 批：通关一段（Boss 层或小怪段，非重打）时结算的海克斯：苦修、无伤大师、速战速决
    void HexClearRewards(int shown, int jade, bool bossFloor) {
        if (!grow_.on) return;
        auto& hx = grow_.hexes;
        if (hx.Has(tower::hex::Ascetic) && !fight_.potion) { ++hx.asceticStacks; pbz::log::Info("影塔海克斯：苦修，第 {} 层未用药，生命上限累计 +{:g}%", shown, hx.asceticStacks * hx.AsceticStep() * 100); Broadcast("info.hex_ascetic", std::format("苦修：生命上限累计 +{:g}%", hx.asceticStacks * tower::hex::kAsceticStep * 100)); }
        if (hx.Has(tower::hex::Flawless) && !fight_.hit) { ++hx.flawlessStacks; pbz::log::Info("影塔海克斯：无伤大师，第 {} 层无伤，攻击累计 +{:g}%", shown, hx.flawlessStacks * hx.FlawlessStep() * 100); Broadcast("info.hex_flawless", std::format("无伤大师：攻击累计 +{:g}%", hx.flawlessStacks * tower::hex::kFlawlessStep * 100)); }
        const double took = fightSince_ > 0 ? now_ - fightSince_ : 1e9;
        // 3.1.0（用户）：60 秒内、只在 Boss 层和无尽层（每层都走这条）生效，小怪段不触发；只给 1 次属性成长，档位按当前层
        (void)jade;
        (void)bossFloor;  // 3.1.0 N7（用户）：Boss 层、小怪段、无尽层都生效
        if (hx.Has(tower::hex::Blitz) && took <= hx.BlitzSeconds() && !grow_.hexAttributes.remaining) {
            // 3.1.0：投掷狂人（每次额外奖励选两次）对速战速决同样生效
            grow_.hexAttributes.BeginAttributes(run_.seed ^ (0x9E3779B97F4A7C15ull * static_cast<uint64_t>(shown + 1)), hx.BlitzPicks() * hx.ThrowerMul(), grow_.Ctx(run_.floor, run_.floors, run_.items));
            pbz::log::Info("影塔海克斯：速战速决，第 {} 层 {:.1f} 秒通关，属性成长 {} 次", shown, took, grow_.hexAttributes.remaining);
            Broadcast("info.hex_blitz", std::format("速战速决：{:.0f} 秒通关，额外 {} 次属性成长{}", took, grow_.hexAttributes.remaining, hx.Has(tower::hex::Thrower) ? "（投掷狂人加成）" : ""));
        }
    }
    // 原生「剩余比例」型减免属性上加法叠加影塔抗性（见 ApplyGrowth 抗性段）。原生值 = 当前值（与上一帧影塔写入值不同则视为游戏重算）
    std::unordered_map<int, std::pair<float, float>> reductionNative_;  // 偏移 → {原生值, 上一帧写入值}
    void ReductionAdd(const Target& p, const std::string& name, double resist) {
        const int o = attr::AttrOffset(name);
        if (o < 0 || resist <= 0) return;
        const float cur = ue::Get<float>(attr::AttrCurrent(p, o), 1.0f);
        auto& [native, wrote] = reductionNative_.try_emplace(o, std::pair<float, float>{cur, -1.0f}).first->second;
        if (std::abs(cur - wrote) > 1e-5f) native = cur;
        const double nativeCut = native > 0 && native < 1 ? 1.0 - native : 0.0;   // 0 / ≥1：没有饰品减免
        const float remain = static_cast<float>(std::max(0.001, 1.0 - std::min(1.0, nativeCut + resist)));  // 0 可能被当成未设置：留 0.1%
        if (wrote < 0) pbz::log::Info("影塔抗性：{} 原生 {:.3f} + 抗性 {:.0f}% → 剩余 {:.3f}", name, native, resist * 100, remain);
        wrote = remain;
        attr::Want(p, attr::AttrCurrent(p, o), Kind::Set, remain);
    }
#ifdef PBZ_PLAYTEST
    std::string playtestGranted_, playtestArmory_;
#endif
    static constexpr int kPlaytestLeech = tower::leech::kPlaytestLeech;
    int LeechRate() const {
        return tower::leech::Rate(grow_.attrs.pm[static_cast<int>(tower::rewards::AttrKind::LifeSteal)], ItemOn(tower::Item::Leech)) + grow_.hexes.LifeSteal() + kPlaytestLeech;
    }
    // 两种模式共用：接住死亡、开层回血、复活符、判负
    void FightRules(const Target& p, const tower::Mods& m) {
        if (deathAt_ >= 0) { DeathTick(p); return; }
        ReviveGuard();
        if (now_ < reviveSafeUntil_) for (int o : taken_) attr::Want(p, attr::AttrCurrent(p, o), Kind::Scale, 0.0f);  // 复活起身无敌
        if (towerentry::Holding()) return;
        if (run_.phase != tower::Phase::Fighting) return;
        if (rebirthCheck_ && fightSince_ > 0) { rebirthCheck_ = false; towerentry::Unstick(std::format("第 {} 层开打", run_.floor).c_str()); }
        // 接住死亡：生命下限抬到 1，掉到 1 就判负（不走游戏原生死亡，不丢魂、不回钟）
        // 用户 2026-11-12：碧海盾破爆炸、舞狮二阶段火球等异常技能一下 / 两下秒杀。开打后每帧把生命下限抬到「当前生命 − 40% 上限」：
        // 单帧（一次或同帧多次命中）最多掉 40% 上限，至少三下才能打死；血量低于 40% 时下限回到 1，正常可以被打死
        float hpFloor = 1.0f;
        if (fightSince_ > 0 && !towerentry::Holding()) {
            const float hpNow = ue::Get<float>(attr::AttrCurrent(p, hp_)), mxNow = ue::Get<float>(attr::AttrCurrent(p, hpMax_));
            if (std::isfinite(hpNow) && std::isfinite(mxNow) && mxNow > 0) {
                if (spikeFloor_ > 1.0f && hpNow <= spikeFloor_ + 0.5f && lastSpikeHp_ - hpNow > 0) {  // 上一帧被下限截住：记一次，便于找出异常技能
                    std::string who;
                    for (auto& a : actors_) if (!a.dead && ue::Live(a.obj)) who += (who.empty() ? "" : "、") + a.name;
                    pbz::log::Info("影塔重击截断：第 {} 层单帧伤害超过生命上限 40%，已截到 40%（{:.0f} → {:.0f}，在场：{}）", run_.floor, lastSpikeHp_, hpNow, who.empty() ? "-" : who);
                }
                hpFloor = std::max(1.0f, hpNow - static_cast<float>(tower::balance::kSpikeCap) * mxNow);
                lastSpikeHp_ = hpNow;
                // 3.2.0 取证（玩家反馈：雷当 / 长鸿一下秒掉大量生命）：1 秒内累计掉血超过上限 50% 记一行（单帧已截到 40%，多段连击会绕过）
                if (now_ - burstSince_ > 1.0 || hpNow > burstHp_) { burstSince_ = now_; burstHp_ = hpNow; burstLogged_ = false; }
                else if (!burstLogged_ && burstHp_ - hpNow > 0.5f * mxNow && burstLogs_ < 30) {
                    burstLogged_ = true; ++burstLogs_;
                    std::string who;
                    for (auto& a : actors_) if (!a.dead && ue::Live(a.obj)) who += (who.empty() ? "" : "、") + a.name;
                    pbz::log::Info("影塔连击重伤：第 {} 层 {:.2f} 秒内 {:.0f} → {:.0f}（上限 {:.0f}，掉 {:.0f}%），在场：{}", run_.floor, now_ - burstSince_, burstHp_, hpNow, mxNow,
                                   (burstHp_ - hpNow) / mxNow * 100, who.empty() ? "-" : who);
                }
            }
        }
        // 3.2.0（用户：投技很长的 Boss，复活后还在吃伤害又死一次）：复活起身 4 秒内生命锁满（下限 = 上限），
        // 与受伤 ×0 一起生效；投技、真实伤害之类不走受伤倍率的也扣不下来。复活符、杀意不灭都走这里
        if (now_ < reviveSafeUntil_ && hpMax_ >= 0) {
            const float mxNow = ue::Get<float>(attr::AttrCurrent(p, hpMax_));
            if (std::isfinite(mxNow) && mxNow > 1) { hpFloor = mxNow; if (ue::Get<float>(attr::AttrCurrent(p, hp_)) < mxNow) SetHp(p, mxNow); }
        }
        spikeFloor_ = hpFloor;
        // 3.1.0 取证（玩家反馈：斩马刀重击的临时损血在塔里不扣血）：性能日志构建里记玩家生命的每次明显变化，带上本帧的下限、上限与影塔吸血
        if (towerdev::kPerfLog && fightSince_ > 0 && hpTraceLogs_ < 300) {
            const float hpNow = ue::Get<float>(attr::AttrCurrent(p, hp_)), mxNow = ue::Get<float>(attr::AttrCurrent(p, hpMax_));
            if (hpTraceLast_ >= 0 && mxNow > 0 && std::abs(hpNow - hpTraceLast_) > 0.005f * mxNow) {
                ++hpTraceLogs_;
                const auto l = towernative::ReadLoadout();
                pbz::log::Info("影塔生命轨迹：{:.0f} → {:.0f} / {:.0f}（上限前帧 {:.0f}），生命下限 {:.0f}，影塔吸血 {}，手持 {}", hpTraceLast_, hpNow, mxNow, hpTraceMax_, hpFloor,
                               leechWroteAt_ == now_ ? "本帧写入" : "无", l.ok && !l.main.empty() ? l.main[0] : std::string("?"));
            }
            hpTraceLast_ = hpNow; hpTraceMax_ = mxNow;
        }
        attr::Want(p, attr::AttrBase(p, hpMin_), Kind::Set, hpFloor);
        attr::Want(p, attr::AttrCurrent(p, hpMin_), Kind::Set, hpFloor);
        if (travelThenSpawn_ || spawnAt_ > 0 || (pending_ > 0 && actors_.empty())) return;
        if (replay_ && fightSince_ <= 0) return;  // 重放层：第一阶段还没到场（玩家还没开遭遇 / 初见过场中）  // 还在去场地 / 等生成：Boss 没出来，不判胜负（错峰时第一只出来就开始算）
        towercombat::Sample(p.obj, ue::Get<float>(attr::AttrCurrent(p, hp_)));
        if (fightSince_ > 0) {  // 附加挑战：受伤 = 生命下降；喝药 = 生命 0.5 秒内回升超过上限 15%（静态判据，待实机标定）
            const float hpNow = ue::Get<float>(attr::AttrCurrent(p, hp_)), mxNow = ue::Get<float>(attr::AttrCurrent(p, hpMax_));
            if (lastHp_ >= 0) {
                // 无伤（v14）：伤害事件可观察时，只有伴随伤害事件的实际扣血才算（格挡后仍扣血算；武器技能主动耗血不经伤害结算，不算）；
                // 观察点在本层没出现过时退回"实际扣血即受伤"（日志写明）
                if (hpNow < lastHp_ - 0.5f && (!towercombat::PlayerDamageRoute() || towercombat::SincePlayerDamage() <= 0.6)) {
                    if (!fight_.hit) pbz::log::Info("影塔第 {} 层：受到伤害（{:.0f} → {:.0f}，{}）", run_.floor, lastHp_, hpNow, towercombat::PlayerDamageRoute() ? "伴随伤害事件" : "伤害事件不可观察，按扣血");
#ifdef PBZ_PLAYTEST
                    {   // 3.4.0 测试（用户：后期加了减伤 / 生命还是扛不住）：每层前 15 次掉血记下生命上限和影塔受伤倍率是否写进原生
                        static int hitFloor = -1, hitLogs = 0;
                        if (hitFloor != run_.floor) { hitFloor = run_.floor; hitLogs = 0; }
                        if (++hitLogs <= 15) pbz::log::Info("影塔受伤：第 {} 层（显示 {}）生命 {:.0f} → {:.0f}（−{:.0f}，上限 {:.0f}，−{:.0f}%）；影塔受伤倍率 ×{:.3f}，原生读回 ×{:.3f}",
                            run_.floor, ShownNow(), lastHp_, hpNow, lastHp_ - hpNow, mxNow, mxNow > 0 ? 100.0 * (lastHp_ - hpNow) / mxNow : 0.0,
                            grow_.hexes.liveTakenMul, grow_.hexes.liveTakenNative);
                    }
#endif
                    fight_.hit = true;
                }
                // 禁药（v14）：药瓶剩余次数减少即用药（回复药 / 增益药都算；MOD 符类、武器自带回血不算）；读不到次数时退回"生命骤升"
                const int uses = Game().MedicineUsesLeft();
                if (uses >= 0) {
                    if (medUses_ >= 0 && uses < medUses_ && !fight_.potion) { fight_.potion = true; pbz::log::Info("影塔第 {} 层：使用了药品（剩余次数 {} → {}）", run_.floor, medUses_, uses); }
                    medUses_ = uses;
                }
                else if (mxNow > 1 && hpNow - lastHp_ > 0.15f * mxNow) fight_.potion = true;
            }
            if (now_ >= nextHpSample_) { lastHp_ = hpNow; nextHpSample_ = now_ + 0.5; }
            else if (hpNow < lastHp_) lastHp_ = hpNow;
            // 3.6.0 反震：逐帧比较生命；伴随伤害事件的掉血 ÷ 影塔受伤倍率 = 减伤前的原始伤害，排给锁定目标（没锁定：15 米内最近的敌人）
            if (grow_.on && grow_.hexes.Has(tower::hex::Thorns) && deathAt_ < 0 && !towerentry::Holding() && std::isfinite(hpNow) && std::isfinite(mxNow)) {
                if (thornsHp_ >= 0 && now_ - thornsHpAt_ < 0.5 && hpNow < thornsHp_ - 0.5f && mxNow > 1 &&
                    (!towercombat::PlayerDamageRoute() || towercombat::SincePlayerDamage() <= 0.6)) {
                    const double taken = std::max(0.05, grow_.hexes.liveTakenMul > 0 ? grow_.hexes.liveTakenMul : 1.0);
                    const double frac = tower::hex::ThornsDamage((thornsHp_ - hpNow) / taken, mxNow, 1.0, grow_.hexes.ThornsShare());
                    if (UObject* target = frac > 0 ? ThornsTarget(p.obj) : nullptr) {
                        if (thornsTarget_ != target) thornsFrac_ = 0;
                        thornsTarget_ = target; thornsFrac_ += frac; thornsAt_ = now_;
                    }
                }
                thornsHp_ = hpNow; thornsHpAt_ = now_;
            } else thornsHp_ = -1;
            if (thornsFrac_ > 0 && now_ - thornsAt_ > 0.5) { thornsFrac_ = 0; thornsTarget_ = nullptr; }  // 目标这期间没出现在本层敌人里（死了 / 被换体）：作废
        }
        if (healPending_) {
            healPending_ = false;
            const float max = ue::Get<float>(attr::AttrCurrent(p, hpMax_));
            const float hp = std::min(max, ue::Get<float>(attr::AttrCurrent(p, hp_)) + static_cast<float>(m.HealFrac()) * max);
            ue::Write(attr::AttrBase(p, hp_), &hp, 4); ue::Write(attr::AttrCurrent(p, hp_), &hp, 4);
        }
        if (const float mx = ue::Get<float>(attr::AttrCurrent(p, hpMax_)); mx > 1)  // 本层最低血量比例（平衡用）
            minHpFrac_ = std::min(minHpFrac_, static_cast<double>(ue::Get<float>(attr::AttrCurrent(p, hp_)) / mx));
        float hpLow = ue::Get<float>(attr::AttrCurrent(p, hp_));
        const bool assist = AssistOn();
        const auto verdict = tower::fate::JudgePlayer(hpLow, now_ - fightSince_, assist, true);
        if (verdict == tower::fate::Verdict::Assisted) {  // 测试辅助开着：投技 / 处决 / 脚本伤害绕过 Core 倍率也不判负（21 层反例）
            if (assistCatches_++ < 5) pbz::log::Info("影塔第 {} 层：致死伤害被测试辅助接住（无敌 / 锁血开着；致死前 {:.0f} → {:.0f}），不判负，本层无伤挑战作废；最近受伤：{}", run_.floor, lastHp_, hpLow, towercombat::LastHits(3));
            fight_.hit = true;
            SetHp(p, ue::Get<float>(attr::AttrCurrent(p, hpMax_)));
            return;
        }
        if (reapPending_ > 0) {  // 收割者：每击败一个小怪回 3% 最大生命
            const int n = reapPending_; reapPending_ = 0;
            const float max = ue::Get<float>(attr::AttrCurrent(p, hpMax_));
            if (verdict == tower::fate::Verdict::Fine && max > 1 && hpLow > 0 && hpLow < max - .01f) {
                const float next = std::min(max, hpLow + max * static_cast<float>(grow_.hexes.ReaperHeal() * n));
                SetHp(p, next); towerwidgets::RefreshHpBarFast(hpLow, next);
                pbz::log::Info("影塔海克斯：收割者 {} 只，生命 {:.0f} → {:.0f}", n, hpLow, next);
                hpLow = next;
            }
        }
        if (verdict == tower::fate::Verdict::Fine && grow_.on && grow_.hexes.Has(tower::hex::Ocean) && fightSince_ > 0 && now_ >= nextOcean_) {
            nextOcean_ = now_ + 1.0;
            const float max = ue::Get<float>(attr::AttrCurrent(p, hpMax_));
            if (max > 1 && hpLow > 1 && hpLow < max - .01f) {
                const float next = std::min(max, hpLow + max * static_cast<float>(grow_.hexes.OceanRate()));
                SetHp(p, next); towerwidgets::RefreshHpBarFast(hpLow, next);
            }
        }
        if (verdict != tower::fate::Verdict::Fine)
            pbz::log::Info("影塔第 {} 层：玩家致死（{:.0f} → {:.0f}，距上次采样 ≤0.5 秒；测试辅助关）；最近受伤：{}", run_.floor, lastHp_, hpLow, towercombat::LastHits(3));
        // 3.7.0 生存战（用户 2026-11-15）：倒下不算爬塔失败——播殒命表现后回准备页、扣一半魂玉、不开驿站；
        // 不消耗杀意不灭 / 复活符。其他创意层能打死对面，倒下照常算失败
        if (verdict != tower::fate::Verdict::Fine && run_.phase == tower::Phase::Fighting && SpecialOn() && special_.kind == tower::special::Kind::Survival) {
            SetHp(p, 1);
            deathAt_ = now_; reviving_ = false; survivalLost_ = true;
            const bool shown = towerwidgets::DeathShow(true);
            pbz::log::Info("影塔创意层：生存战倒下（不算失败），{}", shown ? "播放殒命表现 4.5 秒后回准备页" : "直接回准备页");
            if (!shown) FinishDeath(p);
            return;
        }
        if (verdict != tower::fate::Verdict::Fine && grow_.on && shachiNow_ >= 0 && shachiMax_ >= 0 &&
            grow_.hexes.CanRevive(HexReviveUsedHere(), ue::Get<float>(attr::AttrCurrent(p, shachiNow_), 0.0f), ue::Get<float>(attr::AttrCurrent(p, shachiMax_), 0.0f))) {
            hexReviveRun_ = run_.id; hexReviveFloor_ = ShownNow(); hexReviving_ = true;
            SetHp(p, 1); revivePosOk_ = PawnLoc(revivePos_); deathAt_ = now_; reviving_ = true;
            pbz::log::Info("影塔第 {} 层：杀意不灭触发，优先于复活符", run_.floor);
            if (!towerwidgets::DeathShow(true)) FinishRevive(p);
            return;
        }
        // 1.0.1 用户 2026-11-07：复活符先播"殒命"（玩家要知道自己倒下了），4.5 秒后满血、满杀气、补满药 / 影武弹药复活，HUD 提示"复活符生效"
        if (verdict != tower::fate::Verdict::Fine && run_.phase == tower::Phase::Fighting && run_.items[static_cast<int>(tower::Item::Revive)] > 0) {
            SetHp(p, 1);
            revivePosOk_ = PawnLoc(revivePos_);  // 原地复活：记下倒下的位置
            deathAt_ = now_;
            reviving_ = true;
            const bool shown = towerwidgets::DeathShow(true);
            pbz::log::Info("影塔第 {} 层：倒下，有复活符：{}", run_.floor, shown ? "播放殒命表现 4.5 秒后满状态复活" : "殒命表现不可用，直接满状态复活");
            if (!shown) FinishRevive(p);
            return;
        }
        if (verdict != tower::fate::Verdict::Fine) {
            // 肉鸽规则（用户 2026-11-05）：一条命，倒下本局结束、整座塔从头打（复活符除外）。先冻结死亡这一刻的结算
            settlement_ = grow_.on ? DeathText(p) : Settlement(&p, false);
            // 用户 2026-11-06：不真死——先播原生倒地动作 + "殒命"书法（Boss 冻结、双方不再受伤），4.5 秒后再清场弹结算。
            // 原来直接清场，玩家只看到血条归零、Boss 消失，以为是 Boss 消失的 bug
            SetHp(p, 1);
            deathAt_ = now_;
            const bool shown = towerwidgets::DeathShow(true);
            pbz::log::Info("影塔第 {} 层：倒下，{}", run_.floor, shown ? "播放殒命表现 4.5 秒后结算" : "殒命表现不可用，直接结算");
            if (shown) return;
            FinishDeath(p);
        }
    }
    // 殒命表现期间（每帧）：玩家保持 ≥1 血、不受伤；到时撤下表现，走原来的判负流程
    void DeathTick(const Target& p) {
        for (int o : taken_) attr::Want(p, attr::AttrCurrent(p, o), Kind::Scale, 0.0f);
        if (ue::Get<float>(attr::AttrCurrent(p, hp_)) < 1) SetHp(p, 1);
        if (now_ - deathAt_ >= 4.5) { if (reviving_) FinishRevive(p); else FinishDeath(p); }
    }
    bool reviving_ = false, hexReviving_ = false;
    // 杀意不灭每个显示层一次：按（局，显示层）记，波次之间和技术性重试都不重置；新局、重生符回退后重新可用
    std::string hexReviveRun_; int hexReviveFloor_ = 0;
    bool HexReviveUsedHere() const { return hexReviveFloor_ > 0 && hexReviveRun_ == run_.id && hexReviveFloor_ == ShownNow(); }
    double reviveNoticeUntil_ = 0;
    double revivePos_[3] = {}; bool revivePosOk_ = false; double reviveGuardUntil_ = 0;
    double reviveSafeUntil_ = 0;  // 用户 2026-11-08：复活起身马上吃技能又死一次：起身无敌到这个时刻
    // Only one correction is allowed; never repeatedly pull a moving player back.
    void ReviveGuard() {
        if (reviveGuardUntil_ <= 0) return;
        if (now_ > reviveGuardUntil_ || run_.phase != tower::Phase::Fighting) { reviveGuardUntil_ = 0; return; }
        double at[3];
        if (!PawnLoc(at) || std::hypot(at[0] - revivePos_[0], at[1] - revivePos_[1], at[2] - revivePos_[2]) < 500) return;
        const bool ok = MoveActor(towerwidgets::Pawn(), {revivePos_[0], revivePos_[1], revivePos_[2]});
        reviveGuardUntil_ = 0;
        pbz::log::Info("影塔第 {} 层：复活后被挪开 {:.0f} 米，放回倒下的位置{}", run_.floor,
                       std::hypot(at[0] - revivePos_[0], at[1] - revivePos_[1], at[2] - revivePos_[2]) / 100, ok ? "" : "（失败，稍后再试）");
    }
    // 复活符：撤下殒命表现，扣一个，满血 / 满杀气 / 补满药与影武弹药（同点"下一层"的补满），本层计时和挑战照常继续
    void FinishRevive(const Target& p) {
        towerwidgets::DeathShow(false);
        deathAt_ = -1;
        reviving_ = false;
        const bool byHex = hexReviving_; hexReviving_ = false;
        if (!byHex && !run_.TryRevive()) { FinishDeath(p); return; }
        SetHp(p, ue::Get<float>(attr::AttrCurrent(p, hpMax_)));
        if (byHex) {  // 用户 2026-11-11：杀意不灭 = 满血复活、杀气清空，不补药
            if (shachiNow_ >= 0) { const float zero = 0; ue::Write(attr::AttrCurrent(p, shachiNow_), &zero, 4); ue::Write(attr::AttrBase(p, shachiNow_), &zero, 4); }
            towerwidgets::RefreshHpBar();
        } else if (!towerentry::TopUpNow()) towerentry::FullHealVisible("复活符");
        reviveNoticeUntil_ = now_ + 6;
        // Correct a large displacement immediately, before returning control.
        // Do not continue monitoring player movement after the revive.
        reviveGuardUntil_ = revivePosOk_ ? now_ + 0.01 : 0;
        ReviveGuard(); reviveGuardUntil_ = 0;
        reviveSafeUntil_ = now_ + 4;                  // 无敌挡住已经出手的技能和飞行物
        freezeUntil_ = std::max(freezeUntil_, now_ + 2);  // 敌人短暂定身，起身时不被连续打僵直
        pbz::log::Info("影塔第 {} 层：{}生效，{}（复活符剩 {} 个）", run_.floor, byHex ? "杀意不灭" : "复活符", byHex ? "满血复活、杀气清空" : "满状态复活", run_.items[static_cast<int>(tower::Item::Revive)]);
        Broadcast("info.revived", byHex ? "杀意不灭生效" : "复活符生效");
    }
    // 1.0.1 重生符（用户 2026-11-07）：没有复活符、背包里有重生符 → 殒命后先问用不用；用 = 退后 10 层回准备页（10 层以内倒下回第 0 层），
    // 不用 / 没有 = 照原来结算。只在 50 层正式局（成长模式、不是第 51 层）里生效
    bool rebirthAsk_ = false;
    bool mobWideRoom_ = false;        // 3.6.0：本波小怪改用整个安全区刷（中心挤不下时）
    int lastBossVenue_ = -1;          // 刚打完的 Boss 场地（池下标）：下一段波次层就地打；-1 = 走本区经典场地
    int deathShown_ = 0;              // 倒下那层的显示层号（波次段倒下时内部层号还没加）
    bool rebirthAskInMobs_ = false;   // 等玩家选重生符时记着：倒在波次段
    bool rebirthReload_ = false;      // 用了重生符，等游戏静下来按世界模板重建塔档
    uint64_t rebirthReloadUntil_ = 0, rebirthReloadNext_ = 0;
    bool rebirthCheck_ = false;  // 用过重生符：下一层开打时再检查一次行动锁（towerentry::Unstick）
  public:
    int RebirthCount() const { return run_.items[static_cast<int>(tower::Item::Rebirth)]; }
    bool RebirthOffered() const { return rebirthAsk_; }
    int ShownBoss(int f) const { return tower::floorplay::ShownBoss(f, run_.floors); }
    // 当前显示层号：倒下的那层 > 波次段 > 刚通过的波次段（到 Boss 前的驿站 / 读档接着打）> Boss 层
    int ShownNow() const {
        if (deathShown_ > 0) return deathShown_;
        if (run_.inMobWave) return tower::floorplay::ShownMobs(mob_.floor, run_.floors);
        if (run_.specialStage && specialF_ > 0) return tower::floorplay::ShownMobs(specialF_, run_.floors);  // 3.7.0 主线创意层显示小兵段层号
        if (const int F = run_.floor + 1; run_.mobFloor == F && run_.mobWave > 0) return tower::floorplay::ShownMobs(F, run_.floors);
        return ShownBoss(run_.floor);
    }
    // 下一场要打的层：和 Next() 同一套判断
    int ShownNext() const {
        const int F = run_.floor + 1;
        const bool mobs = grow_.on && !mobOff_ && !s_hidden && !run_.hidden && plan_.empty() && fixedBoss_ < 0 && tower::mobs::HasStage(F, run_.floors) && !MobStageDone(F);
        return mobs ? tower::floorplay::ShownMobs(F, run_.floors) : ShownBoss(F);
    }
    // 无尽层：重生符退 10 层，最低退到第 100 层（准备页停在第 50 层之后）
    int RebirthNext() const { return run_.endless && run_.floor > 50 ? ShownBoss(RebirthTarget() + 1) : run_.floors < 50 ? RebirthTarget() + 1 : tower::floorplay::RebirthDest(deathShown_); }  // 回退后接着打的层（显示层号）
    int RebirthTarget() const { if (run_.endless && run_.floor > 50) return std::max(50, run_.floor - 10); return run_.floors < 50 ? (run_.floor > 10 ? run_.floor - 10 : 0) : tower::floorplay::RebirthBack(tower::floorplay::RebirthDest(deathShown_)); }  // 回到"已通过 N 层"的准备页，下一层开 N+1
  private:
    void FinishDeath(const Target& p) {
        if (std::exchange(survivalLost_, false)) { FinishSurvivalLoss(p); return; }
        deathShown_ = ShownNow();
        const bool diedInMobs = run_.inMobWave || run_.specialStage;
        run_.inMobWave = false; mob_ = {};
        towerwidgets::DeathShow(false);
        deathAt_ = -1;
        if (grow_.on && !s_hidden && !run_.hidden && (run_.floor <= 50 || run_.endless) && (RebirthCount() > 0 || CheckpointShown() > 0) && run_.phase == tower::Phase::Fighting) {
            ReleaseShentong();
            Clear("lost");
            actors_.clear();
            const float max = ue::Get<float>(attr::AttrCurrent(p, hpMax_));
            ue::Write(attr::AttrBase(p, hp_), &max, 4); ue::Write(attr::AttrCurrent(p, hp_), &max, 4);
            run_.Died();
            rebirthAsk_ = true; rebirthAskInMobs_ = diedInMobs;
            Conclude();  // 停在殒命页（保护保持）；局存档先不删，等玩家选
            pbz::log::Info("影塔第 {} 层倒下：没有复活符，有重生符 {} 个，等玩家选择（用了回到第 {} 层准备页）", run_.floor, RebirthCount(), RebirthTarget());
            Broadcast("info.rebirth_ask", "可以使用重生符", std::format(R"("floor":{},"count":{})", run_.floor, RebirthCount()));
            return;
        }
        {
            RunLog(LostLine(diedInMobs, static_cast<int>(std::lround(std::max(0.0, FightSeconds()) * 10)), LoggedBossNames()));
            run_.Died();
            ReleaseShentong();
            DeleteRun();
            pbz::log::Info("影塔第 {} 层失败", run_.floor);
            Clear("lost");
            actors_.clear();
            const float max = ue::Get<float>(attr::AttrCurrent(p, hpMax_));  // 回满血再还原，免得一出塔就死
            ue::Write(attr::AttrBase(p, hp_), &max, 4); ue::Write(attr::AttrCurrent(p, hp_), &max, 4);
            Broadcast("info.lost", "失败", std::format(R"("floor":{})", run_.floor));
            Conclude();
        }
    }

    // 续档 / 存档点共用：把读出来的成长档内容放进当前局（塔档与原生同步由调用方负责）
    void AdoptGrowth(const tower::growthrun::Runner& saved) {
        grow_.equip = saved.equip;
        grow_.profile = saved.profile; grow_.med = saved.med; grow_.acc = saved.acc; grow_.bonus = saved.bonus;
        grow_.hexRewardSecond = saved.hexRewardSecond; grow_.hexBonusSecond = saved.hexBonusSecond;
        grow_.hexAttributes = saved.hexAttributes;
        grow_.hexForgePurchases = saved.hexForgePurchases;
        grow_.bless = saved.bless; grow_.offers = saved.offers; grow_.offered = saved.offered; grow_.seen = saved.seen;
        grow_.rewardRng = saved.rewardRng; grow_.shop = saved.shop; grow_.prep = saved.prep; grow_.quick = saved.quick;
        grow_.buys = saved.buys; grow_.itemBuys = saved.itemBuys;
        grow_.attrs = saved.attrs; grow_.refresh = saved.refresh; grow_.offerFloor = saved.offerFloor; grow_.lastGroup = saved.lastGroup; grow_.draws = saved.draws;
        grow_.hexes = saved.hexes;
        grow_.arsenal = saved.arsenal;
        // 3.4.1（无尽存档点审计）：这几项以前漏了——续档时丢失、回存档点时留着存档点之后的值（回收池、本次驿站交易次数、背包认回标记、锻造进行中、待核实兵器）
        grow_.recycled = saved.recycled; grow_.pendingRecycle = saved.pendingRecycle; grow_.backpackSynced = saved.backpackSynced;
        grow_.tradeVisit = saved.tradeVisit; grow_.hexJournal = saved.hexJournal; grow_.held = saved.held;
    }
    // ---- 无尽模式存档点（用户 2026-11-12）：显示第 100、200 … 层开打前记下整局（武器、点数、海克斯、魂玉、道具），
    // 死在之后的层可以「从第 N 层开始」：整局回到那一刻（海克斯同样回退），不限次数；与重生符并存
    // 3.4.5（用户）：存档点从「每 100 层」改为显示 100 层起「每 20 层」（100 / 120 / 140 …），每个单独一份（无尽档导出可任选）；
    // 死后回最近的一个。旧版的单一存档点（checkpoint-run.txt）照样认，作为兜底
    static std::wstring CheckpointRunFile(int shown = 0) { const auto d = TowerDir(); return d.empty() ? d : shown > 0 ? d + std::format(L"\\checkpoint-{}-run.txt", shown) : d + L"\\checkpoint-run.txt"; }
    static std::wstring CheckpointGrowthFile(int shown = 0) { const auto d = TowerDir(); return d.empty() ? d : shown > 0 ? d + std::format(L"\\checkpoint-{}-growth.txt", shown) : d + L"\\checkpoint-growth.txt"; }
    static bool CheckpointFloor(int shown) { return shown >= 100 && shown % 20 == 0; }
    void SaveCheckpoint() {
        SaveRun();
        const int shown = ShownBoss(run_.floor + 1);
        const bool ok = CopyFileW(RunFile().c_str(), CheckpointRunFile(shown).c_str(), FALSE) && CopyFileW(GrowthFile().c_str(), CheckpointGrowthFile(shown).c_str(), FALSE);
        pbz::log::Info("影塔无尽模式：第 {} 层存档点{}", shown, ok ? "已记录" : "写入失败");
        if (ok) Broadcast("info.checkpoint_saved", std::format("第 {} 层存档点已记录", shown));
    }
    // 本局可回到的最近存档点：返回显示层号，并给出文件（找不到返回 0）。新格式按层号从近到远找，最后看旧版单一存档点
    int FindCheckpoint(std::wstring* runFile, std::wstring* growthFile) const {
        if (!run_.endless) return 0;
        auto ok = [&](const std::wstring& rf, tower::Run* cp) {
            const auto text = ReadText(rf);
            return !text.empty() && cp->Load(text) && cp->id == run_.id && cp->endless && cp->floor + 1 <= run_.floor;
        };
        for (int shown = ShownBoss(run_.floor); shown >= 100; --shown) {
            if (!CheckpointFloor(shown)) continue;
            tower::Run cp;
            if (ok(CheckpointRunFile(shown), &cp)) { if (runFile) *runFile = CheckpointRunFile(shown); if (growthFile) *growthFile = CheckpointGrowthFile(shown); return ShownBoss(cp.floor + 1); }
        }
        tower::Run cp;
        if (ok(CheckpointRunFile(), &cp)) { if (runFile) *runFile = CheckpointRunFile(); if (growthFile) *growthFile = CheckpointGrowthFile(); return ShownBoss(cp.floor + 1); }
        return 0;
    }
  public:
    // 本局可用的存档点（显示层号）；没有返回 0
    int CheckpointShown() const { return FindCheckpoint(nullptr, nullptr); }
  private:
    void CheckpointRestore() {
        if (!rebirthAsk_ || run_.phase != tower::Phase::Lost) { Broadcast("err.checkpoint", "现在不能回到存档点"); return; }
        tower::Run cp; tower::growthrun::Runner saved;
        std::wstring rf, gf;
        if (!FindCheckpoint(&rf, &gf)) { Broadcast("err.checkpoint", "存档点读取失败"); return; }
        const auto runText = ReadText(rf), growText = ReadText(gf);
        if (runText.empty() || !cp.Load(runText) || cp.id != run_.id || !cp.endless || growText.empty() || !tower::growthrun::Runner::Load(growText, &saved)) {
            Broadcast("err.checkpoint", "存档点读取失败"); return;
        }
        rebirthAsk_ = false;
        const int died = run_.floor;
        run_ = cp;  // 存档点 = 整局全部数据回到那一刻（最高层、驿站、海克斯一起回退；用户 2026-11-12 再次确认）
        AdoptGrowth(saved);
        grow_.catalog = LoadCatalog();
        baseAtk_ = baseHp_ = 0; growHpComp_ = growDmgComp_ = 1;
        hexReviveFloor_ = 0; settlement_.clear(); deathShown_ = 0; lastBossVenue_ = -1;
        floorNote_ = std::format("已回到第 {} 层存档点", ShownBoss(run_.floor + 1));
        RunLog(std::format("checkpoint\t{}\t{}", died, run_.floor));
        SaveRun();
        towerentry::Unstick("回到存档点");
        rebirthCheck_ = true;
        rebirthReload_ = grow_.on && trip_.phase() == tower::trip::Phase::Inside;  // 同重生符：按存档点的成长档重建塔档（武器 / 背包回到那一刻）
        rebirthReloadUntil_ = GetTickCount64() + 15000; rebirthReloadNext_ = 0;
        pbz::log::Info("影塔无尽模式：第 {} 层倒下，回到第 {} 层存档点", died, ShownBoss(run_.floor + 1));
        Broadcast("info.checkpoint", floorNote_);
        RebirthReloadTick();
    }
    void RebirthDecide(bool use) {
        if (!rebirthAsk_ || run_.phase != tower::Phase::Lost) { Broadcast("err.rebirth", "现在不能使用重生符"); return; }
        rebirthAsk_ = false;
        if (use && RebirthCount() > 0) {
            const int died = run_.floor, back = RebirthTarget(), dest = RebirthNext();
            --run_.items[static_cast<int>(tower::Item::Rebirth)];
            hexReviveFloor_ = 0;  // 回退后重打的层是新的一次尝试
            // 退到波次层从头打；退到 Boss 层则直接打 Boss。随机种子、解锁兵器和最高进度保留。
            const int waves = run_.endless && back >= 50 ? tower::mobs::WaveCount(back + 1)  // 无尽层没有波次段
                            : run_.floors >= 50 ? (tower::floorplay::RebirthToMobs(dest) ? 0 : tower::mobs::WaveCount(back + 1)) : -1;
            run_.RewindForRebirth(back, waves);
            grow_.offered = false; grow_.bonus.clear(); grow_.shop = {};
            settlement_.clear(); floorNote_.clear();
            RunLog(std::format("rebirth\t{}\t{}", died, back));
            SaveRun();
            towerentry::Unstick("回到准备页");
            rebirthCheck_ = true;  // 下一次真正开打（读秒结束 / 重放第一阶段到场）再查一次
            deathShown_ = 0; lastBossVenue_ = -1;
            // 用户 2026-11-08：第 19 层舞狮打过一次后场景被改，回退再上来互动开不了剧情，大退重进才行。
            // 回退后按世界模板重建一次塔档（回标题再继续，和大退重进走同一套载入），打过的层的场景 / 剧情状态都还原
            rebirthReload_ = grow_.on && trip_.phase() == tower::trip::Phase::Inside;
            rebirthReloadUntil_ = GetTickCount64() + 15000; rebirthReloadNext_ = 0;
            pbz::log::Info("影塔：第 {} 层倒下，使用重生符（剩 {} 个），回到第 {} 层准备页（显示第 {} 层）", died, RebirthCount(), back, dest);
            Broadcast("info.rebirth", std::format("使用重生符：回到第 {} 层", dest));
            RebirthReloadTick();
            return;
        }
        // 不用：照原来的死亡结算
        RunLog(LostLine(rebirthAskInMobs_, 0, ""));
        DeleteRun();
        pbz::log::Info("影塔第 {} 层失败（未使用重生符）", run_.floor);
        Broadcast("info.lost", "失败", std::format(R"("floor":{})", run_.floor));
    }

    // 回退后重载世界模板：游戏可能还在收尾（Boss 刚移除要等 3 秒），Calm 不满足就稍后再试；一直不行就按原样留在准备页
    void RebirthReloadTick() {
        if (!rebirthReload_) return;
        if (run_.phase != tower::Phase::Ready || trip_.phase() != tower::trip::Phase::Inside) { rebirthReload_ = false; return; }
        const auto t = GetTickCount64();
        if (t < rebirthReloadNext_) return;
        rebirthReloadNext_ = t + 500;
        if (trip_.Rebuild(Io(), grow_.profile, tower::save::SavedEquip(grow_.profile, grow_.equip), Stamp())) {
            rebirthReload_ = false;
            SaveTrip();
            pbz::log::Info("影塔：重生符回退，按世界模板重建塔档，回标题重新载入（{}）", grow_.equip.empty() ? "默认装备" : "按当前装备");
            Broadcast("info.enter_reload", "正在还原世界");
        } else if (t >= rebirthReloadUntil_) {
            rebirthReload_ = false;
            pbz::log::Info("影塔：重生符回退后没能重载世界模板（{}），留在准备页继续", trip_.error());
        }
    }
    // 战绩日志：倒在波次段时记"接下来的 Boss 层"，再带上显示层号，免得把已通过的上一层标成倒下
    std::string LostLine(bool inMobs, int tenths, const std::string& names) const {
        return std::format("lost\t{}\t{}\t{}\t{}", run_.floor + (inMobs ? 1 : 0), tenths, names, deathShown_);
    }

    // ---- 1.4.0 小怪波次：开第 F 层 Boss 前，在本区经典场地一轮一轮刷小怪；每轮清完属性三选一，选完马上下一轮
    struct MobStage { bool on = false; int floor = 0, wave = 0, total = 0; double waveAt = 0; } mob_;
    std::unordered_set<int> mobBad_;  // 本局生成失败的小怪（下标 kMobs）
    tower::wave::Recovery waveRecovery_;
    std::unordered_set<int> waveAvoid_, waveBadVenues_;
    bool mobOff_ = false;             // 跳层工具开局可关（验收指定层用）
    bool MobStageDone(int F) const { return run_.mobFloor > F || (run_.mobFloor == F && run_.mobWave >= tower::mobs::WaveCount(F)); }
    void StartMobStage(int F) {
        venuePath_.clear();  // 波次场地只用 Boss 原生场地
        EndlessReset();
        // A prior cleanup may have failed. Never start a retry with old tower
        // enemies still registered, or they can contaminate the next wave.
        if (const size_t left = pbz::enemies::ClearTagged("tower-"); left > 0) {
            const auto why = std::format("上一场仍有 {} 个影塔角色无法清理，未开始小怪关；请稍后重试", left);
            RecordFailure("wave_cleanup", -1, why);
            Broadcast("err.spawn_failed", why);
            return;
        }
        const auto* def = tower::floorplay::Find(F);
        const int done = run_.mobFloor == F ? run_.mobWave : 0;
        if (waveRecovery_.Begin(F, done)) { waveAvoid_.clear(); waveBadVenues_.clear(); }
        const int classic = def && def->zone >= 1 && def->zone <= 5 ? tower::floorplay::PoolIndex(tower::floorplan::kZoneClassic[def->zone - 1]) : -1;
        auto usable = [&](int n) { return n >= 0 && n < static_cast<int>(std::size(tower::kBossPool)) && !waveBadVenues_.count(n) &&
            !NativeBad(n) && towertravel::HasVenue(tower::kBossPool[n].path, tower::kBossPool[n].identity); };
        int saved = -1;
        // 用户 2026-11-08：跨图加载太久。波次层就地在刚打完的 Boss 场地打，只回场内起点，不用等地形流送
        if (run_.mobVenueFloor == F && !run_.mobVenuePath.empty()) {
            saved = tower::floorplay::PoolIndex(run_.mobVenuePath);
            if (usable(saved)) {
                pbz::log::Info("影塔续波：恢复第 {} 场前的小怪场地 {}", F, tower::kBossPool[saved].name);
            } else {
                // An unrecognized, unavailable or failed venue must never be used as an arbitrary path.
                pbz::log::Info("影塔续波：原场地不可用，将改用可用的波次场地（进度保留）");
                Broadcast("info.wave_venue_changed", "原小怪场地不可用，将换场地重试，波次进度保留");
            }
        }
        std::array<int, 5> classics{};
        for (size_t i = 0; i < classics.size(); ++i) classics[i] = tower::floorplay::PoolIndex(tower::floorplan::kZoneClassic[i]);
        const int v = tower::wave::ChooseVenue(saved, lastBossVenue_, classic, classics, usable);
        mob_ = {true, F, done, tower::mobs::WaveCount(F), now_};
        PreloadWave(F, done);
        if (!usable(v)) {
            failedStage_ = {run_.id, run_.floor, run_.floor, F, mob_.total, true, {}};
            mob_ = {};
            floorNote_ = "本层没有可用的小怪场地，请重试";
            pbz::log::Info("影塔小怪：第 {} 层前没有可用场地，保留进度", F);
            SaveRun(); Broadcast("err.spawn_failed", floorNote_);
            return;
        }
        run_.mobVenueFloor = F;
        run_.mobVenuePath = std::string(tower::kBossPool[v].path);
        SaveRun();  // 即使第一波途中退出，也能恢复本段实际选择的场地。
        run_.phase = tower::Phase::Fighting;
        run_.inMobWave = true;
        run_.bosses.clear();
        count_ = 0; replay_ = nullptr; sched_ = false; staggerFloor_ = false; relay_.Clear();
        travelled_ = false; venueArrived_ = false; fightSince_ = 0;
        fight_ = {}; floorNote_.clear();
        towerentry::Begin(true);  // 用户 2026-11-08：波次层不读秒
        const auto& lead = tower::kBossPool[v];
        venueBoss_ = v;
        // 3.6.0（用户）：小场地 Boss（花千笑 / 囚锋 / 鬼差类 / 实测 < 20 米）后面的小怪层一律去榕树之心，Boss 层本身不动
        if (tower::endless::SmallVenue(lead.path)) venuePath_ = std::string(tower::endless::kRongshu);
        towerisolate::Begin(lead.identity);
        towerisolate::SetReplay(nullptr);
        towertravel::RecordHome();
        pbz::log::Info("影塔小怪：第 {} 层前 {} 轮（从第 {} 轮开始），场地 {}{}", F, mob_.total, done + 1, lead.name, venuePath_.empty() ? "" : "（小场地，改去榕树之心）");
        const int went = venuePath_.empty() ? towertravel::GoFloor(run_.Zone(), run_.seed, lead.path, lead.identity, lead.name, lead.scene)
                                            : towertravel::GoFloor(run_.Zone(), run_.seed, venuePath_, "", lead.name, false);
        travelled_ = went >= 0;
        arrivalRetries_ = 0;
        if (went < 0) { CancelWave("无法进入小怪场地", tower::wave::Failure::Venue); return; }
        if (went == 1) { travelThenSpawn_ = true; Broadcast("info.traveling", "前往场地…"); return; }
        spawnAt_ = now_ + 0.01; spawnTries_ = 0; transientTries_ = 0; audited_ = false; floorAudit_ = {}; gateSince_ = -1;
    }
    void SpawnWave() {
        const auto* def = tower::floorplay::Find(mob_.floor);
        const auto plan = tower::mobs::Plan(mob_.floor, mob_.wave, mob_.total);
        if (waveRecovery_.Begin(mob_.floor, mob_.wave)) { waveAvoid_.clear(); waveBadVenues_.clear(); }
        const auto picks = tower::mobs::Pick(run_.seed, mob_.floor, mob_.wave, def ? def->zone : 1, plan, [&](int i) { return mobBad_.count(i) > 0 || waveAvoid_.count(i) > 0; });
        mob_.waveAt = now_;
        actors_.clear();
        before_ = pbz::enemies::OwnedActors();
        failed_ = noRoom_ = transient_ = false;
        pending_ = static_cast<int>(picks.size());
        pbz::log::Info("影塔小怪：第 {} 层前 第 {}/{} 轮，普通 {} 精英 {}", mob_.floor, mob_.wave + 1, mob_.total, plan.normal, plan.elite);
        if (picks.size() != static_cast<size_t>(plan.normal + plan.elite)) { CancelWave("这一区缺少可用的小怪或精英，本波不结算", tower::wave::Failure::NoCandidates); return; }
        PreloadWave(mob_.floor, mob_.wave + 1);  // 打这一波时后台加载下一波
        mobQueue_.clear(); mobActive_ = 0; mobRejected_.clear(); mobWideRoom_ = false;
        for (size_t i = 0; i < picks.size(); ++i) mobQueue_.push_back({picks[i], 500 + 90.0 * static_cast<double>(i), 0});
        PumpMobs();
        Broadcast("info.mob_wave", std::format("第 {} 层：{}/{}", tower::floorplay::ShownMobs(mob_.floor, run_.floors), mob_.wave + 1, mob_.total));
    }
    // 用户 2026-11-08：换波很卡。刷一只怪要在游戏线程建整个角色，几只挤在同一帧就是明显掉帧：一次只发一只，隔 0.1 秒再发下一只
    struct MobJob { int index; double distance; int tries; };
    std::vector<MobJob> mobQueue_;
    int mobActive_ = 0;
    tower::wave::RequestWatch mobRequestWatch_;
    tower::wave::RequestWatch relayRequestWatch_;
    std::unordered_set<int> mobRejected_;  // 这一波里已经刷失败的怪
    double mobNextAt_ = 0;
    bool WaveSpawnRegion(tower::arena::P3& center, double& innerRadius) {
        std::vector<tower::arena::P3> hull;
        double radius = 0, lo = 0, hi = 0;
        if (!tvenue::Boundary(&hull, &center, &radius, &lo, &hi)) return false;
        tower::arena::Boundary area;
        if (!hull.empty()) {
            // The native boss spawner itself can sit near a fog gate. The mean
            // of a convex hull's vertices lies inside the hull and is a more
            // useful stable reference for all waves in this venue.
            center.x = 0; center.y = 0;
            for (const auto& p : hull) { center.x += p.x / hull.size(); center.y += p.y / hull.size(); }
            area.ArmHull(hull, center, lo, hi);
        }
        else { area.Arm(center, radius); area.zmin = lo; area.zmax = hi; }
        innerRadius = area.InnerRadius(250, 1400);
        return std::isfinite(center.x) && std::isfinite(center.y) && innerRadius >= 300;
    }
    void PumpMobs() {
        if (mobActive_ > 0 || mobQueue_.empty()) return;
        const auto next = mobQueue_.front();
        mobQueue_.erase(mobQueue_.begin());
        ++mobActive_;
        mobRequestWatch_.Begin();
        mobNextAt_ = now_ + 0.1;
        SubmitMob(next.index, next.distance, next.tries);
    }
    void PreloadWave(int F, int wave) {
        if (!tower::mobs::HasStage(F, run_.floors)) return;
        const int total = tower::mobs::WaveCount(F);
        if (wave < 0 || wave >= total) return;
        const auto* def = tower::floorplay::Find(F);
        for (int i : tower::mobs::Pick(run_.seed, F, wave, def ? def->zone : 1, tower::mobs::Plan(F, wave, total), [&](int j) { return mobBad_.count(j) > 0; }))
            towernative::PreloadClass(std::string(tower::mobs::kMobs[i].path));
    }
    void SubmitMob(int index, double distance, int tries) {
        const auto& m = tower::mobs::kMobs[index];
        tower::arena::P3 center; double radius = 0;
        if (!WaveSpawnRegion(center, radius)) { CancelWave("无法确认小怪的场内安全区域", tower::wave::Failure::Venue); return; }
        pbz::enemies::Request r;
        r.command = "spawn";
        r.path = std::string(m.path);
        r.requestId = std::format("tower-{}-mob{}-{}-", run_.id, mob_.floor, ++spawnSeq_);
        r.count = 1;
        // 3.6.0（用户）：所有场地通用的保险——小怪落点收到场地中心，区域半径取安全区的一半（最少 3 米）；落地 / 地面检测由核心按区域做
        // 3.6.0 打包前（铁狂小场地实机：8 只挤不下、整波卡住）：半径减半后核心报「没有足够可落地」→ 本波退回整个安全区再刷
        const double spawnR = mobWideRoom_ ? std::max(300.0, radius) : std::clamp(radius * 0.5, 300.0, std::max(300.0, radius));
        r.distanceCm = std::clamp(distance, 200.0, std::max(200.0, spawnR - 100.0));
        r.faction = std::format("tower-region {:.3f} {:.3f} {:.3f}", center.x, center.y, spawnR);
        const auto request = r.requestId; const auto epoch = spawnEpoch_;
        pbz::enemies::Submit(std::move(r), [this, index, distance, tries, request, epoch](std::string json) {
            if (spawnEpoch_ != epoch || !run_.inMobWave || run_.phase != tower::Phase::Fighting) { pbz::enemies::ClearTagged(request); return; }
            mobRequestWatch_.Finish();
            const auto& mob = tower::mobs::kMobs[index];
            --pending_; --mobActive_;
            if (json.find("\"ok\":true") == std::string::npos) {
                pbz::log::Info("影塔小怪：{} 生成失败：{}", mob.name, json.size() > 200 ? json.substr(0, 200) : json);
                if (pbz::enemies::ClearTagged(request) > 0) { CancelWave("失败的刷怪请求无法清理", tower::wave::Failure::Outstanding); return; }
                if (json.find("没有足够可落地") != std::string::npos && !mobWideRoom_) {
                    mobWideRoom_ = true; ++pending_;
                    mobQueue_.push_back({index, distance, tries});
                    pbz::log::Info("影塔小怪：场地中心挤不下，本波改用整个安全区刷 {}", mob.name);
                    return;
                }
                if (json.find("没有足够可落地") != std::string::npos) { CancelWave("小怪落点不可用", tower::wave::Failure::Venue); return; }
                if (TransientSpawnError(json) || json.find("请求队列已满") != std::string::npos) { CancelWave("小怪生成暂时不可用", tower::wave::Failure::Context); return; }
                // 用户 2026-11-08 第 6 层：一只怪刷不出来就取消整层，重开还是这只，玩家卡死。兜底：换一种怪；几次都不行只放弃这一只
                const bool collided = json.find("未返回同一对象") != std::string::npos || json.find("没有足够可落地") != std::string::npos;
                if (!collided && json.find("请求队列已满") == std::string::npos && !TransientSpawnError(json)) mobBad_.insert(index);
                mobRejected_.insert(index);
                const auto* def = tower::floorplay::Find(mob_.floor);
                const int other = tries < 5 ? tower::mobs::Replacement(index, def ? def->zone : 1, [&](int i) { return mobBad_.count(i) > 0 || mobRejected_.count(i) > 0 || waveAvoid_.count(i) > 0; }) : -1;
                if (other >= 0) {
                    pbz::log::Info("影塔小怪：{} 换成 {}", mob.name, tower::mobs::kMobs[other].name);
                    ++pending_;
                    mobQueue_.push_back({other, distance + 130.0 * (tries + 1), tries + 1});
                    return;
                }
                CancelWave("小怪多次生成失败，当前波不结算", tower::wave::Failure::Venue);
                return;
            } else {
                const auto receipt = tower::spawnreceipt::Read(json, request, mob.path);
                std::unordered_set<UObject*> known(before_.begin(), before_.end());
                for (auto& a : actors_) known.insert(a.obj);
                bool found = false;
                if (receipt.ok) for (auto* a : pbz::enemies::OwnedActors())
                    if (!known.count(a) && ue::Live(a) && ue::ObjName(a) == receipt.name) {
                        found = true;
                        actors_.push_back({a, std::string(mob.name)});
                        auto& added = actors_.back();
                        added.boss = -1;
                        added.mobIndex = index;
                        added.mobRequest = request;
                        added.mobActorName = receipt.name;
                        added.mobTries = tries;
                        added.mobHp = tower::mobs::HpTarget(mob.hp, mob.elite, mob_.floor, tower::floorplay::IsEasy(run_.difficulty));
                        // 波次怪不归原生战斗组（实机大多本来就"暂不归组"）；归组要整表扫刷怪点，每只一次，是换波卡顿的来源之一
                        if (towerentry::Holding() && !towerentry::Hold(a)) { CancelEntry(towerentry::Why()); return; }
                        break;
                    }
                if (!found) { CancelWave("小怪生成身份未核验", tower::wave::Failure::Outstanding); return; }
            }
        });
    }
    bool NativeWaveDeath(const Actor& a) const {
        if (a.mobRequest.empty() || !ue::Live(a.obj) || ue::ObjName(a.obj) != a.mobActorName) return false;
        const int componentOffset = ue::PropOffset(ue::ClassOf(a.obj), "Death Component");
        auto* component = componentOffset >= 0 ? ue::Get<UObject*>(a.obj + componentOffset) : nullptr;
        if (!component || !ue::Live(component) || ue::Get<UObject*>(component + ue::Layout::ObjOuter) != a.obj) return false;
        auto* cls = towerperf::ClassNamed("BP_DeathComponent_C");
        if (!cls || !ue::IsA(component, cls)) return false;
        const int fake = ue::PropOffset(cls, "Fake Death Enabled");
        if (fake < 0 || ue::Get<uint8_t>(component + fake, 1) != 0) return false;
        auto* fn = pe::FindFunction(ue::ClassOf(component), "Is Dead");
        if (!fn) return false;
        const int size = ue::Get<int>(fn + ue::Layout::StructSize);
        const int result = ue::PropOffset(fn, "ReturnValue");
        uint8_t args[32] = {};
        if (size <= 0 || size > static_cast<int>(sizeof(args)) || result < 0 || result >= size) return false;
        pe::Call(component, fn, args);
        return args[result] != 0;
    }
    // ---- 无尽混编层（QA 审计计划 @endless）----
    // 无尽层开层：随机场地（容量实测大 / 中档，避开上一层同一场地）+ 随机波次；第一波照常读秒
    bool StartEndlessFloor(tower::special::Floor sp = {}, int mainF = -1) {
        const int nextFloor = run_.floor + 1;
        SpecialReset();
        endlessRegionOff_ = false; preloadedWave_ = -1;
        if (sp.kind != tower::special::Kind::None && !BuildSpecial(sp, mainF > 0 ? mainF : nextFloor, mainF)) {
            if (mainF > 0) return false;
            sp = {};  // 无尽层凑不出特殊阵容：按普通无尽层打
        }
        if (sp.kind == tower::special::Kind::None) endless_ = tower::endless::Generate(run_.seed, nextFloor, EndlessPools());
        if (endless_.empty()) { floorNote_ = "无尽层：没有可直接生成的 Boss"; Broadcast("err.boss_denied", floorNote_); return false; }
        std::vector<tower::endless::Venue> venues;
        for (const auto& v : tower::endless::kVenues) {
            if (v.key.starts_with("fixed:")) { venues.push_back(v); continue; }
            const int b = FindBoss(std::string(v.key));
            if (b >= 0 && !NativeBad(b) && !venueBad_.count(b) && towertravel::HasVenue(tower::kBossPool[b].path, tower::kBossPool[b].identity)) venues.push_back(v);
        }
        if (venues.empty()) { EndlessReset(); floorNote_ = "无尽层：没有可用的大场地"; Broadcast("err.venue_unreachable", floorNote_); return false; }
        uint64_t x = (run_.seed ^ 0xE4D1E55ull) * 0x9E3779B97F4A7C15ull + static_cast<uint64_t>(nextFloor) * 0xD6E8FEB86659FD93ull;
        x ^= x >> 31;
        // 3.6.0：榕树之心约 30%，其余按真实可站面积加权，不连续两层同一场地
        const int pi = tower::endless::Pick(venues, lastEndlessVenue_, x);
        std::string_view pick = venues[static_cast<size_t>(std::max(0, pi))].key;
        lastEndlessVenue_ = std::string(pick);
        if (specialMain_) {  // 主线创意层：不 ++floor（同小兵段），数值按当前层
            run_.bosses = endless_[0].bosses; run_.phase = tower::Phase::Fighting; run_.specialStage = true;
            count_ = static_cast<int>(endless_[0].bosses.size());
        } else count_ = run_.ForcedFloor(endless_[0].bosses);
        if (pick.starts_with("fixed:")) { venuePath_ = std::string(pick); endlessAnchor_ = endless_[0].bosses[0]; }
        else endlessAnchor_ = FindBoss(std::string(pick));
        staggerFloor_ = false;
        std::string desc;
        for (size_t w = 0; w < endless_.size(); ++w) {
            desc += std::format("{}第 {} 波：", w ? "；" : "", w + 1);
            for (int b : endless_[w].bosses) desc += std::string(tower::kBossPool[b].name) + " ";
            desc += std::format("+ 小怪 {} 只", endless_[w].mobs.size());
        }
        pbz::log::Info("影塔无尽模式：第 {} 层（显示 {}）{} 波，场地 {}：{}", run_.floor, ShownBoss(run_.floor), endless_.size(),
                       pick.starts_with("fixed:") ? std::string(pick) : std::string(tower::kBossPool[endlessAnchor_].name), desc);
        return true;
    }
    std::string lastEndlessVenue_;
    double shachiDiagAt_ = 0;  // 3.3.3 杀气上限诊断节流
    // 2026-11-12 容量实测（notes/capacity-sweep-20261112.md）：这些类直接生成时「原生初始化未返回同一对象 / 初始化后位置异常偏移」，无尽层不抽
    static bool EndlessUnstable(std::string_view path) {
        static constexpr std::string_view kBad[] = {"BP_Monster_AquaKiller_YuCha_C", "BP_Monster_AquaKiller_ZhaoDao_C", "BP_Monster_BaiheDisciple_FishingNet_C",
            "BP_Monster_BiYuan_C", "BP_Monster_DaTouWaWa_HaHaHa_Sword_C", "BP_Monster_Danghui_C", "BP_Monster_FeiZei_ZongHuoZei_C", "BP_Monster_FemaleDoll_Golden_C",
            "BP_Monster_FemaleDoll_TieChiSan_C", "BP_Monster_FemaleDoll_Weaponless_C", "BP_Monster_HuaShan_Normal_Red_C", "BP_Monster_MaleDoll_Cannon_C",
            "BP_Monster_MaleDoll_GuiQiangDun_C", "BP_Monster_MoTian_LongZhu_03_C", "BP_Monster_MobMan_Elite_C", "BP_Monster_RenTuZi_Ice_C",
            "BP_Monster_SnowKiller_Sickle_C", "BP_Monster_YingMei_RedShadowBoneBlade_C", "BP_Monster_YuCong_WuLi_C"};
        const auto cls = path.substr(path.rfind('.') + 1);
        return std::any_of(std::begin(kBad), std::end(kBad), [&](std::string_view x) { return x == cls; });
    }
    tower::endless::Pools EndlessPools() const {
        tower::endless::Pools p;
        for (int i = 0; i < kPool; ++i) {
            const auto& b = tower::kBossPool[i];
            const bool direct = tower::servants::DirectSpawnOk(i);  // 3.7.0 左殇·真龙之躯：直接生成可打（用户 2026-11-15）
            if ((b.scene && !direct) || badBosses_.count(i) || !Admitted(i) || b.name.find("暂译") != std::string_view::npos) continue;
            const auto id = b.identity;
            if (direct) { p.bosses.push_back(i); continue; }
            if (id == "Hun" || id == "LengTu" || id.find("LionDancer") != std::string_view::npos || id.find("ZuoShang") != std::string_view::npos ||
                id.find("HuaShan") != std::string_view::npos || id.find("UltimateBoss") != std::string_view::npos) continue;
            if (std::any_of(std::begin(tower::floorplan::kNativeOnly), std::end(tower::floorplan::kNativeOnly), [&](const auto& d) { return d.path == b.path; })) continue;
            if (tower::floorplay::SpawnerFirst(i) || EndlessUnstable(b.path)) continue;  // 只能原生刷怪点刷出 / 容量实测直刷不稳的不抽
            p.bosses.push_back(i);
        }
        p.identity = [](int i) { return tower::kBossPool[i].identity; };
        p.mobCount = static_cast<int>(std::size(tower::mobs::kMobs));
        p.mobOk = [this](int m) { return !mobBad_.count(m) && !EndlessUnstable(tower::mobs::kMobs[m].path); };
        p.solo = [](int i) { return tower::kBossPool[i].identity == std::string_view("MengHua"); };  // 圣母娘娘：同波不放别的 Boss / 小怪
        p.allies = (grow_.on && grow_.hexes.Has(tower::hex::ServantMaster) ? 3 : 0) + (grow_.on ? tower::servants::AssistFor(run_.seed, ShownNow() + 1).count : 0);  // 3.2.1 助战也占场地
        p.maxBosses = ShownNow() + 1 >= 200 ? 3 : 2;  // 用户 2026-11-12：200 层前每波最多 1v2，200 层起才有 1v3
        return p;
    }
    void QueueEndlessMobs(int wave) {
        endlessMobQ_ = endless_[wave].mobs;
        endlessMobPending_ = static_cast<int>(endlessMobQ_.size());
        endlessMobFlight_ = 0;
        endlessMobAt_ = now_ + 0.3;
    }
    void SubmitEndlessMob() {
        const int index = endlessMobQ_.front();
        endlessMobQ_.erase(endlessMobQ_.begin());
        tower::arena::P3 center; double radius = 0;
        if (!WaveSpawnRegion(center, radius)) { --endlessMobPending_; pbz::log::Info("影塔无尽混编层：场内安全区不可用，跳过小怪 {}", tower::mobs::kMobs[index].name); return; }
        const auto& m = tower::mobs::kMobs[index];
        pbz::enemies::Request r;
        r.command = "spawn";
        r.path = std::string(m.path);
        r.requestId = std::format("tower-{}-{}-mob{}-", run_.id, run_.floor, ++spawnSeq_);
        r.count = 1;
        r.distanceCm = std::clamp(600.0, 200.0, std::max(200.0, radius - 300.0));
        r.faction = std::format("tower-region {:.3f} {:.3f} {:.3f}", center.x, center.y, radius);
        ++endlessMobFlight_;
        const auto request = r.requestId; const auto epoch = spawnEpoch_; const int floor = run_.floor;
        pbz::enemies::Submit(std::move(r), [this, index, request, epoch, floor](std::string json) {
            if (spawnEpoch_ != epoch || run_.floor != floor || run_.phase != tower::Phase::Fighting || !EndlessOn()) { pbz::enemies::ClearTagged(request); return; }
            --endlessMobFlight_; --endlessMobPending_;
            endlessMobAt_ = now_ + 0.3;
            const auto& mob = tower::mobs::kMobs[index];
            auto fallback = [&](const std::string& why) {  // 兜底：换一只同档小怪再刷一次（每层最多 2 次），不行就跳过这只
                pbz::enemies::ClearTagged(request);
                const int other = endlessMobRetries_ < 2 ? tower::mobs::Replacement(index, 1, [&](int i) { return mobBad_.count(i) > 0 || i == index; }) : -1;
                if (other >= 0) { ++endlessMobRetries_; ++endlessMobPending_; endlessMobQ_.push_back(other); }
                pbz::log::Info("影塔无尽混编层：小怪 {} {}，{}", mob.name, why, other >= 0 ? std::format("换 {} 再刷一次", tower::mobs::kMobs[other].name) : std::string("跳过这只"));
            };
            if (json.find("\"ok\":true") == std::string::npos) { fallback("生成失败：" + json.substr(0, 160)); return; }
            const auto receipt = tower::spawnreceipt::Read(json, request, mob.path);
            std::unordered_set<UObject*> known(before_.begin(), before_.end());
            for (auto& a : actors_) known.insert(a.obj);
            if (receipt.ok) for (auto* a : pbz::enemies::OwnedActors())
                if (!known.count(a) && ue::Live(a) && ue::ObjName(a) == receipt.name) {
                    actors_.push_back({a, std::string(mob.name)});
                    auto& added = actors_.back();
                    added.boss = -1; added.mobIndex = index; added.mobRequest = request; added.mobActorName = receipt.name;
                    added.mobHp = tower::mobs::HpTarget(mob.hp, mob.elite, MobScaleFloor(), tower::floorplay::IsEasy(run_.difficulty));
                    if (fightSince_ > 0 && !towerentry::Holding()) towercombat::Watch(a, added.name);
                    if (towerentry::Holding() && !towerentry::Hold(a)) { CancelEntry(towerentry::Why()); return; }
                    return;
                }
            fallback("身份未核验");
        });
    }
    // 小怪战斗状态读不到 / 异常消失：算这只结束（保留本波其他进度），兜底补一只
    void EndlessUnresolvedMob(Actor& a, const std::string& why) {
        FinishMissingWaveMob(a);
        if (!a.mobRequest.empty()) pbz::enemies::ClearTagged(a.mobRequest);
        const int other = endlessMobRetries_ < 2 && a.mobIndex >= 0 ? tower::mobs::Replacement(a.mobIndex, 1, [&](int i) { return mobBad_.count(i) > 0 || i == a.mobIndex; }) : -1;
        if (other >= 0) { ++endlessMobRetries_; ++endlessMobPending_; endlessMobQ_.push_back(other); }
        pbz::log::Info("影塔无尽混编层：{}，算这只结束{}", why, other >= 0 ? std::format("，补一只 {}", tower::mobs::kMobs[other].name) : std::string());
    }
    void AdvanceEndless() {
        endlessNextAt_ = -1; clearAt_ = 0;
        for (auto& a : actors_) {  // 同车轮：换波前移除已阵亡的 Boss（收掉残留血条）；小怪尸体随本层结束清理
            if (!a.dead || a.swept || !ue::Live(a.obj) || !fnDestroy_ || a.mobIndex >= 0) continue;
            towerisolate::BlockGroupOf(a.obj);
            uint8_t none[8] = {};
            pe::Call(a.obj, fnDestroy_, none);
        }
        actors_.clear();
        before_ = pbz::enemies::OwnedActors();
        ++endlessWave_;
        const auto& w = endless_[endlessWave_];
        count_ = std::max(1, static_cast<int>(w.bosses.size()));
        endlessRetries_ = 0; relayRetries_ = 0; failed_ = noRoom_ = transient_ = false;
        pending_ = static_cast<int>(w.bosses.size());
        for (size_t i = 0; i < w.bosses.size(); ++i) { run_.bosses.push_back(w.bosses[i]); spawnQueue_.push_back({now_ + 0.8 * static_cast<double>(i), w.bosses[i]}); }
        QueueEndlessMobs(endlessWave_);
        endlessWaveStart_ = now_ + 0.8 * static_cast<double>(w.bosses.size()) + 0.4 * static_cast<double>(w.mobs.size()) + 3.0;  // 到齐后读秒 3 秒
        endlessGuardUntil_ = endlessWaveStart_;  // 新一波入场到读秒结束：玩家无敌
        std::string names;
        for (int b : w.bosses) names += std::string(tower::kBossPool[b].name) + " ";
        pbz::log::Info("影塔无尽混编层：第 {} 层第 {}/{} 波：{}+ 小怪 {} 只", run_.floor, endlessWave_ + 1, endless_.size(), names, w.mobs.size());
        Broadcast("info.relay_next", std::format("第 {}/{} 波", endlessWave_ + 1, endless_.size()));
    }
    void FinishMissingWaveMob(Actor& a) {
        if (!a.dead) { pbz::log::Info("影塔小怪：{} 已确认死亡（战斗属性不可读），保留本波进度", a.name); ++warKills_; }  // 以战养战：秒杀 / 尸体快速回收的也算击败
        a.dead = true;
        if (!a.reaped) {
            a.reaped = true;
            if (grow_.on && grow_.hexes.Has(tower::hex::Reaper)) ++reapPending_;
        }
        towerentry::ForgetEndedWaveActor(a.obj);
    }
    void ReplaceUnresolvedWaveMob(Actor& a, const std::string& why) {
        const auto* floor = tower::floorplay::Find(mob_.floor);
        mobRejected_.insert(a.mobIndex);
        const int replacement = a.mobTries < 5 ? tower::mobs::Replacement(a.mobIndex, floor ? floor->zone : 1,
            [&](int i) { return mobBad_.count(i) || mobRejected_.count(i) || waveAvoid_.count(i); }) : -1;
        if (replacement < 0 || a.mobRequest.empty()) {
            CancelWave(why + "，本波无法安全补怪", tower::wave::Failure::Outstanding);
            return;
        }
        towerentry::ForgetEndedWaveActor(a.obj);
        if (pbz::enemies::ClearTagged(a.mobRequest) != 0) {
            CancelWave(why + "，旧怪无法安全清理", tower::wave::Failure::Outstanding);
            return;
        }
        pbz::log::Info("影塔小怪单只补位：{}，{} → {}，保留已击败的怪", why, a.name, tower::mobs::kMobs[replacement].name);
        ++pending_;
        mobQueue_.push_back({replacement, 600.0, a.mobTries + 1});
        actors_.erase(actors_.begin() + (&a - actors_.data()));
    }
    void MobWaveCleared() {
        const size_t left = Clear("mob_wave");
        if (left) {
            CancelEntry("上一波怪物未能安全清理，本波不结算；点下一层重试");
            return;
        }
        actors_.clear();
        ++mob_.wave;
        run_.mobFloor = mob_.floor; run_.mobWave = mob_.wave;
        pbz::log::Info("影塔小怪：第 {} 层前 第 {}/{} 轮清完", mob_.floor, mob_.wave, mob_.total);
        Broadcast("info.mob_cleared", std::format("第 {} 层：{}/{} 清完", tower::floorplay::ShownMobs(mob_.floor, run_.floors), mob_.wave, mob_.total));
        // 2.0.0 用户 2026-11-08：奖励页只在 Boss 后，波次之间不再三选一；同一场地马上下一轮，最后一轮清完开驿站
        if (mob_.wave < mob_.total) {
            run_.phase = tower::Phase::Ready;  // 记下打到第几轮：退出重进接着打。只写局存档（换波期间成长档没有变化）
            if (const auto path = RunFile(); !path.empty() && !AtomicTowerFile(path, run_.Save())) pbz::log::Info("影塔存档：换波局档写入失败，保留上一次的完整版本");
            run_.phase = tower::Phase::Fighting;
            SpawnWave();
            return;
        }
        FinishMobStage(mob_.floor);
    }
    // 小兵段打完的结算（魂玉、账单、护命符匠、开驿站）。3.7.0：主线创意层打完也走这里
    void FinishMobStage(int F) {
        run_.mobFloor = F; run_.mobWave = tower::mobs::WaveCount(F);
        run_.inMobWave = false; run_.specialStage = false;
        run_.phase = tower::Phase::Ready;
        // 已经打过这一段（回退重打）：shopFloor 记着开过店的段，peak 记着打过的 Boss
        const bool replayed = F <= run_.peak || run_.shopFloor >= F;
        if (!replayed) { HexCharm(tower::floorplay::ShownMobs(F, run_.floors)); grow_.hexes.ClearFloor(tower::floorplay::ShownMobs(F, run_.floors)); }
        int jade = tower::StageJade(F, replayed);
        run_.jade += jade;
        // 3.6.0 魂玉账单：小怪段没有奖励页，打完立即结算（聚宝盆 / 钱生钱 / 积少成多，不吃悬赏令）；重打 1/4、不吃海克斯
        jadeNote_.clear();
        if (!replayed) { SettleBill("上一层未结"); OpenBill(tower::floorplay::ShownMobs(F, run_.floors), jade, false); jade = std::max(jade, SettleBill("小怪段打完")); }
        if (!replayed) HexClearRewards(tower::floorplay::ShownMobs(F, run_.floors), jade, false);
        floorNote_ = replayed ? std::format("第 {} 层已经领过奖励，这次只有魂玉 +{}", tower::floorplay::ShownMobs(F, run_.floors), jade)
                              : jadeNote_.empty() ? std::format("本层收获：魂玉 +{}", jade) : jadeNote_;
        pbz::log::Info("影塔小怪：第 {} 层前波次打完，魂玉 +{}（{}）", F, jade, replayed ? "回退重打 1/4" : "首次");
        MobLeave();
        OpenStageShop(F);
        SaveRun();
    }
    // 护命符匠：首次通关第 15（QA 3）的倍数层，没有复活符时给 1 个（在 ClearFloor 之前调用，重打的层不给）
    void HexCharm(int shown) {
        auto& revive = run_.items[static_cast<int>(tower::Item::Revive)];
        if (!grow_.on || !grow_.hexes.CharmDue(shown, revive)) return;
        ++revive;
        pbz::log::Info("影塔海克斯：护命符匠，通关第 {} 层获得复活符", shown);
        Broadcast("info.hex_charm", "护命符匠：获得复活符");
    }
    // 2.0.0 用户 2026-11-08：驿站在每段小怪打完、Boss 层之前开（取消 5 / 15 / 25 / 35 / 45 层后开店）
    void OpenStageShop(int F) {
        SettleBill("驿站开张前");  // 3.6.0 兜底：账单一定在对冲基金之前结清
        if (!grow_.on || !run_.CanOpenStageShop(F)) return;  // 回退重打的层不再开店
        run_.shopFloor = F;
        if (grow_.hexes.Has(tower::hex::Interest)) {  // 对冲基金：开张时按持有魂玉发利息（每次驿站只开一次，随后 SaveRun）
            if (const int bonus = tower::hex::InterestOn(run_.jade, tower::balance::InterestCap(tower::balance::ShownOfBoss(F)), grow_.hexes.InterestRate()); bonus > 0) {
                run_.jade += bonus;
                pbz::log::Info("影塔海克斯：对冲基金 +{} 魂玉（持有 {}）", bonus, run_.jade);
                Broadcast("info.hex_interest", std::format("对冲基金：魂玉 +{}", bonus));
            }
        }
        // 3.1.0（用户）：有备而来改为原百宝囊效果——开张按各道具获得权重随机给 1 件 MOD 道具（含复活符 / 重生符）；旧测试档的百宝囊按有备而来算
        if (grow_.hexes.Has(tower::hex::Prepared)) for (int give = 0; give < grow_.hexes.PreparedItems(); ++give) {  // 3.6.0：原百宝囊编号改给飞身踢；Lv2 每次 2 件
            std::vector<std::pair<int, int>> pool; int total = 0;
            for (int i = 0; i < tower::kItemCount; ++i) {
                const auto timing = tower::rewards::ItemTiming(tower::kItems[i].item);
                if (timing == tower::rewards::Timing::Retired || tower::kItems[i].weight <= 0 || run_.items[i] >= tower::rewards::kItemStackCap) continue;
                if (tower::kItems[i].item == tower::Item::Foresight && run_.items[i] > 0) continue;
                pool.push_back({i, tower::kItems[i].weight}); total += tower::kItems[i].weight;
            }
            if (total > 0) {
                uint64_t r = (run_.seed ^ (0xD1B54A32D192ED03ull * static_cast<uint64_t>(F + 29)) ^ (0xA24BAED4963EE407ull * static_cast<uint64_t>(give))) * 0x9E3779B97F4A7C15ull; r ^= r >> 31;
                int x = static_cast<int>(r % static_cast<uint64_t>(total)), it = pool.back().first;
                for (auto [i, w] : pool) { if ((x -= w) < 0) { it = i; break; } }
                ++run_.items[it];
                pbz::log::Info("影塔海克斯：有备而来，获得 {}", tower::kItems[it].name);
                Broadcast("info.hex_prepared", std::format("有备而来：获得 {}", tower::kItems[it].name));
            }
        }
        grow_.OpenShop(run_.floor, run_.floors, run_.seed, run_.items, ShopSupplement());
        pbz::log::Info("影塔驿站：第 {} 层 Boss 前开店（{} 件）", F, grow_.shop.shelf.size());
    }
    void MobLeave(bool preserveActive = false) {
        mob_ = {};
        if (!preserveActive) grow_.prep.FloorEnded();
        sunder_ = false; freezeUntil_ = slowUntil_ = 0;  // 道具只作用这一层，不带到 Boss
        towerisolate::End(); tvenue::End(); boundary_.Disarm(); originSet_ = false;
    }
    void MobSkip(const std::string& why) {
        pbz::log::Info("影塔小怪：第 {} 层前的小怪跳过（{}）", mob_.floor, why);
        const int F = mob_.floor, total = mob_.total;
        towerentry::End(); towertravel::Abort("小怪跳过"); travelThenSpawn_ = false; spawnAt_ = 0; pending_ = 0;
        const size_t left = Clear("mob_skip"); actors_.clear();
        if (left) {
            run_.UndoFloor(); MobLeave();
            failedStage_ = {run_.id, run_.floor, run_.floor, F, total, true, {}};
            floorNote_ = "小怪残留无法安全清理，未跳过；请保存退出后反馈";
            SaveRun(); Broadcast("err.spawn_failed", floorNote_); return;
        }
        run_.mobFloor = F; run_.mobWave = total;
        run_.inMobWave = false; run_.phase = tower::Phase::Ready;
        floorNote_ = "这一层出了问题，已跳过，可以直接打下一层";
        MobLeave();
        OpenStageShop(F);
        SaveRun();
    }
    bool FailedStageAvailable() const {
        return failedStage_.Available(run_);
    }
    void SkipFailedStage() {
        if (!FailedStageAvailable()) return;
        const auto failed = failedStage_;
        if (failed.wave) {
            if (failed.mobFloor <= 0 || failed.mobTotal <= 0) return;
            mob_ = {true, failed.mobFloor, run_.mobWave, failed.mobTotal, now_};
            run_.inMobWave = true; run_.phase = tower::Phase::Fighting;
            failedStage_ = {};
            MobSkip("玩家确认本层有问题");
            Broadcast("info.mob_skip", floorNote_);
            return;
        }
        if (!failed.RestoreBoss(run_)) return;
        if (!SkipFloor("玩家确认本层有问题")) {
            run_.UndoFloor();
            Broadcast("err.cannot_skip", "此层暂不能跳过，请重新开战或保存退出");
            return;
        }
        failedStage_ = {};
        SaveRun(); Broadcast("info.badboss", floorNote_);
    }

    // 战斗区：场地数据（雾门凸包 / 出生点为圆心的圆），没有就以玩家此刻的位置为圆心
    void ArmBoundary() {
        originSet_ = PawnLoc(origin_);
        std::vector<tower::arena::P3> hull; tower::arena::P3 c; double r = 0, lo = 0, hi = 0;
        if (tvenue::Boundary(&hull, &c, &r, &lo, &hi)) {
            if (!hull.empty()) boundary_.ArmHull(hull, c, lo, hi); else { boundary_.Arm(c, r); boundary_.zmin = lo; boundary_.zmax = hi; }
            originSet_ = true;
            // 软边（39 层寒岁借新娘阿花场地：凸包 5 点、面积小，追 Boss 到边被反复拉回）：借用场地 15 米，本场地 10 米
            const bool borrowed = venueBoss_ >= 0 && std::find(run_.bosses.begin(), run_.bosses.end(), venueBoss_) == run_.bosses.end();
            boundary_.slack = borrowed ? 1500 : 1000;  // 本场地也给 10 米：导出凸包是内缩的，4 米贴边就拉人（发布版实测第 1 层）
        } else if (originSet_) boundary_.Arm({origin_[0], origin_[1], origin_[2]}, 0);
        else boundary_.Disarm();
        pbz::log::Info("影塔战斗区：{}（{}，软边 {:.0f} 米）", boundary_.Describe(), tvenue::Found() ? "场地数据" : "默认", boundary_.slack / 100);
        ArmWall();
    }
#include "tower_wall_runtime.inc"  // 3.6.0 统一出界拉回 + 隐形硬墙（Tower 成员）
    // 游戏线程：把 Actor 瞬移到 p（K2_SetActorLocation，bTeleport=true、不扫掠），读回偏差 < 3 米算成功
    bool MoveActor(UObject* a, const tower::arena::P3& p) {
        if (!a || !ue::Live(a) || !fnLoc_) return false;
        auto* fn = pe::FindFunction(ue::ClassOf(a), "K2_SetActorLocation");
        if (!fn) return false;
        const int loc = ue::PropOffset(fn, "NewLocation"), tp = ue::PropOffset(fn, "bTeleport"), sw = ue::PropOffset(fn, "bSweep");
        const int size = ue::Get<int>(fn + ue::Layout::StructSize);
        if (loc < 0 || tp < 0 || sw < 0 || size <= 0 || size > 0x200 || loc+24>size || tp>=size || sw>=size) return false;
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || !towerreach::StopMotion(a)) return false;
        std::array<uint8_t, 0x200> args{};
        const double v[3] = {p.x, p.y, p.z};
        std::memcpy(args.data() + loc, v, 24);
        args[tp] = 1; args[sw] = 0;
        pe::Call(a, fn, args.data());
        uint8_t b[32] = {}; pe::Call(a, fnLoc_, b); double q[3]; std::memcpy(q, b, 24);
        return std::hypot(q[0] - p.x, q[1] - p.y) < 300 && std::abs(q[2] - p.z) < 300;
    }
    // 战斗区兜底（32ffb7d 彭猛：没有雾门，可以一直跑远）：以放行时的位置为中心。
    // 玩家离开 50 米超过 8 秒 → 点传送拉回中心；Boss 离开 70 米超过 15 秒 → 本层作废（不发奖、不当胜利），回准备页。返回 true = 已作废
    bool PawnLoc(double out[3]) {
        auto* pawn = towerwidgets::Pawn();
        if (!pawn || !fnLoc_) return false;
        uint8_t b[32] = {}; pe::Call(pawn, fnLoc_, b); std::memcpy(out, b, 24);
        return true;
    }
    // 战斗区边界（tower_arena.h，雾门的等效）：玩家越界立刻放回边界内；Boss 越界 15 秒作废本层
    bool Leash() {
        if (!originSet_ || !fnLoc_ || !boundary_.on) return false;
        // 重放层不用塔的战斗区：舞狮柱上对抗的原生领域约 95 m，塔边界最多 45 m，跳柱时拉人会打断原生机制；边界交给原生领域 / 雾门
        if (replay_) return false;
        auto at = [&](UObject* a) { uint8_t b[32] = {}; pe::Call(a, fnLoc_, b); double p[3]; std::memcpy(p, b, 24); return tower::arena::P3{p[0], p[1], p[2]}; };
        auto* pawn = towerwidgets::Pawn();
        // 原生雾门已关（第 1 层残钢等原生场地）：玩家活动范围交给原生雾墙。导出的雾门凸包比雾墙围出的实际场地小，
        // 塔再按凸包拉人会在雾墙内正常走位时反复闪回（2026-11-05 发布版实测，第 1 层 15 秒内拉回 7 次）。Boss 越界拉回照旧
        if (pawn && !tvenue::DoorsClosed() && !pbz::TeleportBusy() && now_ >= pushCooldown_) {
            const auto p = at(pawn);
            if (boundary_.Player(p) == tower::arena::Act::PushBack) {
                const auto in = boundary_.Inside(p);
                std::string msg;
                const bool ok = pbz::PointTeleport(in.x, in.y, in.z, 0, &msg);
                pushCooldown_ = now_ + 1.0;
                pbz::log::Info("影塔战斗区：玩家越过边界（{}，位置 {:.0f}/{:.0f}/{:.0f}，放行后 {:.2f} 秒），放回边界内（{}）", boundary_.Describe(), p.x, p.y, p.z, now_ - fightSince_,
                               ok ? "已放回" : "失败：" + msg);
                Broadcast("info.leash", "到了战斗区边界");
            }
        }
        bool voided = false;
        // 3.6.0：Boss 的同身份召唤物（雷婷分身等）同样受边界约束
        for (UObject* s : wallSummons_) if (s && ue::Live(s)) WallStep(s, ue::ClassName(s), at(s), pawn);
        for (auto& a : actors_) {
            if (a.dead || a.gone || !ue::Live(a.obj)) continue;
            const auto bp = at(a.obj);
            if (WallStep(a.obj, a.name, bp, pawn)) continue;  // 3.6.0 统一拉回 / 硬墙（有实测边界时取代下面的战斗区规则）
            if (run_.inMobWave) {
                if (std::hypot(bp.x - boundary_.center.x, bp.y - boundary_.center.y) > tower::mobs::kFarCm) {
                    const auto why = std::format("{} 跑出小怪场地过远，本波未结算", a.name);
                    RecordFailure("wave_outside", venueBoss_, why); CancelWave(why, tower::wave::Failure::Venue); return true;
                }
                // Only treat a nearby mob as trapped outside the fog wall when
                // it remains outside a known hull and collision + navigation
                // independently indicate that the player cannot reach it.
                if (!boundary_.hull.empty() && boundary_.Outside(bp)) {
                    if (a.mobOutsideSince < 0) a.mobOutsideSince = now_;
                    if (now_ - a.mobOutsideSince >= 10.0 && pawn && now_ >= a.reachAt + 0.3) {  // 4.0.0：探测每 0.3 秒最多一次
                        a.reachAt = now_;
                        double pl[3];
                        if (PawnLoc(pl)) {
                            const double b[3] = {bp.x, bp.y, bp.z};
                            const auto probe = towerreach::Measure(pawn, pl, a.obj, b);
                            if (towerreach::Decide(probe) == towerreach::Verdict::Walled) {
                                const auto why = std::format("{} 持续在场地外且不可到达，本波未结算", a.name);
                                RecordFailure("wave_walled", venueBoss_, why); CancelWave(why, tower::wave::Failure::Venue); return true;
                            }
                        }
                    }
                } else a.mobOutsideSince = -1;
                continue;
            }
            // 用户 2026-11-12：铁狂 / 残钢打到雾门边被瞬间拉回中间（导出凸包比真实雾墙小）；灭法之法（雷婷）瞬移到雾门外却在软边内没被拉回。
            // 改判「真的在雾门外」：凸包外（不算软边），且 ① 超出软边 15 米以上，或 ② 玩家到 Boss 胸口高度的视线被挡 / 被墙隔开
            bool outside = false;
            if (boundary_.OutsideBy(bp, 0)) {
                if (boundary_.OutsideBy(bp, boundary_.slack + 1500)) outside = true;
                else if (now_ < a.reachAt + 0.3) outside = a.reachOut;  // 4.0.0：射线 + 导航探测每帧做太贵，0.3 秒内沿用上次结果
                else if (double pl[3]; pawn && PawnLoc(pl)) {
                    const double b[3] = {bp.x, bp.y, bp.z};
                    const auto probe = towerreach::Measure(pawn, pl, a.obj, b);
                    outside = (probe.lineKnown && probe.lineBlocked) || towerreach::Decide(probe) == towerreach::Verdict::Walled;
                    a.reachAt = now_; a.reachOut = outside;
                }
            }
            const auto act = boundary_.BossAt(outside, now_, 1.5);
            if (act == tower::arena::Act::PullBack) {
                // 39 层寒岁：Boss 跑出界 15 秒就作废、Boss 被清。先拉回界内（最多 3 次），拉不回 / 拉完仍出界才作废
                // 48 层冷荼（断魂冰面，ui11）：拉回落点的胶囊净空三次都被冰面 WorldDynamic 体积挡住 → 15 秒作废。
                // 拉回只要求静态通道无阻挡（地面射线已确认），动态通道重叠照放，同 34 层落点
                // 1.1.1（玩家反馈 18 层疯医瞬移到雾门外脱战 → 本层作废）：原先只试"沿出界方向往里 60%"一个点，那里没地面就连败 3 次作废。
                // 依次试：出界方向往里 60% → 战斗区中心 → 玩家身边（往中心 3 米；玩家站着的地方一定有地面）→ 玩家脚下，哪个落得下就放哪
                // 先放回雾门内靠边处（85%），不再先拉到场地中间
                std::vector<tower::arena::P3> tries{boundary_.InsideDeep(bp, 0.85), boundary_.InsideDeep(bp, 0.6), boundary_.center};
                if (double pl[3]; PawnLoc(pl)) {
                    const double dx = boundary_.center.x - pl[0], dy = boundary_.center.y - pl[1], d = std::max(1.0, std::hypot(dx, dy));
                    tries.push_back({pl[0] + dx / d * std::min(300.0, d), pl[1] + dy / d * std::min(300.0, d), pl[2]});
                    tries.push_back({pl[0], pl[1], pl[2]});
                }
                bool ok = false; std::string why; int tried = 0;
                for (const auto& c : tries) {
                    ++tried;
                    const double nearAt[3] = {c.x, c.y, (tried <= 2 ? boundary_.center.z : c.z) + 50}; double landing[3]; std::string w;
                    if (!towerreach::Landing(a.obj, nearAt, landing, &w, true)) { why += (why.empty() ? "" : "；") + w; continue; }
                    const tower::arena::P3 dst{landing[0], landing[1], landing[2]};
                    if (boundary_.Outside(dst) || !MoveActor(a.obj, dst) || boundary_.Outside(at(a.obj))) { why += (why.empty() ? "" : "；") + std::string("落点放下后仍在界外"); continue; }
                    ok = true; break;
                }
                if (ok && tried > 1) pbz::log::Info("影塔战斗区：{} 用第 {} 个候选落点拉回（前面的：{}）", a.name, tried, why);
                boundary_.Pulled(now_, ok);
                if(!ok)pbz::log::Info("影塔战斗区：拒绝无有效支撑的拉回：{}",why);
                pbz::log::Info("影塔战斗区：{} 越界（位置 {:.0f}/{:.0f}/{:.0f}），拉回界内第 {} 次（{}）", a.name, bp.x, bp.y, bp.z, boundary_.pulls, ok ? "已拉回" : "拉回失败");
            } else if (act == tower::arena::Act::Void) voided = true;
            // 塔生成（不是原生刷怪点接管）的单 Boss 反复越界：多半是本体自己瞬移（荆野带马），拉回没用。
            // 场地有原生刷怪点、本层还没这样试过 → 不等 15 秒作废，直接改用原生刷怪点重开
            // 只认真正的瞬移（离战斗区中心 300 米以上；荆野是 1–2 公里），Boss 正常追打跑出边界几米不算（用户 2026-11-07：别把没 bug 的层修坏）
            if (act == tower::arena::Act::PullBack && std::hypot(bp.x - boundary_.center.x, bp.y - boundary_.center.y) > 30000) ++teleportPulls_;
            if (teleportPulls_ >= 3 && !a.adopted && a.boss >= 0 && actors_.size() == 1 &&
                nativeRetryFloor_ != run_.floor && venuePath_.empty() && tvenue::Spawner() && venueBoss_ == a.boss) {
                nativeRetryPending_ = true; nativeRetryBoss_ = a.boss;
                const std::string why = std::format("{} 塔生成后反复瞬移出场地（{} 次离场 300 米以上）", a.name, teleportPulls_);
                RecordFailure("leash_jumper", venueBoss_, why);
                CancelEntry(why);
                return true;
            }
        }
        if (!voided) StuckRescue(at, pawn);
        if (!voided) return false;
        const std::string why = std::format("Boss 离开战斗区（{}）超过 15 秒（脱战 / 卸载）：本层作废、不发奖", boundary_.Describe());
        RecordFailure("leash", venueBoss_, why);
        CancelEntry(why);
        return true;
    }
    // 1.1.1（用户 2026-11-07 第 18 层谢秋冬实机：Boss 陷进地里，血条在、人看不见，3 分多钟双方都没掉血）：
    // 战斗区高度上下各 15 米，陷地不算越界，拉回不触发。卡住判定（每秒一次）：
    //   Boss 比玩家低 4 米以上持续 3 秒 → 尝试放回地面；不再把一段时间没掉血视为卡住（护盾与召唤阶段可能完全正常）。
    double stuckFight_ = -1, stuckNext_ = 0, stuckLow_ = -1, stuckBossHurt_ = 0, stuckCooldown_ = 0;
    float stuckBossHp_ = -1;
    int stuckRescues_ = 0;
    bool stuckWarned_ = false;
    template <class At>
    void StuckRescue(At& at, UObject* pawn) {
        if (fightSince_ <= 0 || !pawn || deathAt_ >= 0 || towerentry::Holding()) return;
        if (stuckFight_ != fightSince_) { stuckFight_ = fightSince_; stuckLow_ = -1; stuckBossHurt_ = now_; stuckBossHp_ = -1; stuckRescues_ = 0; stuckWarned_ = false; stuckCooldown_ = 0; stuckNext_ = 0; }
        if (now_ < stuckNext_) return;
        stuckNext_ = now_ + 1.0;
        float hp = 0; bool any = false;
        for (auto& a : actors_) if (!a.dead && !a.gone && a.hasTarget && ue::Live(a.obj)) { hp += ue::Get<float>(attr::AttrCurrent(a.target, hp_)); any = true; }
        if (!any) return;
        if (stuckBossHp_ < 0 || hp < stuckBossHp_ - 0.5f) { stuckBossHurt_ = now_; stuckRescues_ = 0; stuckWarned_ = false; }
        stuckBossHp_ = hp;
        const auto pl = at(pawn);
        for (auto& a : actors_) {
            if (a.dead || a.gone || !ue::Live(a.obj)) continue;
            const auto bp = at(a.obj);
            const bool low = pl.z - bp.z > 400 && std::hypot(bp.x - pl.x, bp.y - pl.y) < 6000;
            if (!low) stuckLow_ = -1; else if (stuckLow_ < 0) stuckLow_ = now_;
            const bool sunk = stuckLow_ >= 0 && now_ - stuckLow_ >= 3;
            if (!sunk || now_ < stuckCooldown_) continue;
            // 安全兜底只能提示，绝不因计时或多次拉回而消灭 Boss、发放通关奖励。
            if (stuckRescues_ >= 3) {
                if (!stuckWarned_) {
                    stuckWarned_ = true;
                    pbz::log::Info("影塔第 {} 层：已尝试拉回 3 次仍疑似卡住，保留战斗，不自动结算", run_.floor);
                    Broadcast("info.stuck_help", "敌人可能卡住，可在影塔菜单选择本层问题");
                }
                return;
            }
            // 玩家身前 4 米（朝 Boss 水平方向；Boss 就在脚下时朝战斗区中心）
            double dx = bp.x - pl.x, dy = bp.y - pl.y, d = std::hypot(dx, dy);
            if (d < 100) { dx = boundary_.center.x - pl.x; dy = boundary_.center.y - pl.y; d = std::hypot(dx, dy); }
            if (d < 1) { dx = 1; dy = 0; d = 1; }
            std::string why; double landing[3]; bool ok = false;
            for (const double r : {400.0, 250.0, 0.0}) {
                const double nearAt[3] = {pl.x + dx / d * r, pl.y + dy / d * r, pl.z + 50};
                std::string w;
                if (!towerreach::Landing(a.obj, nearAt, landing, &w, true)) { why += (why.empty() ? "" : "；") + w; continue; }
                if (MoveActor(a.obj, {landing[0], landing[1], landing[2]})) { ok = true; break; }
            }
            stuckCooldown_ = now_ + 10; stuckLow_ = -1; stuckBossHurt_ = now_;
            ++stuckRescues_;
            pbz::log::Info("影塔第 {} 层：{} 疑似卡住（比玩家低 4 米以上 3 秒；Boss {:.0f}/{:.0f}/{:.0f}，玩家 {:.0f}/{:.0f}/{:.0f}），放到玩家身前第 {} 次（{}）", run_.floor, a.name,
                           bp.x, bp.y, bp.z, pl.x, pl.y, pl.z,
                           stuckRescues_, ok ? std::format("已放到 {:.0f}/{:.0f}/{:.0f}", landing[0], landing[1], landing[2]) : "失败：" + why);
            break;
        }
    }
    // 4.0.0：入场准入时把落在雾门凸包外的敌人放回凸包内（只在玩家本人站在凸包内时做；每次入场最多 3 轮）
    void PlaceBossesInside() {
        double pl[3];
        if (!fnLoc_ || !PawnLoc(pl) || !boundary_.Contains({pl[0], pl[1], pl[2]}, 200)) return;
        bool any = false, moved = false;
        for (auto& a : actors_) {
            if (a.dead || a.gone || !ue::Live(a.obj)) continue;
            uint8_t b[32] = {}; pe::Call(a.obj, fnLoc_, b); double v[3]; std::memcpy(v, b, 24);
            const tower::arena::P3 bp{v[0], v[1], v[2]};
            if (boundary_.Contains(bp, 200)) continue;
            any = true;
            bool ok = false; std::string why; double landing[3] = {};
            for (const double frac : {0.8, 0.6, 0.3}) {
                const auto c = boundary_.InsideDeep(bp, frac);
                for (const double z : {pl[2], boundary_.center.z}) {
                    const double nearAt[3] = {c.x, c.y, z + 50}; std::string w;
                    if (!towerreach::Landing(a.obj, nearAt, landing, &w, true)) { why += (why.empty() ? "" : "；") + w; continue; }
                    const tower::arena::P3 dst{landing[0], landing[1], landing[2]};
                    if (!boundary_.Contains(dst, 200) || !MoveActor(a.obj, dst)) { why += (why.empty() ? "" : "；") + std::string("放下后仍不在场内"); continue; }
                    ok = true; break;
                }
                if (ok) break;
            }
            moved = moved || ok;
            pbz::log::Info("影塔入场：{} 落在雾门外（{:.0f}/{:.0f}/{:.0f}），放回场内第 {} 轮（{}）", a.name, bp.x, bp.y, bp.z, bossPlaced_ + 1,
                           ok ? std::format("已放到 {:.0f}/{:.0f}/{:.0f}", landing[0], landing[1], landing[2]) : "失败：" + why);
        }
        if (!any) return;
        ++bossPlaced_;
        if (moved) { admitSince_ = -1; lastAdmitWhy_.clear(); reachDone_ = false; nextReach_ = now_ + 0.5; }
    }
    int admitRecalls_ = 0;
    void RecallFarBosses() {
        auto at = [&](UObject* a) { uint8_t b[32] = {}; pe::Call(a, fnLoc_, b); double v[3]; std::memcpy(v, b, 24); return tower::arena::P3{v[0], v[1], v[2]}; };
        const auto c = boundary_.center;
        for (auto& a : actors_) {
            if (a.dead || a.gone || !ue::Live(a.obj)) continue;
            const auto bp = at(a.obj);
            if (boundary_.Contains(bp, 200) || std::hypot(bp.x - c.x, bp.y - c.y) < 10000) continue;
            ++admitRecalls_;
            tower::arena::P3 dst{};
            bool have = false;
            if (auto* sp = static_cast<UObject*>(tvenue::Spawner()); sp && ue::Live(sp)) { dst = at(sp); have = boundary_.Contains(dst, 0); }
            double pl[3];
            if (!have && PawnLoc(pl)) {  // 玩家身前 8 米（朝战斗区中心方向）
                const double dx = c.x - pl[0], dy = c.y - pl[1], d = std::max(1.0, std::hypot(dx, dy));
                dst = {pl[0] + dx / d * 800, pl[1] + dy / d * 800, pl[2]};
                have = true;
            }
            if (!have) continue;
            const double nearAt[3] = {dst.x, dst.y, dst.z + 100};
            double landing[3]; std::string why;
            bool ok = towerreach::Landing(a.obj, nearAt, landing, &why, true);
            if (ok) ok = MoveActor(a.obj, {landing[0], landing[1], landing[2]});
            pbz::log::Info("影塔入场：{} 生成在场外（{:.0f}/{:.0f}/{:.0f}，离中心 {:.0f} 米），放回场地第 {} 次（{}）", a.name, bp.x, bp.y, bp.z,
                           std::hypot(bp.x - c.x, bp.y - c.y) / 100, admitRecalls_, ok ? std::format("已放到 {:.0f}/{:.0f}/{:.0f}", landing[0], landing[1], landing[2]) : "失败：" + why);
            if (ok) { admitSince_ = -1; lastAdmitWhy_.clear(); reachDone_ = false; nextReach_ = now_ + 0.5; }
        }
    }
    // 本局这只 Boss 的原生场地已判坏：去不了 / 没地面（venueBad_）或剧情冻结失败（本机 venue-unsafe）
    // 本局判坏 / 剧情冻结失败 / 实机落不了地（floors_plan.py LANDING_BAD，v6）
    bool NativeBad(int b) const {
        return venueBad_.count(b) > 0 || (arenaOn_ && towerisolate::Unsafe(tower::kBossPool[b].identity)) || !tower::floorplay::LandingBad(b).empty();
    }
    tower::arena::Choice VenueFor(int b) const {
        const auto& c = tower::kBossPool[b];
        return tower::arena::ChooseVenue(towertravel::HasVenue(c.path, c.identity), NativeBad(b), c.scene, towertravel::ZoneArena(run_.Zone(), run_.seed),
                                         [](int i) { return towertravel::ArenaVerified(i); });
    }
    // 类名是不是池里别的 Boss 身份（不是本层的）：返回池下标，否则 -1
    static int OtherBossIdentity(const std::string& cls, const std::vector<std::string>& floorIds) {
        for (int i = 0; i < static_cast<int>(std::size(tower::kBossPool)); ++i) {
            const std::string id(tower::kBossPool[i].identity);
            if (!tower::IdentityIn(cls, id)) continue;
            if (std::find(floorIds.begin(), floorIds.end(), id) != floorIds.end()) continue;
            return i;
        }
        return -1;
    }
    // 本层战斗用时（游戏时间，暂停不计）：读秒结束放行起算；没开打返回 -1
    double FightSeconds() const { return run_.phase == tower::Phase::Fighting && fightSince_ > 0 && !towerentry::Holding() ? std::max(0.0,now_ - fightSince_ - cinematicSeconds_) : -1; }
    tower::floorplay::Challenge Chal() const { return sched_ && run_.floor >= 1 && run_.floor <= 50 ? chal_[run_.floor] : tower::floorplay::Challenge::None; }
    // 通关一层：记用时、发附加挑战 / 限时挑战层的额外奖励（方案第 5 节；用户 2026-11-01：附加挑战没有惩罚）
    void FloorBonus(bool replayed = false) {
        const int tenths = static_cast<int>(std::lround(std::max(0.0, fight_.seconds) * 10));
        if (static_cast<int>(run_.times.size()) < run_.floor) run_.times.resize(run_.floor, 0);
        run_.times[run_.floor - 1] = tenths;
        bonusText_.clear();
        bonusEarned_ = false;
        if (sched_ && !replayed) {  // 回退后重打：挑战不再计次、不发额外奖励
            const auto c = Chal();
            if (c != tower::floorplay::Challenge::None) {
                ++run_.chalOffered;
                const bool done = tower::floorplay::Done(c, fight_, run_.difficulty);
                if (done) ++run_.chalDone;
                if (done && c == tower::floorplay::Challenge::Timed) ++run_.doneTimed;
                if (done && c == tower::floorplay::Challenge::NoPotion) ++run_.donePotion;
                bonusEarned_ = tower::floorplay::BonusItem(c, fight_, run_.difficulty);
                bonusText_ = std::format("{}：{}", tower::floorplay::ChallengeName(c), done ? "完成" : "未达成");
                pbz::log::Info("影塔第 {} 层挑战「{}」：{}（用时 {:.1f} 秒，受 Boss 伤害 {}，用药 {}）", run_.floor, tower::floorplay::ChallengeName(c), done ? "完成" : "未达成",
                               fight_.seconds, fight_.hit ? "是" : "否", fight_.potion ? "是" : "否");
            }
        }
        const auto rec = Record(run_.floor, tenths);
        pbz::log::Info("影塔第 {} 层用时 {}{}", run_.floor, tower::floorplay::Clock(fight_.seconds), rec ? "；新纪录" : "");
    }
    // 额外挑战（限时奖励 / 无伤 / 禁药）成功：随机一件符合当前进度的非 Boss 关联原生武器 / 药 / 饰品，走原生获取界面；
    // 独立发放，不替代 Boss 掉落、不占普通奖励。同一层只发一次（结算防重）
    void BonusChallengeItem() {
        if (!bonusEarned_ || !grow_.on) return;
        bonusEarned_ = false;
        if (bonusFloor_ == run_.floor && bonusRun_ == run_.id) return;
        bonusFloor_ = run_.floor; bonusRun_ = run_.id;
        grow_.hexBonusSecond = 0;
        // 1.0.1 用户 2026-11-07：不再自动发一件，改为额外三选一（普通奖励之后弹）；池含非 Boss 原生装备 / 药 / 饰品与 MOD 道具（含复活符、重生符）
        if (const int n = grow_.DrawBonus(run_.floor, run_.floors, run_.seed, run_.items); n > 0) {
            bonusText_ += "　额外奖励：领完本层奖励后三选一";
            std::string list;
            for (auto& o : grow_.bonus) list += (list.empty() ? "" : "、") + tower::rewards::Label(o);
            pbz::log::Info("影塔第 {} 层挑战奖励：额外三选一 {}", run_.floor, list);
        } else {
            pbz::log::Info("影塔第 {} 层挑战奖励：额外奖励池没有可发的（装备都已持有、道具已满），本次不发", run_.floor);
        }
    }
    // 本机每层最快用时：%LOCALAPPDATA%\PBZTower\records.txt（一行 "层 0.1 秒"）；返回 true = 打破纪录
    bool Record(int floor, int tenths) {
        if (tenths <= 0) return false;
        const auto path = TowerFile(L"records.txt");
        std::map<int, int> best;
        { std::istringstream in(ReadText(path)); int f = 0, t = 0; while (in >> f >> t) best[f] = t; }
        const bool better = !best.count(floor) || tenths < best[floor];
        if (better) {
            best[floor] = tenths;
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            for (auto& [f, t] : best) out << f << ' ' << t << '\n';
        }
        return better;
    }
  public:
    // 左上 HUD（tower_hud）：层数 / 层型 / 计时 / 附加挑战
    // v14（用户 2026-11-05）：只显示当前层数（不写 1v1 / 车轮等类型）。层数只在实际开下一层时更新：
    // 领奖、整备、购物、看背包期间仍显示刚完成的那一层（开局为第 0 层）
    std::string HudFloor() const {
        if (run_.phase == tower::Phase::Idle || trip_.phase() != tower::trip::Phase::Inside && grow_.on) return {};
        // 1.0.1 用户 2026-11-07：层数旁边写难度，玩家一眼知道在打哪种
        if (run_.inMobWave) return std::format("第 {} 层（{}）：{}/{}", ShownNow(), tower::floorplay::DifficultyName(run_.difficulty), mob_.wave + 1, mob_.total);
        // 用户 2026-11-12：无尽混编层有多波时把「当前波/总波数」写在层号旁（单波层不写）
        if (EndlessOn() && run_.phase == tower::Phase::Fighting && endless_.size() > 1)
            return std::format("第 {} 层（{}）：{}/{}", ShownNow(), tower::floorplay::DifficultyName(run_.difficulty), endlessWave_ + 1, endless_.size());
        return std::format("第 {} 层（{}）", ShownNow(), tower::floorplay::DifficultyName(run_.difficulty));
    }
    std::string HudClock() const { return run_.phase == tower::Phase::Fighting ? tower::floorplay::Clock(FightSeconds()) : std::string(); }
    std::string HudGoal() const {
        if (run_.phase == tower::Phase::Fighting && now_ < reviveNoticeUntil_)
            return std::format("复活符生效（剩 {} 个）", run_.items[static_cast<int>(tower::Item::Revive)]);
        if (run_.phase == tower::Phase::Fighting && stragglers_ > 0) return std::format("剩余敌人 {}", stragglers_);  // 4.1.0：打完了却不结算时提示还有几只
        if (run_.phase == tower::Phase::Fighting && SpecialOn()) {  // 3.7.0（用户）：计时器下写明玩法
            if (special_.kind == tower::special::Kind::Survival)
                return std::format("生存时长 {}", tower::floorplay::Clock(std::max(0.0, special_.survivalSec - std::max(0.0, FightSeconds()))));
            if (const auto note = tower::special::HudNote(special_.kind); !note.empty()) return std::string(note);
        }
        if (run_.phase == tower::Phase::Fighting && EndlessOn() && endlessWave_ > 0 && now_ < endlessWaveStart_)
            return std::format("第 {}/{} 波 · {} 秒后开战", endlessWave_ + 1, endless_.size(), static_cast<int>(std::ceil(endlessWaveStart_ - now_)));
        if (run_.phase != tower::Phase::Fighting || !sched_) return {};
        return tower::floorplay::ChallengeHud(Chal(), fight_, std::max(0.0, FightSeconds()), run_.difficulty);
    }
    const std::string& BonusText() const { return bonusText_; }
    std::string TimesText() const {
        std::string s; int total = 0;
        for (size_t i = 0; i < run_.times.size(); ++i) if (run_.times[i] > 0) { s += std::format("{}第 {} 层 {}", s.empty() ? "" : "\n", ShownBoss(static_cast<int>(i) + 1), tower::floorplay::Clock(run_.times[i] / 10.0)); total += run_.times[i]; }
        if (!s.empty()) s += std::format("\n合计 {}", tower::floorplay::Clock(total / 10.0));
        if (run_.chalOffered) s += std::format("\n附加挑战完成 {}/{}", run_.chalDone, run_.chalOffered);
        return s;
    }
  private:
    // 有塔 Boss 不在登记里、又没见到它死：5 秒内找同身份的原生换体（不在登记、不是别层残留、类名含本层 Boss 身份）接上；
    // 找不到就作废本层（不发奖、退回开层前），提示原因。返回 true = 本层已作废
    bool Vanished() {
        if (replay_) {  // 重放层：没归零就消失的阶段角色，等到转阶段时限再判
            for (auto& a : actors_) {
                // 34 层力者和锯者：一阶段两个同类角色，打死一个后原生转阶段（Seq114）把另一个清掉，二阶段正常到场。
                // 已被后续阶段取代的阶段里、没死就被原生清掉的角色 = 原生转阶段的正常清场，按死亡算，不作废
                if (a.gone && !a.dead && a.phase >= 0 && a.phase < rp_.seen) {
                    a.dead = true;
                    pbz::log::Info("影塔重放：第 {} 阶段的 {} 被原生转阶段清掉（第 {} 阶段已到场），按阶段结束算", a.phase + 1, a.name, rp_.seen + 1);
                    continue;
                }
                // 同阶段已有新角色在场（原生把它销毁后重刷 / 机制换体）：旧的按被替换处理，不再按"没死就消失"作废
                if (a.gone && !a.dead && a.phase >= 0 &&
                    std::any_of(actors_.begin(), actors_.end(), [&](const Actor& b) { return &b != &a && b.phase == a.phase && !b.gone && !b.dead && ue::Live(b.obj); })) {
                    a.dead = true;
                    pbz::log::Info("影塔重放：第 {} 阶段的 {} 已被同阶段新角色替换，旧的按结束算", a.phase + 1, a.name);
                    continue;
                }
                // 阶段角色没死就消失、也没人替补：非过场时间 10 秒后请求该阶段刷怪点重新刷出，仍不来才按时限作废
                if (a.gone && !a.dead && a.phase >= 0 && a.phase == rp_.seen) KickWhenQuiet(a.phase, 10, "阶段角色没死就消失");
                if (a.gone && !a.dead && now_ - a.goneAt > replay_->phaseGap) {
                    const std::string why = std::format("原生遭遇重放：{} 没死就消失了 {:.0f} 秒：本层作废、不发奖", a.name, now_ - a.goneAt);
                    RecordFailure("vanished", a.boss, why);
                    CancelEntry(why);
                    return true;
                }
            }
            return false;
        }
        std::unordered_set<UObject*> mine;
        for (auto& a : actors_) mine.insert(a.obj);
        for (auto& a : actors_) {
            if (!a.gone || a.dead) continue;
            // 3.0.0：原生死亡组件确认真死（不是假死换阶段）后尸体很快被回收：按击杀算，不再 12 秒后"异常消失"作废本层
            if (DiedNatively(a)) {
                a.dead = true; ++warKills_;  // 以战养战
                pbz::log::Info("影塔第 {} 层：{} 原生死亡已确认、尸体已回收，按击杀算", run_.floor, a.name);
                continue;
            }
            // 4.1.0：无尽 / 创意层的 Boss 是单阶段直刷，已见归零后模型消失 = 死亡（不等 12 秒、不判"异常消失"作废）
            if (EndlessOn() && a.mobHp <= 0 && a.zeroSeen) {
                a.dead = true; ++warKills_;
                pbz::log::Info("影塔第 {} 层：{} 已归零、模型已移除，按击杀算", run_.floor, a.name);
                continue;
            }
            const std::string identity = a.boss >= 0 ? std::string(tower::kBossPool[a.boss].identity) : std::string();
            UObject* found = nullptr;
            if (!identity.empty())
                for (auto& t : attr::Targets()) {
                    if (t.who != attr::Who::Enemies || !attr::Alive(t) || mine.count(t.obj)) continue;
                    const std::string candidate = ue::ClassName(t.obj);
                    const auto original = tower::kBossPool[a.boss].path;
                    const auto ownerClass = original.substr(original.rfind('.') + 1);
                    if (!tower::encounter::RequiredBy(ownerClass, candidate).empty()) continue; // 机制替身 / 召唤物不是死亡后的阶段换体
                    if (!tower::IdentityIn(candidate, identity)) continue;
                    found = t.obj;
                    break;
                }
            if (found) {
                pbz::log::Info("影塔第 {} 层：{} 换体为 {}（同身份原生阶段角色），继续本层", run_.floor, a.name, ue::ClassName(found));
                const int boss = a.boss;
                a = Actor{found, a.name};
                a.boss = boss;
                a.adopted = true;
                // 换体同样按本层血量目标缩放、补满（走正常的首次登记流程）
                mine.insert(found);
                continue;
            }
            if (now_ - a.goneAt < (towerstorygate::Cinematic() ? 60.0 : 12.0)) continue;  // 机制短暂离场（遁地 / 飞走）留 12 秒，过场中不判
            const std::string why = std::format("{} 异常消失（没有死亡记录，也没有同身份换体）", a.name);
            RecordFailure("vanished", a.boss, why);
            if (run_.inMobWave) ReplaceUnresolvedWaveMob(a, why);
            else if (MobActor(a)) { EndlessUnresolvedMob(a, why); continue; }  // 无尽混编层的小怪：算这只结束，兜底补一只
            else CancelEntry(why + "：本层作废、不发奖");
            return true;
        }
        return false;
    }
    // 通关 / 失败：独立塔档里先停在结算页（保护保持、塔档不动），玩家点"退出影塔"（stop）才出塔恢复原档。
    // 旧做法直接 End()：自动回原档，结算页读到的已经是原档
    void Conclude() {
        if (trip_.phase() != tower::trip::Phase::Inside) { End(); return; }
        towerentry::End(); towertravel::Abort("结算"); EndReplay(); towerisolate::End(); tvenue::End(); boundary_.Disarm(); originSet_ = false; relay_.Clear();
        travelThenSpawn_ = false; spawnAt_ = 0; spawnQueue_.clear(); pending_ = 0;
        pbz::log::Info("影塔结算：停在结算页，等玩家点退出影塔（保护保持）\n{}", settlement_);
    }
    // 结算快照：一项一行。p = 玩家属性目标（死亡这一刻，回满血之前）
    // 死亡结算（在 run_.floor 还是倒下那一层时调用）：精简到七八行，原生确认框放得下按钮
    std::string DeathText(const Target& p) {
        auto v = [&](const char* n) { const int o = attr::AttrOffset(n); return o >= 0 ? ue::Get<float>(attr::AttrCurrent(p, o)) : 0.0f; };
        auto pct = [](double x) { auto t = std::format("{:.1f}", x); if (t.ends_with(".0")) t.resize(t.size() - 2); return "+" + t + "%"; };
        std::vector<std::string> lines;
        lines.push_back(std::format("很遗憾，倒在了第 {} 层。整备一下，再接再厉！", ShownNow()));
        lines.push_back(std::format("已完成 {} / {} 层", std::max(0, ShownNow() - 1), tower::floorplay::ShownTotal(run_.floors)));
        if (!run_.bosses.empty()) {
            std::string b;
            for (int i : run_.bosses) b += (b.empty() ? "" : "、") + std::string(tower::kBossPool[i].name);
            lines.push_back("对手：" + b);
        }
        lines.push_back(std::format("生命上限 {:.0f}　杀气上限 {:.0f}", v("Health_Max"), v("Shachi_Max")));
        lines.push_back("攻击力加成 " + pct((grow_.attrs.Mul(tower::rewards::AttrKind::Attack) - 1) * 100) +
                        "　破防能力加成 " + pct((grow_.attrs.Mul(tower::rewards::AttrKind::Break) - 1) * 100));
        if (grow_.on) {
            const auto l = towernative::ReadLoadout();
            auto named = [](const std::vector<std::string>& ids, int kind) {
                std::string o;
                for (auto& id : ids) {
                    if (id.empty() || id == "None") continue;
                    const int w = tower::growth::WeaponIndex(id, kind);
                    o += (o.empty() ? "" : "、") + (w >= 0 ? std::string(tower::growth::kWeapons[w].name) : id) + std::format(" {} 级", std::max(0, towernative::Level(id, kind)));
                }
                return o.empty() ? std::string("无") : o;
            };
            lines.push_back("主兵器：" + named(l.main, 0));
            lines.push_back("影之武装：" + named(l.shaqi, 1));
        }
        std::string out;
        for (auto& l : lines) out += (out.empty() ? "" : "\n") + l;
        return out;
    }
    std::string Settlement(const Target* p, bool won) {
        std::vector<std::string> lines;
        if (won && (run_.hidden || run_.floor > 50)) lines.push_back("第 100 层：左殇 · 魔渊 · 沐小葵，故人试剑，历练已成");
        lines.push_back(won ? std::format("结果：通关，共 {} 层", ShownBoss(run_.floors)) : std::format("结果：止步第 {} 层（已完成 {} 层，共 {} 层）", ShownNow(), std::max(0, ShownNow() - 1), tower::floorplay::ShownTotal(run_.floors)));
        if (!won && !run_.bosses.empty()) {
            std::string b;
            for (int i : run_.bosses) b += (b.empty() ? "" : "、") + std::string(tower::kBossPool[i].name);
            lines.push_back("失败 Boss：" + b);
            if (venueBoss_ >= 0) lines.push_back(std::string("场地：") + std::string(tower::kBossPool[venueBoss_].name) + " 的场地");
        }
        if (p) {
            auto v = [&](const char* n) { const int o = attr::AttrOffset(n); return o >= 0 ? ue::Get<float>(attr::AttrCurrent(*p, o)) : 0.0f; };
            // 用户 2026-11-06：攻击力 / 破防能力按全部武器的百分比加成显示
            auto pct = [](double x) { auto s = std::format("{:.1f}", x); if (s.ends_with(".0")) s.resize(s.size() - 2); return "+" + s + "%"; };
            const double edge = ItemOn(tower::Item::Edge) ? 1.15 : 1.0;
            lines.push_back("攻击力加成 " + pct((grow_.attrs.Mul(tower::rewards::AttrKind::Attack) * edge - 1) * 100));
            lines.push_back("破防能力加成 " + pct((grow_.attrs.Mul(tower::rewards::AttrKind::Break) * edge - 1) * 100));
            lines.push_back(std::format("生命 {:.0f}/{:.0f}", v("Health"), v("Health_Max")));
            lines.push_back(std::format("杀气 {:.0f}/{:.0f}", v("Shachi"), v("Shachi_Max")));
        }
        if (grow_.on) {
            const auto l = towernative::ReadLoadout();
            auto named = [](const std::vector<std::string>& ids, int kind) {
                std::string s;
                for (auto& id : ids) if (!id.empty()) s += (s.empty() ? "" : "、") + id + std::format(" {} 级", std::max(0, towernative::Level(id, kind)));
                return s.empty() ? std::string("无") : s;
            };
            lines.push_back("主兵器：" + named(l.main, 0));
            lines.push_back("影之武装：" + named(l.shaqi, 1));
            lines.push_back("药：" + (l.medBottom.empty() ? std::string("无") : l.medBottom) + (l.medTop.empty() ? "" : "、" + l.medTop));
            std::string acc;
            for (auto& a : l.accessories) acc += (acc.empty() ? "" : "、") + a;
            lines.push_back("饰品：" + (acc.empty() ? std::string("无") : acc));
            std::string bless;
            for (int i = 0; i < tower::growth::kBlessCount; ++i)
                if (grow_.bless.stacks[i]) bless += (bless.empty() ? "" : "、") + std::format("{}×{}", tower::growth::kBlessings[i].name, grow_.bless.stacks[i]);
            lines.push_back("祝福：" + (bless.empty() ? std::string("无") : bless));
            lines.push_back(std::format("淬炼点 {}　修为点 {}　铸魂令 {}", grow_.profile.temper, grow_.profile.cultivate, grow_.profile.soulTokens));
        }
        lines.push_back(std::format("魂玉 {}", run_.jade));
        std::string items;
        for (int i = 0; i < tower::kItemCount; ++i) if (run_.items[i] > 0) items += (items.empty() ? "" : "、") + std::format("{}×{}", tower::kItems[i].name, run_.items[i]);
        lines.push_back("道具：" + (items.empty() ? std::string("无") : items));
        std::string out;
        for (auto& s : lines) out += (out.empty() ? "" : "\n") + s;
        return out;
    }
  public:
    const std::string& SettlementText() const { return settlement_; }
  private:

    // 多 Boss 层的陪打：不吃场景（A 类）、和已选的不同身份（主 Boss 在它的原生场地，陪打要在哪都能完整打）
    static bool Companion(int chosen, int candidate) {
        const auto& c = tower::kBossPool[candidate];
        return c.cls == 'A' && !c.scene && c.identity != tower::kBossPool[chosen].identity;
    }

    tower::floorplay::Resolved ResolveScheduled(int floor, tower::floorplan::Floor& def, bool log) const {
        def = tower::floorplay::ScheduledFloor(run_.seed, floor, run_.shuffleBosses);
        auto nativeOk = [&](int b) {
            if (tower::floorplay::NativeOnly(b).empty()) return true;
            const auto& c = tower::kBossPool[b];
            const bool ok = !NativeBad(b) && towertravel::HasVenue(c.path, c.identity);
            if (!ok && log) pbz::log::Info("影塔方案：{} 只能在原生场地打（{}），原生场地本局不可用", c.name, tower::floorplay::NativeOnly(b));
            return ok;
        };
        if (run_.shuffleBosses && floor <= 50 && def.replay.empty() && !run_.clearedRoute[floor].empty()) {
            const auto& prior = run_.clearedRoute[floor];
            bool usable = true;
            for (const auto& path : prior) { const int b = tower::floorplay::PoolIndex(path); usable = usable && b >= 0 && Admitted(b) && !badBosses_.count(b) && nativeOk(b); }
            if (usable) {
                const bool venueFollowsBoss = def.venue == def.bosses[0];
                for (int i = 0; i < 3; ++i) def.bosses[i] = i < static_cast<int>(prior.size()) ? std::string_view(prior[i]) : std::string_view{};
                def.count = static_cast<int>(prior.size()); def.altN = 0;
                if (venueFollowsBoss) def.venue = def.bosses[0];
            } else if (log) pbz::log::Info("影塔方案：第 {} 层路线记录的 Boss 本次不可用，按本层计划和备选开打", floor);
        }
        return tower::floorplay::Resolve(def, [&](int b) { return Admitted(b) && !badBosses_.count(b) && nativeOk(b); }, ShownBoss(floor));
    }

    void Next() {
        if (run_.phase != tower::Phase::Ready) { Broadcast("err.cannot_next", "现在不能开下一层"); return; }
        if (grow_.hexJournal.active && !FinishHexJournal()) { Broadcast("err.cannot_next", "正在恢复奖励交易，请稍后重试"); return; }
        if (grow_.on && grow_.hexAttributes.remaining) { Broadcast("err.cannot_next", grow_.hexAttributes.blitz ? "请先领取速战速决的属性奖励" : "请先领取属性叠属性的奖励"); return; }
        if (grow_.on && grow_.forgeBatch.Active()) { Broadcast("err.cannot_next", "请先在驿站选完锻造的成长属性"); return; }  // 3.6.1 批量锻造
        if (rebirthReload_ || (grow_.on && trip_.phase() == tower::trip::Phase::Entering)) { Broadcast("err.cannot_next", "世界还在载入，稍后再开"); return; }
        if (run_.floor >= run_.floors && !run_.endless) { Broadcast("err.cannot_next", "后续层数暂未开放"); return; }  // v14：第 50 / 51 层不是终点，后续层预留
        towermusic::Stop("开下一层");
        if (towerwidgets::PanelActive()) { Broadcast("err.cannot_next", "请先关闭原生整备面板"); return; }
        if (travelThenSpawn_ || towertravel::Busy()) { Broadcast("err.traveling", "正在前往场地"); return; }
        if (trip_.phase() == tower::trip::Phase::Entering) { Broadcast("err.enter_pending", "还没进入塔档：请回到标题画面，再点“继续”"); return; }
#ifdef PBZ_RELEASE
        // 独立塔档硬门槛：没有载入并核验过塔档（Inside + 原生成长开着）就不开层，绝不在玩家原档上打
        if (!grow_.on || trip_.phase() != tower::trip::Phase::Inside) {
            pbz::log::Info("影塔：拒绝开层——未在塔档里（成长 {}，塔档 {}）", grow_.on ? "开" : "关", tower::trip::PhaseName(trip_.phase()));
            Broadcast("err.not_in_tower_save", "还没进入独立塔档，未开层（原存档未改动）。请退出影塔后重新进入");
            return;
        }
#endif
        if (grow_.on && grow_.hexes.enabled) {
            const int nextShown = ShownNext();
            const auto previous = grow_.hexes;
            const int missed = grow_.hexes.pending ? 0 : grow_.hexes.MissedExtra(nextShown);  // 3.1.0：旧局补发
            if (grow_.hexes.pending || (missed && grow_.hexes.Begin(missed, HexDrawMask())) || grow_.hexes.Begin(nextShown, HexDrawMask())) {
                if (missed && grow_.hexes.pending == missed) pbz::log::Info("影塔海克斯：补发第 {} 层的选择（3.6.0 每 5 层一个，按已选数量补）", missed);
#ifdef PBZ_PLAYTEST
                if (!grow_.hexes.Has(tower::hex::Forge)) {  // 3.3.4 测试（用户）：锻造大师固定放第 1 张，测能否正常选择和使用
                    auto& o = grow_.hexes.offers;
                    if (auto it = std::find(o.begin(), o.end(), static_cast<int>(tower::hex::Forge)); it != o.end()) std::iter_swap(o.begin(), it);
                    else o[0] = tower::hex::Forge;
                }
#endif
                if (!SaveHex()) { grow_.hexes = previous; Broadcast("err.hex_save", "海克斯保存失败，请重试"); return; }
                pbz::log::Info("影塔海克斯：第 {} 层候选 {} / {} / {}", grow_.hexes.pending, tower::hex::defs[grow_.hexes.offers[0]].name, tower::hex::defs[grow_.hexes.offers[1]].name, tower::hex::defs[grow_.hexes.offers[2]].name);
                Broadcast("info.hex_offer", "请选择海克斯"); return;
            }
            const int mile = tower::hex::Milestone(nextShown);
            if (mile >= 0 && !(grow_.hexes.completed & (1u << mile))) { Broadcast("err.hex_pool", "可用海克斯不足，本层暂不开启"); return; }
        }
        FloorGc();
        failedStage_ = {};  // 只有实际开层时才清理旧失败快照，展示海克斯不算开层。
        originSet_ = false;
        boundary_.Disarm();
        towergate::Touch();
        if (grow_.on && !GrowBeforeFloor()) return;
        towerentry::TopUpNow();  // 开层装备（含换的药）已套用：立刻补满，读秒时再补满并钉住
        // 先定本层 Boss，再定去哪：主 Boss 吃场景（且标定过原生场地）→ 它的原生场地，否则本区通用擂台；
        // 不在那里先传过去（跨关卡走钟），到了再生成。第一次离开前备份存档（钟传送会让游戏存位置）、记下进塔位置
        if (arenaOn_ && !backedUp_) {
            std::string where;
            if (!BackupSave(&where)) { Broadcast("err.backup_failed", "存档备份失败，未开层：" + where); return; }
            backedUp_ = true;
            pbz::log::Info("影塔：已备份游戏存档 → {}", where);
        }
        int specialMainF = -1; tower::special::Floor specialPick{};
        // 1.4.0 小怪波次：开第 F 层 Boss 之前先打完本段的几轮（用户 2026-11-08：回退重打也要打，只是不再开店）
        if (const int F = run_.floor + 1; grow_.on && !mobOff_ && !s_hidden && !run_.hidden && plan_.empty() && fixedBoss_ < 0 &&
            tower::mobs::HasStage(F, run_.floors)) {
            tower::special::Floor sp = tower::special::MainFloor(run_.seed, F, run_.specialFrom);  // 4.0.0：老局从读档进度往后按比例放
            if (run_.mobFloor == F && run_.mobWave > 0) sp = {};  // 4.1.0：已经打了几轮的小兵段不换成创意层（读档接着打）
#if defined(PBZ_HEX_TEST) || defined(PBZ_PLAYTEST)
            if (qaSpecial_ != tower::special::Kind::None) {  // 打完（FinishSpecialMain）才清掉；4.1.0：前 50 层按前期规则（属性减半、生存 1:00）
                const bool early = F < tower::special::kFirstMainF;
                sp = {qaSpecial_, qaSpecial_ == tower::special::Kind::Survival ? (early ? tower::special::kEarlySurvivalSec : tower::special::kSurvivalFirstSec) : 0, early};
                specialMainF = F; specialPick = sp;
            }
            else
#endif
            if (!MobStageDone(F)) {
                if (sp.kind == tower::special::Kind::None) { StartMobStage(F); return; }
                specialMainF = F; specialPick = sp;  // 3.7.0 创意层：这一段换成特殊层，走无尽层引擎（大场地），打完按小兵段结算
            }
            if (specialMainF < 0 && run_.CanOpenStageShop(F)) { OpenStageShop(F); SaveRun(); Broadcast("info.shop", "驿站开了"); return; }  // 读档接着打的情况：小怪打完了但店还没开
        }
        // 吃场景的 Boss 只在有原生场地（游戏数据里的刷怪点或手动标定）时才抽，不硬放到通用擂台上打出问题
        // 本局生成失败 / 场地去不了的不再抽
        std::unordered_set<int> placeable;
        for (int i = 0; i < kPool; ++i)
            if ((!tower::kBossPool[i].scene || towertravel::HasVenue(tower::kBossPool[i].path, tower::kBossPool[i].identity)) && !badBosses_.count(i) &&
                (!NativeBad(i) || VenueFor(i).kind == tower::arena::Kind::Arena))  // 原生场地坏了：只有已验证竞技场可回退时才抽
                placeable.insert(i);
        travelled_ = false;
        venueArrived_ = false;
        fightSince_ = 0;
        relay_.Clear();
        const bool endlessFloor = specialMainF > 0 || (run_.endless && run_.floor + 1 > 50 && plan_.empty() && fixedBoss_ < 0);
        sched_ = !endlessFloor && !schedOff_ && plan_.empty() && fixedBoss_ < 0 && run_.floors >= 50 && tower::floorplay::Find(run_.floor + 1);
        // 准入（硬条件）：固定 Boss 时只有它、每层 1v1；否则正式池只放验证过的；准入池凑不出多 Boss 就退成 1v1
        auto admit = [&](int c) { return Admitted(c) && (fixedBoss_ < 0 || c == fixedBoss_); };
        staggerFloor_ = false;
        venuePath_.clear();
        EndlessReset();
        const bool wasArena = arenaOn_;
        if (endlessFloor) {  // 无尽模式（第 51 层 = 显示 100 起）：随机大场地 + 随机波次（tower_endless.h）
            if (specialMainF < 0 && CheckpointFloor(ShownBoss(run_.floor + 1))) SaveCheckpoint();  // 3.4.5：显示 100 层起每 20 层存档点，开打前记下整局
            if (specialMainF > 0) { if (!StartEndlessFloor(specialPick, specialMainF)) { StartMobStage(specialMainF); return; } }  // 凑不出阵容：退回普通小兵段
            else if (!StartEndlessFloor(tower::special::EndlessFloor(run_.seed, run_.floor + 1))) return;
        } else if (sched_) {  // 50 层方案（notes/tower-50-floors-plan.md）：这一层打谁、什么层型、在哪个场地都按逐层表；主选不可用换备选
            const int nextFloor = run_.floor + 1;
            tower::floorplan::Floor def{};
            plan_f_ = ResolveScheduled(nextFloor, def, true);
            if (!plan_f_.ok) {
                // No actor was created: only retain the planned identity for
                // an explicit skip, never record a victory or grant a drop.
                if (grow_.on && run_.floors >= 50 && nextFloor < run_.floors && !run_.hidden && !s_hidden)
                    failedStage_ = tower::FailedStage::Planned(run_, def);
                floorNote_ = plan_f_.why;
                if (FailedStageAvailable()) floorNote_ += "；可重试，或在本层问题中选择跳过";
                Broadcast("err.boss_denied", floorNote_);
                return;
            }
            if (!plan_f_.swaps.empty()) pbz::log::Info("影塔方案：第 {} 层换用备选（{}）", def.floor, plan_f_.swaps);
            count_ = run_.ForcedFloor(plan_f_.bosses);
            // 用户 2026-11-05：第 51 层隐藏 1v3 是三只同时生成、同时开打，不错峰
            staggerFloor_ = plan_f_.mode == tower::floorplay::Mode::Trio && run_.floor <= 50;
            if (staggerFloor_ && std::all_of(plan_f_.bosses.begin(), plan_f_.bosses.end(), [&](int b) { return tower::kBossPool[b].identity == tower::kBossPool[plan_f_.bosses[0]].identity; }))
                staggerFloor_ = false;  // 同一身份的三形态（12 层玉琮 P3）：原生是同时刷出，不错峰
            replay_ = nullptr;
            if (plan_f_.mode == tower::floorplay::Mode::Replay) {
                replay_ = replayOn_ && arenaOn_ ? tower::replay::Find(def.replay) : nullptr;
                if (replay_ && storyDegraded_) {  // 剧情保护不可用：重放离不开蓝图执行门，改为按 1v1 打第一阶段（不再整层跳过）
                    pbz::log::Info("影塔方案：第 {} 层原生遭遇重放需要剧情保护，当前已降级，改为直接开打第一阶段", def.floor);
                    replay_ = nullptr;
                }
                if (replay_ && directFloor_ == def.floor) {  // 这一层的原生入场刚才没启动：直接开打
                    pbz::log::Info("影塔方案：第 {} 层原生入场上次没有启动，本次在原生场地直接开打第一阶段", def.floor);
                    replay_ = nullptr;
                }
                pbz::log::Info("影塔方案：第 {} 层原生遭遇重放 {}", def.floor, replay_ ? std::string(replay_->name) : std::string("未启用（按 1v1 打第一阶段）：") + (replayOn_ ? "没有遭遇契约 / 场地传送关闭" : "开局 replay:off"));
            }
            pbz::log::Info("影塔方案：第 {} 层 {}，{} 只，场地 {}", def.floor, tower::floorplay::ModeName(plan_f_.mode, count_), count_,
                           plan_f_.venue >= 0 ? tower::kBossPool[plan_f_.venue].name : std::string_view("本区经典场地"));
        } else if (!plan_.empty()) {  // 审计计划：这一层就是计划里写的组合（开局时已校验准入与身份）
            const size_t idx = static_cast<size_t>(run_.floor);
            if (idx >= plan_.size()) { Broadcast("err.plan", "审计计划已经打完"); return; }
            for (int b : plan_[idx].bosses)
                if (!Admitted(b)) { Broadcast("err.boss_denied", std::string("计划里的 ") + std::string(tower::kBossPool[b].name) + " 已被禁用，本层不开"); return; }
            for (int b : plan_[idx].bosses)
                if (NativeBad(b) && VenueFor(b).kind != tower::arena::Kind::Arena) {  // 32ffb7d 教学左殇：无钟可达，旧逻辑每次重试同一条坏路、像没反应
                    floorNote_ = std::string("计划里的 ") + std::string(tower::kBossPool[b].name) + "：" + VenueFor(b).why + (lastTravelWhy_.empty() ? "" : "（" + lastTravelWhy_ + "）") + "，本层不开；请换计划或暂离";
                    Broadcast("err.venue_unreachable", floorNote_);
                    return;
                }
            if (plan_[idx].endless) {  // 无尽混编层：计划里写的 Boss 只定场地，波次按种子随机
                endless_ = tower::endless::Generate(run_.seed, run_.floor + 1, EndlessPools());
                if (endless_.empty()) { Broadcast("err.boss_denied", "无尽混编层：没有可直接生成的 Boss"); return; }
                endlessAnchor_ = plan_[idx].bosses[0];
                count_ = run_.ForcedFloor(endless_[0].bosses);
                std::string desc;
                for (size_t w = 0; w < endless_.size(); ++w) {
                    desc += std::format("{}第 {} 波：", w ? "；" : "", w + 1);
                    for (int b : endless_[w].bosses) desc += std::string(tower::kBossPool[b].name) + " ";
                    desc += std::format("+ 小怪 {} 只", endless_[w].mobs.size());
                }
                pbz::log::Info("影塔无尽混编层：第 {} 层 {} 波（场地 {}）：{}", run_.floor, endless_.size(), tower::kBossPool[endlessAnchor_].name, desc);
            } else count_ = run_.ForcedFloor(plan_[idx].bosses);
            staggerFloor_ = plan_[idx].stagger && !EndlessOn();
            if (plan_[idx].here) arenaOn_ = false;
            venuePath_ = plan_[idx].venue;
            if (!venuePath_.empty()) pbz::log::Info("影塔审计计划：第 {} 层在固定场地 {} 打（影塔自己刷，原生刷怪点只静默）", idx + 1, venuePath_);
        } else {
        const int maxCount = 1; // Authored group initialization/coordination must be proved before enabling multi-boss floors.
        count_ = run_.NextFloor(kPool, &Tower::Companion, [&](int c) { return placeable.count(c) > 0; }, admit, maxCount);
        if (!count_ && fixedBoss_ < 0) {
            count_ = run_.NextFloor(kPool, &Tower::Companion, [&](int c) { return placeable.count(c) > 0; }, admit, 1);
            if (count_) pbz::log::Info("影塔准入：第 {} 层准入池凑不出多 Boss，改为 1v1", run_.floor);
        }
        staggerFloor_ = count_ >= 3;  // 正式 1v3：第二只 7 秒、第三只 14 秒入场
        }
        if (!count_) { Broadcast("err.pool_empty", "准入池里没有可用的 Boss（只放实机验证过的内容）"); return; }
#ifndef PBZ_RELEASE
        // 验收指定 Boss 时也使用其正式原生遭遇契约；正式随机层和发布版不受此开发入口影响。
        if (fixedBoss_ >= 0 && replayOn_ && arenaOn_ && !storyDegraded_ && run_.bosses.size() == 1) {
            const auto path = tower::kBossPool[run_.bosses[0]].path;
            const auto dot = path.rfind('.');
            const auto cls = dot == std::string_view::npos ? path : path.substr(dot + 1);
            for (const auto& c : tower::replay::kContracts)
                if (c.phaseN > 0 && c.phases[0] == cls) { replay_ = &c; break; }
            if (replay_) pbz::log::Info("影塔开发验收：固定 Boss 使用原生遭遇契约 {}", replay_->id);
        }
#endif
        if (grow_.on && grow_.shop.Open()) tower::rewards::CloseShop(grow_.shop);
        // 原生重放的等待和CG由遭遇自己驱动；普通入场门的90秒从开层算，不能覆盖玩家找入口的时间。
        if (replay_ && !tower::replay::UsesCountdown(*replay_)) towerentry::BeginReplay();
        else towerentry::Begin();

        if (arenaOn_) {
            // 用谁的场地：有吃场景的就用它的（陪打都是 A 类，在哪都能打）；否则用本层最强（基础血量最高）且有场地的那只
            // 排序键：吃场景 > 有场地 > 基础血量
            auto rank = [](int b) {
                const auto& c = tower::kBossPool[b];
                return std::tuple(c.scene, towertravel::HasVenue(c.path, c.identity), c.hp);
            };
            int venue = run_.bosses[0];
            for (int b : run_.bosses) if (rank(b) > rank(venue)) venue = b;
            if (sched_) {
                const int planned = plan_f_.venue >= 0 ? plan_f_.venue : plan_f_.zoneClassic;
                venue = planned;
                std::string because;
                auto usableVenue = [&](int v) { return v >= 0 && !NativeBad(v) && towertravel::HasVenue(tower::kBossPool[v].path, tower::kBossPool[v].identity); };
                const bool sceneBound = std::any_of(run_.bosses.begin(), run_.bosses.end(), [](int b) { return tower::kBossPool[b].scene; });
                if (stayFloor_ == run_.floor && stayVenue_ >= 0 && stayVenue_ != venue && !sceneBound && usableVenue(stayVenue_) && !usableVenue(venue)) {
                    because = std::format("同层换 Boss 后留在已到的 {} 场地，不再跨图", tower::kBossPool[stayVenue_].name);
                    venue = stayVenue_;
                }
                if (!usableVenue(venue)) {
                    because = venue < 0 ? "没有计划场地" : NativeBad(venue) ? "本局已判坏（落地 / 剧情冻结失败）" : "没有场地数据 / 去不了";
                    venue = plan_f_.zoneClassic;
                }
                // 回退资格（36-38：原场地坏了又退到同样坏的毕峰场地，再失败一轮）：经典场地本局也坏了就不再去，取消并说明
                if (!usableVenue(venue)) {
                    const std::string why = std::format("第 {} 层场地 {} {}，本区经典场地 {} 本局也不可用：本层不开，请暂离或换段", ShownBoss(run_.floor),
                                                        planned >= 0 ? tower::kBossPool[planned].name : std::string_view("-"), because,
                                                        plan_f_.zoneClassic >= 0 ? tower::kBossPool[plan_f_.zoneClassic].name : std::string_view("-"));
                    arenaOn_ = wasArena;
                    run_.UndoFloor(); floorNote_ = why;
                    SaveRun();
                    pbz::log::Info("影塔方案：{}", why);
                    Broadcast("err.venue_unreachable", why);
                    return;
                }
                if (venue != planned) pbz::log::Info("影塔方案：第 {} 层实际场地 {}（原定 {}：{}；只换场地，Boss 不变，不算原生场地通过）", run_.floor, tower::kBossPool[venue].name,
                                                    planned >= 0 ? tower::kBossPool[planned].name : std::string_view("-"), because);
            }
            if (EndlessOn() && endlessAnchor_ >= 0) venue = endlessAnchor_;  // 无尽混编层：场地按计划里写的 Boss
            const auto& lead = tower::kBossPool[venue];
            venueBoss_ = venue;
            towerisolate::Begin(lead.identity);  // 出发之前就开隔离：进场地本身可能触发原生流程
            towerisolate::SetReplay(replay_);     // 重放层：契约里的刷怪点 / 过场 / 交互物件放行（战后过场照样冻结）
            towertravel::RecordHome();
            int went;
            if (!venuePath_.empty()) went = towertravel::GoFloor(run_.Zone(), run_.seed, venuePath_, "", lead.name, false);
            else if (NativeBad(venue)) {  // 原生场地本局判坏：去已验证的经典竞技场（选 Boss 时已确认可回退）
                const auto choice = VenueFor(venue);
                pbz::log::Info("影塔场地：{} {}", lead.name, choice.why);
                went = choice.kind == tower::arena::Kind::Arena ? towertravel::GoArena(choice.arena) : -1;
            } else went = towertravel::GoFloor(run_.Zone(), run_.seed, lead.path, lead.identity, lead.name, lead.scene);
            travelled_ = went >= 0;  // 0 = 已在场地里，同样算"在这个场地生成"
            arrivalRetries_ = 0;
            if (went < 0) { arenaOn_ = wasArena; CancelEntry("无法进入本层场地，未在错误地点生成 Boss"); return; }
            if (went == 1) {
                travelThenSpawn_ = true;
                Broadcast("info.traveling", "前往场地…");
                return;
            }
        }
        if (!arenaOn_) { towerisolate::Begin(""); towerisolate::SetReplay(nullptr); replay_ = nullptr; }
        spawnAt_ = now_ + 0.01; spawnTries_ = 0; transientTries_ = 0; audited_ = false; floorAudit_ = {}; gateSince_ = -1;
        arenaOn_ = wasArena;
    }

    void Spawn() {
        gateArmed_ = admitted_ = reachDone_ = false; admitSince_ = -1; admitPushed_ = false; admitRecalls_ = 0; teleportPulls_ = 0; pushedAt_ = 0; lastAdmitWhy_.clear(); nextReach_ = 0; lastReach_.clear(); nativeTried_ = false; nativeAdoptUntil_ = 0; replayWaitSince_ = -1; bossPlaced_ = 0;  // 每次入场重新准入（回退场地后整套重验）
        transient_ = false;
        pending_ = count_;
        failed_ = noRoom_ = false;
        before_ = pbz::enemies::OwnedActors();
        spawnQueue_.clear();
        if (!tvenue::CloseDoors()) {
            if (run_.inMobWave) CancelWave("小怪场地内墙未准备好", tower::wave::Failure::Venue);
            else CancelEntry("场地内墙未准备好，本层未开始；请保存退出后反馈");
            return;
        }
        if (run_.inMobWave) { SpawnWave(); return; }
        // 只能原生刷出的 Boss（荆野带马）：在自己的原生场地、单 Boss 层，直接让原生刷怪点刷，按类名接管；刷怪点不可用才走直接生成
        if (run_.bosses.size() == 1 && travelled_ && venueBoss_ == run_.bosses[0] &&
            (tower::floorplay::SpawnerFirst(run_.bosses[0]) || (nativeRetryFloor_ == run_.floor && nativeRetryBoss_ == run_.bosses[0]))) {
            pbz::log::Info("影塔第 {} 层：{} 只由原生刷怪点刷出（直接生成会落在原点）", run_.floor, tower::kBossPool[run_.bosses[0]].name);
            if (TryNativeSpawner(run_.bosses[0])) { Broadcast("info.spawning", "生成中"); return; }
        }
        if (sched_ && tower::floorplay::Relay(plan_f_.mode)) {  // 车轮 / 连战 / 塔主：先刷第一只，后面的等前一只确认死亡
            relay_.Begin(run_.bosses);
            pending_ = 1;
            SubmitBoss(run_.bosses[0]);
            Broadcast("info.spawning", std::format("车轮第 1/{} 位", relay_.total));
            return;
        }
        const auto sides = sched_ && plan_f_.mode == tower::floorplay::Mode::Brawl ? tower::floorplay::Factions(run_.bosses) : std::vector<std::string>{};
        for (size_t i = 0; i < run_.bosses.size(); ++i) {
            // 3.7.0 创意层：入场读秒要等全部 Boss 回执（pending_ 归零），错峰队列在开战后才处理 → 实机卡在读秒前。
            // 改为读秒前一起生成（每只约 100–150 ms 的卡顿落在入场加载阶段，不在战斗中）
            const double delay = staggerFloor_ ? tower::EntryDelay(3, static_cast<int>(i)) : 0.0;
            if (SpecialOn() && i >= kSpecialInFlight) spawnQueue_.push_back({now_, run_.bosses[i]});  // 3.7.0 实机：一次提交 5 只 → 核心"请求队列已满"，整层作废；最多同时 3 个，其余回执后补交
            else if (delay > 0) spawnQueue_.push_back({now_ + delay, run_.bosses[i]});  // 错峰：到时再生成（Tick）
            else SubmitBoss(run_.bosses[i], i < sides.size() ? sides[i] : FactionFor(run_.bosses[i]));
        }
        if (EndlessOn()) QueueEndlessMobs(0);
        staggerBase_ = now_;
        if (!spawnQueue_.empty()) pbz::log::Info("影塔第 {} 层：1v{} 错峰入场（{} 只稍后进场）", run_.floor, count_, spawnQueue_.size());
        Broadcast("info.spawning", "生成中");
    }
    void SubmitBoss(int boss, const std::string& faction = "") {
        const int floor = run_.floor;
        lastSubmitBoss_ = boss;
        const bool relayNext = fightSince_ > 0 && relay_.total > 1 && relay_.index > 1;
        if (relayNext) relayRequestWatch_.Begin();
        {
            const auto& b = tower::kBossPool[boss];
            pbz::enemies::Request r;
            if (!faction.empty() && faction != "enemy") r.faction = faction;  // 混战：enemy2 / chaos，Boss 之间互相攻击
            r.command = "spawn";
            r.path = std::string(b.path);
            r.requestId = std::format("tower-{}-{}-{}-", run_.id, run_.floor, ++spawnSeq_); // trailing boundary prevents seq1 clearing seq10
            r.count = 1;
            r.distanceCm = 800;
            // 4.0.0（用户：无尽层很多 Boss 刷在雾门外）：原先 Boss 一律刷在玩家周围 8 米，第 2 / 3 波玩家常在场边、入场时玩家在门口，
            // Boss 就落到雾门外；准入再把战斗区改成包住它的圆，Boss 留在门外。无尽 / 创意层的敌方 Boss 改为和小怪一样刷在场内安全区
            // （雾门凸包中心附近，核心按区域找落点；核心只对 "-mob" 请求放行区域，所以请求号带 -mobb）。乙队（enemy2）核心不支持区域，照旧刷在玩家附近，由入场放回 / 出界拉回兜底
            bool region = false;
            if (EndlessOn() && r.faction.empty() && !endlessRegionOff_) {
                tower::arena::P3 c; double rad = 0;
                if (WaveSpawnRegion(c, rad)) {
                    region = true;
                    r.requestId = std::format("tower-{}-{}-mobb{}-", run_.id, run_.floor, spawnSeq_);
                    r.distanceCm = std::clamp(rad * 0.5, 300.0, 800.0);
                    r.faction = std::format("tower-region {:.3f} {:.3f} {:.3f}", c.x, c.y, rad);
                }
            }
            // 回执在游戏线程按顺序到达：新出现、又不属于本层已记下的就是这只
            const auto request=r.requestId;const auto epoch=spawnEpoch_;
            pbz::enemies::Submit(std::move(r), [this, floor, boss, request,epoch,relayNext,region,name = std::string(b.name)](std::string json) {
                if (spawnEpoch_!=epoch || run_.floor != floor || run_.phase != tower::Phase::Fighting) {
                    pbz::enemies::ClearTagged(request);pbz::log::Info("影塔生成回执：旧入场已取消，清理迟到请求 {}，不接到新层",request);return;
                }
                if (relayNext) relayRequestWatch_.Finish();
                --pending_;
                if (relayNext && json.find("\"ok\":true") == std::string::npos) {
                    RecordFailure("relay_spawn", boss, json);
                    const auto left = pbz::enemies::ClearTagged(request);
                    if (tower::floorplay::RetryRelaySpawn(true, relayRetries_, left == 0)) {
                        ++relayRetries_;
                        pending_ = 1;
                        spawnQueue_.push_back({now_ + 1.5, boss});
                        pbz::log::Info("影塔第 {} 层车轮：{} 生成失败，清理后重试 {}/3；{}", ShownBoss(floor), name, relayRetries_, json.substr(0, 300));
                        Broadcast("info.relay_next", std::format("{} 未能出场，正在重试（{}/3）", name, relayRetries_));
                        return;
                    }
                    const auto why = left ? std::format("{} 生成失败且残留角色无法清理，本层未结算", name)
                                          : std::format("{} 多次未能出场，本层未结算；可重新开战或选择本层问题", name);
                    CancelEntry(why);
                    return;
                }
                if (json.find("\"ok\":true") == std::string::npos) {
                    pbz::log::Info("影塔生成失败：{}", json.size() > 300 ? json.substr(0, 300) : json);
                    failed_ = true;
                    // 失败分类（36 层：最后一次"游戏暂停中"被记成蛊鹤生成失败、污染本局准入）：
                    //   没地方落 -> noRoom_（场地问题，按地面审计决定重试 / 判场地）；暂停 / 加载 / 忙 -> 上下文问题，不记 Boss、等就绪后重试；
                    //   其余（类加载 / 初始化失败等）-> 这只 Boss 本局不再用
                    if (json.find("没有足够可落地") != std::string::npos) {
                        noRoom_ = true;
                        // 4.0.0：场内安全区找不到落点 → 本层后面的 Boss 退回玩家附近生成（入场放回 / 出界拉回兜底），不因区域太小判场地坏
                        if (region && !endlessRegionOff_) { endlessRegionOff_ = true; pbz::log::Info("影塔无尽混编层：场内安全区没有 Boss 落点，本层改回玩家附近生成"); }
                    }
                    else if (TransientSpawnError(json)) { transient_ = true; pbz::log::Info("影塔生成失败分类：上下文暂时不可生成（暂停 / 加载 / 忙），不记为 Boss 失败"); }
                    else { badBosses_.insert(boss); RecordFailure("spawn", boss, json); }
                } else {
                    const auto receipt=tower::spawnreceipt::Read(json,request,tower::kBossPool[boss].path);
                    if(!receipt.ok){RecordFailure("spawn_receipt",boss,receipt.why);CancelEntry("生成身份核验失败："+receipt.why);return;}
                    std::unordered_set<UObject*> known(before_.begin(), before_.end());
                    for (auto& a : actors_) known.insert(a.obj);
                    for (auto* a : pbz::enemies::OwnedActors())
                        if (!known.count(a) && ue::Live(a) && ue::ObjName(a)==receipt.name) {
                            actors_.push_back({a, name});
                            if (fightSince_ > 0 && !towerentry::Holding()) towercombat::Watch(a, name);  // 车轮下一位 / 错峰到场
                            if (EndlessOn() && fightSince_ > 0 && tower::kBossPool[boss].identity == std::string_view("MoTian")) {
                                motianHoldUntil_ = now_ + 2.5;
                                pbz::log::Info("影塔第 {} 层：魔渊开战后到场，其他怪定身 2.5 秒、玩家无敌（出场对话）", run_.floor);
                            }
                            actors_.back().boss = boss;
                            // 4.0.0：开战后到场（第 2 / 3 波、补位）不在场内区域生成的，出了实测雾门线 0.5 秒就拉回（按"瞬移出界"处理，不等 3 秒）
                            if (EndlessOn() && fightSince_ > 0 && !region) wallTrack_[a].jumped = true;
                            towerisolate::Adopt(a);  // 场地原生战斗组：场地机制 / 多 Boss 配合照常，清组脚本被阻断
                            // 32ffb7d 起的缺口：读秒结束后才到的角色（1v3 第 7 / 14 秒、车轮下一位）Hold 必然失败 → 取消整层。只在入场冻结期间冻结
                            if (towerentry::Holding() && !towerentry::Hold(a)) { CancelEntry(towerentry::Why()); return; }
                            break;
                        }
                    if(std::none_of(actors_.begin(),actors_.end(),[&](const Actor& a){return ue::Live(a.obj) && ue::ObjName(a.obj)==receipt.name;})){
                        RecordFailure("spawn_receipt",boss,"回执Actor不在当前Owned列表");CancelEntry("生成回执对象已丢失，本层未开战");return;
                    }
                }
                if (pending_ > 0) return;
                // 无尽混编层开战后某只 Boss 没刷出来：兜底再刷一次，仍不行就跳过这只，不作废整层（用户 2026-11-12：一定要一遍过）
                if (failed_ && fightSince_ > 0 && EndlessOn()) {
                    failed_ = noRoom_ = transient_ = false;
                    if (endlessRetries_ < 1 && lastSubmitBoss_ >= 0) {
                        ++endlessRetries_; ++pending_;
                        spawnQueue_.push_back({now_ + 1.5, lastSubmitBoss_});
                        pbz::log::Info("影塔无尽混编层：{} 没刷出来，1.5 秒后再刷一次", tower::kBossPool[lastSubmitBoss_].name);
                    } else pbz::log::Info("影塔无尽混编层：{} 兜底后仍没刷出来，本波跳过这只", lastSubmitBoss_ >= 0 ? tower::kBossPool[lastSubmitBoss_].name : std::string_view("-"));
                    return;
                }
                // 车轮下一位落点被拒（31 层毕峰 ui7：36 候选全无地面 → 直接作废，玩家看到"打完没后续"）：1.5 秒后原地再试，最多 3 次，仍不行才作废
                if (failed_ && fightSince_ > 0 && noRoom_ && lastSubmitBoss_ >= 0 && relayRetries_ < 3) {
                    ++relayRetries_;
                    failed_ = noRoom_ = false;
                    pending_ = 1;
                    spawnQueue_.push_back({now_ + 1.5, lastSubmitBoss_});
                    pbz::log::Info("影塔第 {} 层：下一位 {} 落点被拒，1.5 秒后重试（第 {} 次）", run_.floor, tower::kBossPool[lastSubmitBoss_].name, relayRetries_);
                    return;
                }
                if (failed_ && fightSince_ > 0) { CancelEntry("开战后生成下一位失败：本层作废、不发奖"); return; }  // 车轮 / 错峰开打后补刷失败：不在战斗中重抽
                if (fightSince_ > 0) { relay_.guardUntil = now_ + tower::floorplay::RelayQueue::kGuard; return; }  // 开战后补刷成功：出场保护从到场算起；不重置开层状态（准备道具效果、回血、计时）
                if (failed_) {  // 有一只没生成出来：清掉已生成的
                    spawnQueue_.clear();
                    Clear("partial"); actors_.clear();
                    towerentry::Begin(); // Release locks on destroyed partial spawns before retrying.
                    const bool anyBad = std::any_of(run_.bosses.begin(), run_.bosses.end(), [&](int x) { return badBosses_.count(x) > 0; });
                    if (!anyBad && transient_ && !noRoom_ && ++transientTries_ <= 5) {  // 暂停 / 加载：等生成前就绪门重新放行
                        transient_ = false;
                        spawnAt_ = now_ + 1.0; gateSince_ = -1;
                        Broadcast("info.spawn_retry", "游戏暂停 / 加载中，就绪后重试");
                        return;
                    }
                    // 地面审计已说明原因（地面不是 WorldStatic / 起点在实体里）：同一位置重试不会变，不再空等 6 次
                    const auto verdict = towerreach::Classify(floorAudit_);
                    const bool explained = verdict == towerreach::FloorVerdict::NotStatic || verdict == towerreach::FloorVerdict::StartInSolid;
                    // 同类兜底（42 层蛊鹤：高空空间候选点全部"起点在实体里"，运行库拒绝落点；35 层赤影骨刃：直接生成即自毁）：
                    // 已到 Boss 自己的原生场地、单 Boss 层、找到了它的原生刷怪点 → 让原生刷怪点 ClaimSeqSpawn 自己刷，刷出来按类名接管
                    // 候选点全无有效支撑、起点重叠而玩家站得稳（Codex 174792c 起判 Unknown 保留不确定）：运行库重试必然同样被拒，
                    // 只在"走原生刷怪点兜底"这一步当作已说明，判场地坏仍按原规则
                    const bool coreRefuses = noRoom_ && towerreach::CoreWillRefuse(floorAudit_);
                    const bool wouldGiveUp = (noRoom_ && (explained || coreRefuses || spawnTries_ >= 6)) || anyBad;
                    if (wouldGiveUp && !nativeTried_ && travelled_ && run_.bosses.size() == 1 && venueBoss_ == run_.bosses[0] && TryNativeSpawner(run_.bosses[0])) return;
                    if (!anyBad && noRoom_ && !explained && ++spawnTries_ <= 6) {  // 只是没地方落：1.5 秒后原地重试
                        spawnAt_ = now_ + 1.5;
                        Broadcast("info.spawn_retry", std::format("等地面加载，第 {} 次重试", spawnTries_));
                        return;
                    }
                    // 重试耗尽仍无地面：这个场地本局作废并写失败记录，重抽不会再抽到同一 Boss 的同一场地（Codex 2026-10-31 实测铁杀被连抽两次）。
                    // 没走场地（原地开打）时坏的是脚下这块地，换 Boss 也没用：停下等玩家换地方
                    if (!anyBad && noRoom_) {
                        if (travelled_) {
                            venueBad_.insert(venueBoss_);
                            const std::string cause = explained ? std::format("{}；{}", towerreach::Why(verdict), towerreach::Describe(floorAudit_))
                                                                : std::format("场地重试 {} 次仍无可落地位置；{}", std::max(0, spawnTries_ - 1), towerreach::Describe(floorAudit_));
                            RecordFailure(verdict == towerreach::FloorVerdict::NotStatic ? "floor_type" : "no_ground", venueBoss_, cause);
                            pbz::log::Info("影塔场地：{} 本局判为不可落地（{}），之后不再用", tower::kBossPool[venueBoss_].name, cause);
                        } else {
                            repicks_ = 0;
                            run_.UndoFloor();
                            SaveRun();
                            Broadcast("err.spawn_failed", "这里没有可落地的位置，换个开阔地方再开这一层");
                            return;
                        }
                    }
                    // 35 层：赤影骨刃在戏梦原生场地生成失败 → 换备选玉鼎（冷荼版）→ 备选没场地数据又跨图去风雪刀客场地。
                    // 场地本身是好的（地面 / 雾门都过了），换 Boss 重抽时留在这里，不再二次跨图
                    if (anyBad && travelled_ && tvenue::Found() && !venueBad_.count(venueBoss_)) { stayVenue_ = venueBoss_; stayFloor_ = run_.floor; }
                    run_.UndoFloor();
                    Repick(anyBad ? "有 Boss 生成失败，已换下" : "这里没有可落地的位置，本局不再用这个场地");
                    return;
                }
                repicks_ = 0;
                // The clock starts when the entry gate releases, after visible 3/2/1.
                minHpFrac_ = 1.0;
                healPending_ = run_.mods.HealFrac() > 0;
                sunder_ = false; freezeUntil_ = slowUntil_ = 0; prepPending_ = false;
                Broadcast("info.entry_ready", "等待入场倒计时", std::format(R"("floor":{},"count":{})", floor, count_));
            });
        }
    }

    // 隔离场地原有敌群（用户硬规则 2026-10-31：塔里只打塔生成的 Boss / 具名精英，不能满地原生普怪）：
    // 本层进行中每 0.5 秒移除附近无关怪物。原生召唤常没有正确的 Owner / Instigator；
    // 机制角色还须按“当前活着的本层 Boss -> 配置里的原生依赖”保留，不能把处决目标 / 机关驱动者当杂兵。
    // 只在存档沙盒开着时做：移除原生怪属于塔内状态，出塔还原基准时一并恢复；沙盒关着就不碰（不能把原生怪的变化带进真进度）
    void Sweep() {
        playerSummons_.clear();
        wallSummons_.swap(wallSummonsNext_); wallSummonsNext_.clear();  // 3.6.0：上一轮扫到的 Boss 召唤物交给边界（用前再核 Live）
        if (!towersb::Active()) {
            if (!sweepWarned_) { sweepWarned_ = true; pbz::log::Info("影塔：存档沙盒没开，不清场地原有敌群（会写进真进度）；请先开沙盒"); }
            return;
        }
        if (!npcCls_) {
            npcCls_ = towerperf::ClassNamed("PBZCharacter_NPC");
            auto* actor = towerperf::ClassNamed("Actor");
            offMonsterNpc_ = npcCls_ ? ue::PropOffset(npcCls_, "BIsMonster") : -1;
            fnLoc_ = actor ? pe::FindFunction(actor, "K2_GetActorLocation") : nullptr;
            fnDestroy_ = actor ? pe::FindFunction(actor, "K2_DestroyActor") : nullptr;
            offOwner_ = actor ? ue::PropOffset(actor, "Owner") : -1;
            offInstigator_ = actor ? ue::PropOffset(actor, "Instigator") : -1;
            controllerCls_ = towerperf::ClassNamed("Controller");
            offControllerPawn_ = controllerCls_ ? ue::PropOffset(controllerCls_, "Pawn") : -1;
        }
        if (!npcCls_ || offMonsterNpc_ < 0 || !fnLoc_ || !fnDestroy_) return;
        const Target* me = Player();
        if (!me) return;
        auto where = [&](UObject* a, double out[3]) { uint8_t b[32] = {}; pe::Call(a, fnLoc_, b); std::memcpy(out, b, 24); };
        double pp[3];
        where(me->obj, pp);
        const auto owned = pbz::enemies::OwnedActors();
        std::unordered_set<UObject*> ours(owned.begin(), owned.end());
        for (auto& a : actors_) ours.insert(a.obj);  // 接上的原生换体也是本层 Boss
        std::vector<std::string> combatRoots;
        std::vector<UObject*> interactionActors;
        std::unordered_set<UObject*> registeredSummons;
        bool summonRegistryReady = RegisteredSummons(me->obj, registeredSummons);
        if (summonRegistryReady) for (auto* summon : registeredSummons) {
            const int index = ue::Get<int>(summon + ue::Layout::ObjIndex, -1);
            if (index >= 0) playerSummons_[summon] = index;
        }
        for (auto& a : actors_) if (!a.dead && !a.gone && ue::Live(a.obj)) {
            combatRoots.push_back(ue::ClassName(a.obj));
            interactionActors.push_back(a.obj); // 原生接管的阶段不在 Core OwnedActors 里，也拥有自己的机制交互
            if (!RegisteredSummons(a.obj, registeredSummons)) summonRegistryReady = false;
        }
        // 仆从的召唤物（用户 2026-11-12：召唤型仆从要保留召唤物，否则缺战斗力）：原生能力登记的 + Owner/Instigator 是仆从的
        std::unordered_set<UObject*> servantRoots, servantReg;
        for (auto& a : servants_.actors) if (!a.dead && ue::Live(a.obj)) { servantRoots.insert(a.obj); RegisteredSummons(a.obj, servantReg); }
        int removed = 0;
        // 本层 Boss 身份（同身份替身也要清：梦华战后残留 BP_NPC_MengHua）
        std::vector<std::string> floorIds;
        for (int b : run_.bosses) floorIds.push_back(std::string(tower::kBossPool[b].identity));
        const bool awaitingSwap = std::any_of(actors_.begin(), actors_.end(), [](const Actor& a) { return a.gone && !a.dead; });
        for (auto& t : attr::Targets()) {
            if (t.who == attr::Who::Player || !attr::Alive(t) || ours.count(t.obj) || !ue::IsA(t.obj, npcCls_)) continue;
            if (!towerisolate::InPlayerWorld(t.obj)) continue;  // 过场占位 / 模板：不清也不计
            const std::string cls = ue::ClassName(t.obj);
            if (cls == "BP_Monster_Hun_Summon_Double_C") {
                auto* pc = towerwidgets::Controller();
                for (int off : {offOwner_, offInstigator_}) if (off >= 0) {
                    auto* owner = ue::Get<UObject*>(t.obj + off);
                    if (owner && (owner == me->obj || (pc && owner == pc))) {
                        const int index = ue::Get<int>(t.obj + ue::Layout::ObjIndex, -1);
                        if (index >= 0) playerSummons_[t.obj] = index;
                        break;
                    }
                }
            }
            if (replay_ && tower::replay::PhaseOf(*replay_, cls) >= 0) continue; // 阶段归 ReplayTick
            if (!servantRoots.empty()) {
                // 3.4.1（玩家：召唤型 Boss 当仆从时，它召出来的怪不是己方，打玩家也打仆从）：原来只认原生能力登记 + 直接 Owner / Instigator 是仆从本体；
                // 很多召唤的 Owner 是召唤者的 AI 控制器或中间生成器 → 认不出、保持敌方。改为沿 Owner / Instigator 往上追 4 层（控制器转到它控制的角色），链上有仆从即归友军
                bool mine = servantReg.count(t.obj) > 0;
                std::string chain;
                if (!mine) {
                    std::vector<UObject*> frontier{t.obj};
                    std::unordered_set<UObject*> seen{t.obj};
                    for (int depth = 0; depth < 4 && !mine && !frontier.empty(); ++depth) {
                        std::vector<UObject*> next;
                        for (auto* x : frontier) for (int off : {offOwner_, offInstigator_}) if (off >= 0) {
                            auto* o = ue::Get<UObject*>(x + off);
                            if (!o || !ue::Live(o) || !seen.insert(o).second) continue;
                            if (offControllerPawn_ >= 0 && controllerCls_ && ue::IsA(o, controllerCls_)) {
                                if (auto* pawn = ue::Get<UObject*>(o + offControllerPawn_); pawn && ue::Live(pawn)) { if (chain.size() < 160) chain += ue::ObjName(o) + ">"; o = pawn; seen.insert(o); }
                            }
                            if (chain.size() < 160) chain += ue::ObjName(o) + " ";
                            if (servantRoots.count(o)) { mine = true; break; }
                            next.push_back(o);
                        }
                        frontier = std::move(next);
                    }
                }
                if (mine) { AdoptServantSummon(t.obj, cls); continue; }
                if (!ours.count(t.obj) && !registeredSummons.count(t.obj) && unadoptedLogged_.insert(t.obj).second && unadoptedLogged_.size() <= 30)
                    pbz::log::Info("影塔仆从召唤物：未认出归属 {} {}（Owner 链：{}）", ue::ObjName(t.obj), cls, chain.empty() ? "空" : chain);
            }
            if (registeredSummons.count(t.obj)) {
                interactionActors.push_back(t.obj);
                if (mechanismLogged_.insert(t.obj).second)
                    pbz::log::Info("影塔召唤物：保留原生能力登记的 actor={} class={}（不计胜负）", ue::ObjName(t.obj), cls);
                // 3.4.2（用户：舞狮娃娃锁血卡关，其它召唤 Boss 也要查）：敌方 Boss 的召唤物（仆从的已在上面分走）锁血超过 10 秒只记日志，不改原生
                if (hpMin_ >= 0 && hp_ >= 0) {
                    const float h = ue::Get<float>(attr::AttrCurrent(t, hp_), -1.0f), mn = ue::Get<float>(attr::AttrCurrent(t, hpMin_), -1.0f);
                    if (mn > 0.5f && h >= 0 && h <= mn + 0.5f) {
                        auto [it, fresh] = summonLockSince_.try_emplace(t.obj, now_);
                        if (!fresh && now_ - it->second >= 10.0 && summonLockLogged_.insert(t.obj).second)
                            pbz::log::Info("影塔召唤物锁血：第 {} 层 {} 生命锁在 {:.1f} 已 10 秒（原生等处决或转阶段；只记录不修改）", run_.floor, cls, mn);
                    } else summonLockSince_.erase(t.obj);
                }
                continue;
            }
            // 原生反射 / 能力系统尚未就绪时不能断言召唤物归属，宁可暂留可疑召唤物。
            if (!summonRegistryReady && cls.find("_Summon") != std::string::npos) {
                if (mechanismLogged_.insert(t.obj).second)
                    pbz::log::Info("影塔召唤物：原生登记未就绪，暂不清理 {}", cls);
                continue;
            }
            // 1.3.4（用户 2026-11-07：绝影铸魂后「化影」召的分身一出来就被清）：分身类 BP_Monster_Hun_Summon_Double 名字里带 _Hun_，
            // 撞上 Boss 池里「魂（镜像）」的身份 Hun，被当成"别的 Boss"销毁。玩家这边的召唤物（Owner / Instigator 是玩家或玩家控制器，
            // 或者就是魂的召唤类）一律不碰
            if (PlayerSide(t.obj, cls, me->obj)) {
                if (mechanismLogged_.insert(t.obj).second) pbz::log::Info("影塔召唤物：保留玩家的 {}（兵器技能召唤，不清场）", cls);
                // 用户 2026-11-12：绝影「化影」分身很脆 → 生命上限 / 攻击 / 破防 / 杀气上限按玩家当前值 50%（同仆从口径）；
                // 原生重算属性后被改回时（生命上限低于目标 95%）每秒补写一次，分身原生技能不动
                if (cls.find("Hun_Summon_Double") != std::string::npos) {
                    auto& next = cloneNext_[t.obj];
                    if (now_ >= next) {
                        next = now_ + 1.0;
                        std::array<float, 4> want{};
                        const Target* pl = Player();
                        const float have = ue::Get<float>(attr::AttrCurrent(ServantTarget(t.obj), hpMax_), -1.f);
                        if (pl && ServantStats(*pl, want, tower::servants::kCloneShare) && (!std::isfinite(have) || have < want[0] * 0.95f) && InitServantNow(t.obj, "绝影分身", tower::servants::kCloneShare))
                            pbz::log::Info("影塔召唤物：绝影分身 {} 属性写为玩家 50%", ue::ObjName(t.obj));
                    }
                }
                continue;
            }
            std::string_view mechanism;
            for (const auto& root : combatRoots) {
                mechanism = tower::encounter::RequiredBy(root, cls);
                if (!mechanism.empty()) break;
            }
            // 28 层剑阵：原生 Seq076 刷出的 6 个白衣弟子（HuaShan_White）不在支援类名单里，被清场连清两轮 → 只剩大师兄。
            // 同类：重放层里属于契约原生阶段组的成员一律保留（按组归属，不靠类名）
            const bool support = replay_ && (tower::replay::In(replay_->supportClasses, replay_->supportN, cls) || towerisolate::NativeMember(t.obj));
            if (support || !mechanism.empty()) {
                double ep[3]; where(t.obj, ep);
                if (std::hypot(ep[0] - pp[0], ep[1] - pp[1]) <= 15000 && std::abs(ep[2] - pp[2]) <= 5000) {
                    // 阿瑾的同组兵可能在 ReplayArm 以前由场地自然刷出；保留时立即尝试冻结，
                    // 不等 Boss 接管，也不把支援兵加入胜负队列。
                    if (replay_ && replay_->kickGroup && towerentry::Holding() &&
                        tower::replay::In(replay_->supportClasses, replay_->supportN, cls) && towerentry::Hold(t.obj, false)) {
                        if (entrySupportLogged_.insert(t.obj).second)
                            pbz::log::Info("影塔入场：同组支援 {} {} 已提前冻结", ue::ObjName(t.obj), cls);
                    }
                    interactionActors.push_back(t.obj);
                    if (mechanismLogged_.insert(t.obj).second)
                        pbz::log::Info("影塔机制依赖：保留 actor={} class={} source={}（不计阶段 / 胜负）", ue::ObjName(t.obj), cls,
                                       support ? replay_->id : mechanism);
                    continue;
                }
            }
            const int other = OtherBossIdentity(cls, floorIds);  // 别的 Boss 身份（白赤练混进混沌层）
            // 塔 Boss 招式派生的角色（Owner / Instigator 是塔生成的）不算原场地敌群——除非它是别的 Boss 身份（用户 32ffb7d：一层只打指定 Boss）
            bool derived = offOwner_ >= 0 && ours.count(ue::Get<UObject*>(t.obj + offOwner_));
            if (offInstigator_ >= 0) { UObject* ins = ue::Get<UObject*>(t.obj + offInstigator_); derived = derived || (ins && ours.count(ins)); }
            // 同身份替身：本层 Boss 自己派生的、或正等着接换体时出现的（多阶段合法换 Actor）不清
            bool double_ = !derived && !awaitingSwap &&
                                 std::any_of(floorIds.begin(), floorIds.end(), [&](const std::string& id) { return tower::IdentityIn(cls, id); });
            // F41 雷婷：BP_Monster_QiShu_Lei_Stand 由 Boss 自身被动 GA_SummonPassive_QiShu_Lei_Stand 召出，自带 AIController，
            // Owner / Instigator 都不指向塔 Boss，按"同身份替身"被清掉 → 机制被拆。同身份但不是 Boss 池里任何类的怪物 = Boss 的召唤物，保留不计胜负。
            // 真正的替身（梦华战后残留 BP_NPC_MengHua）不是 BP_Monster，或本身就是 Boss 池里的类，照旧清。
            if (double_ && other < 0 && cls.rfind("BP_Monster", 0) == 0 && !InBossPool(cls)) {
                wallSummonsNext_.push_back(t.obj);  // 3.6.0：分身也要被边界管住
                if (mechanismLogged_.insert(t.obj).second)
                    pbz::log::Info("影塔召唤物：保留 actor={} class={}（本层 Boss 同身份召唤，不是替身；不计阶段 / 胜负）", ue::ObjName(t.obj), cls);
                continue;
            }
            if (derived && other < 0) continue;
            const bool monster = ue::Get<uint8_t>(t.obj + offMonsterNpc_) || cls.rfind("BP_Monster", 0) == 0;
            if (!monster && !double_ && other < 0) continue;  // 不是怪物也不是 Boss 替身（友方 NPC / 商人）
            double ep[3];
            where(t.obj, ep);
            if (std::hypot(ep[0] - pp[0], ep[1] - pp[1]) > 15000 || std::abs(ep[2] - pp[2]) > 5000) continue;
            if (other >= 0 || double_)
                pbz::log::Info("影塔清场：移除{} {}（{}）", other >= 0 ? "别的 Boss" : "本层 Boss 的原生替身", cls, derived ? "由塔 Boss 招式 / 阶段派生" : "场地原生");
            auto* owner = offOwner_ >= 0 ? ue::Get<UObject*>(t.obj + offOwner_) : nullptr;
            auto* instigator = offInstigator_ >= 0 ? ue::Get<UObject*>(t.obj + offInstigator_) : nullptr;
            pbz::log::Info("影塔清场对象：actor={} class={} owner={} instigator={}", ue::ObjName(t.obj), cls,
                           owner && ue::Live(owner) ? ue::ObjName(owner) : "-",
                           instigator && ue::Live(instigator) ? ue::ObjName(instigator) : "-");
            towerisolate::BlockGroupOf(t.obj);  // 先阻断它所在战斗组的"清组"，移除成员不会触发清组剧情 / 脚本
            uint8_t none[8] = {};
            pe::Call(t.obj, fnDestroy_, none);
            ++removed;
        }
        towerinteractions::AllowActors(interactionActors); // 替换本轮快照；失去活着的 Boss 根时不再豁免机制交互
        if (removed) { swept_ += removed; pbz::log::Info("影塔：清掉场地原有怪物 {} 只（本层累计 {}）", removed, swept_); }
    }

    // 重放层兜底：等待条件持续、且累计非过场时间（原生过场 / QTE 播放时不计）到 quiet 秒后，请求契约里第 phase 阶段的原生刷怪点
    // ClaimSeqSpawn（唤醒 + 入战），刷出来照常按类名接管。原生自己刷过的刷怪点重复请求不会多刷。每个等待只请求一次
    void KickWhenQuiet(int phase, double quiet, const char* why) {
        if (!replay_ || phase < 0 || phase >= replay_->spawnerN) return;
        if (kickFor_ != phase || now_ - kickTick_ > 1.0) { kickFor_ = phase; kickQuiet_ = 0; kickDone_ = false; kickTick_ = now_; }
        const double dt = now_ - kickTick_;
        kickTick_ = now_;
        if (!towerstorygate::Cinematic()) kickQuiet_ += dt;
        if (kickDone_ || kickQuiet_ < quiet) return;
        kickDone_ = true;
        UObject* sp = nullptr;
        ue::ForEach([&](int, UObject* o) { if (!ue::IsDefault(o) && ue::Live(o) && ue::ObjName(o) == replay_->spawners[phase] && towerisolate::InPlayerWorld(o)) { sp = o; return false; } return true; });
        UObject* fn = sp ? pe::FindFunction(ue::ClassOf(sp), "ClaimSeqSpawn") : nullptr;
        if (!fn) { pbz::log::Info("影塔重放兜底：{}，第 {} 阶段刷怪点 {} 不在 / 无 ClaimSeqSpawn，继续等原生", why, phase + 1, replay_->spawners[phase]); return; }
        std::array<uint8_t, 0x40> a{};
        if (const int o = ue::PropOffset(fn, "bAwakeAI"); o >= 0 && o < 0x40) a[o] = 1;
        if (const int o = ue::PropOffset(fn, "bForceIntoCombat"); o >= 0 && o < 0x40) a[o] = 1;
        towerisolate::AllowTowerSpawn(sp);
        pe::Call(sp, fn, a.data());
        pbz::log::Info("影塔重放兜底：{}（非过场 {:.0f} 秒），已请求第 {} 阶段刷怪点 {} 原生刷出", why, kickQuiet_, phase + 1, replay_->spawners[phase]);
    }
    // 32 层荒行子：下一阶段在高空独立空间，原生靠传送门 / 破钟过去，塔的隔离会拦非塔传送。上一阶段死后等 12 秒（转阶段过场），
    // 下一阶段仍没出现 → 塔点传送到下一阶段刷怪点（场地数据坐标），到了再对该刷怪点 ClaimSeqSpawn；按类名接管照旧走 ReplayTick
    void PhaseTravel() {
        const int next = rp_.seen + 1;
        if (next < 1 || next >= replay_->spawnerN) return;
        if (phaseDeadAt_ < 0 || phaseTravelFor_ != next) { phaseDeadAt_ = now_; phaseTravelFor_ = next; phaseTravelAt_ = -1; phaseKicked_ = false; }
        const std::string_view cls = replay_->phases[next];
        const tower::VenueDef* def = nullptr;
        for (const auto& v : tower::kVenues)
            if (v.path.size() > cls.size() && v.path.substr(v.path.size() - cls.size()) == cls && v.spawner == replay_->spawners[next]) { def = &v; break; }
        if (!def) return;
        if (phaseTravelAt_ < 0 && now_ - phaseDeadAt_ >= 12 && !pbz::TeleportBusy()) {
            std::string msg;
            const bool ok = pbz::PointTeleport(def->sx, def->sy, def->sz + 100, def->yaw, &msg);
            phaseTravelAt_ = now_;
            pbz::log::Info("影塔重放：第 {} 阶段在别处，塔传送到其刷怪点 ({:.0f}, {:.0f}, {:.0f})：{}", next + 1, def->sx, def->sy, def->sz, ok ? msg : "失败：" + msg);
            if (!ok) { auto* pawn = towerwidgets::Pawn(); MoveActor(pawn, {def->sx, def->sy, def->sz + 100}); }
        }
        if (phaseTravelAt_ >= 0 && !phaseKicked_ && now_ - phaseTravelAt_ >= 4 && !pbz::TeleportBusy()) {
            phaseKicked_ = true;
            UObject* sp = nullptr;
            ue::ForEach([&](int, UObject* o) { if (!ue::IsDefault(o) && ue::Live(o) && ue::ObjName(o) == replay_->spawners[next]) { sp = o; return false; } return true; });
            UObject* fn = sp ? pe::FindFunction(ue::ClassOf(sp), "ClaimSeqSpawn") : nullptr;
            if (!fn) { pbz::log::Info("影塔重放：第 {} 阶段刷怪点 {} 未找到 / 无 ClaimSeqSpawn，等原生自行刷出", next + 1, replay_->spawners[next]); return; }
            std::array<uint8_t, 0x40> a{};
            if (const int o = ue::PropOffset(fn, "bAwakeAI"); o >= 0 && o < 0x40) a[o] = 1;
            if (const int o = ue::PropOffset(fn, "bForceIntoCombat"); o >= 0 && o < 0x40) a[o] = 1;
            pe::Call(sp, fn, a.data());
            pbz::log::Info("影塔重放：第 {} 阶段刷怪点 {} 已请求原生刷出", next + 1, replay_->spawners[next]);
        }
    }
    // 原生刷怪点兜底：ClaimSeqSpawn（不隐藏 / 唤醒 / 入战），10 秒内按类名接管；接不到就按原来的失败路径（判场地 / 换 Boss）
    bool TryNativeSpawner(int boss) {
        nativeTried_ = true;
        if (!venuePath_.empty()) { pbz::log::Info("影塔生成兜底：固定场地 {} 不唤醒原生刷怪点", venuePath_); return false; }
        auto* sp = static_cast<UObject*>(tvenue::Spawner());
        UObject* fn = sp ? pe::FindFunction(ue::ClassOf(sp), "ClaimSeqSpawn") : nullptr;
        if (!fn) { pbz::log::Info("影塔生成兜底：没有原生刷怪点 / ClaimSeqSpawn，按原失败路径处理"); return false; }
        std::array<uint8_t, 0x40> a{};
        if (const int o = ue::PropOffset(fn, "bAwakeAI"); o >= 0 && o < 0x40) a[o] = 1;
        if (const int o = ue::PropOffset(fn, "bForceIntoCombat"); o >= 0 && o < 0x40) a[o] = 1;
        // 场地扫描已把它静默（停刷 + 拦过场 / 异步生成）：塔自己请求的这一次要放行，否则 ClaimSeqSpawn 被会话隔离挡下（42 层 71ad9ff）
        towerisolate::AllowTowerSpawn(sp);
        pe::Call(sp, fn, a.data());
        nativeAdoptBoss_ = boss; nativeAdoptUntil_ = now_ + 10; earlyHeld_ = false;
        failed_ = noRoom_ = transient_ = false; badBosses_.erase(boss); pending_ = 0;
        pbz::log::Info("影塔生成兜底：运行库没生成出 {}，改由原生刷怪点 {} 刷出（ClaimSeqSpawn）", tower::kBossPool[boss].name, ue::ObjName(sp));
        return true;
    }
    void NativeAdoptTick() {
        const auto path = tower::kBossPool[nativeAdoptBoss_].path;
        const auto cls = path.substr(path.rfind('.') + 1);
        // 刷怪点刚刷出的角色先直接冻结：不等角色表刷新（42 层 e703aba：ClaimSeqSpawn 唤醒 + 入战，约 1.3 秒后才接管，
        // Boss 先出手、读秒后开）。冻结失败（属性还没初始化）不算失败，下一帧再试；接管仍走下面的角色表
        if (!earlyHeld_ && towerentry::Holding()) {
            if (auto* sp = static_cast<UObject*>(tvenue::Spawner()); sp && ue::Live(sp)) {
                const int off = ue::PropOffset(ue::ClassOf(sp), "SpawnedAI");
                auto* ai = off >= 0 ? ue::Get<UObject*>(sp + off) : nullptr;
                if (ai && ue::Live(ai) && ue::ClassName(ai) == cls && towerisolate::InPlayerWorld(ai) && towerentry::Hold(ai)) {
                    earlyHeld_ = true;
                    pbz::log::Info("影塔生成兜底：原生刷出的 {} 已先冻结（{}），等接管", tower::kBossPool[nativeAdoptBoss_].name, ue::ObjName(ai));
                }
            }
        }
        for (auto& t : attr::Targets()) {
            if (t.who != attr::Who::Enemies || !attr::Alive(t) || ue::ClassName(t.obj) != cls || !towerisolate::InPlayerWorld(t.obj)) continue;
            if (std::any_of(actors_.begin(), actors_.end(), [&](const Actor& x) { return x.obj == t.obj; })) continue;
            actors_.push_back({t.obj, std::string(tower::kBossPool[nativeAdoptBoss_].name)});
            actors_.back().boss = nativeAdoptBoss_; actors_.back().adopted = true;
            towerisolate::Adopt(t.obj);
            nativeAdoptUntil_ = 0;
            pbz::log::Info("影塔生成兜底：已接管原生刷出的 {}（{}）", tower::kBossPool[nativeAdoptBoss_].name, ue::ObjName(t.obj));
            if (towerentry::Holding() && !towerentry::Hold(t.obj)) { CancelEntry(towerentry::Why()); return; }
            repicks_ = 0; minHpFrac_ = 1.0; healPending_ = run_.mods.HealFrac() > 0; sunder_ = false; freezeUntil_ = slowUntil_ = 0; prepPending_ = false;
            Broadcast("info.entry_ready", "等待入场倒计时", std::format(R"("floor":{},"count":{})", run_.floor, count_));
            return;
        }
        if (now_ < nativeAdoptUntil_) return;
        nativeAdoptUntil_ = 0;
        const std::string why = std::format("原生刷怪点 10 秒内也没刷出 {}", tower::kBossPool[nativeAdoptBoss_].name);
        RecordFailure("native_spawner", nativeAdoptBoss_, why);
        venueBad_.insert(venueBoss_);
        pbz::log::Info("影塔生成兜底：{}，本局不再用该场地", why);
        towerisolate::EndTowerSpawn();
        run_.UndoFloor();
        Repick(why);
    }
    // 重放：某阶段的一个角色死了；该阶段所有角色都死了才通知进度（同阶段多角色，34 层力者和锯者）
    void PhaseMemberDead(int k, UObject* self = nullptr) {
        // self：刚确认死亡的这个角色（它的 dead 标记在本次调用之后才写，不能把它当成"同阶段还活着"；da6b217 34 层二阶段死后不结算）
        for (auto& x : actors_) if (x.phase == k && x.obj != self && !x.dead && !x.gone && ue::Live(x.obj)) return;
        rp_.Dead(k, now_);
    }
    // 类名是不是 Boss 池里某只 Boss 的类（池里路径 "/…/BP_X.BP_X_C"，cls 为 "BP_X_C"）
    // 玩家这边的召唤物（兵器技能召出的分身等）：清场 / 计胜负都不碰
    bool PlayerSide(UObject* obj, std::string_view cls, UObject* pawn) const {
        if (cls.rfind("BP_Monster_Hun_Summon", 0) == 0) return true;
        // 白蟒赤练铸魂「百结蛇」：召出的蛇借用白赤练的蛇类（BP_Monster_ChiLian_Snake_*）；不是白赤练的层上出现 = 玩家召的
        if (cls.rfind("BP_Monster_ChiLian_Snake", 0) == 0 &&
            std::none_of(run_.bosses.begin(), run_.bosses.end(), [](int b) { return tower::kBossPool[b].identity == std::string_view("ChiLian"); }))
            return true;
        UObject* pc = towerwidgets::Controller();
        for (int off : {offOwner_, offInstigator_}) {
            if (off < 0) continue;
            UObject* o = ue::Get<UObject*>(obj + off);
            if (o && (o == pawn || o == pc)) return true;
        }
        return false;
    }
    static bool InBossPool(std::string_view cls) {
        for (const auto& b : tower::kBossPool) {
            const auto path = b.path;
            if (path.size() > cls.size() && path.substr(path.size() - cls.size()) == cls && path[path.size() - cls.size() - 1] == '.') return true;
        }
        return false;
    }
    // 本层作废后自动重抽一次（连续两次都不行就停下等玩家，免得在坏数据上来回跳）
    void Repick(const std::string& why) {
        floorNote_ = why;
        if (repicks_ < 2 && run_.phase == tower::Phase::Ready) {
            ++repicks_;
            Broadcast("info.repick", why + "，重抽本层");
            Next();
            return;
        }
        repicks_ = 0;
        Broadcast("err.spawn_failed", why);
    }
    // 失败记录（给 Codex 核对数据）：%LOCALAPPDATA%\PBZTower\spawn-failures.txt，一行一条
    void RecordFailure(const char* kind, int boss, const std::string& detail) {
        wchar_t base[MAX_PATH];
        if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return;
        SYSTEMTIME t; GetLocalTime(&t);
        std::ofstream f(std::wstring(base) + L"\\PBZTower\\spawn-failures.txt", std::ios::binary | std::ios::app);
        f << std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}\t{}\t{}\t{}\t{}\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, kind,
                         boss >= 0 ? tower::kBossPool[boss].path : std::string_view("-"), boss >= 0 ? tower::kBossPool[boss].name : std::string_view("-"), detail.substr(0, 400));
    }

    // 玩家强度相关属性：当前值和 1 级基准（用来标定统一起点）
    void Probe() {
        static const char* const names[] = {"Level", "Attack", "Attack_Lv1", "Health_Max", "Health_Max_Lv1", "Shachi_Max", "Shachi_Max_Lv1",
                                            "ShachiAttack", "ShachiAttack_Lv1", "Infliction_Max", "Infliction_Max_Lv1", "PowerScore",
                                            "Amplification_Physical", "Amplification_PhysicalSharp", "Amplification_PhysicalBlunt",
                                            "Amplification_ImpactShachi", "DealtReductionMultiplier_Physical", "TakenReductionMultiplier_Physical",
                                            "UltimateCharge_Max", "SpiritShadow_Max", "XP", "Coin"};
        std::string out;
        for (auto& t : attr::Targets()) {
            if (t.who != attr::Who::Player || !attr::Alive(t)) continue;
            for (auto* n : names) {
                const int o = attr::AttrOffset(n);
                if (o < 0) continue;
                out += std::format(R"({}"{}":[{:.3f},{:.3f}])", out.empty() ? "" : ",", n, ue::Get<float>(attr::AttrBase(t, o)), ue::Get<float>(attr::AttrCurrent(t, o)));
            }
            break;
        }
        pbz::log::Info("影塔强度探测：{}", out);
        pbz::ipc::Send(std::format(R"({{"op":"tower","event":"probe","attrs":{{{}}}}})", out));
    }

    // 只清本塔生成的（request_id 以 "tower-" 开头），修改器刷的怪不动
    size_t Clear(const char* why) {
        const bool keepServants = std::string_view(why) == "mob_wave" && mob_.wave + 1 < mob_.total;
        const auto pendingServant = std::exchange(servants_.pending, {});
#ifdef PBZ_HEX_TEST
        if (arcaneTest_ && std::string_view(why) == "mob_wave") towerarcane::ResetWave();
        else { towerarcane::Stop(); arcaneTest_ = false; }
#else
        towerarcane::Stop();
#endif
        leech_.Reset(); nextLeech_ = 0; nextLeechBar_ = 0;
        summonPermit_.Reset();
        playerSummons_.clear();
        if (deathAt_ >= 0) { towerwidgets::DeathShow(false); deathAt_ = -1; reviving_ = false; }  // 殒命表现中被取消 / 暂离：一并撤下
        ++spawnEpoch_;
        mobQueue_.clear(); mobActive_ = 0; mobRequestWatch_.Finish(); relayRequestWatch_.Finish();  // 取消入场后不能留着旧请求
        // First revoke encounter permissions; cleanup must never start native
        // ending movies/QTE/loot through Destroyed or group-clear callbacks.
        towerisolate::SetReplay(nullptr);
        towerinteractions::AllowActors({});
        towercombat::End(why);
        std::unordered_set<UObject*> cleanup=mechanismLogged_;
        for(auto& a:actors_)if(a.adopted)cleanup.insert(a.obj);
        auto* actor=towerperf::ClassNamed("Actor");
        auto* destroy=actor?pe::FindFunction(actor,"K2_DestroyActor"):nullptr;
        if(destroy)for(auto* obj:cleanup)if(obj && ue::Live(obj)){
            g_lastEncounterClear = GetTickCount64();
            towerisolate::BlockGroupOf(obj);
            pbz::log::Info("影塔遭遇清理：{} class={} reason={}（不播战后流程）",ue::ObjName(obj),ue::ClassName(obj),why);
            pe::Call(obj,destroy,nullptr);
        }
        mechanismLogged_.clear(); placeholderLogged_.clear();
        if (!actors_.empty()) g_lastEncounterClear = GetTickCount64();
        // 中间换波只清本局敌方（tower-<run>-），影塔路径的仆从（tower-sv-）保留；其余情况清全部 tower- 并撤下仆从
        size_t left = pbz::enemies::ClearTagged(keepServants ? "tower-" + run_.id + "-" : std::string("tower-"));
        if (keepServants) {  // 小怪层中间换波：同层仆从保留；只作废还在路上的请求
            if (!pendingServant.empty()) left += pbz::enemies::ClearTagged(pendingServant);
        } else left += DismissServants(why);
        if (left) pbz::log::Info("影塔清场（{}）：{} 个未能销毁", why, left);
        return left;
    }
    // 正常结束（通关 / 失败 / 停止 / 暂离）：放开自己持有的 tower.run，属性下一帧自动还原；结算画面靠最后一次广播
    // 返回 0 = 没有塔档会话；1 = 出塔已受理（放回原档 / 等加载落稳后放回，后续由 TripTick 推进并广播）；-1 = 出塔被拒（保护保持）
    int End() {
        MeltRestore();
        towerpotion::Clear();
        towerentry::End();
        EndReplay();
        boundary_.Disarm(); originSet_ = false; relay_.Clear(); tvenue::End();
        towertravel::Abort("结束本局");  // 313987b 反例：加载中暂离后仍执行落点传送；先停掉后续传送，已发出的原生加载由 Calm 等落稳
        towerisolate::End();
        Unwear("end"); ending_ = true; pbz::ReleaseFeature(this, "tower"); ending_ = false;
        travelThenSpawn_ = false; spawnAt_ = 0;
        // 出塔回程：独立塔档时不传送。跨图钟传送本身会存档再载入，会把放回去的原档基准盖成塔内状态（13f7e98 实机反例）；
        // 原档基准里有进塔前的位置，回标题继续即回到那里
        if (trip_.phase() == tower::trip::Phase::Inside || trip_.phase() == tower::trip::Phase::Entering) towertravel::ForgetHome();
        else towertravel::GoHome();  // 出塔：传回进塔位置（没离开过就没有记录）
        towerwidgets::ResetMaxBarSync();
        towerpoints::Enable(false, false);  // 原游戏升级交易恢复
        if (trip_.phase() == tower::trip::Phase::Inside || trip_.phase() == tower::trip::Phase::Entering) {
            // 出塔：进塔基准放回内存；玩家回标题 → 继续进档后才关沙盒（实机通过的路径）。塔档本身在 growth.txt 里另存
            int r = 1;
            if (trip_.Exit(Io())) {
                SaveTrip();  // R6：出塔事务先落盘，后面的清理即使出错，重开后也知道要恢复原进度
                towerisolate::Session(false, run_.floor, run_.id); // ReleaseFeature disables Tick: close session explicitly before original-world recovery
                lastStep_ = tower::trip::Step::None; lastTripError_.clear();
                pbz::log::Info("影塔独立塔档：出塔事务 {} 已受理（{}）", trip_.txn(), trip_.pending() ? "原生加载未落稳或放回失败，稍后重试：" + trip_.error() : "基准已放回内存");
                Broadcast(trip_.pending() ? "info.exit_pending" : "info.exit_reload", ExitText());
            } else {
                r = -1;
                pbz::log::Info("影塔独立塔档：出塔被拒：{}（沙盒保持开启）", trip_.error());
                Broadcast("err.exit_failed", "出塔还原失败：" + trip_.error() + "。沙盒保持开启");
            }
            grow_.Release();
            SaveTrip();
            return r;
        }
        return 0;
    }

    tower::trip::IIo& Io() { return towernative::Trip([this](std::string* where) { return BackupSave(where); }); }
    static std::string Stamp() {
        SYSTEMTIME t; GetLocalTime(&t);
        return std::format("{:04}.{:02}.{:02}-{:02}.{:02}.{:02}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    }
    std::string EnterText() const {
        return trip_.automatic() ? std::format("正在载入影塔（事务 {}）：自动返回标题并继续", trip_.txn()) : "塔档已准备好：请回到标题画面，再点“继续”进入影塔";
    }
    std::string ExitText() const {
        if (trip_.pending()) return std::format("正在恢复原进度（事务 {}）：等原生加载落稳后放回原档，保护保持开启", trip_.txn());
        return trip_.automatic() ? std::format("正在恢复原进度（事务 {}）：自动返回标题并继续，核验一致后关闭保护", trip_.txn())
                                 : "出塔：请回到标题画面，再点“继续”进档，完成后存档沙盒自动关闭";
    }
    std::string TripBusyText() const {
        if (trip_.busy() && trip_.failed()) return "上次切档没有完成（" + trip_.error() + "）：保护保持开启，可在影塔菜单里重试";
        switch (trip_.phase()) {
            case tower::trip::Phase::Entering: return trip_.automatic() ? "正在载入影塔，请稍候" : "正在进入塔档：请回到标题画面，再点“继续”";
            case tower::trip::Phase::Exiting: return trip_.automatic() ? "正在恢复原进度，请稍候" : "正在出塔：请回到标题画面，再点“继续”";
            case tower::trip::Phase::Stale: return "上次影塔会话没有正常结束：先在影塔菜单里选“恢复原进度”或“已是原进度”";
            default: return "已经在影塔里";
        }
    }
    // 进出塔档的推进：进完档 → 在塔里（核对装备是塔档的）；出完档 → 关沙盒
    void TripTick() {
        if (trip_.phase() != tower::trip::Phase::Entering && trip_.phase() != tower::trip::Phase::Exiting) return;
        const auto t = GetTickCount64();
        if (t < nextTripTick_) return;
        nextTripTick_ = t + 500;
        if (trip_.step() == tower::trip::Step::Settle && t >= nextSettleLog_) {  // 收尾标定：原生存档脏不脏、沙盒写入计数
            nextSettleLog_ = t + 5000;
            std::string why;
            const bool clean = towergate::SaveClean(&why);
            pbz::log::Info("影塔独立塔档：收尾中，原生存档{}；{}", clean ? "干净" : why, towersb::State());
        }
        if (trip_.flushed() != lastFlushed_ && trip_.flushed() > 0) {  // 收尾期间原生存档的写入落进了沙盒（证据：被沙盒挡下的写）
            lastFlushed_ = trip_.flushed();
            pbz::log::Info("影塔独立塔档：收尾期间原生存档写入落进沙盒（第 {} 次，真档不变）；{}", lastFlushed_, towersb::State());
        }
        const bool wasExiting = trip_.phase() == tower::trip::Phase::Exiting;
        if (trip_.Tick(Io())) {  // 已逐项核验载入内容就是塔内存档
            grow_.Adopt(grow_.profile);
            const auto l = towernative::ReadLoadout();
            pbz::log::Info("影塔独立塔档：已载入并核验塔档 主[{}] 影[{}] 药[{} / {}]", Csv(l.main), Csv(l.shaqi), l.medBottom, l.medTop);
            towerpoints::Enable(nativePoints_, dustGate_);
            SaveTrip();
            SaveRun();
            if (importedSeedInFlight_) {
                uint64_t current = 0; int difficulty = 0;
                if (ImportedSeed(&current, &difficulty) == 1 && current == importedSeedInFlight_ && difficulty == importedSeedDifficulty_ && SeedSaveVerified())
                    DeleteFileW((TowerDir() + L"\\pending-seed.txt").c_str());
                else pbz::log::Info("影塔种子：新局存档未通过读回或待用种子已变化，保留导入文件");
                importedSeedInFlight_ = 0;
            }
            lastTripError_.clear();
            Broadcast("info.tower_entered", "已进入影塔（塔档）");
        } else if (trip_.failed() && lastStep_ != tower::trip::Step::Failed) {
            // 自动切档超限：保护保持、失败原因保留；不宣称完成。玩家可手动回标题继续（仍会核验），或在菜单里重试
            lastStep_ = tower::trip::Step::Failed;
            lastTripError_ = trip_.error();
            pbz::log::Info("影塔独立塔档：事务 {} 停在失败：{}（保护保持开启）", trip_.txn(), lastTripError_);
            if (wasExiting) restartAfterExit_ = false;
            Broadcast(wasExiting ? "err.exit_failed" : "err.enter_failed", (wasExiting ? "恢复原进度没有完成：" : "进入塔档没有完成：") + lastTripError_ + "。保护保持开启，可重试或手动回标题继续");
        } else if (!trip_.error().empty() && trip_.error() != lastTripError_ && !trip_.failed()) {
            lastTripError_ = trip_.error();  // 核验未通过 / 放回失败：保护保持，自动再走一次（手动模式提示再回一次标题）
            pbz::log::Info("影塔独立塔档：事务 {}：{}", trip_.txn(), lastTripError_);
            // 放存档待重试 → *_pending；核验未通过 → err.*_verify；自动回标题 / 继续没成功（会再试）→ info.trip_retrying
            Broadcast(trip_.pending() ? (wasExiting ? "info.exit_pending" : "info.enter_pending")
                      : trip_.retries() > 0 ? (wasExiting ? "err.exit_verify" : "err.enter_verify") : "info.trip_retrying", lastTripError_);
        } else if (trip_.busy() && trip_.step() != lastStep_ && !trip_.failed()) {
            lastStep_ = trip_.step();
            const char* what = wasExiting ? "原进度" : "影塔";
            const std::string text = lastStep_ == tower::trip::Step::Put ? std::string("等原生加载落稳后切换存档")
                                   : lastStep_ == tower::trip::Step::Title ? std::string("正在返回标题")
                                   : lastStep_ == tower::trip::Step::Settle ? std::string("原档已核验，等原生存档落稳后关闭保护")
                                   : std::format("正在载入{}", what);
            pbz::log::Info("影塔独立塔档：事务 {} → {}（{}）", trip_.txn(), tower::trip::StepName(lastStep_), text);
            if (trip_.automatic()) Broadcast("info.trip_step", text);
        } else if (wasExiting && trip_.phase() == tower::trip::Phase::Outside) {
            SaveTrip();
            pbz::log::Info("影塔独立塔档：已重新进档，出塔完成（存档沙盒已关、成就恢复）");
            Broadcast("info.exit_done", "出塔完成：存档沙盒已关闭，原进度已恢复");
            if (restartAfterExit_) { restartAfterExit_ = false; PostRestart(); }
        }
    }
    static void PostRestart() {
        pbz::log::Info("影塔：重新开始，开 50 层新局");
        const std::string line = std::format(R"({{"op":"tower","cmd":"start","floors":50,"client":"ingame","takeover":true,"difficulty":"{}"}})", s_lastDiff ? "easy" : "hard");
        pbz::gamethread::Post([line] { towerapi::Command(line); });
    }
    static std::wstring TripFile() { auto f = RunFile(); return f.empty() ? f : f.substr(0, f.rfind(L'\\')) + L"\\tower-session.txt"; }
    void SaveTrip() { if (const auto f = TripFile(); !f.empty() && !AtomicTowerFile(f, trip_.Save())) pbz::log::Info("影塔存档：会话标记写入失败，保留上一次的完整版本"); }

  public:
    void LoadTrip() {
        std::ifstream f(TripFile(), std::ios::binary);
        if (!f) return;
        std::stringstream ss; ss << f.rdbuf();
        trip_ = tower::trip::Trip::Load(ss.str());
        if (trip_.phase() == tower::trip::Phase::Stale) {
            towergate::MarkForeign();  // 内存可能仍是塔档：不许把它记成新基准
            pbz::log::Info("影塔独立塔档：上次会话没有正常结束，等玩家选择恢复原进度或确认已是原进度");
        }
    }
    tower::trip::Phase TripPhase() const { return trip_.phase(); }
    bool TripFailed() const { return trip_.busy() && trip_.failed(); }
    std::string TripText() const {
        const bool exiting = trip_.phase() == tower::trip::Phase::Exiting;
        if (TripFailed()) return std::format("{}失败：{}", exiting ? "恢复原进度" : "进入影塔", trip_.error());
        if (!trip_.automatic()) return exiting ? "回标题后点“继续”" : "回标题后点“继续”";
        const char* what = exiting ? "恢复原进度" : "载入影塔";
        switch (trip_.step()) {
            case tower::trip::Step::Put: return std::format("正在{}…", what);
            case tower::trip::Step::Title: return std::format("正在{}…", what);
            case tower::trip::Step::Load: return std::format("正在{}…", what);
            case tower::trip::Step::Settle: return "正在恢复原进度…";
            default: return std::format("正在{}…", what);
        }
    }

  private:
    // 准入
    static std::wstring TowerFile(const wchar_t* leaf) { auto f = RunFile(); return f.empty() ? f : f.substr(0, f.rfind(L'\\') + 1) + leaf; }
    // 本局战绩（安装器"战绩"页读，1.0.1 用户 2026-11-06）：%LOCALAPPDATA%/PBZTower/run-log.txt，UTF-8，每行 Tab 分隔
    //   start  局 id  开局时间(Unix 秒)  总层数
    //   floor  层  用时(0.1 秒)  Boss 名（、分隔）  挑战名  挑战达成(1/0)
    //   lost   层  用时(0.1 秒)  Boss 名        won  层
    // 开新局时清空重写；死亡 / 通关只追加结尾，留到下一局开始
    void RunLog(const std::string& line, bool fresh = false) {
        const auto path = TowerFile(L"run-log.txt");
        if (path.empty()) return;
        std::ofstream f(path, std::ios::binary | (fresh ? std::ios::trunc : std::ios::app));
        f << line << '\n';
    }
    // 战绩记录用：无尽混编层按波写（波之间用 |，同波 Boss 用 、），安装器按波分行显示；其他层同 BossNames
    std::string LoggedBossNames() const {
        if (!EndlessOn() || endless_.empty()) return BossNames();
        std::string s;
        for (const auto& w : endless_) {
            std::string names;
            for (int b : w.bosses) if (b >= 0 && b < kPool) names += (names.empty() ? "" : "、") + std::string(tower::kBossPool[b].name);
            if (!names.empty()) s += (s.empty() ? "" : "|") + names;
        }
        return s.empty() ? BossNames() : s;
    }
    std::string BossNames() const {
        std::string s;
        for (int b : run_.bosses) if (b >= 0 && b < kPool) {
            const std::string n(tower::kBossPool[b].name);
            if (s.find(n) == std::string::npos) s += (s.empty() ? "" : "、") + n;
        }
        return s;
    }
    static std::string ReadText(const std::wstring& path) { std::ifstream f(path, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
    void LoadAdmission() { verifiedText_ = ReadText(TowerFile(L"boss-verified.txt")); deniedText_ = ReadText(TowerFile(L"boss-denied.txt")); }
    bool Admitted(int i) const { return tower::admission::Admitted(tower::kBossPool[i].path, poolAudit_, verifiedText_, deniedText_); }
    tower::plan::PoolView Pool() {
        tower::plan::PoolView v;
        v.size = kPool;
        v.path = [](int i) { return tower::kBossPool[i].path; };
        v.identity = [](int i) { return tower::kBossPool[i].identity; };
        v.admitted = [this](int i) { return Admitted(i); };
        return v;
    }
    static int FindBoss(const std::string& key) {
        if (key.empty()) return -1;
        for (int i = 0; i < kPool; ++i) if (tower::kBossPool[i].path == key) return i;
        for (int i = 0; i < kPool; ++i) if (tower::kBossPool[i].identity == key) return i;
        return -1;
    }
    void BadBoss(const std::string& reason) {
        if (run_.phase != tower::Phase::Fighting || run_.bosses.empty()) { Broadcast("err.badboss", "只有本层战斗中能报告"); return; }
        SYSTEMTIME t; GetLocalTime(&t);
        for (int b : run_.bosses) {
            std::ofstream(TowerFile(L"boss-denied.txt"), std::ios::binary | std::ios::app)
                << std::format("{}\t{:04}-{:02}-{:02} {:02}:{:02} {}\n", tower::kBossPool[b].path, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, reason);
            badBosses_.insert(b);
            RecordFailure("player_report", b, reason);
            pbz::log::Info("影塔准入：玩家报告 {} 有问题（{}），本机禁用", tower::kBossPool[b].name, reason);
        }
        LoadAdmission();
        Clear("badboss");
        actors_.clear();
        if (fixedBoss_ >= 0 && badBosses_.count(fixedBoss_)) fixedBoss_ = -1;
        if (SkipFloor("玩家报告 Boss 有问题")) { SaveRun(); Broadcast("info.badboss", floorNote_); return; }
        run_.UndoFloor();
        Broadcast("info.badboss", "已记下这个 Boss 有问题，本层作废、不扣任何东西；可以重新开这一层");
    }

    tower::session::IGame& Game() { return towernative::Game([this](std::string* where) { return BackupSave(where); }); }
    static std::string Csv(const std::vector<std::string>& v) { std::string o; for (auto& x : v) o += (o.empty() ? "" : ",") + x; return o; }

    // 开层前（玩家已明确点"开第 N 层"）：原生界面里的升级 / 点能力对账（含塔档以外的兵器）、核对真实装备、算 Boss 补偿
    bool GrowBeforeFloor() {
        if (const auto note = grow_.Reconcile(Game(), true); !note.empty()) { pbz::log::Info("影塔原生成长：{}", note); Broadcast("info.grow_reconciled", note); }
        const auto l = towernative::ReadLoadout();
        bool anyMain = false;
        for (auto& m : l.main) anyMain |= !m.empty() && m != "None";
        if (l.ok && !anyMain) { Broadcast("err.no_weapon", "没有装备主兵器：先在 Esc → 角色里装上再开层"); return false; }
        pbz::log::Info("影塔原生成长：开层装备 主[{}] 影[{}] 药[{} / {}] 饰品 {}/{}", Csv(l.main), Csv(l.shaqi), l.medBottom, l.medTop, l.accessories.size(), l.accessorySlots);
        // Boss 补偿按玩家原生攻击 / 生命相对塔档起点的涨幅（方案第 8 节：约八成 / 七成）；祝福另由 BuffHpComp 补偿
        if (const Target* p = Player(); p && baseAtk_ > 0 && attack_ >= 0) {
            const double atk = ue::Get<float>(attr::AttrCurrent(*p, attack_));
            const double hp = ue::Get<float>(attr::AttrCurrent(*p, hpMax_));
            growHpComp_ = growDmgComp_ = 1;  // v14：Boss 配置按逐层表固定，不跟玩家实时属性同步
            pbz::log::Info("影塔原生成长：攻击 {:.0f}（起点 {:.0f}）生命 {:.0f}（起点 {:.0f}）；Boss 按逐层表，不随玩家属性变化", atk, baseAtk_, hp, baseHp_);
        }
        return true;
    }

    // 3.6.1 批量锻造确认：按组依次发放；选的那项已失效改发同组另一个，三个都失效退还这一组的魂玉。一次存档（交易日志防中途崩溃）
    void ForgePick(const std::vector<int>& picks) {
        if (run_.phase != tower::Phase::Ready || !grow_.forgeBatch.Active()) { Broadcast("err.forge", "现在没有待选的锻造"); return; }
        if (grow_.hexJournal.active && !FinishHexJournal()) { Broadcast("err.forge", "正在恢复上次交易，请稍后重试"); return; }
        {   // 用户 2026-11-15：每组都要选，没选完不能确定
            const auto& rows = grow_.forgeBatch.rows;
            bool all = picks.size() == rows.size();
            for (size_t i = 0; all && i < rows.size(); ++i) all = picks[i] >= 0 && picks[i] < static_cast<int>(rows[i].size());
            if (!all) { Broadcast("err.forge", "还有组没选，请每组选一项"); return; }
        }
        const auto before = grow_.forgeBatch; const auto oldAttrs = grow_.attrs; const auto oldProfile = grow_.profile; const auto oldBless = grow_.bless;
        const auto oldItems = run_.items; const int oldJade = run_.jade;
        // 用户 2026-11-15：买了不退款；照玩家选的发，有上限的属性超出上限的部分作废（玩家自己算）
        int wasted = 0;
        const int granted = grow_.forgeBatch.Confirm(picks,
            [this](const tower::rewards::Offer& o) { return o.kind == tower::rewards::Kind::Attr ? grow_.attrs.Add(o.index, o.amount) : 0; }, &wasted);
        grow_.hexJournal = {true, run_.jade, run_.items};
        if (!SaveHex()) {
            grow_.forgeBatch = before; grow_.attrs = oldAttrs; grow_.profile = oldProfile; grow_.bless = oldBless; grow_.hexJournal = {}; run_.items = oldItems; run_.jade = oldJade;
            Broadcast("err.forge", "锻造保存失败，请重试"); return;
        }
        grow_.session.Sync(Game(), grow_.profile);
        if (!FinishHexJournal()) { Broadcast("err.forge", "锻造已记录，正在等待保存；请稍后重试"); return; }
        pbz::log::Info("影塔批量锻造：确认 {} 组{}", granted, wasted ? std::format("，其中 {} 组超出属性上限的部分作废", wasted) : std::string());
        Broadcast("info.forge", wasted ? std::format("已加到属性：{} 组（{} 组超出上限的部分作废）", granted, wasted) : std::format("已加到属性：{} 组", granted));
    }
    // 影塔菜单里的成长操作（只在准备阶段；战斗中不改装备 / 等级）
    void Grow(const std::string& action, int index, int to) {
        using tower::growth::Err;
        if (action == "state") { Broadcast("info.state", ""); return; }
        if (action == "character") {  // 试验：直接打开原生角色页
            std::string why;
            if (towernative::OpenCharacterPage(&why)) Broadcast("info.character_page", "已请求打开角色页");
            else Broadcast("err.character_page", "打不开角色页（" + why + "）：请按 Esc → 角色");
            return;
        }
        if (!grow_.on) { Broadcast("err.grow_off", "本局没有开原生成长"); return; }
        if (run_.phase != tower::Phase::Ready) { Broadcast("err.grow_phase", "开层前才能升级 / 换配置"); return; }
        if (grow_.hexJournal.active && !FinishHexJournal()) { Broadcast("err.grow", "正在恢复奖励交易，请稍后重试"); return; }
        Err e = Err::Bad;
        std::string what;
        const int nw = tower::growth::kWeaponCount, nn = tower::growth::kNodeCount;
        if (action == "upgrade" && index >= 0 && index < nw) { e = grow_.Upgrade(Game(), index); what = std::format("{} +1 级", tower::growth::kWeapons[index].name); }
        else if (action == "downgrade" && index >= 0 && index < nw) { e = grow_.Downgrade(Game(), index, to); what = std::format("{} 回退到 {} 级", tower::growth::kWeapons[index].name, to); }
        else if (action == "snap" && index >= 0 && index < nw) { e = grow_.Snap(Game(), index); what = std::format("{} 铸魂", tower::growth::kWeapons[index].name); }
        else if (action == "node" && index >= 0 && index < nn) { e = grow_.Activate(Game(), index, run_.floor); what = std::format("{} +1 级", tower::growth::kNodes[index].name); }
        else if (action == "reset_nodes") { grow_.ResetNodes(Game()); e = Err::Ok; what = "洗点"; }
        else if (action == "med") { const int m = index >= 0 && index < static_cast<int>(grow_.med.size()) ? grow_.med[index] : -1; e = grow_.PickMedicine(Game(), index); if (m >= 0) what = std::format("得到新药 {}", tower::growth::kMedicines[m].name); }
        else if (action == "acc") { const int a = index >= 0 && index < static_cast<int>(grow_.acc.size()) ? grow_.acc[index] : -1; e = grow_.PickAccessory(Game(), index); if (a >= 0) what = std::format("得到饰品 {}", tower::growth::kAccessories[a].name); }
        else if (action == "hex_attr") {
            const auto before = grow_.hexAttributes; const auto oldAttrs = grow_.attrs; const auto oldProfile = grow_.profile; const auto oldBless = grow_.bless;
            const auto oldItems = run_.items; const int oldJade = run_.jade;
            tower::rewards::Offer got;
            if (!grow_.hexAttributes.Pick(index, to, grow_.Ctx(run_.floor, run_.floors, run_.items), [this](const tower::rewards::Offer& o) {
                return grow_.Grant(Game(), o, run_.items, run_.jade, false);
            }, &got)) {
                Broadcast("err.hex_attr", "奖励已变化，请重新选择"); return;
            }
            if (got.kind == tower::rewards::Kind::Jade) InvestorBonus(got.amount);
            grow_.hexJournal = {true, run_.jade, run_.items};
            if (!SaveHex()) { grow_.hexAttributes = before; grow_.attrs = oldAttrs; grow_.profile = oldProfile; grow_.bless = oldBless; grow_.hexJournal = {}; run_.items = oldItems; run_.jade = oldJade; Broadcast("err.hex_save", "奖励保存失败，请重试"); return; }
            grow_.session.Sync(Game(), grow_.profile);
            if (!FinishHexJournal()) { Broadcast("err.hex_save", "奖励已记录，正在等待保存；请稍后重试"); return; }
            pbz::log::Info("影塔{}：领取 {}，剩余 {} 次", grow_.hexAttributes.blitz ? "速战速决" : "属性叠属性", tower::rewards::Label(got), grow_.hexAttributes.remaining);
            Broadcast("info.grow", "已领取：" + tower::rewards::Label(got)); return;
        }
        else if (action == "bonus") {
            tower::rewards::Offer got;
            e = grow_.PickBonus(Game(), index, run_.floor, run_.floors, run_.items, run_.jade, &got);
            if (e == Err::Ok) {
                if (got.kind == tower::rewards::Kind::Jade) InvestorBonus(got.amount);  // 钱生钱 / 积少成多同样加成
                what = "额外奖励 " + tower::rewards::Label(got);
                if (grow_.hexBonusSecond < grow_.hexes.ThrowerExtraPages()) {
                    ++grow_.hexBonusSecond;
                    if (grow_.DrawBonus(run_.floor, run_.floors, run_.seed, run_.items) > 0) what += std::format("；请选择第 {} 次额外奖励", grow_.hexBonusSecond + 1);
                    else grow_.hexBonusSecond = 0;
                } else grow_.hexBonusSecond = 0;
            }
        }
        else if (action == "reconcile") { const auto note = grow_.Reconcile(Game(), true); SaveRun(); Broadcast("info.grow_reconciled", note.empty() ? "没有变化" : note); return; }
        if (e != Err::Ok) { Broadcast("err.grow", std::string(tower::growth::ErrText(e))); return; }
        SaveRun();
        pbz::log::Info("影塔原生成长：{}（淬炼 {} 修为 {}）", what, grow_.profile.temper, grow_.profile.cultivate);
        Broadcast("info.grow", what);
    }

  public:
    int TemperLevels(const std::string& id, int kind, int levels) const {
        if (levels <= 0 || !grow_.on || !grow_.hexes.Has(tower::hex::TemperMaster)) return levels;
        const int w = tower::growth::WeaponIndex(id, kind), slot = w >= 0 ? grow_.profile.Find(w) : -1;
        if (slot < 0) return levels;
        const int cap = std::min(grow_.hexes.Has(tower::hex::Forge) ? 60 : tower::growth::kWeaponCap, tower::growth::kWeapons[w].maxLevel);
        return std::max(levels, std::min(levels * 2, cap - grow_.profile.weapons[slot].level));
    }
    float MeltPowerMul(UObject* cfg) const { if (auto it = meltMul_.find(cfg); it != meltMul_.end()) return it->second; return 1.0f; }
    // 塔内原生面板的一次升级 / 激活：按塔规则记账（tower_points.cpp 调用，游戏线程）
    bool ChargeWeapon(const std::string& id, int kind, int levels, std::string* why) {
        if (!grow_.on || trip_.phase() != tower::trip::Phase::Inside) { *why = "不在影塔里"; return false; }
        if (run_.phase == tower::Phase::Fighting) { *why = "战斗中不能升级"; return false; }
        const int w = tower::growth::WeaponIndex(id, kind);
        const int slot = w >= 0 ? grow_.profile.Find(w) : -1;
        if (slot < 0) { *why = "这把不在塔档里"; return false; }
        auto& ow = grow_.profile.weapons[slot];
        const int cap = std::min(grow_.hexes.Has(tower::hex::Forge) ? 60 : tower::growth::kWeaponCap, tower::growth::kWeapons[w].maxLevel);
        if (ow.level + levels > cap) { *why = std::format("塔内上限 {} 级", cap); return false; }
        // 3.1.0 淬炼大师：1 点升 2 级（最大升级 = 当前等级 + 淬炼点 × 2，不超过上限；奇数级向上取整扣点）
        grow_.profile.temperMaster = grow_.hexes.Has(tower::hex::TemperMaster);
        const int cost = grow_.profile.LevelCost(levels);
        if (grow_.profile.temper < cost) { *why = std::format("淬炼点不足（需要 {}，还有 {}）", cost, grow_.profile.temper); return false; }
        grow_.profile.temper -= cost;
        ow.level += levels;
        if (ow.spent >= 0) ow.spent += cost;  // 3.4.0 记实际花的点
        SaveRun();
        pbz::log::Info("影塔原生成长：原生面板 {} +{} 级（淬炼点 −{}，剩 {}）", tower::growth::kWeapons[w].name, levels, cost, grow_.profile.temper);
        return true;
    }
    // 用户 2026-11-11：饰品升级花淬炼点。上限按原生 GetAccessoryMaxRefineLevel（饰品本身决定能升几级）
    bool ChargeAccessory(const std::string& id, int levels, std::string* why) {
        if (!grow_.on || trip_.phase() != tower::trip::Phase::Inside) { *why = "不在影塔里"; return false; }
        if (run_.phase == tower::Phase::Fighting) { *why = "战斗中不能升级"; return false; }
        const int a = tower::growth::AccessoryIndex(id);
        auto it = a >= 0 ? std::find_if(grow_.profile.accessories.begin(), grow_.profile.accessories.end(), [a](const auto& x) { return x.index == a; }) : grow_.profile.accessories.end();
        if (it == grow_.profile.accessories.end()) { *why = "这件饰品不在塔档里"; return false; }
        const int cap = towernative::AccessoryMaxLevel(id);
        if (cap <= 0) { *why = "原生饰品上限读不到"; return false; }
        if (it->refine + levels > cap) { *why = cap <= 1 ? "这件饰品不能打磨" : std::format("最多打磨到 +{}", cap - 1); return false; }
        const int cost = levels * tower::growth::kAccessoryRefineTemper;
        if (grow_.profile.temper < cost) { *why = std::format("淬炼点不足（需要 {}，还有 {}）", cost, grow_.profile.temper); return false; }
        grow_.profile.temper -= cost;
        it->refine += levels;
        SaveRun();
        pbz::log::Info("影塔原生成长：饰品 {} 打磨 +{} → +{}（淬炼点 −{}，剩 {}）", tower::growth::kAccessories[a].name, levels, it->refine - 1, cost, grow_.profile.temper);
        return true;
    }
    void RefundAccessory(const std::string& id, int levels) {
        const int a = tower::growth::AccessoryIndex(id);
        for (auto& x : grow_.profile.accessories) if (x.index == a && x.refine > levels) { x.refine -= levels; grow_.profile.temper += levels * tower::growth::kAccessoryRefineTemper; }
        SaveRun();
    }
    bool ChargeNode(const std::string& id, std::string* why) {
        if (!grow_.on || trip_.phase() != tower::trip::Phase::Inside) { *why = "不在影塔里"; return false; }
        if (run_.phase == tower::Phase::Fighting) { *why = "战斗中不能点能力"; return false; }
        const int n = tower::growth::NodeIndex(id);
        if (n < 0) { *why = "未知节点"; return false; }
        const auto e = grow_.profile.Activate(n, run_.floor);  // 末列要轮回点：每通过 15 层 1 个
        if (e != tower::growth::Err::Ok) { *why = std::string(tower::growth::ErrText(e)); return false; }
        SaveRun();
        pbz::log::Info("影塔原生成长：原生面板 {} → {} 级（修为点 −{}，剩 {}）", tower::growth::kNodes[n].name, grow_.profile.nodes[n], tower::growth::NodeCost(n), grow_.profile.cultivate);
        return true;
    }
    bool ChargeSoul(const std::string& id, int kind, std::string* why) {
        if (!grow_.on || trip_.phase() != tower::trip::Phase::Inside || run_.phase != tower::Phase::Ready) { *why = "请在影塔准备阶段铸魂"; return false; }
        const int w = tower::growth::WeaponIndex(id, kind);
        if (w < 0) { *why = "未知兵器"; return false; }
        const auto result = grow_.profile.Snap(w);
        if (result != tower::growth::Err::Ok) { *why = std::string(tower::growth::ErrText(result)); return false; }
        SaveRun(); pbz::log::Info("影塔原生铸魂：{} 铸魂令 -1，剩 {}", id, grow_.profile.soulTokens); return true;
    }
    void RefundSoul(const std::string& id, int kind) {
        const int w = tower::growth::WeaponIndex(id, kind), slot = w >= 0 ? grow_.profile.Find(w) : -1;
        if (slot >= 0 && grow_.profile.weapons[slot].snapped) { grow_.profile.weapons[slot].snapped = false; ++grow_.profile.soulTokens; SaveRun(); }
    }
    bool NativeDowngrade(const std::string& id, int kind, int target, std::string* why) {
        if (!grow_.on || trip_.phase() != tower::trip::Phase::Inside || run_.phase != tower::Phase::Ready) { *why = "请在影塔准备阶段回退"; return false; }
        const int w = tower::growth::WeaponIndex(id, kind);
        if (w < 0) { *why = "未知兵器"; return false; }
        auto next = grow_.profile;
        next.temperMaster = grow_.hexes.Has(tower::hex::TemperMaster);
        const auto e = next.Downgrade(w, target);
        if (e != tower::growth::Err::Ok) { *why = std::string(tower::growth::ErrText(e)); return false; }
        if (!Game().SetWeaponLevel(id, kind, target)) { *why = "原生回退读回失败，点数未变"; return false; }
        grow_.profile = std::move(next); SaveRun();
        pbz::log::Info("影塔原生回退：{} → {} 级，淬炼点 {}", id, target, grow_.profile.temper); return true;
    }
    bool NativeResetNodes(const std::string& id, bool all, std::string* why) {
        if (!grow_.on || trip_.phase() != tower::trip::Phase::Inside || run_.phase != tower::Phase::Ready) { *why = "请在影塔准备阶段洗点"; return false; }
        const int which = all ? -1 : tower::growth::NodeIndex(id);
        if (!all && which < 0) { *why = "未知能力"; return false; }
        bool ok = true;
        for (int n = 0; n < tower::growth::kNodeCount; ++n) {
            if (!all && n != which) continue;
            const int before = grow_.profile.nodes[n];
            if (!before) continue;
            if (!Game().SetNodeLevel(tower::growth::kNodes[n].id, 0)) { ok = false; continue; }
            grow_.profile.nodes[n] = 0; grow_.profile.cultivate += tower::growth::NodeCost(n) * before;
        }
        SaveRun(); if (!ok) *why = "部分能力未能回退，已按实际成功项返还修为";
        pbz::log::Info("影塔原生洗点：{} all={} 修为点 {} success={}", id, all, grow_.profile.cultivate, ok); return ok;
    }
    void Refund(const std::string& id, int kind, int levels) {
        if (kind == 2) {
            const int n = tower::growth::NodeIndex(id);
            if (n >= 0 && grow_.profile.nodes[n] > 0) { --grow_.profile.nodes[n]; grow_.profile.cultivate += tower::growth::NodeCost(n); }
        } else if (const int w = tower::growth::WeaponIndex(id, kind); w >= 0) {
            if (const int slot = grow_.profile.Find(w); slot >= 0) { auto& ow = grow_.profile.weapons[slot]; const int c = grow_.profile.LevelCost(levels); ow.level -= levels; grow_.profile.temper += c; if (ow.spent >= 0) ow.spent = std::max(0, ow.spent - c); }  // 升级未生效：按扣掉的点退
        }
        SaveRun();
    }
    void Notify(const std::string& code, const std::string& msg) { Broadcast(code, msg); }
    const tower::growthrun::Runner& Growth() const { return grow_; }
    const std::string& Gains() const { return gains_; }

  private:
    // ---- 3.7.0 创意层（tower_special.h；方案 notes/tower-3.7.0-special-floors.md）
    tower::special::Floor special_{};
    int specialF_ = -1, specialKey_ = 0, specialAllies_ = 0;
    bool specialMain_ = false, specialDone_ = false;
    double specialRefillAt_ = 0; int specialRefills_ = 0;  // 生存战兜底补 Boss
    std::unordered_map<int, std::string> specialFaction_;  // Boss 池序号 → 阵营（乙队 enemy2）
#if defined(PBZ_HEX_TEST) || defined(PBZ_PLAYTEST)
    tower::special::Kind qaSpecial_ = tower::special::Kind::None;
#endif
    static constexpr int kSpecialInFlight = 3;
    bool SpecialOn() const { return special_.kind != tower::special::Kind::None && !endless_.empty(); }
    void SpecialReset() { special_ = {}; specialF_ = -1; specialKey_ = 0; specialAllies_ = 0; specialMain_ = specialDone_ = false; specialFaction_.clear(); specialRefillAt_ = 0; specialRefills_ = 0; }
    std::string FactionFor(int b) const { const auto it = specialFaction_.find(b); return it != specialFaction_.end() ? it->second : std::string(); }
    tower::servants::Assist SpecialAssist() const {
        if (!SpecialOn() || specialAllies_ <= 0) return {};
        return {specialAllies_, tower::special::AllyShare(special_.kind, ShownNow())};
    }
    // 组阵容：只用不依赖场地的 Boss（无尽池），同层身份不重复；乙队记 enemy2（与甲队互打）
    bool BuildSpecial(tower::special::Floor sp, int key, int mainF) {
        const auto L = tower::special::LineupOf(sp.kind);
        const auto pools = EndlessPools();
        const int nb = L.enemyBosses + L.enemy2Bosses;
        auto bosses = tower::special::PickBosses(run_.seed, key, nb, pools.bosses, pools.identity, pools.solo);
        if (static_cast<int>(bosses.size()) < nb) {
            pbz::log::Info("影塔创意层：{} 需要 {} 只 Boss，可用身份只有 {} 个，本层不换", tower::special::Name(sp.kind), nb, bosses.size());
            return false;
        }
        tower::endless::Wave w; w.bosses = bosses;
        uint64_t r = run_.seed ^ (static_cast<uint64_t>(key) * 0x9E3779B97F4A7C15ull) ^ 0x5EC1A1ull;
        for (int tries = 0; static_cast<int>(w.mobs.size()) < L.enemyMobs && tries < 64 && pools.mobCount > 0; ++tries) {
            r ^= r << 13; r ^= r >> 7; r ^= r << 17;
            const int m = static_cast<int>(r % static_cast<uint64_t>(pools.mobCount));
            if (!pools.mobOk || pools.mobOk(m)) w.mobs.push_back(m);
        }
        endless_ = {w};
        special_ = sp; specialF_ = mainF; specialKey_ = key; specialMain_ = mainF > 0; specialDone_ = false;
#ifdef PBZ_HEX_TEST
        if (auto_.on && special_.kind == tower::special::Kind::Survival) special_.survivalSec = 20;  // 4.0.0 自动爬塔：生存战只撑 20 秒（流程照走）
#endif
        specialFaction_.clear();
        for (int i = L.enemyBosses; i < nb; ++i) specialFaction_[bosses[static_cast<size_t>(i)]] = "enemy2";
        specialAllies_ = tower::special::AllyCount(run_.seed, key, sp.kind);
        std::string names;
        for (size_t i = 0; i < bosses.size(); ++i) names += std::format("{}{}{}", i ? "、" : "", tower::kBossPool[bosses[i]].name, specialFaction_.count(bosses[i]) ? "（乙队）" : "");
        pbz::log::Info("影塔创意层：{}（{}，显示第 {} 层）敌方 {} + 小怪 {} 只；助战 Boss {} 个（玩家 {:.0f}%）{}", tower::special::Name(sp.kind), mainF > 0 ? "主线小兵段" : "无尽层",
                       mainF > 0 ? tower::floorplay::ShownMobs(mainF, run_.floors) : ShownBoss(key), names, w.mobs.size(), specialAllies_,
                       tower::special::AllyShare(sp.kind, mainF > 0 ? tower::floorplay::ShownMobs(mainF, run_.floors) : ShownBoss(key)) * 100, sp.kind == tower::special::Kind::Survival ? std::format("；生存 {} 秒", sp.survivalSec) : std::string());
        return true;
    }
    // 生存：敌方打不死（血量目标 ×10000，掉 10% 立即补满，不碰锁血线 / 原生流程；实机：只按 60% 补、本层血量 1043 时一套连招直接打死）；时间到 → 敌方生命归零，按无尽层残血兜底同一条死亡确认结算
    void SpecialTick(bool paused) {
        if (!SpecialOn() || run_.phase != tower::Phase::Fighting || special_.kind != tower::special::Kind::Survival || hp_ < 0 || hpMax_ < 0) return;
        const double t = FightSeconds();
        if (!specialDone_ && t >= 0 && t >= special_.survivalSec && !paused && deathAt_ < 0) {
            specialDone_ = true;
            int n = 0;
            for (auto& a : actors_) {
                if (a.dead || a.gone || !a.hasTarget || !ue::Live(a.obj) || IsServant(a.obj)) continue;
                const float zero = 0.0f;
                ue::Write(attr::AttrBase(a.target, hp_), &zero, 4); ue::Write(attr::AttrCurrent(a.target, hp_), &zero, 4);
                towercombat::Adjusted(a.obj, 0.0f, "生存战时间到");
                ++n;
            }
            pbz::log::Info("影塔创意层：生存 {} 秒到，清场 {} 个敌人，按过关结算", special_.survivalSec, n);
            Broadcast("info.special_survived", "生存成功");
            return;
        }
        if (specialDone_) return;
        // 兜底（用户 2026-11-15）：敌方 Boss 不管什么原因死了，立刻补一只同阵营的新 Boss，保证场上一直 5 只；友军死了不补（友军走仆从系统，这里只看敌方）
        if (t >= 0 && special_.survivalSec - t > 5.0 && pending_ <= 0 && spawnQueue_.empty() && now_ >= specialRefillAt_ && specialRefills_ < 30) {
            int alive = 0; std::vector<std::string_view> ids;
            for (const auto& a : actors_) if (a.boss >= 0 && !a.dead && !a.gone && ue::Live(a.obj) && !IsServant(a.obj)) { ++alive; ids.push_back(tower::kBossPool[a.boss].identity); }
            const int want = tower::special::LineupOf(special_.kind).enemyBosses;
            if (alive < want) {
                auto pools = EndlessPools();
                std::vector<int> pool;
                for (int b : pools.bosses) if (std::find(ids.begin(), ids.end(), tower::kBossPool[b].identity) == ids.end()) pool.push_back(b);
                const auto pick = tower::special::PickBosses(run_.seed, specialKey_ * 131 + 7 + specialRefills_, 1, pool, pools.identity, [](int) { return false; });
                if (!pick.empty()) {
                    before_ = pbz::enemies::OwnedActors();
                    ++pending_; ++specialRefills_;
                    run_.bosses.push_back(pick[0]);
                    pbz::log::Info("影塔创意层：生存战敌方只剩 {} 只，补 {}（第 {} 次兜底）", alive, tower::kBossPool[pick[0]].name, specialRefills_);
                    SubmitBoss(pick[0], FactionFor(pick[0]));
                    specialRefillAt_ = now_ + 2.0;
                    if (run_.phase != tower::Phase::Fighting) return;
                } else specialRefillAt_ = now_ + 5.0;
            }
        }
        for (auto& a : actors_) {
            if (a.dead || a.gone || !a.hasTarget || !a.filled || !ue::Live(a.obj) || IsServant(a.obj)) continue;
            const float hp = ue::Get<float>(attr::AttrCurrent(a.target, hp_), -1.0f), mx = ue::Get<float>(attr::AttrCurrent(a.target, hpMax_), -1.0f);
            if (mx > 0 && hp >= 0 && hp < tower::special::kSurvivalRefillAt * mx) { attr::Hold(a.target, attr::AttrBase(a.target, hp_), mx); attr::Hold(a.target, attr::AttrCurrent(a.target, hp_), mx); }
            if (shachiNow_ >= 0 && shachiMax_ >= 0) {  // 不能被破防（破防后处决会直接打死，不看血量）
                const float sh = ue::Get<float>(attr::AttrCurrent(a.target, shachiNow_), -1.0f), sm = ue::Get<float>(attr::AttrCurrent(a.target, shachiMax_), -1.0f);
                if (sm > 0 && sh >= 0 && sh < tower::special::kSurvivalRefillAt * sm) { attr::Hold(a.target, attr::AttrBase(a.target, shachiNow_), sm); attr::Hold(a.target, attr::AttrCurrent(a.target, shachiNow_), sm); }
            }
        }
    }
    bool survivalLost_ = false;
    // 生存战倒下：满血、扣一半魂玉、这一段算过去但不开驿站，回准备页（不结束本局）
    void FinishSurvivalLoss(const Target& p) {
        towerwidgets::DeathShow(false);
        deathAt_ = -1; reviving_ = false;
        const float max = ue::Get<float>(attr::AttrCurrent(p, hpMax_));
        if (max > 1) SetHp(p, max);
        const int lost = run_.jade / 2;
        run_.jade -= lost;
        const bool mainStage = specialMain_ && run_.specialStage;
        const int F = specialF_;
        Clear("survival_lost");
        actors_.clear();
        EndlessReset();
#if defined(PBZ_HEX_TEST) || defined(PBZ_PLAYTEST)
        qaSpecial_ = tower::special::Kind::None;
#endif
        run_.bosses.clear();
        if (mainStage) {  // 主线：这一小兵段算打过，驿站不开（shopFloor 记成这一段）
            run_.mobFloor = F; run_.mobWave = tower::mobs::WaveCount(F);
            run_.shopFloor = std::max(run_.shopFloor, F);
            run_.inMobWave = false; run_.specialStage = false;
            run_.phase = tower::Phase::Ready;
            MobLeave();
        } else {  // 无尽：这一层已经 ++floor，算过去、不发奖励
            run_.phase = tower::Phase::Ready;
            grow_.prep.FloorEnded();
            towerisolate::End(); tvenue::End(); boundary_.Disarm(); originSet_ = false;
        }
        floorNote_ = std::format("生存失败：魂玉 -{}，本段不开驿站", lost);
        pbz::log::Info("影塔创意层：生存战失败，魂玉 -{}（剩 {}），{}，回准备页", lost, run_.jade, mainStage ? "小兵段算过、不开驿站" : "无尽层算过、不发奖励");
        SaveRun();
        Broadcast("info.survival_lost", floorNote_);
    }
    // 主线创意层打完：清场、复位，然后按小兵段结算（魂玉、驿站），层号不动
    void FinishSpecialMain() {
        const int F = specialF_;
        pbz::log::Info("影塔创意层：{} 打完（显示第 {} 层），按小兵段结算", tower::special::Name(special_.kind), tower::floorplay::ShownMobs(F, run_.floors));
        Clear("special");
        actors_.clear();
        EndlessReset();
#if defined(PBZ_HEX_TEST) || defined(PBZ_PLAYTEST)
        qaSpecial_ = tower::special::Kind::None;
#endif
        run_.bosses.clear();
        FinishMobStage(F);
    }
    // ---- 3.7.0 连跳：本作跳跃是能力 GA_Hun_Jump（修改器"无限空中跳"同一接口）。空中（MOVE_Falling）按跳跃：
    // 结束仍在进行的跳跃能力、重新激活（动作），再按原生起跳速度 LaunchCharacter（实机：只激活能力时有时不起跳）；落地清零。
    // 输入：读增强输入里跳跃动作的值（MultiJumpNativeDown，任何按键 / 手柄，跟随玩家改键）；接口不可用时退回键盘空格。
    // （AbilityInputComponent.HandleInputAction 记录器能看到，但核心解析不到这个函数，挂不上——3.7.0 实机）
    UObject *mjJumpCls_ = nullptr, *mjActivate_ = nullptr, *mjEnd_ = nullptr, *mjJump_ = nullptr, *mjLaunch_ = nullptr, *mjCmc_ = nullptr;
    int mjModeOff_ = -1, mjAscOff_ = -1, mjCmcOff_ = -1, mjZOff_ = -1, mjAir_ = 0, mjObs_ = 0, mjInputLogs_ = 0;
    bool mjDown_ = true, mjResolved_ = false, mjLogged_ = false, mjPendingAir_ = false;
    double mjLastInput_ = -1;
    double mjAirSince_ = -1;
    bool mjLaunchLogged_ = false;
    static constexpr double kMultiJumpVz = 750.0;  // 空中再跳的竖直速度（厘米 / 秒），约 2.5 米
    // 原生跳跃输入：每帧读增强输入里跳跃动作（名字带 Jump 的 InputAction，优先 IA_Jump）的值——键盘、手柄、改键都跟随游戏自己的绑定。
    // 接口：EnhancedInputComponent.GetBoundActionValue(Action)，依次问角色和控制器的 InputComponent（实机：EnhancedPlayerInput.GetActionValue 不是反射函数）。
    // 不可用时退回键盘空格
    UObject *mjIA_ = nullptr, *mjGetValue_ = nullptr;
    int mjInputCompOff_ = -1;
    double mjIAScanAt_ = 0;
    bool mjNativeDown_ = false, mjNativeLogged_ = false, mjNativeFnTried_ = false;
    bool BoundJumpDown(UObject* comp) {
        if (!comp || !ue::Live(comp)) return false;
        std::vector<uint8_t> b(0x80, 0);
        auto* act = Arg(mjGetValue_, b, "Action");
        if (!act) return false;
        std::memcpy(act, &mjIA_, 8);
        pe::Call(comp, mjGetValue_, b.data());
        const auto* ret = Arg(mjGetValue_, b, "ReturnValue");
        if (!ret) return false;
        double d = 0; float f = 0; std::memcpy(&d, ret, 8); std::memcpy(&f, ret, 4);
        return (std::isfinite(d) && std::abs(d) > 0.5 && std::abs(d) < 100) || (std::isfinite(f) && std::abs(f) > 0.5f && std::abs(f) < 100);
    }
    int MultiJumpNativeDown(UObject* pawn) {  // 1 按下中 / 0 没按 / -1 接口不可用
        if (!mjNativeFnTried_) {
            mjNativeFnTried_ = true;
            auto* eic = towerperf::ClassNamed("EnhancedInputComponent");
            mjGetValue_ = eic ? pe::FindFunction(eic, "GetBoundActionValue") : nullptr;
            auto* actor = towerperf::ClassNamed("Actor");
            mjInputCompOff_ = actor ? ue::PropOffset(actor, "InputComponent") : -1;
            if (!mjGetValue_) mjNativeFnTried_ = !eic;  // 类还没加载：之后再试
        }
        if ((!mjIA_ || !ue::Live(mjIA_)) && now_ >= mjIAScanAt_) {
            mjIAScanAt_ = now_ + 5.0;
            mjIA_ = nullptr;
            std::string names;
            ue::ForEach([&](int, UObject* o) {
                if (ue::IsDefault(o) || ue::ClassName(o) != "InputAction") return true;
                const auto n = ue::ObjName(o);
                if (n.find("Jump") == std::string::npos) return true;
                names += n + " ";
                if (!mjIA_ || n == "IA_Jump") mjIA_ = o;
                return true;
            });
            if (!mjNativeLogged_) { mjNativeLogged_ = true; pbz::log::Info("影塔海克斯：连跳原生跳跃输入 动作 {}（候选 {}）读值接口 {} InputComponent 偏移 {}", mjIA_ ? ue::ObjName(mjIA_) : std::string("无"), names.empty() ? "无" : names, mjGetValue_ != nullptr, mjInputCompOff_); }
        }
        if (!mjIA_ || !mjGetValue_ || mjInputCompOff_ < 0) return -1;
        auto* pc = towerwidgets::Controller();
        UObject* comps[2] = {pawn ? ue::Get<UObject*>(pawn + mjInputCompOff_) : nullptr, pc && ue::Live(pc) ? ue::Get<UObject*>(pc + mjInputCompOff_) : nullptr};
        auto* eic = towerperf::ClassNamed("EnhancedInputComponent");
        bool any = false, down = false;
        for (auto* c : comps) if (c && ue::Live(c) && eic && ue::IsA(c, eic)) { any = true; down = down || BoundJumpDown(c); }
        return any ? (down ? 1 : 0) : -1;
    }
    void MultiJumpTick(UObject* pawn) {
        const bool down = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
        const bool spacePressed = down && !mjDown_;
        mjDown_ = down;
        const int rank = grow_.on ? grow_.hexes.Rank(tower::hex::MultiJump) : 0;
        if (rank <= 0 || !pawn || !ue::Live(pawn)) { mjAir_ = 0; mjPendingAir_ = false; return; }
        if (!mjResolved_) {
            mjResolved_ = true;
            mjJumpCls_ = towerperf::ClassNamed("GA_Hun_Jump_C");
            auto* asc = towerperf::ClassNamed("AbilitySystemComponent");
            auto* ga = towerperf::ClassNamed("GameplayAbility");
            auto* cmc = towerperf::ClassNamed("CharacterMovementComponent");
            auto* chr = towerperf::ClassNamed("PBZCharacter");
            auto* character = towerperf::ClassNamed("Character");
            mjActivate_ = asc ? pe::FindFunction(asc, "TryActivateAbilityByClass") : nullptr;
            mjEnd_ = ga ? pe::FindFunction(ga, "K2_EndAbility") : nullptr;
            mjLaunch_ = character ? pe::FindFunction(character, "LaunchCharacter") : nullptr;
            mjModeOff_ = cmc ? ue::PropOffset(cmc, "MovementMode") : -1;
            mjZOff_ = cmc ? ue::PropOffset(cmc, "JumpZVelocity") : -1;
            mjAscOff_ = chr ? ue::PropOffset(chr, "AbilitySystemComponent") : -1;
            mjCmcOff_ = character ? ue::PropOffset(character, "CharacterMovement") : -1;
            if (!mjLogged_) { mjLogged_ = true; pbz::log::Info("影塔海克斯：连跳接口 跳跃能力 {} 激活 {} 结束 {} 弹起 {} 移动模式 {} 起跳速度 {} ASC {} 移动组件 {}", mjJumpCls_ != nullptr, mjActivate_ != nullptr, mjEnd_ != nullptr, mjLaunch_ != nullptr, mjModeOff_, mjZOff_, mjAscOff_, mjCmcOff_); }
        }
        if (!mjJumpCls_ || !mjActivate_ || !mjEnd_ || mjModeOff_ < 0 || mjAscOff_ < 0 || mjCmcOff_ < 0) { mjResolved_ = mjJumpCls_ != nullptr; return; }  // 跳跃能力类还没加载：下帧再找
        auto* cmc = ue::Get<UObject*>(pawn + mjCmcOff_);
        if (!cmc || !ue::Live(cmc)) return;
        mjCmc_ = cmc;
        const bool air = ue::Get<uint8_t>(cmc + mjModeOff_) == 3;
        // 实机（t370o）：起跳那一帧已经是 Falling，地面起跳被当成空中再跳，结束了原生跳跃能力 → 连地面都跳不起来。
        // 现在：落地清零；只有离地满 0.15 秒之后的新按键才算空中再跳；不再结束 / 重新激活原生跳跃能力，只做一次向上弹起
        const int nativeDown = MultiJumpNativeDown(pawn);
        const bool nativePressed = nativeDown == 1 && !mjNativeDown_;
        if (nativeDown >= 0) mjNativeDown_ = nativeDown == 1;
        if (!air) { mjAir_ = 0; mjAirSince_ = -1; return; }
        if (mjAirSince_ < 0) mjAirSince_ = now_;
        const bool pressed = nativeDown >= 0 ? nativePressed : spacePressed;  // 原生输入可用就只认原生（跟随改键 / 手柄），否则空格
        if (!pressed || now_ - mjAirSince_ < 0.15 || mjAir_ >= tower::hex::MultiJumpAirJumps(rank) || towerwidgets::PanelActive() || towerwidgets::MenuBusy() || !mjLaunch_) return;
        if (nativeDown < 0) { DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid); if (pid != GetCurrentProcessId()) return; }
        std::vector<uint8_t> lb(0x40, 0);
        auto* v = Arg(mjLaunch_, lb, "LaunchVelocity");
        auto* xy = Arg(mjLaunch_, lb, "bXYOverride");
        auto* zo = Arg(mjLaunch_, lb, "bZOverride");
        if (!mjLaunchLogged_) { mjLaunchLogged_ = true; pbz::log::Info("影塔海克斯：连跳弹起接口 LaunchVelocity {} bXYOverride {} bZOverride {} 参数大小 {}", v != nullptr, xy != nullptr, zo != nullptr, ue::Get<int>(mjLaunch_ + ue::Layout::StructSize)); }
        if (!v || !zo) return;  // 参数对不上就不弹（绝不写错速度）
        const double vel[3]{0, 0, kMultiJumpVz};
        std::memcpy(v, vel, sizeof vel);
        if (xy) *xy = 0;
        *zo = 1;
        pe::Call(pawn, mjLaunch_, lb.data());
        ++mjAir_;
        pbz::log::Info("影塔海克斯：连跳 Lv{} 空中第 {} 次（输入 {}，离地 {:.2f} 秒）", rank, mjAir_, nativeDown >= 0 ? "原生跳跃" : "空格", now_ - mjAirSince_);
    }
    // ---- 3.7.0 玩法类海克斯（数值 tower_playstyle.h）
#ifdef PBZ_PLAYTEST
    static constexpr tower::hex::Id kPlaytestHexes[] = {tower::hex::LastStand, tower::hex::BreakChain, tower::hex::MultiJump};  // 测试包新局赠送（接好一个加一个）
#endif
    tower::playstyle::LastStand lastStand_;
    bool lastStandLogged_ = false;
    std::unordered_map<UObject*, float> chainLast_;
    int chainDiag_ = 0;
    void PlaystyleTick(const Target& p, bool fighting) {
        auto& hx = grow_.hexes;
        // 绝境时缓：每层生命首次低于 30%，本塔敌人的时间倍率乘 0.3 若干秒（与入场冻结同一接口，实测）
        if (hx.Has(tower::hex::LastStand) && hp_ >= 0 && hpMax_ >= 0) {
            const float hp = ue::Get<float>(attr::AttrCurrent(p, hp_), -1.0f), mx = ue::Get<float>(attr::AttrCurrent(p, hpMax_), -1.0f);
            const float frac = fighting && deathAt_ < 0 && mx > 0 && hp >= 0 ? hp / mx : -1.0f;
            const float scale = lastStand_.Tick(hx.Rank(tower::hex::LastStand), ShownNow(), now_, frac);
            if ((scale < 1.0f) != lastStandLogged_) {
                lastStandLogged_ = scale < 1.0f;
                pbz::log::Info("影塔海克斯：绝境时缓{}", lastStandLogged_ ? std::format("触发 Lv{}（生命 {:.0f}%），敌人速度 ×{:.1f}", hx.Rank(tower::hex::LastStand), frac * 100, scale) : std::string("结束"));
            }
            // 与缓时符叠加（用户）：缓时符本身 ×0.6，同时生效时这里乘 0.5，合计 0.3；定身符期间不管（已经 ×0.01）
            const float mul = scale < 1.0f && now_ < slowUntil_ ? tower::playstyle::kLastStandWithSlowItem / 0.6f : scale;
            if (mul < 1.0f) if (const int dil = attr::ActorOffset("CustomTimeDilation"); dil >= 0)
                for (const auto& a : actors_) if (!a.dead && !a.gone && a.hasTarget && ue::Live(a.obj) && !IsServant(a.obj))
                    attr::Want(a.target, reinterpret_cast<float*>(a.obj + dil), Kind::Scale, mul);
        }
        // 破防连锁：本塔敌人杀气从 >0 掉到 0 = 破防；半径内其他敌人各削一截杀气，最少留 1 点（不替原生判破防）
        if (!hx.Has(tower::hex::BreakChain) || !fighting || shachiNow_ < 0 || shachiMax_ < 0 || !fnLoc_) { chainLast_.clear(); return; }
        const int rank = hx.Rank(tower::hex::BreakChain);
        std::vector<const Actor*> live;
        for (const auto& a : actors_) if (!a.dead && !a.gone && a.hasTarget && ue::Live(a.obj) && attr::Alive(a.target) && !IsServant(a.obj)) live.push_back(&a);
        std::unordered_map<UObject*, float> next;
        for (const Actor* a : live) {
            const float sh = ue::Get<float>(attr::AttrCurrent(a->target, shachiNow_), -1.0f);
            next[a->obj] = sh;
            const auto it = chainLast_.find(a->obj);
#ifdef PBZ_PLAYTEST
            {   // 3.7.0 取证（实机：小怪被打到残血杀气仍满）：敌人杀气一帧掉 >10% 上限或掉到 5% 以下时记一行，确认破防时杀气怎么变
                const float mx = ue::Get<float>(attr::AttrCurrent(a->target, shachiMax_), -1.0f);
                if (it != chainLast_.end() && mx > 0 && sh >= 0 && it->second >= 0 && (it->second - sh > 0.10f * mx || (sh < 0.05f * mx && it->second >= 0.05f * mx)) && chainDiag_ < 300) {
                    ++chainDiag_;
                    pbz::log::Info("影塔破防取证：{} 杀气 {:.1f} → {:.1f} / {:.1f}", a->name, it->second, sh, mx);
                }
            }
#endif
            if (it == chainLast_.end() || !tower::playstyle::BrokeNow(it->second, sh)) continue;
            double at[4]{}; pe::Call(a->obj, fnLoc_, at);
            int hit = 0;
            for (const Actor* b : live) {
                if (b == a) continue;
                double bt[4]{}; pe::Call(b->obj, fnLoc_, bt);
                if (!tower::playstyle::InChainRadius(rank, bt[0] - at[0], bt[1] - at[1], bt[2] - at[2])) continue;
                const float bs = ue::Get<float>(attr::AttrCurrent(b->target, shachiNow_), -1.0f), bm = ue::Get<float>(attr::AttrCurrent(b->target, shachiMax_), -1.0f);
                const float after = tower::playstyle::ChainShachiAfter(rank, bs, bm);
                if (after >= bs) continue;
                attr::Hold(b->target, attr::AttrBase(b->target, shachiNow_), after); attr::Hold(b->target, attr::AttrCurrent(b->target, shachiNow_), after);
                next[b->obj] = after; ++hit;
            }
            pbz::log::Info("影塔海克斯：破防连锁 Lv{}：{} 破防，附近 {} 个敌人被削杀气", rank, a->name, hit);
        }
        chainLast_ = std::move(next);
    }

    const Target* Player() {
        for (auto& t : attr::Targets())
            if (t.who == attr::Who::Player && attr::Alive(t)) return &t;
        return nullptr;
    }
    void SetHp(const Target& p, float v) {
        v = std::clamp(v, 1.0f, ue::Get<float>(attr::AttrCurrent(p, hpMax_)));
        ue::Write(attr::AttrBase(p, hp_), &v, 4); ue::Write(attr::AttrCurrent(p, hp_), &v, 4);
    }

    void UseItem(const std::string& id) {
        int idx = -1;
        for (int i = 0; i < tower::kItemCount; ++i) if (tower::kItems[i].id == id) idx = i;
        if (idx < 0) { Broadcast("err.unknown_item", "没有这个道具"); return; }
        const auto item = tower::kItems[idx].item;
        const bool fighting = run_.phase == tower::Phase::Fighting;
        if (item == tower::Item::Intel || item == tower::Item::Borrow) { Broadcast("err.item_later", "这个道具在团战 / 观看局版本里生效"); return; }
        if (item == tower::Item::Revive) { Broadcast("err.item_passive", "复活符会在倒下时自动生效"); return; }
        if (item == tower::Item::Reroll) { if (!run_.Reroll()) Broadcast("err.cannot_reroll", "没有刷新令，或现在不能刷新"); else { SaveRun(); Broadcast("info.rerolled", "已刷新"); } return; }
        // 用户 2026-11-01：不再有战斗中即时使用的道具。准备道具走预装（prep）；成长模式下这里一律拒绝
        if (grow_.on || tower::rewards::ItemTiming(item) == tower::rewards::Timing::Prep) {
            Broadcast("err.item_prep", std::string(tower::kItems[idx].name) + "：开层前在背包里预装，下一层开战时生效");
            return;
        }
        const Target* p = Player();
        if (!p) { Broadcast("err.no_player", "找不到玩家角色"); return; }
        if (!run_.Consume(item)) { Broadcast("err.no_item", "背包里没有" + std::string(tower::kItems[idx].name), std::format(R"("item":"{}")", J(tower::kItems[idx].id))); return; }
        switch (item) {
            case tower::Item::Heal: SetHp(*p, ue::Get<float>(attr::AttrCurrent(*p, hp_)) + 0.4f * ue::Get<float>(attr::AttrCurrent(*p, hpMax_))); break;
            case tower::Item::Freeze: freezeUntil_ = now_ + 3; break;
            case tower::Item::Slow: slowUntil_ = now_ + 5; break;
            case tower::Item::Sunder: sunder_ = true; break;
            case tower::Item::Shachi:
                if (shachiNow_ >= 0 && shachiMax_ >= 0) {
                    const float full = ue::Get<float>(attr::AttrCurrent(*p, shachiMax_));
                    ue::Write(attr::AttrBase(*p, shachiNow_), &full, 4); ue::Write(attr::AttrCurrent(*p, shachiNow_), &full, 4);
                }
                break;
            default: break;
        }
        pbz::log::Info("影塔道具：{}", tower::kItems[idx].name);
        if (!fighting) SaveRun();
        Broadcast("info.item_used", "已使用：" + std::string(tower::kItems[idx].name), std::format(R"("item":"{}")", J(tower::kItems[idx].id)));
    }

    // 神通：开启运行库里对应的修改器功能，只到本层（或下一层）结束；用户在修改器里自己开着的不碰、不消耗
    void UseShentong(int slot) {
        if (run_.phase != tower::Phase::Ready && run_.phase != tower::Phase::Fighting) { Broadcast("err.st_phase", "开战前或战斗中才能用神通"); return; }
        if (slot < 0 || slot >= static_cast<int>(run_.shentong.size())) { Broadcast("err.st_slot", "神通栏里没有这一个"); return; }
        const auto& st = tower::kShentong[run_.shentong[slot]];
        auto* f = pbz::FindFeature(std::string(st.feature));
        if (!f || !f->available) { Broadcast("err.st_unavailable", std::string(st.name) + " 暂时不可用：" + (f ? f->reason : "功能不存在"), std::format(R"("shentong":"{}")", J(st.id))); return; }
        if (f->enabled) { Broadcast("err.st_already_on", std::string(st.name) + " 已经在修改器里开着，未消耗", std::format(R"("shentong":"{}")", J(st.id))); return; }
        if (!pbz::HoldFeature(f, "tower", st.value)) { Broadcast("err.st_unavailable", std::string(st.name) + " 暂时不可用", std::format(R"("shentong":"{}")", J(st.id))); return; }
        run_.TakeShentong(slot);
        stActive_.push_back(f);
        pbz::log::Info("影塔神通：{}（{}）", st.name, st.feature);
        if (run_.phase == tower::Phase::Ready) SaveRun();
        Broadcast("info.st_on", "神通生效：" + std::string(st.name), std::format(R"("shentong":"{}")", J(st.id)));
    }
    void ReleaseShentong() {
        for (auto* f : stActive_) pbz::ReleaseFeature(f, "tower");  // 用户之后在修改器里也开了的话，由持有者机制保持开启
        stActive_.clear();
    }

public:
    // 本层正在生效的神通 / 符（界面显示用）
    std::string Effects() const {
        std::string s;
        auto add = [&](std::string_view x) { s += (s.empty() ? "" : "、") + std::string(x); };
        for (auto* f : stActive_)
            for (auto& st : tower::kShentong) if (st.feature == f->Id()) { add("神通·" + std::string(st.name)); break; }
        if (grow_.on) { for (int i : grow_.prep.active) add(std::string(tower::kItems[i].name)); }
        else {
            if (sunder_) add("破甲符");
            if (now_ < freezeUntil_) add("定身符");
            if (now_ < slowUntil_) add("缓时符");
        }
        return s;
    }
    // 空闲时：上次没换回的塔内兵器（游戏崩溃 / 运行库热换后标记还在）在这里换回
    void Housekeep() {
        TripTick();
        if (trip_.phase() != tower::trip::Phase::Inside) towerisolate::Session(false, run_.floor, run_.id);
        if (run_.phase != tower::Phase::Idle) return;
        if (pushed_) { Unwear("idle"); return; }
        if (GetFileAttributesW(LoadoutFile().c_str()) == INVALID_FILE_ATTRIBUTES) return;
        UObject* bag = Bag();
        if (!bag) return;  // 还没进存档：等下次
        if (HasSnapshot(bag) && popFn_) { pe::Call(bag, popFn_, nullptr); pbz::log::Info("影塔兵器：上次没换回（崩溃或热换），已换回进塔前的兵器"); }
        else pbz::log::Info("影塔兵器：有未换回标记但游戏里没有临时装备，按已换回处理");
        DeleteFileW(LoadoutFile().c_str());
    }

private:
    // ---- 塔内换兵器：游戏自带的临时装备栈 PBZBackPackSubsystem.PushEquippedWeapons / PopEquippedWeapons ----
    // （静态：SDK 里 Push 时把当前装备存进 EquippedSnapshot，Pop 时还原；用户 2026-10-30：塔内所得不带出塔、不写游戏收藏）
    // 只换已拥有的兵器：不往背包里加东西，避免触发首次获得 / 收藏 / 成就这些带出塔的记录。
    // 第一次换之前备份游戏存档；暂离 / 出塔 / 运行库停用时 Pop；崩溃留下的标记由 Housekeep 换回。
    struct FArr { void* data; int32_t num, max; };
    bool ResolveBag() {
        if (bagCls_) return pushFn_ && popFn_ && hasFn_ && offMain_ >= 0 && offShaqi_ >= 0;
        bagCls_ = towerperf::ClassNamed("PBZBackPackSubsystem");
        strLib_ = towerperf::ClassNamed("KismetStringLibrary");
        if (!bagCls_ || !strLib_) { bagCls_ = nullptr; return false; }
        pushFn_ = pe::FindFunction(bagCls_, "PushEquippedWeapons");
        popFn_ = pe::FindFunction(bagCls_, "PopEquippedWeapons");
        hasFn_ = pe::FindFunction(bagCls_, "HasWeapon");
        toName_ = pe::FindFunction(strLib_, "Conv_StringToName");
        offMain_ = ue::PropOffset(bagCls_, "MainWeapons"); offShaqi_ = ue::PropOffset(bagCls_, "ShaqiWeapons");
        offMainIdx_ = ue::PropOffset(bagCls_, "CurrentWeaponIndex"); offShaqiIdx_ = ue::PropOffset(bagCls_, "CurrentShaqiWeaponIndex");
        offSnap_ = ue::PropOffset(bagCls_, "bHasEquippedSnapshot");
        ue::ForEach([&](int, UObject* o) { if (ue::ClassOf(o) == strLib_ && ue::IsDefault(o)) { strCdo_ = o; return false; } return true; });
        const bool ok = pushFn_ && popFn_ && hasFn_ && toName_ && strCdo_ && offMain_ >= 0 && offShaqi_ >= 0 && offMainIdx_ >= 0 && offShaqiIdx_ >= 0 && offSnap_ >= 0;
        if (!ok) pbz::log::Info("影塔兵器：临时装备接口缺失，塔内换兵器不可用");
        return ok;
    }
    UObject* Bag() {
        if (!ResolveBag()) return nullptr;
        if (bag_ && ue::ClassOf(bag_) == bagCls_ && !ue::IsDefault(bag_)) return bag_;
        bag_ = nullptr;
        ue::ForEach([&](int, UObject* o) { if (ue::ClassOf(o) == bagCls_ && !ue::IsDefault(o)) { bag_ = o; return false; } return true; });
        return bag_;
    }
    bool HasSnapshot(UObject* bag) { return ue::Get<uint8_t>(bag + offSnap_) != 0; }
    // 参数缓冲：按参数名取偏移
    static uint8_t* Arg(UObject* fn, std::vector<uint8_t>& buf, std::string_view name) {
        const int o = ue::PropOffset(fn, name);
        static uint8_t sink[64];
        return o >= 0 && o + 16 <= static_cast<int>(buf.size()) ? buf.data() + o : sink;
    }
    uint64_t ToName(const std::string& s) {
        std::wstring w(s.begin(), s.end());  // 兵器 ID 全是 ASCII
        std::vector<uint8_t> buf(0x80, 0);
        FArr str{const_cast<wchar_t*>(w.c_str()), static_cast<int32_t>(w.size() + 1), static_cast<int32_t>(w.size() + 1)};
        std::memcpy(Arg(toName_, buf, "InString"), &str, sizeof str);
        pe::Call(strCdo_, toName_, buf.data());
        return ue::Get<uint64_t>(Arg(toName_, buf, "ReturnValue"));
    }
    bool Owns(UObject* bag, uint64_t name, bool normal) {
        std::vector<uint8_t> buf(0x40, 0);
        std::memcpy(Arg(hasFn_, buf, "WeaponID"), &name, 8);
        *Arg(hasFn_, buf, "bIsNormalWeapon") = normal;
        pe::Call(bag, hasFn_, buf.data());
        return *Arg(hasFn_, buf, "ReturnValue") != 0;
    }
    static std::vector<uint64_t> Names(UObject* bag, int off) {
        const auto a = ue::Get<FArr>(bag + off);
        std::vector<uint64_t> out;
        for (int i = 0; a.data && i < a.num && i < 16; ++i) out.push_back(ue::Get<uint64_t>(static_cast<uint8_t*>(a.data) + i * 8));
        return out;
    }
    // 把 want 放进 list：已在里面就选中它，否则顶替当前选中的那一格
    static int Place(std::vector<uint64_t>& list, int cur, uint64_t want) {
        for (size_t i = 0; i < list.size(); ++i) if (list[i] == want) return static_cast<int>(i);
        if (list.empty()) { list.push_back(want); return 0; }
        cur = std::clamp(cur, 0, static_cast<int>(list.size()) - 1);
        list[cur] = want;
        return cur;
    }
    static std::wstring LoadoutFile() { auto f = RunFile(); return f.empty() ? f : f.substr(0, f.rfind(L'\\')) + L"\\loadout.txt"; }

    // 游戏存档目录（…\ProjectPBZ\Saved\SaveGames）整个复制到 %LOCALAPPDATA%\PBZTower\save-backup\<时间>，留最近 5 份
    // 3.4.1（玩家：开始爬塔必报「真档备份失败：复制失败」，save-backup 不存在报 3、存在报 5）：
    // ① 逐级建目录并检查（原来 create_directories 的错误被忽略）；② 不用 fs::copy 整树复制（建子目录会套用源目录属性模板），
    // 自己逐个文件复制，被占用时用全共享方式读出再写；③ 报错带步骤、路径和 Windows 错误码；
    // ④ %LOCALAPPDATA% 下建不了（杀毒 / 系统防护拦截）时改存到游戏自己的 Saved\PBZTower-backup（不在沙盒改道范围内）
    static std::string WinErr(DWORD e) { return std::format("错误码 {}", e); }
    static bool MakeDirs(const std::filesystem::path& dir, std::string* why) {
        std::filesystem::path cur;
        for (const auto& part : dir) {
            cur /= part;
            const auto w = cur.wstring();
            if (w.size() <= 3 && w.find(L':') != std::wstring::npos) continue;  // 盘符
            if (CreateDirectoryW(w.c_str(), nullptr)) continue;
            const DWORD e = GetLastError();
            if (e == ERROR_ALREADY_EXISTS) {
                const DWORD a = GetFileAttributesW(w.c_str());
                if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) continue;
                *why = "建目录失败（同名文件挡住）：" + tower::mods::Utf8(cur); return false;
            }
            *why = "建目录失败：" + tower::mods::Utf8(cur) + "，" + WinErr(e); return false;
        }
        return true;
    }
    static bool CopyOneFile(const std::wstring& from, const std::wstring& to, std::string* why) {
        if (CopyFileW(from.c_str(), to.c_str(), FALSE)) return true;
        const DWORD first = GetLastError();
        // 被游戏占用（共享冲突 / 拒绝）：用全共享只读打开，按字节复制
        HANDLE in = CreateFileW(from.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (in == INVALID_HANDLE_VALUE) { *why = "读取失败：" + tower::mods::Utf8(std::filesystem::path(from)) + "，" + WinErr(first) + " / " + WinErr(GetLastError()); return false; }
        HANDLE out = CreateFileW(to.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out == INVALID_HANDLE_VALUE) { const DWORD e = GetLastError(); CloseHandle(in); *why = "写入失败：" + tower::mods::Utf8(std::filesystem::path(to)) + "，" + WinErr(e); return false; }
        std::vector<char> buf(1 << 20); DWORD n = 0; bool ok = true;
        while (ok && ReadFile(in, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr) && n > 0) { DWORD w = 0; ok = WriteFile(out, buf.data(), n, &w, nullptr) && w == n; }
        const DWORD e = GetLastError();
        CloseHandle(in); CloseHandle(out);
        if (!ok) { *why = "写入失败：" + tower::mods::Utf8(std::filesystem::path(to)) + "，" + WinErr(e); return false; }
        return true;
    }
    static bool CopyTree(const std::filesystem::path& src, const std::filesystem::path& dst, std::string* why, int* files) {
        namespace fs = std::filesystem;
        if (!MakeDirs(dst, why)) return false;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(src, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            const fs::path rel = fs::relative(it->path(), src, ec);
            if (ec) break;
            const fs::path to = dst / rel;
            std::error_code e2;
            if (it->is_directory(e2)) { if (!MakeDirs(to, why)) return false; continue; }
            if (!it->is_regular_file(e2)) continue;
            if (!MakeDirs(to.parent_path(), why) || !CopyOneFile(it->path().wstring(), to.wstring(), why)) return false;
            ++*files;
        }
        if (ec) { *why = "读取存档目录失败：" + tower::mods::Utf8(src) + "，" + ec.message(); return false; }
        return true;
    }
    bool BackupSave(std::string* where) {
        namespace fs = std::filesystem;
        std::error_code ec;
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        fs::path src;
        for (fs::path p = fs::path(exe).parent_path(); !p.empty() && p != p.root_path(); p = p.parent_path())
            if (fs::is_directory(p / L"ProjectPBZ" / L"Saved" / L"SaveGames", ec)) { src = p / L"ProjectPBZ" / L"Saved" / L"SaveGames"; break; }
        const auto run = RunFile();
        if (src.empty() || run.empty()) { *where = "找不到游戏存档目录"; return false; }
        SYSTEMTIME t; GetLocalTime(&t);
        const std::wstring stamp = std::format(L"{:04}{:02}{:02}-{:02}{:02}{:02}-real", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        // 首选 %LOCALAPPDATA%\PBZTower\save-backup；不行再用游戏 Saved\PBZTower-backup
        const fs::path roots[] = {fs::path(run).parent_path() / L"save-backup", src.parent_path() / L"PBZTower-backup"};
        std::string errors;
        for (const auto& root : roots) {
            const fs::path dst = root / stamp;
            std::string why; int files = 0;
            // 备份的是磁盘上的真档：复制时绕开沙盒改道（否则沙盒开着时读到的是影子档，2026-10-31 实测主档大小不同）
            towersb::BeginReal();
            const bool ok = CopyTree(src, dst, &why, &files);
            towersb::EndReal();
            if (!ok || files == 0) {
                if (ok) why = "存档目录里没有文件：" + tower::mods::Utf8(src);
                pbz::log::Info("影塔存档备份：{} 失败（{}）", tower::mods::Utf8(root), why);
                errors += (errors.empty() ? "" : "；") + why;
                std::error_code e3; fs::remove_all(dst, e3);
                continue;
            }
            std::vector<fs::path> all;
            for (auto& e : fs::directory_iterator(root, ec)) if (e.is_directory()) all.push_back(e.path());
            std::sort(all.begin(), all.end());
            for (size_t i = 0; i + 5 < all.size(); ++i) fs::remove_all(all[i], ec);
            pbz::log::Info("影塔存档备份：{} 个文件 → {}", files, tower::mods::Utf8(dst));
            *where = tower::mods::Utf8(dst);
            return true;
        }
        *where = "复制失败：" + errors;
        return false;
    }

    void Wear(const std::string& id) {
        if (run_.phase != tower::Phase::Ready) { Broadcast("err.weapon_phase", "开层前才能换兵器"); return; }
        int drop = -1;
        for (int w : run_.weapons) if (tower::kWeaponDrops[w].weapon == id) drop = w;
        if (drop < 0) { Broadcast("err.weapon_unknown", "本局没有得到这把兵器"); return; }
        const auto& d = tower::kWeaponDrops[drop];
        UObject* bag = Bag();
        if (!bag) { Broadcast("err.weapon_unavailable", "塔内换兵器暂时不可用"); return; }
        const uint64_t name = ToName(std::string(d.weapon));
        if (!name || !Owns(bag, name, !d.shaqi)) {
            Broadcast("err.weapon_not_owned", std::string(d.name) + " 你在游戏里还没有：塔内只能换上已拥有的兵器（不往游戏背包里加东西）", std::format(R"("weapon":"{}")", J(d.weapon)));
            return;
        }
        if (!pushed_ && HasSnapshot(bag)) { Broadcast("err.weapon_busy", "游戏正处于临时装备状态（剧情 / 试炼），现在不能换"); return; }
        if (!backedUp_) {
            std::string where;
            if (!BackupSave(&where)) { Broadcast("err.backup_failed", "存档备份失败，未换兵器：" + where); return; }
            backedUp_ = true;
            pbz::log::Info("影塔兵器：已备份游戏存档 → {}", where);
        }
        if (!pushed_) {  // 进塔前的装备：之后每次换都从它出发
            origMain_ = Names(bag, offMain_); origShaqi_ = Names(bag, offShaqi_);
            origMainIdx_ = ue::Get<int32_t>(bag + offMainIdx_); origShaqiIdx_ = ue::Get<int32_t>(bag + offShaqiIdx_);
        }
        (d.shaqi ? run_.wearShaqi : run_.wearMain) = drop;
        auto main = origMain_, shaqi = origShaqi_;
        int mainIdx = origMainIdx_, shaqiIdx = origShaqiIdx_;
        if (run_.wearMain >= 0) mainIdx = Place(main, origMainIdx_, ToName(std::string(tower::kWeaponDrops[run_.wearMain].weapon)));
        if (run_.wearShaqi >= 0) shaqiIdx = Place(shaqi, origShaqiIdx_, ToName(std::string(tower::kWeaponDrops[run_.wearShaqi].weapon)));
        if (pushed_ && HasSnapshot(bag)) pe::Call(bag, popFn_, nullptr);  // 先换回原样再换新的，快照始终是进塔前的
        std::vector<uint8_t> buf(0x80, 0);
        FArr a{main.data(), static_cast<int32_t>(main.size()), static_cast<int32_t>(main.size())};
        FArr b{shaqi.data(), static_cast<int32_t>(shaqi.size()), static_cast<int32_t>(shaqi.size())};
        std::memcpy(Arg(pushFn_, buf, "NewMainWeapons"), &a, sizeof a);
        std::memcpy(Arg(pushFn_, buf, "NewShaqiWeapons"), &b, sizeof b);
        std::memcpy(Arg(pushFn_, buf, "DesiredMainIndex"), &mainIdx, 4);
        std::memcpy(Arg(pushFn_, buf, "DesiredShaqiIndex"), &shaqiIdx, 4);
        pe::Call(bag, pushFn_, buf.data());
        pushed_ = HasSnapshot(bag);
        if (!pushed_) { run_.wearMain = run_.wearShaqi = -1; Broadcast("err.weapon_failed", "换兵器没有生效"); pbz::log::Info("影塔兵器：Push 后没有快照，换装失败"); return; }
        std::ofstream(LoadoutFile(), std::ios::binary | std::ios::trunc) << run_.id;
        pbz::log::Info("影塔兵器：换上 {}（{}，主 {} 格 / 杀气 {} 格）", d.name, d.shaqi ? "杀气兵器" : "主兵器", main.size(), shaqi.size());
        Broadcast("info.weapon_on", "已换上：" + std::string(d.name), std::format(R"("weapon":"{}")", J(d.weapon)));
    }
    void Unwear(const char* why) {
        run_.wearMain = run_.wearShaqi = -1;
        if (!pushed_) return;
        pushed_ = false;
        if (UObject* bag = Bag(); bag && HasSnapshot(bag)) pe::Call(bag, popFn_, nullptr);
        DeleteFileW(LoadoutFile().c_str());
        pbz::log::Info("影塔兵器：换回进塔前的兵器（{}）", why);
    }

    // ---- 兵器领取账本 %LOCALAPPDATA%\PBZTower\ledger.txt：每行 局号\t兵器ID\tpending|granted|owned ----
    // 掉落即记 pending（通关 / 失败 / 退出游戏都不丢），写进存档成功才改 granted，已拥有记 owned（M1 不折算）
    struct Entry { std::string run, weapon, state; };
    static std::wstring LedgerFile() { auto f = RunFile(); return f.empty() ? f : f.substr(0, f.rfind(L'\\')) + L"\\ledger.txt"; }
    std::vector<Entry> LedgerLoad() {
        std::vector<Entry> out;
        std::ifstream f(LedgerFile(), std::ios::binary);
        std::string ln;
        while (std::getline(f, ln)) {
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            const auto t1 = ln.find('\t'), t2 = ln.find('\t', t1 + 1);
            if (t1 == std::string::npos || t2 == std::string::npos) continue;
            out.push_back({ln.substr(0, t1), ln.substr(t1 + 1, t2 - t1 - 1), ln.substr(t2 + 1)});
        }
        return out;
    }
    void LedgerSave(const std::vector<Entry>& es) {
        std::string data;
        for (auto& e : es) data += e.run + '\t' + e.weapon + '\t' + e.state + '\n';
        if (!AtomicTowerFile(LedgerFile(), data)) pbz::log::Info("影塔存档：兵器账本写入失败，保留上一次的完整版本");
    }
    void LedgerAdd() {
        if (run_.last.weapons.empty()) return;
        auto es = LedgerLoad();
        for (int w : run_.last.weapons) {
            const std::string id(tower::kWeaponDrops[w].weapon);
            if (std::none_of(es.begin(), es.end(), [&](const Entry& e) { return e.run == run_.id && e.weapon == id; })) es.push_back({run_.id, id, "pending"});
        }
        LedgerSave(es);
    }
    bool Granted(std::string_view weapon) {
        for (auto& e : LedgerLoad()) if (e.run == run_.id && e.weapon == weapon && e.state != "pending") return true;
        return false;
    }

    // 兵器写进存档：前端先完整备份并校验，再带 backup_ok:true（以及备份时读到的背包 session）调用；session 不一致说明换了存档，拒绝
    void Grant(bool backupOk, const std::string& expectSession) {
        // 用户 2026-10-30：塔内所得只在塔内有效，不带出塔、不写游戏存档（塔和游戏分开存）。账本只作塔内记录
        (void)backupOk; (void)expectSession;
        Broadcast("err.grant_disabled", "影塔的所得只在塔内有效，不带出塔");
    }
    void GrantToSaveDisabled(bool backupOk, const std::string& expectSession) {  // 原写存档实现，保留备查，不再调用
        if (!backupOk) { Broadcast("err.backup_required", "写存档前请先备份存档（前端备份后带 backup_ok:true）"); return; }
        auto es = LedgerLoad();
        if (std::none_of(es.begin(), es.end(), [](const Entry& e) { return e.state == "pending"; })) { Broadcast("err.nothing_to_grant", "没有待领取的兵器"); return; }
        const std::string list = pbz::inv::List();
        const auto sp = list.find("\"session\":\"");
        if (list.find("\"ok\":true") == std::string::npos || sp == std::string::npos) { Broadcast("err.inventory", "读不到背包，稍后再试"); return; }
        const std::string session = list.substr(sp + 11, list.find('"', sp + 11) - sp - 11);
        if (!expectSession.empty() && expectSession != session) { Broadcast("err.save_changed", "存档已变化（备份之后换过档），请重新备份"); return; }
        const auto op = list.find("\"owned\":[");
        const std::string owned = op == std::string::npos ? "" : list.substr(op, list.find(']', op) - op);
        std::vector<std::string> added, have, failed;
        for (auto& e : es) {
            if (e.state != "pending") continue;
            const auto* d = std::find_if(std::begin(tower::kWeaponDrops), std::end(tower::kWeaponDrops), [&](const tower::WeaponDrop& x) { return x.weapon == e.weapon; });
            if (d == std::end(tower::kWeaponDrops)) continue;
            if (owned.find("\"" + e.weapon + "\"") != std::string::npos) { e.state = "owned"; have.push_back(e.weapon); continue; }
            const std::string r = pbz::inv::Apply(e.weapon, d->shaqi ? "shaqi_weapon" : "weapon", 1, session);
            if (r.find("\"ok\":true") != std::string::npos) { e.state = "granted"; added.push_back(e.weapon); }
            else { failed.push_back(e.weapon); pbz::log::Info("影塔发放兵器失败：{} {}", e.weapon, r.size() > 200 ? r.substr(0, 200) : r); }
        }
        LedgerSave(es);
        auto ids = [](const std::vector<std::string>& v) { std::string o; for (auto& x : v) o += (o.empty() ? "\"" : ",\"") + J(x) + "\""; return "[" + o + "]"; };
        auto names = [](const std::vector<std::string>& v) {
            std::string o;
            for (auto& x : v)
                for (auto& d : tower::kWeaponDrops) if (d.weapon == x) { o += (o.empty() ? "" : "、") + std::string(d.name); break; }
            return o.empty() ? std::string("无") : o;
        };
        pbz::log::Info("影塔兵器入存档：新增 [{}] 已有 [{}] 失败 [{}]", names(added), names(have), names(failed));
        Broadcast(failed.empty() ? "info.granted" : "err.grant_partial",
                  std::format("兵器入存档：新增 {}；已有 {}；失败 {}", names(added), names(have), names(failed)),
                  std::format(R"("added":{},"owned":{},"failed":{},"complete":{})", ids(added), ids(have), ids(failed), failed.empty()));
    }

    // 暂离：保存后结束本次（之后 resume 继续）；战斗中暂离则退回开层前，本层 Boss 清掉
    void Suspend() {
        if (run_.phase == tower::Phase::Idle || run_.phase == tower::Phase::Won || run_.phase == tower::Phase::Lost) { Broadcast("err.nothing_to_suspend", "没有进行中的局"); return; }
        if (run_.phase == tower::Phase::Fighting) { Clear("suspend"); actors_.clear(); run_.UndoFloor(); }
        ReleaseShentong();
        towermusic::Stop("保存并退出");
        SaveRun();
        pbz::log::Info("影塔暂离：第 {}/{} 层", run_.floor, run_.floors);
        run_ = {}; controller_.clear();
        s_hidden = false;  // 第 51 层局保存在隐藏层文件里；之后的"继续上一局"指正常局
        if (End() == 0) Broadcast("info.suspended", "已暂离，可以继续上一局");  // 塔档会话：End 已广播出塔受理（exit_reload / exit_pending）或 err.exit_failed
    }

    // ---- 局存档 ----
    // 第 51 层（v14）：从入口进的局用单独的文件（hidden-run / hidden-growth），不覆盖正常局的存档；
    // 解锁时的快照另存 hidden51-*.txt，每次从入口进都从这份快照出发
    static inline bool s_hidden = false;
    static inline int s_lastDiff = 0;  // 最近一局的难度（死亡 / 结算后"重新开始"沿用）
    static std::wstring TowerDir() {
        wchar_t base[MAX_PATH];
        if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return {};
        std::wstring dir = std::wstring(base) + L"\\PBZTower";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir;
    }
    uint64_t importedSeedInFlight_ = 0;
    int importedSeedDifficulty_ = 0;
    static std::wstring RunFile() { const auto d = TowerDir(); return d.empty() ? d : d + (s_hidden ? L"\\hidden-run.txt" : L"\\run.txt"); }
    static std::wstring PlanFile() { const auto d = TowerDir(); return d.empty() ? d : d + L"\\audit-plan.txt"; }
    static std::wstring GrowthFile() { const auto d = TowerDir(); return d.empty() ? d : d + (s_hidden ? L"\\hidden-growth.txt" : L"\\growth.txt"); }
    static std::wstring HiddenSnapRun() { const auto d = TowerDir(); return d.empty() ? d : d + L"\\hidden51-run.txt"; }
    static std::wstring HiddenSnapGrowth() { const auto d = TowerDir(); return d.empty() ? d : d + L"\\hidden51-growth.txt"; }
    bool SeedSaveVerified() const {
        std::ifstream rf(RunFile(), std::ios::binary), gf(GrowthFile(), std::ios::binary);
        if (!rf || !gf) return false;
        std::stringstream rs, gs; rs << rf.rdbuf(); gs << gf.rdbuf();
        tower::Run saved; tower::growthrun::Runner growth;
        return saved.Load(rs.str()) && !saved.legacy && saved.id == run_.id && saved.seed == importedSeedInFlight_ &&
               saved.floor == 0 && saved.difficulty == importedSeedDifficulty_ && tower::growthrun::Runner::Load(gs.str(), &growth);
    }
  public:
    // 用户 2026-11-12：第 51 隐藏层去掉，改为通关后接无尽模式；旧快照文件保留不删，入口不再显示
    static bool HiddenUnlocked() { return false; }
  private:
    // 第 50 层通关、本轮额外·限时 ≥2 且额外·禁药 ≥2：存解锁快照（领完第 50 层奖励之后的状态），本轮"下一层"直接开第 51 层
    // 用户 2026-11-05：通过第 50 层（领完奖励）都要显示通关和结算；开了第 51 层的结算页多一个"下一层"。第 51 层打完同样结算。
    // 返回 true = 本轮已结束（不再存局）
    bool TowerCleared() {
        if (run_.phase != tower::Phase::Ready || run_.floor < 50 || run_.floors < 50) return false;
        if (run_.endless) {  // 无尽模式：第 50 层停在通关结算页（带下一层）；之后的无尽层不再结算，直接回层间
            if (run_.floor == 50) {
                settlement_ = Settlement(Player(), true);
                pbz::log::Info("影塔：通关 50 层，结算页带下一层（无尽模式）");
                Broadcast("info.won", "通关");
            }
            return false;
        }
        if (!run_.hidden && run_.floor < run_.floors) {  // 已开第 51 层：停在层间，界面按结算页显示（带下一层）
            const int keep = run_.floors; run_.floors = 50;
            settlement_ = Settlement(Player(), true);
            run_.floors = keep;
            pbz::log::Info("影塔：通关 50 层，已开启第 51 层（结算页带下一层）");
            Broadcast("info.won", "通关");
            return false;
        }
        Win();
        return true;
    }
    void Win() {
        run_.phase = tower::Phase::Won;
        RunLog(std::format("won\t{}", run_.floor));
        settlement_ = Settlement(Player(), true);
        DeleteRun();
        pbz::log::Info("影塔：通关第 {} 层，结算", run_.floor);
        Broadcast("info.won", "通关");
        Conclude();
    }
    void HiddenUnlockCheck() {
        // 第 50 层（显示 99 左殇）领完奖励：开启无尽模式（取代旧的第 51 隐藏层解锁；旧局已开的第 51 层同样转成无尽）
        if (!run_.hidden && !run_.endless && run_.floors >= 50 && run_.floor == 50 && run_.phase == tower::Phase::Ready && grow_.on) {
            run_.endless = true; run_.floors = 50;
            pbz::log::Info("影塔：通关第 50 层（显示第 99 层），开启无尽模式");
            Broadcast("info.endless_unlocked", "无尽模式已开启");
        }

    }
    // 从入口进第 51 层：复制解锁快照到隐藏层工作档，再按"继续上一局"进塔
    void EnterHidden() {
        if (!HiddenUnlocked()) { Broadcast("err.hidden_locked", "第 100 层尚未开启"); return; }
        if (run_.phase != tower::Phase::Idle) { Broadcast("err.hidden_busy", "请先保存并退出当前一轮"); return; }
        s_hidden = true;
        if (!CopyFileW(HiddenSnapRun().c_str(), RunFile().c_str(), FALSE) || !CopyFileW(HiddenSnapGrowth().c_str(), GrowthFile().c_str(), FALSE)) {
            s_hidden = false; Broadcast("err.hidden_copy", "第 100 层存档读取失败"); return;
        }
        pbz::log::Info("影塔第 51 层：从入口进入（使用解锁时的快照）");
        Resume();
    }
    static bool AtomicTowerFile(const std::wstring& path, const std::string& data) {
        if (path.empty()) return false;
        const auto temp = path + L".hex-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        { std::ofstream f(temp, std::ios::binary | std::ios::trunc); f << data; f.flush(); if (!f) { f.close(); DeleteFileW(temp.c_str()); return false; } }
        // 外部程序（安装器 / 记录窗口）读文件时不带删除共享，替换会短暂失败：最多重试 3 次
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
            Sleep(15);
        }
        DeleteFileW(temp.c_str()); return false;
    }
    bool SaveHex() {
        return grow_.on && trip_.phase() == tower::trip::Phase::Inside && AtomicTowerFile(GrowthFile(), grow_.Save());
    }
    bool FinishHexJournal() {
        if (!grow_.hexJournal.active) return true;
        run_.jade = grow_.hexJournal.jade; run_.items = grow_.hexJournal.items;
        if (!AtomicTowerFile(RunFile(), run_.Save())) return false;
        const auto journal = grow_.hexJournal; grow_.hexJournal = {};
        if (!SaveHex()) { grow_.hexJournal = journal; return false; }
        return true;
    }
    void SaveRun() {
        if (run_.phase != tower::Phase::Ready && run_.phase != tower::Phase::Reward) return;
        if (const auto g = GrowthFile(); !g.empty()) {
            if (grow_.on && trip_.phase() == tower::trip::Phase::Inside) {  // 记下实际装备（继续上一局按它还原，不回起手配置）
                const auto l = towernative::ReadLoadout();
                if (l.ok && !l.main.empty()) {
                    auto join = [](const std::vector<std::string>& v) { std::string o; for (auto& x : v) o += (o.empty() ? "" : ",") + (x.empty() ? std::string("None") : x); return o; };
                    grow_.equip = join(l.main) + "|" + join(l.shaqi) + "|" + (l.medBottom.empty() ? "None" : l.medBottom) + "," + (l.medTop.empty() ? "None" : l.medTop) + "|" + join(l.accessories);
                }
            }
            // 2026-11-11：原子写（临时文件 + 替换）。直接截断重写时中途崩溃会留下残缺成长档，续档时整局作废
            if (grow_.on) { if (!AtomicTowerFile(g, grow_.Save())) pbz::log::Info("影塔存档：成长档写入失败，保留上一次的完整版本"); }
            else DeleteFileW(g.c_str());
        }
        const auto path = RunFile();
        if (path.empty()) return;
        if (!AtomicTowerFile(path, run_.Save())) pbz::log::Info("影塔存档：局档写入失败，保留上一次的完整版本");
    }
    // ---- 3.5.0 无尽存档与爬塔局分开存放 ----
    // 当前局 = run.txt / growth.txt（「继续上一局」照旧读它）。不在玩的那一局停放在 endless-* 或 climb-*；
    // 开局菜单选哪条线，就把另一条挪开、把要玩的换进来，再走原来的继续流程进塔
    static std::wstring SlotPath(const wchar_t* name) { const auto d = TowerDir(); return d.empty() ? d : d + L"\\" + name; }
    static bool FileThere(const std::wstring& p) { return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
    static bool ReadRunAt(const std::wstring& p, tower::Run* r) { const auto t = ReadText(p); return !t.empty() && r->Load(t); }
    static int ActiveKind() {  // 0 无 1 爬塔 2 无尽存档
        if (s_hidden) return 0;
        tower::Run r;
        return ReadRunAt(RunFile(), &r) ? (r.endlessSave ? 2 : 1) : 0;
    }
    static bool MovePair(const std::wstring& r1, const std::wstring& g1, const std::wstring& r2, const std::wstring& g2) {
        if (!MoveFileExW(r1.c_str(), r2.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) return false;
        if (FileThere(g1)) return MoveFileExW(g1.c_str(), g2.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0;
        DeleteFileW(g2.c_str());
        return true;
    }
    static bool ParkActive() {
        const int k = ActiveKind();
        if (k == 0) return true;
        const bool ok = k == 2 ? MovePair(RunFile(), GrowthFile(), SlotPath(L"endless-run.txt"), SlotPath(L"endless-growth.txt"))
                               : MovePair(RunFile(), GrowthFile(), SlotPath(L"climb-run.txt"), SlotPath(L"climb-growth.txt"));
        pbz::log::Info("影塔存档位：当前{}挪到{}存档位{}", k == 2 ? "无尽存档" : "爬塔局", k == 2 ? "无尽" : "爬塔", ok ? "" : "失败");
        return ok;
    }
    // 把要玩的一条线换到当前位置：endless = 无尽存档，否则爬塔局。换不进来（没有）返回 false
    static bool SwapIn(bool endless) {
        const int want = endless ? 2 : 1, k = ActiveKind();
        if (k == want) return true;
        const auto rs = SlotPath(endless ? L"endless-run.txt" : L"climb-run.txt"), gs = SlotPath(endless ? L"endless-growth.txt" : L"climb-growth.txt");
        if (!FileThere(rs)) return false;
        if (!ParkActive()) return false;
        const bool ok = MovePair(rs, gs, RunFile(), GrowthFile());
        pbz::log::Info("影塔存档位：{}换到当前位置{}", endless ? "无尽存档" : "爬塔局", ok ? "" : "失败");
        return ok;
    }
  public:
    static bool ClimbAvailable() { return ActiveKind() == 1 || FileThere(SlotPath(L"climb-run.txt")); }
    // 无尽存档的下一层（显示层），没有返回 0
    static int EndlessNext() {
        tower::Run r;
        if (ActiveKind() == 2) ReadRunAt(RunFile(), &r);
        else if (!ReadRunAt(SlotPath(L"endless-run.txt"), &r)) return 0;
        return r.endlessSave ? tower::floorplay::ShownBoss(r.floor + 1, 50) : 0;
    }
    static bool EndlessImportInfo(tower::endlessshare::Share* out) {
        std::string why;
        return tower::endlessshare::Parse(ReadText(SlotPath(L"endless-import.txt")), out, &why);
    }
  private:
    // 导入的无尽档 → 新开一局无尽存档：数据照原作者，局号 / 种子 / 随机流重新生成，驿站与待领奖励清空
    bool StartFromEndlessImport(std::string* why) {
        tower::endlessshare::Share sh;
        if (!tower::endlessshare::Parse(ReadText(SlotPath(L"endless-import.txt")), &sh, why)) return false;
        tower::Run r; tower::growthrun::Runner g;
        if (!r.Load(sh.runText) || !tower::growthrun::Runner::Load(sh.growthText, &g)) { *why = "局档或成长档内容无效（可能来自不兼容的版本）"; return false; }
        if (!r.endless || r.floor < 50 || r.legacy) { *why = "不是无尽存档点"; return false; }
        const uint64_t seed = (GetTickCount64() * 0x9E3779B97F4A7C15ull) ^ 0xE5D1E55ull ^ r.seed;
        r.seed = seed ? seed : 1; r.rng = tower::Rng(r.seed);
        r.id = std::format("{:016x}", r.seed ^ (GetTickCount64() * 0xD1B54A32D192ED03ull));
        r.endlessSave = true; r.difficulty = sh.difficulty; r.phase = tower::Phase::Ready;
        r.source = (sh.author.empty() ? std::string("佚名") : sh.author) + "|" + std::to_string(sh.shown);
        g.hexes.rng = (r.seed ^ 0xF1BBCDCBFA53E0A9ull) | 1ull; g.hexes.pending = 0; g.hexes.offers = {-1, -1, -1}; g.hexes.rerolled = 0;
        g.shop = {}; g.offers = {}; g.offered = false; g.bonus.clear(); g.tradeVisit = {}; g.hexJournal = {}; g.held.clear(); g.backpackSynced = true;
        g.draws = 0; g.med.clear(); g.acc.clear();
        std::erase_if(g.profile.weapons, [](const tower::growth::OwnedWeapon& w) { return tower::rewards::RetiredWeapon(w.index); });  // 废案兵器不带进来
        // 当前位置：无尽存档被覆盖（玩家已确认），爬塔局挪去停放
        if (ActiveKind() == 2) DeleteRun(); else if (!ParkActive()) { *why = "爬塔局挪位失败"; return false; }
        for (const wchar_t* n : {L"endless-run.txt", L"endless-growth.txt"}) DeleteFileW(SlotPath(n).c_str());
        if (!AtomicTowerFile(RunFile(), r.Save()) || !AtomicTowerFile(GrowthFile(), g.Save())) { *why = "写入局存档失败"; return false; }
        DeleteFileW(SlotPath(L"endless-import.txt").c_str());
        pbz::log::Info("影塔无尽档：导入 {} 的第 {} 层无尽档开局（新局 {}，{}），种子重新生成", sh.author.empty() ? "佚名" : sh.author, sh.shown, r.id, sh.difficulty ? "简单" : "困难");
        return true;
    }
    void DeleteRun() {
        if (auto path = RunFile(); !path.empty()) DeleteFileW(path.c_str());
        if (auto path = GrowthFile(); !path.empty()) DeleteFileW(path.c_str());
    }
    void Resume() {
        if (!available) { Broadcast("err.not_ready", "影塔还没就绪：" + reason); return; }
        RetireLegacyRuns();
        const auto path = RunFile();
        std::ifstream f(path, std::ios::binary);
        std::stringstream ss; ss << f.rdbuf();
        tower::Run r;
        if (!f || !r.Load(ss.str())) { Broadcast("err.no_saved_run", "没有可继续的局"); return; }
        if (r.legacy) { Broadcast("err.no_saved_run", "旧版爬塔进度不能继续，请开始新局"); return; }
        if (r.plan != tower::kPlanVersion) {  // 旧方案的局（v5 之前存的）：已过的层、用时、成长原样保留；下一层起按当前方案的逐层表开
            pbz::log::Info("影塔续局：局存档按方案 v{} 记录（第 {} 层 {}），按当前方案 v{} 开第 {} 层；已过的层、用时、纪录、成长保留，v4 的 1v2 / 混战 / 1v3 层不再沿用",
                           r.plan == 0 ? 4 : r.plan, r.floor, r.phase == tower::Phase::Reward ? "reward" : "ready", tower::kPlanVersion, r.floor + 1);
            r.plan = tower::kPlanVersion;
        }
        if (run_.phase != tower::Phase::Idle) { Clear("resume"); ReleaseShentong(); Unwear("resume"); }
        if (trip_.phase() != tower::trip::Phase::Outside) { Broadcast("err.exit_pending", TripBusyText()); return; }
        grow_ = {};
        grow_.difficulty = r.difficulty;
        baseAtk_ = baseHp_ = 0;
        growHpComp_ = growDmgComp_ = 1;
        {
            std::ifstream gf(GrowthFile(), std::ios::binary);
            std::stringstream gs; gs << gf.rdbuf();
            tower::growthrun::Runner saved;
            if (gf && tower::growthrun::Runner::Load(gs.str(), &saved)) {
                if (saved.hexJournal.active) {
                    r.jade = saved.hexJournal.jade; r.items = saved.hexJournal.items;
                    if (!AtomicTowerFile(RunFile(), r.Save())) { Broadcast("err.hex_save", "恢复奖励交易失败，请重试"); return; }
                    saved.hexJournal = {};
                    if (!AtomicTowerFile(GrowthFile(), saved.Save())) { Broadcast("err.hex_save", "完成奖励交易失败，请重试"); return; }
                }
                trip_.SetAuto(resumeAuto_);
                const auto equip = tower::save::SavedEquip(saved.profile, saved.equip);
                if (!saved.equip.empty()) pbz::log::Info("影塔继续：按保存时的装备还原（{}）", saved.equip);
                if (!trip_.Enter(Io(), saved.profile, equip, Stamp())) { Broadcast("err.growth_enter", "继续失败，游戏未改动：" + trip_.error()); return; }
                AdoptGrowth(saved);
                if (grow_.hexes.ReplaceRetiredOffer(HexDrawMask()))
                    pbz::log::Info("影塔海克斯：续档已替换退役候选，保留其余选项");
                grow_.catalog = LoadCatalog();
                if (const size_t before = grow_.med.size() + grow_.acc.size(); (grow_.Readmit(r.floor, r.floors), grow_.med.size() + grow_.acc.size()) != before)
                    pbz::log::Info("影塔装备进度表：读回的待选药匣 / 饰品匣里有合同未批准的项，已去掉（剩药 {} 饰品 {}）", grow_.med.size(), grow_.acc.size());
                SaveTrip();
                pbz::log::Info("影塔独立塔档：继续上一局，塔内存档已放进内存，淬炼 {} 修为 {}", grow_.profile.temper, grow_.profile.cultivate);
            } else {
                // 2026-11-05 玩家反馈"进塔还是原来的血量"：成长档缺失 / 旧版本格式读不出时，原来会跳过进塔档、直接在原档上继续。
                // 独立塔档是硬规则：读不到塔档就不继续（原档不被碰），请玩家开新局
                pbz::log::Info("影塔继续：塔档数据（{}）缺失或版本不兼容，拒绝在原档上继续", gf ? "格式不兼容" : "文件不存在");
                Broadcast("err.no_tower_save", "这一局的塔档数据缺失或来自旧版本，无法继续。请开新局（原存档未改动）");
                return;
            }
        }
        run_ = r; actors_.clear(); s_lastDiff = run_.difficulty; deathShown_ = 0; rebirthReload_ = false; lastBossVenue_ = -1;
        grow_.RetirePendingWeapons(run_.floor, run_.floors, run_.seed, run_.items);
        if (tower::rewards::RepriceShelf(grow_.shop, grow_.Ctx(grow_.shop.floor, run_.floors, run_.items)))
            pbz::log::Info("影塔驿站：续档已更新报价，货架与购买记录保留");
        // Recover the last single-Boss venue after save/exit, so a resumed
        // wave can use that Boss's arena rather than silently changing maps.
        if (run_.floor > 0 && run_.floor <= 50 && run_.clearedRoute[run_.floor].size() == 1 &&
            tower::floorplay::ScheduledFloor(run_.seed, run_.floor, run_.shuffleBosses).replay.empty()) {
            const int previousBoss = FindBoss(run_.clearedRoute[run_.floor][0]);
            if (previousBoss >= 0 && towertravel::HasVenue(tower::kBossPool[previousBoss].path, tower::kBossPool[previousBoss].identity)) {
                lastBossVenue_ = previousBoss;
                pbz::log::Info("影塔续局：下一段波次优先使用已击败 Boss {} 的场地", tower::kBossPool[previousBoss].name);
            }
        }
        run_.peak = std::max(run_.peak, run_.floor);  // 旧存档没有这个字段
        // 池契约：局存档记下开局用的池（pool=audit / verified），续局沿用；续局命令显式写 pool 时以命令为准并记日志。
        // 审计池只放宽"未实机验证"这一条，禁用名单（boss-denied）照样生效；正式池的验证白名单不被续局改写
        poolAudit_ = resumePool_.empty() ? run_.audit : resumePool_ == "audit";
        if (!resumePool_.empty() && poolAudit_ != run_.audit) pbz::log::Info("影塔续局：命令指定 {} 池，局存档记的是 {} 池，按命令", poolAudit_ ? "审计" : "正式", run_.audit ? "审计" : "正式");
        run_.audit = poolAudit_;
        pbz::log::Info("影塔续局：Boss 池 {}（{}）", poolAudit_ ? "审计（开发验收，禁用名单仍生效）" : "正式（只放实机验证过的）", resumePool_.empty() ? "沿用局存档" : "续局命令指定");
        resumePool_.clear();
        LoadAdmission();
        // 3.2.1（用户：旧档更新后新货要无缝上架）：读档时驿站还开着，按新版规则补货（上架数、珍品位、常驻抗异常饰品）；
        // 同一次驿站的抽选键不变，原来在架的仍在、已买的不补
        if (grow_.shop.Open()) {
            const size_t before = grow_.shop.shelf.size();
            grow_.Restock(run_.floor, run_.floors, run_.items, ShopSupplement());
            if (grow_.shop.shelf.size() != before) pbz::log::Info("影塔驿站：续档按新版规则补货（{} → {} 件）", before, grow_.shop.shelf.size());
        }
        plan_.clear();
        if (const auto saved = ReadText(PlanFile()); !saved.empty()) {  // 审计计划续局：只认同一局号
            const auto nl = saved.find('\n');
            std::string why;
            if (nl != std::string::npos && saved.substr(0, nl) == run_.id) {
                if (tower::plan::Parse(saved.substr(nl + 1), Pool(), &plan_, &why) && static_cast<int>(plan_.size()) == run_.floors)
                    pbz::log::Info("影塔续局：恢复审计计划 {} 层", plan_.size());
                else { plan_.clear(); pbz::log::Info("影塔续局：审计计划无法恢复（{}），本局按随机层继续", why.empty() ? "层数不符" : why); }
            }
        }
        chal_ = tower::floorplay::Challenges(run_.seed, run_.floors, run_.difficulty, run_.shuffleBosses);
        // 旧版本存下的待领取（旧十二卡）：成长模式按新模型重抽一组（旧持有物不动）
        if (grow_.on || !grow_.profile.weapons.empty())
            if (run_.phase == tower::Phase::Reward && !grow_.OffersFor(run_.floor)) grow_.DrawRewards(run_.floor, run_.floors, run_.seed, run_.items);  // 读档：本层已抽的一组原样保留；层号不对才重抽（不白给刷新）
        controller_ = resumeClient_;
        clampHp_ = true;
        backedUp_ = false;
        pbz::HoldFeature(this, "tower");
        pbz::log::Info("影塔继续：第 {}/{} 层，种子 {}", run_.floor, run_.floors, run_.seed);
        if (run_.phase == tower::Phase::Ready && SettleBill("读档补结") > 0) SaveRun();  // 3.6.0：奖励页已选完但账单没结（旧进程中途退出）
        if (trip_.phase() == tower::trip::Phase::Entering) Broadcast("info.enter_reload", EnterText());
        else Broadcast("info.resumed", "已继续上一局");
    }

    static std::string OfferArgs(const tower::Offer& o) {
        switch (o.kind) {
            case tower::OfferKind::Card: return o.index >= 0 ? std::format(R"("kind":"card","id":"{}")", J(tower::kCards[o.index].id)) : "";
            case tower::OfferKind::Item: return o.index >= 0 ? std::format(R"("kind":"item","id":"{}")", J(tower::kItems[o.index].id)) : "";
            case tower::OfferKind::Jade: return std::format(R"("kind":"jade","amount":{})", o.amount);
        }
        return "";
    }
    static std::string OfferText(const tower::Offer& o) {
        switch (o.kind) {
            case tower::OfferKind::Card: return o.index >= 0 ? std::string(tower::kCards[o.index].name) : "";
            case tower::OfferKind::Item: return o.index >= 0 ? std::string(tower::kItems[o.index].name) : "";
            case tower::OfferKind::Jade: return std::format("魂玉 {}", o.amount);
        }
        return "";
    }

    // 状态广播：code 是稳定的消息代码（err.* 表示失败），msg 是中文文案（前端按 code + args 走本地词典），args 是 JSON 对象内容
    void Broadcast(const std::string& code, const std::string& msg, const std::string& args = "") {
        lastCode_ = code;
        // 用户 2026-11-06（第 8 层"点下一层无事发生"）：广播只发管道，发布版没有调试面板，开不了层的原因玩家看不到。
        // err.* 记下来给游戏内界面弹原生确认框；本层取消（entry_failed）由准备菜单顶部的说明显示，管道同步类不弹
        if (code.rfind("err.", 0) == 0 && !msg.empty() && code != "err.entry_failed" && code != "err.not_controller" &&
            code != "err.stale_run" && code != "err.stale_revision") {
            ++errSeq_; errMsg_ = msg;
            pbz::log::Info("影塔提示玩家（{}）：{}", code, msg);
        }
        ++rev_;
        auto comma = [](std::string& to) { if (!to.empty()) to += ","; };
        std::string offer;
        if (!grow_.on) for (auto& o : run_.offer) {  // 成长模式的旧 Run::offer 不是真奖励（313987b：state 仍报铁骨 / 魂玉槽位），只发 growth.offers
            if (o.kind != tower::OfferKind::Jade && o.index < 0) continue;
            if (o.kind == tower::OfferKind::Jade && o.amount <= 0) continue;
            comma(offer);
            if (o.kind == tower::OfferKind::Card) {
                const auto& c = tower::kCards[o.index];
                offer += std::format(R"({{"kind":"card","id":"{}","name":"{}","school":"{}","text":"{}","rarity":{}}})", J(c.id), J(c.name), J(c.school), J(c.text), static_cast<int>(c.rarity));
            } else if (o.kind == tower::OfferKind::Item) {
                const auto& d = tower::kItems[o.index];
                offer += std::format(R"({{"kind":"item","id":"{}","name":"{}","text":"{}","count":{}}})", J(d.id), J(d.name), J(d.text), o.amount);
            } else {
                offer += std::format(R"({{"kind":"jade","id":"jade","name":"一袋魂玉","text":"魂玉 +{}","amount":{}}})", o.amount, o.amount);
            }
        }
        std::string picked, items, bag, weapons, gotW, gotS, bosses;
        for (int i : run_.picked) { comma(picked); picked += std::format(R"({{"id":"{}","name":"{}"}})", J(tower::kCards[i].id), J(tower::kCards[i].name)); }
        for (int i = 0; i < tower::kItemCount; ++i)
            if (run_.items[i] > 0) { comma(items); items += std::format(R"({{"id":"{}","name":"{}","text":"{}","count":{}}})", J(tower::kItems[i].id), J(tower::kItems[i].name), J(tower::kItems[i].text), run_.items[i]); }
        for (int i : run_.shentong) { comma(bag); bag += std::format(R"({{"id":"{}","name":"{}","text":"{}","tier":{}}})", J(tower::kShentong[i].id), J(tower::kShentong[i].name), J(tower::kShentong[i].text), static_cast<int>(tower::kShentong[i].tier)); }
        for (int w : run_.weapons) { const auto& d = tower::kWeaponDrops[w]; comma(weapons); weapons += std::format(R"({{"id":"{}","name":"{}","shaqi":{},"verified":{},"granted":{}}})", J(d.weapon), J(d.name), d.shaqi, d.verified, Granted(d.weapon)); }
        for (int w : run_.last.weapons) { comma(gotW); gotW += std::format(R"({{"id":"{}","name":"{}"}})", J(tower::kWeaponDrops[w].weapon), J(tower::kWeaponDrops[w].name)); }
        for (int i : run_.last.shentong) { comma(gotS); gotS += std::format(R"({{"id":"{}","name":"{}"}})", J(tower::kShentong[i].id), J(tower::kShentong[i].name)); }
        std::string boss;
        if (run_.phase != tower::Phase::Idle)
            for (int b : run_.bosses) {
                const auto path = tower::kBossPool[b].path;
                const auto slash = path.rfind('/'), dot = path.rfind('.');
                comma(bosses);
                bosses += std::format(R"({{"id":"{}","path":"{}","name":"{}"}})", J(path.substr(slash + 1, dot - slash - 1)), J(path), J(tower::kBossPool[b].name));
                boss += (boss.empty() ? "" : "、") + std::string(tower::kBossPool[b].name);
            }
        const auto& m = run_.mods;
        pbz::ipc::Send(std::format(
            R"({{"op":"tower","event":"state","proto":2,"run_id":"{}","revision":{},"controller":"{}","phase":"{}","floor":{},"floors":{},"zone":{},"seed":"{}","count":{},)"
            R"("boss":"{}","bosses":[{}],"code":"{}","args":{{{}}},"msg":"{}","offer":[{}],"picked":[{}],)"
            R"("jade":{},"items":[{}],"shentong":[{}],"weapons":[{}],"gains":{{"jade":{},"weapons":[{}],"shentong":[{}]}},)"
            R"("mods":{{"dealt":{:.2f},"taken":{:.2f},"max_hp":{:.2f},"shachi":{:.2f},"speed":{:.2f},"posture":{:.2f},"heal":{:.2f}}},)"
            R"("trip":{{{}}},"growth":{{{}}},)"
            R"("caps":["start","next","pick","use","reroll","st","resume","suspend","stop","probe","state","arena","recover","shop","prep"],)"
            R"("todo":["pause_input","difficulty","records","shop","data_tables","weapon_swap","weapon_convert"]}})",
            J(run_.id), rev_, J(controller_), tower::PhaseName(run_.phase), run_.floor, run_.floors, run_.Zone() + 1, run_.seed, static_cast<int>(run_.bosses.size()),
            J(boss), bosses, J(code), args, J(msg), offer, picked,
            run_.jade, items, bag, weapons, run_.last.jade, gotW, gotS,
            m.DealtMul(), m.TakenMul(), m.MaxHpMul(), m.ShachiMul(), m.SpeedMul(), m.PostureMul(), m.HealFrac(), TripJson(), GrowthJson()));
    }
    std::string TripJson() const {
        return std::format(R"("phase":"{}","step":"{}","txn":{},"auto":{},"failed":{},"pending":{},"error":"{}")", tower::trip::PhaseName(trip_.phase()),
                           tower::trip::StepName(trip_.step()), trip_.txn(), trip_.automatic(), trip_.failed(), trip_.pending(), J(trip_.error()));
    }
    // 成长模式的真实奖励 / 驿站 / 预装（界面与核验按这里的 slot / ver 操作）
    std::string GrowthJson() const {
        if (!grow_.on) return R"("on":false)";
        static constexpr const char* kinds[] = {"blessing", "temper", "cultivate", "weapon", "accessory", "refine", "medicine", "item", "jade", "attr", "soul_token"};  // 和 rewards::Kind 一一对应（v14 漏了 attr：越界读空指针，std::format 抛异常、写崩溃转储，领奖那一帧卡 1.3 秒）
        auto offerJson = [](const tower::rewards::Offer& o) {
            return std::format(R"("kind":"{}","index":{},"amount":{},"quality":{},"label":"{}")", static_cast<size_t>(o.kind) < std::size(kinds) ? kinds[static_cast<int>(o.kind)] : "other", o.index, o.amount, o.quality, J(tower::rewards::Label(o)));
        };
        std::string offers, shelf;
        if (grow_.offered)
            for (int i = 0; i < 3; ++i) offers += std::format(R"({}{{"slot":{},{}}})", i ? "," : "", i, offerJson(grow_.offers[i]));
        const auto& s = grow_.shop;
        for (size_t i = 0; i < s.shelf.size(); ++i)
            shelf += std::format(R"({}{{"slot":{},{},"price":{},"sold":{},"empty":{}}})", i ? "," : "", i, offerJson(s.shelf[i].offer), s.shelf[i].price, s.shelf[i].sold, tower::rewards::Empty(s.shelf[i].offer));
        std::string bless;
        for (int i = 0; i < tower::growth::kBlessCount; ++i)
            if (grow_.bless.stacks[i] > 0) bless += std::format(R"({}{{"name":"{}","stacks":{}}})", bless.empty() ? "" : ",", J(tower::growth::kBlessings[i].name), grow_.bless.stacks[i]);
        return std::format(R"("on":true,"offered":{},"offers":[{}],"shop":{{"open":{},"floor":{},"ver":{},"refreshes":{},"refresh_cost":{},"shelf":[{}]}},"blessings":[{}])",
                           grow_.offered, offers, s.Open(), s.floor, s.version, s.refreshes, tower::rewards::RefreshCost(s.refreshes, s.floor), shelf, bless);
    }

    tower::Run run_;
    std::vector<Actor> actors_;
    std::unordered_map<UObject*, double> cloneNext_;  // 绝影分身：下次核对属性的时刻
    float spikeFloor_ = 1.0f, lastSpikeHp_ = -1.0f;  // 重击截断：本帧写入的生命下限 / 上一帧生命
    int hp_ = -1, hpMax_ = -1, hpMin_ = -1, shachi_ = -1, dilation_ = -1, spawnSeq_ = 0, count_ = 1, pending_ = 0;
#ifdef PBZ_HEX_TEST
    bool arcaneTest_ = false;
    float arcaneChargeMul_ = 1.2f;
    float arcaneLastCharge_ = -1, arcaneLastCount_ = -1; int arcaneSamples_ = 0;
#endif
    int walkSpeed_ = -1, jogSpeed_ = -1, sprintSpeed_ = -1;
    double nextOcean_ = 0;
    int reapPending_ = 0;  // 收割者：本帧新确认死亡的小怪数
    float echoLastUlt_ = -1, echoLastCharge_ = -1; double echoUntil_ = 0;  // 绝技余韵
    int warKills_ = 0, warFloor_ = -1, warLogged_ = 0; double warFightMark_ = -2;               // 3.1.0 以战养战
    bool revengeArmed_ = false, enemyHitSeen_ = false; double revengeUntil_ = 0; float revLastHp_ = -1;  // 3.1.0 以伤换伤
    int echoLastTag_ = -1; bool tagReadDead_ = false;
    // 玩家身上某个 GameplayTag 的计数（ASC.GetGameplayTagCount）；-1 = 读不到
    static int PlayerTagCount(UObject* pawn, const char* tagName) {
        if (!pawn || !ue::Live(pawn)) return -1;
        static int ascOff = -2;
        if (ascOff == -2 || ascOff < 0) ascOff = ue::PropOffset(ue::ClassOf(pawn), "AbilitySystemComponent");
        auto* asc = ascOff >= 0 ? ue::Get<UObject*>(pawn + ascOff) : nullptr;
        if (!asc || !ue::Live(asc)) { static bool told = false; if (!told) { told = true; pbz::log::Info("影塔标签读取：角色上没有能力组件（偏移 {}）", ascOff); } return -1; }
        static UObject* fn = nullptr; static int param = -1, result = -1; static uint64_t tag = 0; static std::string cached;
        if (!fn) { fn = pe::FindFunction(ue::ClassOf(asc), "GetGameplayTagCount"); param = fn ? ue::PropOffset(fn, "GameplayTag") : -1; result = fn ? ue::PropOffset(fn, "ReturnValue") : -1; }
        if (cached != tagName) { cached = tagName; tag = towerwidgets::Name(tagName); }
        if (!tag || param < 0 || param + 8 > 64 || result < 0 || result + 4 > 64) {
            static bool told = false;
            if (!told) { told = true; pbz::log::Info("影塔标签读取：{} 失败（标签名 {}，接口 {}，参数 {}/{}）", tagName, tag ? "有" : "不在名字表", fn ? "有" : "无", param, result); }
            return -1;
        }
        uint8_t args[64] = {}; std::memcpy(args + param, &tag, 8); pe::Call(asc, fn, args);
        const int n = ue::Get<int32_t>(args + result, -1);
        return n >= 0 && n < 65536 ? n : -1;
    }
    int ultCountOff_ = -1, ultChargeOff_ = -1, ultMaxOff_ = -1, echoLogs_ = 0; bool echoProbeLogged_ = false; float echoLastShachi_ = -1; double echoSnapAt_ = 0;
    double basicsMedAt_ = 0;
    bool arcaneOn_ = false; void* arcanePawn_ = nullptr; double arcaneRetryAt_ = 0;
    float arcaneLastUlt_ = -1; int arcaneSamples2_ = 0;
    // 秘术冲拳（G 批）：持有且战斗中把原生「绝技充能效率」(UltimateChargeAdditiveRate) 乘 kArcaneChargeMul（+200%），
    // 与武器无关：任何武器 / 任何命中的原生充能都按倍率放大；主武器切换冷却每帧清零。离开战斗不再写，属性自动还原。
    int arcaneRateOff_ = -1;
    void HexArcane(const Target& p, bool want) {
        if (want != arcaneOn_) {
            arcaneOn_ = want; arcaneLastUlt_ = -1; arcaneSamples2_ = 0;
            pbz::log::Info("影塔海克斯：秘术冲拳{}", want ? std::format("开启（绝技充能效率 ×{:g}，主武器切换无冷却）", tower::hex::kArcaneChargeMul) : std::string("关闭"));
        }
        if (!want) return;
        if (arcaneRateOff_ < 0) arcaneRateOff_ = attr::AttrOffset("UltimateChargeAdditiveRate");  // 3.0.1：不再 static 缓存 -1
        const int rate = arcaneRateOff_;
        if (rate >= 0) attr::Want(p, attr::AttrCurrent(p, rate), Kind::Additive, tower::hex::kArcaneChargeMul);
        ArcaneCooldownTick(p.obj);
        if (ultChargeOff_ < 0) ultChargeOff_ = attr::AttrOffset("UltimateCharge");
        if (ultMaxOff_ < 0) ultMaxOff_ = attr::AttrOffset("UltimateCharge_Max");
        const int cur = ultChargeOff_, mx = ultMaxOff_;
        if (cur < 0 || mx < 0) return;
        const float c = ue::Get<float>(attr::AttrCurrent(p, cur), -1.0f), m = ue::Get<float>(attr::AttrCurrent(p, mx), -1.0f);
        if (arcaneLastUlt_ >= 0 && c > arcaneLastUlt_ + 0.05f && arcaneSamples2_ < 30) {
            ++arcaneSamples2_;
            pbz::log::Info("影塔海克斯：秘术冲拳充能 +{:.1f}（{:.1f}/{:.1f}，效率 {:.2f}）", c - arcaneLastUlt_, c, m,
                           rate >= 0 ? ue::Get<float>(attr::AttrCurrent(p, rate), -1.0f) : -1.0f);
        }
        arcaneLastUlt_ = c;
    }
    // 用户 2026-11-11 实机：每帧清切换冷却会吃掉右下角 HUD 的切换动效。改为：看到切换冷却开始后等 0.3 秒（动效已起）再清；
    // 读不到冷却（没找到槽标签）时退回每 0.3 秒清一次
    double arcaneCdSeen_ = 0, arcaneCdAt_ = 0;
    void ArcaneCooldownTick(UObject* pawn) {
        UObject* comp = ArcaneWeapons(pawn);
        if (!comp) return;
        static UObject* reset = nullptr; if (!reset) reset = pe::FindFunction(ue::ClassOf(comp), "ResetChangeWeaponCooldowns");
        if (!reset) return;
        const auto tags = WeaponSlotTags(comp);
        float cd = -1;
        for (uint64_t t : {tags[0], tags[1]}) if (t) cd = std::max(cd, SlotCooldown(comp, t));
        if (cd < 0) { if (now_ >= arcaneCdAt_) { arcaneCdAt_ = now_ + 0.3; pe::Call(comp, reset, nullptr); } return; }
        if (cd <= 0.001f) { arcaneCdSeen_ = 0; return; }
        if (arcaneCdSeen_ <= 0) arcaneCdSeen_ = now_;
        if (now_ - arcaneCdSeen_ >= 0.3) { pe::Call(comp, reset, nullptr); arcaneCdSeen_ = 0; }
    }
    static float SlotCooldown(UObject* comp, uint64_t tag) {
        static UObject* f = nullptr; if (!f) f = pe::FindFunction(ue::ClassOf(comp), "GetCurrentChangeWeaponCooldown");
        if (!f) return -1;
        std::vector<uint8_t> b(0x40, 0); std::memcpy(Arg(f, b, "SlotTag"), &tag, 8);
        pe::Call(comp, f, b.data());
        const float v = ue::Get<float>(Arg(f, b, "ReturnValue"));
        return std::isfinite(v) ? v : -1;
    }
    // 角色武器组件 RuntimeWeaponData（Map<GameplayTag, RuntimeWeaponSlotData>）里的槽标签：[0] 主武器、[1] 影之武装（按名字含 Shaqi/ShaChi 区分）
    std::array<uint64_t, 2> slotTags_{};
    UObject* slotTagsComp_ = nullptr;
    std::array<uint64_t, 2> WeaponSlotTags(UObject* comp) {
        if (comp == slotTagsComp_) return slotTags_;
        slotTagsComp_ = comp; slotTags_ = {};
        static int off = -2; if (off == -2) off = ue::PropOffset(ue::ClassOf(comp), "RuntimeWeaponData");
        if (off < 0) return slotTags_;
        const auto a = ue::Get<FArr>(comp + off);  // TSparseArray 元素：TPair<FGameplayTag 8, RuntimeWeaponSlotData 0x20> + 8 = 0x30
        std::string seen;
        for (int i = 0; a.data && i < a.num && i < 8; ++i) {
            const uint64_t tag = ue::Get<uint64_t>(static_cast<uint8_t*>(a.data) + i * 0x30);
            const std::string n = ue::Name(static_cast<uint32_t>(tag));
            if (n.empty() || n.find('.') == std::string::npos) continue;
            seen += (seen.empty() ? "" : ", ") + n;
            std::string low = n; for (auto& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (low.find("shaqi") != std::string::npos || low.find("shachi") != std::string::npos) { if (!slotTags_[1]) slotTags_[1] = tag; }
            else if (low.find("main") != std::string::npos || low.find("normal") != std::string::npos) { if (!slotTags_[0]) slotTags_[0] = tag; }
        }
        pbz::log::Info("影塔武库扩建：武器槽标签 {}（主武器 {}，影之武装 {}）", seen.empty() ? "无" : seen, slotTags_[0] ? "已识别" : "未识别", slotTags_[1] ? "已识别" : "未识别");
        return slotTags_;
    }
    // 槽内当前序号（原生武器组件；背包里的 CurrentShaqiWeaponIndex 切换影之武装时不变）
    int SlotIndex(UObject* comp, uint64_t tag) {
        static UObject* f = nullptr; if (!f) f = pe::FindFunction(ue::ClassOf(comp), "GetCurrentWeaponIndex");
        if (!f || !tag) return -1;
        std::vector<uint8_t> b(0x40, 0); std::memcpy(Arg(f, b, "SlotTag"), &tag, 8);
        pe::Call(comp, f, b.data());
        return ue::Get<int32_t>(Arg(f, b, "ReturnValue"));
    }
    static UObject* ArcaneWeapons(UObject* pawn) {
        static int off = -2; if (off == -2 && pawn) off = ue::PropOffset(ue::ClassOf(pawn), "Character Weapons Component");
        UObject* c = off >= 0 && pawn ? ue::Get<UObject*>(pawn + off) : nullptr;
        return c && ue::Live(c) ? c : nullptr;
    }
    // 3.2.0（玩家反馈：同一个种子只有 Boss 一致、海克斯不一致）：抽海克斯（开选择页 / 刷新 / 质变）的候选池原来用 HexAvailable，
    // 它取决于本机这一刻游戏属性接口有没有读到（开局头几帧、不同电脑会不同），候选数一变随机序列就全乱。
    // 抽取改用固定池（只随版本变），同版本 + 同种子 + 同选择 = 同样的海克斯；接口确实不可用的那张在选的时候提示（见 hex_pick）
    tower::hex::Mask HexDrawMask() const {
#ifdef PBZ_HEX_TEST
        return HexAvailable();  // QA 自动爬塔沿用旧行为
#else
        return tower::hex::kPlayableMask;
#endif
    }
    tower::hex::Mask HexAvailable() const {
        tower::hex::Mask mask = tower::hex::kPlayableMask;
        // 3.0.0（用户 2026-11-12）：正式版卡池放入全部海克斯；只保留运行时能力检查（接口不可用的那一个不进卡池）
        if (!towerweaponcap::Ready()) mask &= ~(tower::hex::Bit(tower::hex::Forge));
        if (walkSpeed_ < 0 || jogSpeed_ < 0 || sprintSpeed_ < 0 || hpMax_ < 0 || attack_ < 0 || shachiAttack_ < 0 || shachiMax_ < 0 || !towerspeed::Resolve()) mask &= ~(tower::hex::Bit(tower::hex::Scientist));
        if (walkSpeed_ < 0 || jogSpeed_ < 0 || sprintSpeed_ < 0) mask &= ~((tower::hex::Bit(tower::hex::Swift)) | (tower::hex::Bit(tower::hex::Keen)));
        if (walkSpeed_ < 0 || jogSpeed_ < 0 || sprintSpeed_ < 0 || hp_ < 0 || hpMax_ < 0) mask &= ~(tower::hex::Bit(tower::hex::Escape));
        if (hp_ < 0) mask &= ~(tower::hex::Bit(tower::hex::Grit));
        if (shachiNow_ < 0 || shachiMax_ < 0) mask &= ~(tower::hex::Bit(tower::hex::KillingIntent));
        if (!ServantStatsReady() || !pbz::FindFeature("ui.faction_bars")) mask &= ~(tower::hex::Bit(tower::hex::ServantMaster));
        if (hpMax_ < 0 || attack_ < 0 || !towerweaponprobe::Supported()) mask &= ~(tower::hex::Bit(tower::hex::Heavy));
        if (hp_ < 0 || hpMax_ < 0 || hpMin_ < 0) mask &= ~(tower::hex::Bit(tower::hex::Thorns));  // 3.6.0 反震要读写敌人生命 / 锁血线
        return mask;
    }
    uint64_t spawnEpoch_=0;
    int attack_ = -1, shachiAttack_ = -1, shachiMax_ = -1, infliction_ = -1, shachiNow_ = -1, worldDilation_ = -1, spirit_ = -1, spiritMax_ = -1;
    double freezeUntil_ = 0, slowUntil_ = 0;
    bool prepPending_ = false;  // 重放层：准备道具等入场过场播完再生效
    bool sunder_ = false;
    std::vector<pbz::IFeature*> stActive_;   // 本层由神通开启的修改器功能
    bool ending_ = false;
    // ---- 原生遭遇重放（tower_replay.h）
    const tower::replay::Contract* replay_ = nullptr;
    SummonPermit summonPermit_;
    std::unordered_map<UObject*, int> playerSummons_;
    std::unordered_set<UObject*> mechanismLogged_;
    tower::replay::Progress rp_;
    double replayArmedAt_ = -1;
    bool replayOn_ = true;
    bool replayEntryStarted_=false;
    double replayEntryWait_=0;
    bool replaySupportsReady_ = true;
    double replayGroupKickAt_ = -1;
    std::unordered_set<UObject*> entrySupportLogged_;
    int kickFor_ = -1; double kickQuiet_ = 0, kickTick_ = -1; bool kickDone_ = false;  // 重放层刷怪点兜底（KickWhenQuiet）
    double cinematicSeconds_=0;
    void ReplayArm() {
        replayEntryStarted_=false;
        replayEntryWait_=0;
        replaySupportsReady_=true;
        replayGroupKickAt_=-1;
        entrySupportLogged_.clear();
        // 场地就绪后由玩家交互 / 原生过场开战，不再走普通生成层的90秒等待与3秒读秒。
        // End 成对释放普通入场持有的输入 / 能力锁并重置 Gate；不修改原生过场自身的控制权。
        if (!tower::replay::UsesCountdown(*replay_)) towerentry::End();
        else {
            gateArmed_=admitted_=reachDone_=admitPushed_=false;
            admitSince_=-1; nextReach_=0; lastReach_.clear(); lastAdmitWhy_.clear();
        }
        rp_.Begin(replay_->phaseN);
        replayArmedAt_ = now_; spawnerKicked_ = false; phaseDeadAt_ = phaseTravelAt_ = -1; phaseTravelFor_ = -1; phaseKicked_ = false;
        actors_.clear(); pending_ = 0; spawnQueue_.clear(); fightSince_ = 0;
        cinematicSeconds_=0;
        before_ = pbz::enemies::OwnedActors();
        pbz::log::Info("影塔重放：第 {} 层 {} 就绪，等玩家开遭遇（{}）", run_.floor, replay_->name, replay_->hint);
        Broadcast("info.replay_ready", std::string(replay_->hint));
    }
    // 接管原生刷出来的阶段角色：按类名对契约；第一次接到 = 本层开战（初见过场已播完、一阶段已刷出）
    void ReplayTick() {
        if(!rp_.Started() && replayEntryWait_>90){RecordFailure("replay_first_phase",venueBoss_,"原生初见启动后90秒无一阶段（真实播放/QTE暂停时间不计）");CancelEntry("原生初见结束后未出现一阶段，保留现场取证，不发奖");return;}
        // 赤影骨刃类：没有出场 Flow，由契约刷怪点 ClaimSeqSpawn 原生刷出（塔直接生成会在 BeginPlay 自毁）
        if(replay_->spawnerKick && !rp_.Started() && !spawnerKicked_ && now_-replayArmedAt_>=1.0){
            spawnerKicked_=true;
            auto* sp=static_cast<UObject*>(tvenue::Spawner());
            UObject* fn=sp?pe::FindFunction(ue::ClassOf(sp),"ClaimSeqSpawn"):nullptr;
            if(!sp || ue::ObjName(sp)!=replay_->spawners[0] || !fn){RecordFailure("replay_entry",venueBoss_,"契约刷怪点未就绪或没有 ClaimSeqSpawn");CancelEntry("原生刷怪点未就绪，本层未开始");return;}
            std::array<uint8_t,0x40> a{};
            if(const int o=ue::PropOffset(fn,"bSpawnHidden");o>=0&&o<0x40)a[o]=0;
            if(const int o=ue::PropOffset(fn,"bAwakeAI");o>=0&&o<0x40)a[o]=1;
            if(const int o=ue::PropOffset(fn,"bForceIntoCombat");o>=0&&o<0x40)a[o]=1;
            pe::Call(sp,fn,a.data());
            pbz::log::Info("影塔重放：契约刷怪点 {} 已请求原生刷出（ClaimSeqSpawn 不隐藏 / 唤醒 / 入战）",ue::ObjName(sp));
            if(replay_->kickGroup){  // 20 层阿瑾：同组的红衣兵刷怪点一起刷（原生同伴，不计胜负）
                replayGroupKickAt_=now_;
                int n=0;
                for(void* m:tvenue::GroupMates()){
                    auto* mo=static_cast<UObject*>(m);
                    UObject* mf=pe::FindFunction(ue::ClassOf(mo),"ClaimSeqSpawn");
                    if(!mf)continue;
                    std::array<uint8_t,0x40> b{};
                    if(const int o=ue::PropOffset(mf,"bSpawnHidden");o>=0&&o<0x40)b[o]=0;
                    if(const int o=ue::PropOffset(mf,"bAwakeAI");o>=0&&o<0x40)b[o]=1;
                    if(const int o=ue::PropOffset(mf,"bForceIntoCombat");o>=0&&o<0x40)b[o]=1;
                    towerisolate::AllowTowerSpawn(mo);
                    pe::Call(mo,mf,b.data());
                    ++n;
                }
                pbz::log::Info("影塔重放：同组刷怪点 {} 个已请求原生刷出（原生同伴）",n);
            }
        }
        // 阿瑾的红衣兵是同组原生支援，不属于 actors_，旧入场门只冻结 Boss。
        // 直接从已确认的同组刷怪点读取 SpawnedAI，每帧处理，避免等待两秒一次的角色表刷新。
        // 仅冻结契约列出的支援类，且仍不计阶段/胜负；未初始化时暂不开始读秒。
        replaySupportsReady_ = true;
        if (replay_->kickGroup && towerentry::Holding()) {
            // 给异步刷怪点一点时间登记角色，避免只因 Boss 已到场就立即开始倒计时。
            if (replayGroupKickAt_ >= 0 && now_ - replayGroupKickAt_ < 2.0) replaySupportsReady_ = false;
            for (void* member : tvenue::GroupMates()) {
                auto* sp = static_cast<UObject*>(member);
                if (!sp || !ue::Live(sp)) continue;
                const int off = ue::PropOffset(ue::ClassOf(sp), "SpawnedAI");
                if (off < 0) { replaySupportsReady_ = false; continue; }
                auto* ai = ue::Get<UObject*>(sp + off);
                if (!ai || !ue::Live(ai) || !towerisolate::InPlayerWorld(ai)) continue;
                const auto cls = ue::ClassName(ai);
                if (!tower::replay::In(replay_->supportClasses, replay_->supportN, cls)) continue;
                if (!towerentry::Hold(ai, false)) { replaySupportsReady_ = false; continue; }
                if (entrySupportLogged_.insert(ai).second)
                    pbz::log::Info("影塔入场：同组支援 {} {} 已冻结，倒计时结束随 Boss 释放", ue::ObjName(ai), cls);
            }
            // 场地可自行提前刷出红衣兵，此时未触发主动刷怪分支；从现有角色表再核一次。
            double pp[3];
            if (fnLoc_ && PawnLoc(pp)) for (const auto& t : attr::Targets()) {
                if (t.who == attr::Who::Player || !attr::Alive(t) || !towerisolate::InPlayerWorld(t.obj)) continue;
                const auto cls = ue::ClassName(t.obj);
                if (!tower::replay::In(replay_->supportClasses, replay_->supportN, cls) || PlayerSide(t.obj, cls, towerwidgets::Pawn())) continue;
                uint8_t b[32] = {}; pe::Call(t.obj, fnLoc_, b); double ep[3]; std::memcpy(ep, b, sizeof(ep));
                if (std::hypot(ep[0] - pp[0], ep[1] - pp[1]) > 15000 || std::abs(ep[2] - pp[2]) > 5000) continue;
                if (!towerentry::Hold(t.obj, false)) { replaySupportsReady_ = false; continue; }
                if (entrySupportLogged_.insert(t.obj).second)
                    pbz::log::Info("影塔入场：同组支援 {} {} 已冻结，倒计时结束随 Boss 释放", ue::ObjName(t.obj), cls);
            }
        }
        if(!replayEntryStarted_ && !rp_.Started()){
            std::string why;const auto entry=towerstorygate::Start(*replay_,&why);
            if(entry==towerstorygate::Entry::Started)replayEntryStarted_=true;
            else if(entry==towerstorygate::Entry::Failed || now_-replayArmedAt_>20){RecordFailure("replay_entry",venueBoss_,why);CancelEntry("原生入场未启动："+why);return;}
        }
        // 26 层万钧（ui18）：契约刷怪点不静默，玩家一到场它就自己刷出了万钧，塔当场接管、rp_ 已开始 → 入场 Seq074 永远不跑，
        // Boss 待在原地不动、玩家也没被过场带进战斗区。有入场节点的契约：入场真正启动前不接管任何阶段角色
        if(replay_->entryAsset!=std::array<uint32_t,4>{} && !replayEntryStarted_ && !rp_.Started())return;
        for (auto& t : attr::Targets()) {
            if (t.who != attr::Who::Enemies || !attr::Alive(t) || IsServant(t.obj)) continue;
            const std::string cls = ue::ClassName(t.obj);
            const int k = tower::replay::PhaseOf(*replay_, cls);
            if (k >= 0 && !towerisolate::InPlayerWorld(t.obj)) {  // F37：过场占位模板不是原生阶段
                if (placeholderLogged_.insert(t.obj).second)
                    pbz::log::Info("影塔重放：忽略不在玩家世界的同名阶段对象 {}（过场占位 / 模板）", ue::ObjName(t.obj));
                continue;
            }
            if (k < 0) continue;
            if (std::any_of(actors_.begin(), actors_.end(), [&](const Actor& a) { return a.obj == t.obj; })) continue;
            const bool join = !rp_.CanAdopt(k) && rp_.CanJoin(k);
            if(!rp_.CanAdopt(k) && !join){RecordFailure("replay_phase_order",venueBoss_,std::format("阶段重复或次序异常：已见{}，新出现第{}阶段 {}",rp_.seen+1,k+1,ue::ObjName(t.obj)));CancelEntry("原生阶段重复或次序异常，本层不计通过，保留现场");return;}
            std::string groupWhy;
            if(!towerisolate::ReplayPhaseReady(k,&groupWhy)){RecordFailure("replay_group_milestone",venueBoss_,groupWhy);CancelEntry("原生阶段通知未就绪："+groupWhy);return;}
            int pool = -1;
            for (int i = 0; i < static_cast<int>(std::size(tower::kBossPool)); ++i) {
                const auto path = tower::kBossPool[i].path;
                if (path.size() > cls.size() && path.substr(path.size() - cls.size()) == cls) { pool = i; break; }
            }
            Actor a{t.obj, pool >= 0 ? std::string(tower::kBossPool[pool].name) : cls};
            a.adopted = true; a.boss = pool; a.phase = k; a.target = t; a.hasTarget = true;
            actors_.push_back(a);
            if (towerentry::Holding() && !towerentry::Hold(t.obj)) { CancelEntry(towerentry::Why()); return; }
            if (join) {  // 同阶段的另一个原生角色：一起计血量 / 胜负，阶段要全部死完才算过
                pbz::log::Info("影塔重放：第 {} 阶段另一个原生角色 {} 到场，并入本阶段", k + 1, actors_.back().name);
                towercombat::Watch(t.obj, actors_.back().name);
                continue;
            }
            const bool first = !rp_.Started();
            rp_.Seen(k, now_);
            pbz::log::Info("影塔重放：第 {} 阶段 {} 到场（原生刷出，塔接管血量 / 胜负）", k + 1, actors_.back().name);
            if (tower::travel::CenterPair cp; first && pool >= 0 && fnLoc_ && tower::travel::CenteredPair(tower::kBossPool[pool].path, &cp)) {
                // 9 层拙壁：原生刷怪点压在雾门圈边，挪到圈中间、玩家对面（落点按地面射线核过才放，失败就留在原地）
                const double nearAt[3] = {cp.boss.x, cp.boss.y, cp.boss.z + 100};
                double landing[3]; std::string why;
                bool ok = towerreach::Landing(t.obj, nearAt, landing, &why, true);
                if (ok) ok = MoveActor(t.obj, {landing[0], landing[1], landing[2]});
                pbz::log::Info("影塔重放：{} 放到雾门圈中间（{}）", actors_.back().name,
                               ok ? std::format("{:.0f}/{:.0f}/{:.0f}", landing[0], landing[1], landing[2]) : "失败，留在刷怪点：" + why);
            }
            if (first && !towerentry::Holding()) {
                fightSince_ = now_; minHpFrac_ = 1.0; fight_ = {}; lastHp_ = -1;
                // 用户 2026-11-06："定身符没用"——重放层原来从不触发准备道具。一阶段到场后，等入场过场播完再生效
                prepPending_ = true;
                ArmBoundary();
                towercombat::Begin(run_.floor, towerwidgets::Pawn());
                Broadcast("info.floor_started", std::format("第 {} 层，开战", ShownBoss(run_.floor)));
            } else if (!first) {
                Broadcast("info.relay_next", std::format("第 {} 阶段：{}", k + 1, actors_.back().name));
            }
            // 第 38 层沐小葵转二阶段：循环播放主题曲《心湖》（本机 mp3）；打完不离开可一直听，开下一层 / 出塔 / 取消本层时停
            if (k >= 1 && replay_->id == "muxiaokui") towermusic::Play("沐小葵二阶段：心湖");
            towercombat::Watch(t.obj, actors_.back().name);
        }
    }
    void EndReplay() {
        towerinteractions::AllowActors({});
        if (!replay_ && replayArmedAt_ < 0) return;
        replay_ = nullptr; rp_ = {}; replayArmedAt_ = -1;
        // Keep the completed floor's clock until reward accounting has read it.
        towerisolate::SetReplay(nullptr);
    }
    int rewardedFloor_ = -1, assistCatches_ = 0;
    bool gateArmed_ = false, admitted_ = false, reachDone_ = false, transient_ = false, audited_ = false;
    int transientTries_ = 0, arrivalRetries_ = 0;
    bool menuWaitLogged_ = false;
    double gateSince_ = -1, gateZ_ = 0, gateStableSince_ = -1;
    double nextSupportLog_=0;
    towerreach::FloorAudit floorAudit_;
    std::string gateWhy_;
    // 生成前就绪门（36-38：加载模态 WB_AdditionalLoading_C 还在顶层就提交生成）：原生界面不在顶层、玩家脚下有地面、高度稳定 0.3 秒；
    // 第一次放行前跑一次地面审计（只读）。20 秒还不就绪就取消回准备，说明是哪一项
    bool SpawnGate() {
        // 1.1.1（玩家截图：简单难度第 8 层"生成前就绪超时：原生界面在顶层 W_Systembase_C"，之后 9、10 层"未处于目标场地"一路跳过）：
        // W_Systembase_C 是 ESC 系统菜单。菜单开着 / 游戏暂停时传送落不下去、就绪也等不到——这是玩家在看菜单，不是本层坏了：
        // 只等，不计时、不判；关掉菜单后不在到达点（同一关卡）先放回到达点，最多 2 次，还不行才取消
        UObject* menu = towerwidgets::NativeModal();
        const bool menuOpen = towerentry::GamePaused() || (menu && ue::ClassName(menu).find("Systembase") != std::string::npos);
        if (menuOpen) {
            if (!menuWaitLogged_) { menuWaitLogged_ = true; pbz::log::Info("影塔生成前就绪：游戏暂停 / 系统菜单打开，等玩家关掉再继续（不计超时）"); }
            gateSince_ = -1;
            return false;
        }
        menuWaitLogged_ = false;
        if (travelled_ && !towertravel::AtDestination()) {
            if (pbz::TeleportBusy()) return false;
            if (double a[4]; arrivalRetries_ < 2 && towertravel::ArrivalHere(a)) {
                ++arrivalRetries_;
                std::string msg;
                const bool ok = pbz::PointTeleport(a[0], a[1], a[2] + 50, a[3], &msg);
                pbz::log::Info("影塔生成前就绪：不在到达点，放回到达点第 {} 次（{}）", arrivalRetries_, ok ? "已传送" : "失败：" + msg);
                gateSince_ = -1;
                return false;
            }
            RecordFailure("arrival_lost",venueBoss_,"入场位置已偏离目标关卡或目标高度");
            CancelEntry("未处于目标场地，已取消本层；不会在死亡复活点开战"); return false;
        }
        if (gateSince_ < 0) { gateSince_ = now_; gateStableSince_ = -1; gateWhy_.clear(); storyWaitSince_ = -1; PreloadFloorClasses(); }
        std::string why;
        double pl[3] = {};
        UObject* pawn = towerwidgets::Pawn();
        bool storyOk = towerstorygate::Protected(&why);
        if (!storyOk) {
            // 1.0.1（玩家反馈每层"剧情真实执行门或精确 Flow 身份尚未就绪"被跳过）：蓝图执行门要核对游戏代码字节，
            // 装了别的挂接蓝图执行的 Mod / 游戏版本不同就一直装不上。持续 6 秒不可用就降级开打：会话隔离（拦战后剧情 / 掉落 /
            // 清组脚本）不依赖它照常生效，原档安全；原生遭遇重放层降级为在原生场地按 1v1 打第一阶段（见 Next）
            if (storyWaitSince_ < 0) storyWaitSince_ = now_;
            if (now_ - storyWaitSince_ >= 6) {
                if (!storyDegraded_) {
                    storyDegraded_ = true;
                    pbz::log::Info("影塔：剧情保护（蓝图执行门）不可用，降级开打：{}", why);
                    Broadcast("err.story_degraded", "剧情保护不可用（可能与其他 Mod 冲突），已降级继续：普通层照常开打，原生遭遇层改为直接开打第一阶段");
                }
                why.clear(); storyOk = true;
            }
        } else storyWaitSince_ = -1;
        if (!storyOk) {}
        else if (UObject* m = towerwidgets::NativeModal()) why = "原生界面在顶层：" + ue::ClassName(m);
        else if (!pawn || !PawnLoc(pl)) why = "玩家未就绪";
        else {
            const auto probe = towerreach::Measure(pawn, pl, nullptr, pl);
            if(now_>=nextSupportLog_){nextSupportLog_=now_+1;pbz::log::Info("影塔生成前支撑：{}",towerreach::Describe(probe));}
            if(!probe.groundKnown)why="地面探测接口未就绪，不开始生成";
            else if(towerreach::Decide(probe)==towerreach::Verdict::NoGround)why=towerreach::Why(towerreach::Verdict::NoGround);
            else if (gateStableSince_ < 0 || std::abs(pl[2] - gateZ_) > 5) { gateZ_ = pl[2]; gateStableSince_ = now_; why = "玩家高度还在变"; }
            else if (now_ - gateStableSince_ < 0.3) why = "玩家高度还在变";
        }
        if (why.empty()) {
            if (!audited_) {
                audited_ = true;
                floorAudit_ = towerreach::Audit(pawn, pl, 96, 800);
                const auto v = towerreach::Classify(floorAudit_);
                pbz::log::Info("影塔生成前地面审计：{}（判定：{}）", towerreach::Describe(floorAudit_), v == towerreach::FloorVerdict::Ok ? "WorldStatic 可落地" : towerreach::Why(v));
            }
            if (now_ - gateSince_ > 0.5) pbz::log::Info("影塔生成前就绪：放行（等了 {:.1f} 秒）", now_ - gateSince_);
            gateSince_ = -1;
            return true;
        }
        if (why != gateWhy_) { gateWhy_ = why; pbz::log::Info("影塔生成前就绪：等待（{}）", why); }
        if (now_ - gateSince_ > 20) { gateSince_ = -1; CancelEntry("生成前就绪超时：" + why); }
        return false;
    }
    static bool TransientSpawnError(const std::string& json) { return tower::floorplay::TransientSpawnError(json); }
    double admitSince_ = -1, nextReach_ = 0;
    bool admitPushed_ = false;  // 本次入场已把玩家放进界内过（只放一次）
    double pushedAt_ = 0;       // 放入的时刻
    std::string lastAdmitWhy_;  // 上一次记过的准入失败理由
    std::string lastReach_;
    std::string rewardedRun_;
    // QA 验收辅助（用户 2026-11-11：「为我打开攻击倍率和无硬直，我速通去选取海克斯」）：在塔内（准备 / 战斗）持有
    // 伤害 ×10 和不硬直，出塔或本局结束放手。只在 QA 构建编进来；用持有者机制，修改器里自己开的不受影响
#if defined(PBZ_HEX_TEST) || defined(PBZ_PLAYTEST)
#include "tower_venue_measure.inc"  // 3.6.0 场地实测（测试包 qa_measure）
#endif
#ifdef PBZ_HEX_TEST
#include "tower_capacity_probe.inc"
    bool CapGuardOn() const { return cap_.has_value() && cap_->stage >= 3; }
#else
    bool CapGuardOn() const { return false; }
#endif
    // 透视眼（用户 2026-11-12）：下一层 Boss 写到 %LOCALAPPDATA%\\PBZTower\\foresight.txt，安装器战绩页显示；没有透视眼 / 不在层间就删掉
    std::string foresightShown_; double foresightAt_ = 0;
    // 3.0.0（用户 2026-11-12）：安装器战绩页右上角按顺序列出本局选过的海克斯，并写出种子（玩家拿好种子互相分享）。
    // %LOCALAPPDATA%\PBZTower\hexes.txt：第一行 seed<TAB>种子<TAB>难度<TAB>局号；之后每行 hex<TAB>显示层<TAB>名字<TAB>品质
    std::string hexListShown_;
    void HexListFile() {
        std::string body;
        if (run_.phase != tower::Phase::Idle && !run_.id.empty()) {
            body = std::format("seed\t{}\t{}\t{}\n", run_.seed, run_.difficulty ? "easy" : "hard", run_.id);
            if (grow_.on) {
                const auto& x = grow_.hexes;
                std::vector<std::pair<int, int>> list;  // 显示层, id
                for (size_t i = 0; i < x.picks.size(); ++i) if (tower::hex::Playable(x.picks[i])) list.push_back({x.pickedAt[i], x.picks[i]});
                std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
                for (size_t i = 0; i < x.extra.size() && i < x.extraAt.size(); ++i) if (tower::hex::Playable(x.extra[i])) list.push_back({x.extraAt[i], x.extra[i]});
                std::array<int, tower::hex::Count> lvSeen{};  // 3.6.0：同一海克斯第 n 张 = Lvn
                for (auto& [at, id] : list) body += std::format("hex\t{}\t{}\t{}\n", at, tower::hex::NameAt(id, ++lvSeen[id]), tower::hex::TierOf(id));
            }
        }
        if (body == hexListShown_) return;
        hexListShown_ = body;
        const auto path = TowerFile(L"hexes.txt");
        if (path.empty()) return;
        if (body.empty()) DeleteFileW(path.c_str());
        else AtomicTowerFile(path, body);
    }
    void ForesightFile() {
        if (now_ < foresightAt_) return;
        foresightAt_ = now_ + 1.0;
        HexListFile();
        const auto text = NextBossPreview();
        const std::string body = text.empty() ? std::string() : std::format("{}\t{}\n", ShownBoss(run_.floor + 1), text);
        if (body == foresightShown_) return;
        foresightShown_ = body;
        const auto path = TowerFile(L"foresight.txt");
        if (path.empty()) return;
        if (body.empty()) DeleteFileW(path.c_str());
        else AtomicTowerFile(path, body);
    }

#ifdef PBZ_HEX_TEST
    // ---- QA 自动爬塔（用户 2026-11-12：3.0.0 压力烟测，后台无人值守）。{"cmd":"qa_auto","value":"on|off","to":显示层,"seed":种子}
    // 不在塔里：开 50 层新局；之后每 0.4 秒推进：海克斯（QA 计划逐个选，第 25 层起把其余全部发放，全部同时生效测兼容）、
    // 属性三选一 / 额外奖励、驿站（按价从低到高买、锻造、离开）、淬炼 / 修为 / 饰品打磨、奖励页、下一层；战斗中无敌 + 秒杀。
    // 每层一行写 %LOCALAPPDATA%\PBZTower\autoclimb.tsv（属性实际值、血条 / 杀气条长度、帧时间、场上人数）；
    // 异常（卡关、失败、死亡）写 autoclimb-events.log 并尽量自救（超时跳层）。到目标显示层停下
    struct AutoClimb {
        bool on = false; int to = 130; uint64_t seed = 0;
        unsigned long long next = 0, phaseSince = 0, floorSince = 0, idleSince = 0, closeAt = 0;
        tower::Phase phase = tower::Phase::Idle; int floor = -1, shopDoneFloor = -1, buys = 0, stalls = 0, actions = 0;
        bool grantedAll = false, started = false;
        double frameSum = 0, frameMax = 0; int frames = 0, hitches = 0, peakPeople = 0;
        double bossHpMax = 0, bossAtk = 0;
        std::string lastErr;
    } auto_;
    void AutoEvent(const std::string& what) {
        pbz::log::Info("影塔自动爬塔：{}", what);
        if (const auto path = TowerFile(L"autoclimb-events.log"); !path.empty()) {
            std::ofstream f(path, std::ios::binary | std::ios::app);
            SYSTEMTIME t; GetLocalTime(&t);
            f << std::format("{:02}:{:02}:{:02}\t显示{}\t{}\n", t.wHour, t.wMinute, t.wSecond, ShownNow(), what);
        }
    }
    void AutoSend(const std::string& json) { ++auto_.actions; Command(json); }
    // 场上其余人物（敌方 Boss / 小怪 / 友军仆从）的数量；顺带记 Boss 最大生命 / 攻击
    int AutoPeople() {
        int n = 0;
        static const int hpMax = attr::AttrOffset("Health_Max"), atk = attr::AttrOffset("Attack");
        for (auto& t : attr::Targets()) if (t.who == attr::Who::Enemies && attr::Alive(t)) {
            ++n;
            if (hpMax >= 0) auto_.bossHpMax = std::max(auto_.bossHpMax, static_cast<double>(ue::Get<float>(attr::AttrCurrent(t, hpMax))));
            if (atk >= 0) auto_.bossAtk = std::max(auto_.bossAtk, static_cast<double>(ue::Get<float>(attr::AttrCurrent(t, atk))));
        }
        return n;
    }
    void AutoRow() {
        const auto path = TowerFile(L"autoclimb.tsv");
        if (path.empty()) return;
        const bool fresh = GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
        std::ofstream f(path, std::ios::binary | std::ios::app);
        if (fresh) f << "显示层\t内部层\t战斗秒\t整层秒\t生命\t生命上限\t原生生命上限\t杀气\t杀气上限\t原生杀气上限\t攻击\t破防\t攻速\t移速\t魂玉\t本局购买\t海克斯数\t仆从\t峰值人数\tBoss生命上限\tBoss攻击\t平均帧ms\t最长帧ms\t卡顿帧\t血条\tBoss\n";
        const Target* p = Player();
        auto v = [&](const char* n) -> double { const int o = attr::AttrOffset(n); return p && o >= 0 ? ue::Get<float>(attr::AttrCurrent(*p, o)) : -1.0; };
        const auto& x = grow_.hexes;
        int hexes = 0; for (int id : x.picks) hexes += tower::hex::Playable(id); hexes += static_cast<int>(x.extra.size());
        int servants = 0; for (auto& a : servants_.actors) servants += !a.dead && ue::Live(a.obj);
        const int tenths = run_.floor >= 1 && run_.floor <= static_cast<int>(run_.times.size()) ? run_.times[run_.floor - 1] : 0;
        f << std::format("{}\t{}\t{:.1f}\t{:.0f}\t{:.0f}\t{:.0f}\t{:.0f}\t{:.0f}\t{:.0f}\t{:.0f}\t{:.0f}\t{:.0f}\t{:.3f}\t{:.3f}\t{}\t{}\t{}\t{}\t{}\t{:.0f}\t{:.0f}\t{:.1f}\t{:.0f}\t{}\t{}\t{}\n",
                         ShownBoss(run_.floor), run_.floor, tenths / 10.0, (GetTickCount64() - auto_.floorSince) / 1000.0,
                         v("Health"), v("Health_Max"), v("Health_Max_Lv1"), v("Shachi"), v("Shachi_Max"), v("Shachi_Max_Lv1"), v("Attack"), v("ShachiAttack"),
                         v("AttackSpeed"), v("MoveSpeed"), run_.jade, auto_.buys, hexes, servants, auto_.peakPeople, auto_.bossHpMax, auto_.bossAtk,
                         auto_.frames ? auto_.frameSum / auto_.frames * 1000.0 : 0.0, auto_.frameMax * 1000.0, auto_.hitches, towerwidgets::BarDebug(), LoggedBossNames());
        auto_.frameSum = auto_.frameMax = 0; auto_.frames = auto_.hitches = auto_.peakPeople = 0; auto_.bossHpMax = auto_.bossAtk = 0;
    }
    // QA 计划选完后，把其余可用海克斯一次性全部发放（仆从大师补满 3 次；如有神助排在回归基本功之后）
    void AutoGrantAll() {
        auto& x = grow_.hexes;
        std::string names;
        const tower::hex::Mask avail = HexAvailable();
        for (int pass = 0; pass < 2; ++pass)
            for (int id = 0; id < tower::hex::Count; ++id) {
                if (!(avail & (tower::hex::Bit(id)))) continue;
                if (id == tower::hex::Scavenger) continue;  // 4.0.0（用户）：拾荒者打完 Boss 弹装备界面，会卡住自动爬塔
                while (x.CanPick(static_cast<tower::hex::Id>(id), 120)) {
                    x.extra.push_back(id); x.extraAt.push_back(-120); x.extraBase.push_back(x.endlessCleared);  // 3.6.0：记作赠送（负层号），同层多张正层号读档会判损坏
                    grow_.GrantHexChoice(static_cast<tower::hex::Id>(id));
                    if (id == tower::hex::AttributeStack) grow_.hexAttributes.BeginMul(run_.seed, x.ThrowerMul(), grow_.Ctx(run_.floor, run_.floors, run_.items));
                    names += (names.empty() ? "" : "、") + std::string(tower::hex::defs[id].name);
                    if (id != tower::hex::ServantMaster) break;
                }
            }
        grow_.session.Sync(Game(), grow_.profile);
        SaveHex(); SaveRun();
        AutoEvent("其余海克斯全部发放：" + (names.empty() ? std::string("（无）") : names));
    }
    // 淬炼点：最低等级的兵器 +1（隔层先打磨一件饰品）；修为点：能点的节点依次点
    void AutoSpend() {
        auto& pr = grow_.profile;
        if (run_.floor % 2 == 0 && !pr.accessories.empty() && pr.temper >= tower::growth::kAccessoryRefineTemper) {
            const auto& a = pr.accessories[static_cast<size_t>(run_.floor / 2) % pr.accessories.size()];
            std::string why;
            if (a.refine < tower::growth::kAccessories[a.index].refine && ChargeAccessory(std::string(tower::growth::kAccessories[a.index].id), 1, &why))
                AutoEvent(std::format("饰品打磨：{} → {}", tower::growth::kAccessories[a.index].name, a.refine));
        }
        for (int guard = 0; guard < 40 && pr.temper > 0; ++guard) {
            int best = -1, lv = 1 << 30;
            for (auto& w : pr.weapons) if (w.level < lv) { lv = w.level; best = w.index; }
            if (best < 0 || grow_.Upgrade(Game(), best) != tower::growth::Err::Ok) break;
        }
        for (int guard = 0; guard < 40 && pr.cultivate > 0; ++guard) {
            bool any = false;
            for (int n = 0; n < tower::growth::kNodeCount && pr.cultivate > 0; ++n)
                if (grow_.Activate(Game(), n, run_.floor) == tower::growth::Err::Ok) any = true;
            if (!any) break;
        }
    }
    bool AutoShop() {
        auto& s = grow_.shop;
        if (!s.Open() || auto_.shopDoneFloor == s.floor) return false;
        // 便宜的先买，每次驿站最多 6 件（武器、饰品、精炼、药、MOD 道具、点数都会买到）
        int bought = 0;
        for (int round = 0; round < 6; ++round) {
            int slot = -1, price = 1 << 30;
            for (int i = 0; i < static_cast<int>(s.shelf.size()); ++i)
                if (!s.shelf[i].sold && s.shelf[i].price <= run_.jade && s.shelf[i].price < price) { price = s.shelf[i].price; slot = i; }
            if (slot < 0) break;
            const auto label = tower::rewards::Label(s.shelf[slot].offer);
            const int before = run_.jade;
            AutoSend(std::format(R"({{"cmd":"shop","value":"buy","slot":{},"ver":{}}})", slot, s.version));
            if (run_.jade >= before) { AutoEvent("驿站购买未成交：" + label); break; }
            ++bought; ++auto_.buys;
        }
        if (grow_.hexes.Has(tower::hex::AttributeStack) && !grow_.hexAttributes.remaining && run_.jade >= tower::hex::ForgePrice(run_.floor, run_.floors))
            AutoSend(std::format(R"({{"cmd":"shop","value":"forge","ver":{},"slot":0}})", s.version));  // 3.6.1：压测按「最大」批量买
        AutoSend(R"({"cmd":"shop","value":"leave"})");
        auto_.shopDoneFloor = s.floor;
        AutoEvent(std::format("驿站：买 {} 件，剩魂玉 {}", bought, run_.jade));
        return true;
    }
    // 无人操作时角色不会出手：开战 3 秒后，每 2 秒处理一个塔方敌人，走游戏真实的"生命归零"死亡路径
    // （2026-11-12 实机：直接调 PBZCharacter_NPC::Kill 会让怪没有死亡记录就消失，塔按"异常消失"补位，无限循环）：
    //   普通敌人：生命基值 / 当前值写 0，再按 BP_DeathComponent 自己的零血路径 AddPendingState(Dead)（核心生成物同款）；
    //   原生假死（多阶段）的：只把生命压到 1，由仆从 / 秒杀的真实伤害打完，阶段转换照原生走
    unsigned long long autoKillAt_ = 0;
    bool AutoZero(UObject* actor, const Target& t) {
        static const int ho = attr::AttrOffset("Health"), mo = attr::AttrOffset("Health_Min");
        if (ho < 0) return false;
        auto* cls = ue::ClassOf(actor);
        const int off = ue::PropOffset(cls, "Death Component");
        auto* death = off >= 0 ? ue::Get<UObject*>(actor + off) : nullptr;
        auto* dc = towerperf::ClassNamed("BP_DeathComponent_C");
        const int fo = dc ? ue::PropOffset(dc, "Fake Death Enabled") : -1, so = dc ? ue::PropOffset(dc, "Actor State Component") : -1, to = dc ? ue::PropOffset(dc, "Dead State Tag") : -1;
        const bool fake = !death || !dc || !ue::Live(death) || !ue::IsA(death, dc) || fo < 0 || ue::Get<uint8_t>(death + fo, 1) != 0;
        const float floorHp = mo >= 0 ? std::max(0.0f, ue::Get<float>(attr::AttrCurrent(t, mo), 0.0f)) : 0.0f;
        if (fake || floorHp > 0) {  // 假死 / 有生命下限（阶段）的：压到 1（或下限），等真实伤害
            const float v = std::max(1.0f, floorHp);
            ue::Write(attr::AttrBase(t, ho), &v, 4); ue::Write(attr::AttrCurrent(t, ho), &v, 4);
            return false;
        }
        const float zero = 0.0f;
        ue::Write(attr::AttrBase(t, ho), &zero, 4); ue::Write(attr::AttrCurrent(t, ho), &zero, 4);
        auto* state = so >= 0 ? ue::Get<UObject*>(death + so) : nullptr;
        const uint64_t tag = to >= 0 ? ue::Get<uint64_t>(death + to) : 0;
        auto* add = state && ue::Live(state) ? pe::FindFunction(ue::ClassOf(state), "AddPendingState") : nullptr;
        const int at = add ? ue::PropOffset(add, "NewStateTag") : -1;
        if (!add || at < 0 || !static_cast<uint32_t>(tag)) return false;
        std::array<uint8_t, 32> args{}; std::memcpy(args.data() + at, &tag, 8);
        pe::Call(state, add, args.data());
        return true;
    }
    // 用户 2026-11-12：自动爬塔时剧情 / 转阶段 CG 全部跳过（原生 EventPipelineGameInstance.SetForceSkipAllSequence，GameInstance 级，停下时关回）
    void AutoSkipSequences(bool on) {
        static UObject* inst = nullptr;
        if (!inst || !ue::Live(inst)) {
            inst = nullptr;
            auto* cls = towerperf::ClassNamed("EventPipelineGameInstance");
            if (cls) ue::ForEach([&](int, UObject* o) { if (!ue::IsDefault(o) && ue::IsA(o, cls) && ue::Live(o)) { inst = o; return false; } return true; });
        }
        auto* fn = inst ? pe::FindFunction(ue::ClassOf(inst), "SetForceSkipAllSequence") : nullptr;
        const int at = fn ? ue::PropOffset(fn, "NewValue") : -1;
        if (!fn || at < 0) { AutoEvent("跳过全部过场：接口不可用"); return; }
        std::array<uint8_t, 32> args{}; args[static_cast<size_t>(at)] = on ? 1 : 0;
        pe::Call(inst, fn, args.data());
    }
    void AutoKill() {
        if (fightSince_ <= 0 || towerentry::Holding() || now_ - fightSince_ < 1.0) return;  // 4.0.0：开战 3 → 1 秒后开始
        const auto t = GetTickCount64();
        if (t < autoKillAt_) return;
        autoKillAt_ = t + 500;  // 4.0.0：每 2 → 0.5 秒清一只
        for (auto& a : actors_) {
            if (a.dead || a.gone || !a.hasTarget || !ue::Live(a.obj) || IsServant(a.obj)) continue;
            AutoZero(a.obj, a.target);
            return;  // 一次一个，给阶段转换 / 下一波留时间
        }
    }
    void AutoClimbTick(float dt) {
        if (!auto_.on) return;
        const auto now = GetTickCount64();
        if (run_.phase == tower::Phase::Fighting && dt > 0 && dt < 5) {
            auto_.frameSum += dt; ++auto_.frames; auto_.frameMax = std::max(auto_.frameMax, static_cast<double>(dt));
            if (dt > 0.1) ++auto_.hitches;
        }
        if (now < auto_.next) return;
        auto_.next = now + 250;  // 4.0.0（用户：一层 1 分钟内）：400 → 250 ms
        if (run_.phase != auto_.phase || run_.floor != auto_.floor) {
            if (run_.phase == tower::Phase::Reward && auto_.phase == tower::Phase::Fighting) AutoRow();
            if (run_.phase == tower::Phase::Fighting && auto_.phase != tower::Phase::Fighting) auto_.floorSince = now;
            auto_.phase = run_.phase; auto_.floor = run_.floor; auto_.phaseSince = now;
        }
        const double inPhase = static_cast<double>(static_cast<long long>(now) - static_cast<long long>(auto_.phaseSince)) / 1000.0;
        if (const auto e = lastCode_; !e.empty() && e.rfind("err.", 0) == 0 && e != auto_.lastErr) { auto_.lastErr = e; AutoEvent("命令被拒：" + e); }
        if (trip_.phase() != tower::trip::Phase::Inside) {
            if (run_.phase == tower::Phase::Idle && trip_.phase() == tower::trip::Phase::Outside && !auto_.started) {
                auto_.started = true;
                AutoEvent(std::format("开 50 层新局，种子 {}", auto_.seed));
                AutoSend(std::format(R"({{"cmd":"start","floors":50,"seed":"{}"}})", auto_.seed ? auto_.seed : GetTickCount64()));
            } else if (inPhase > 180 && static_cast<int>(inPhase) % 60 == 0) AutoEvent(std::format("等进塔已 {:.0f} 秒（塔档阶段 {}）", inPhase, static_cast<int>(trip_.phase())));
            return;
        }
        if (!auto_.started) {
            auto_.started = true;
            AutoEvent(std::format("塔内重开 50 层新局，种子 {}", auto_.seed));
            AutoSend(std::format(R"({{"cmd":"start","editor":"floor","floors":50,"from":1,"seed":"{}"}})", auto_.seed ? auto_.seed : GetTickCount64()));
            auto_.phaseSince = now;
            return;
        }
        qaGod_ = true;
        if (ShownNow() >= auto_.to && run_.phase == tower::Phase::Ready) { AutoEvent("到达目标层，自动爬塔停止"); auto_.on = false; qaGod_ = false; AutoSkipSequences(false); return; }
        switch (run_.phase) {
            case tower::Phase::Ready: {
                if (inPhase < 0.6) return;  // 让奖励 / 驿站 / 获得界面先落定（4.0.0：1.5 → 0.6 秒）
                if (inPhase > 2.0 && towerwidgets::PanelActive()) {  // 4.0.0：原生弹窗 / 整备面板挡住开层 → 直接关掉
                    if (const auto closed = towerwidgets::QaCloseBlocking(); !closed.empty()) AutoEvent("关闭挡住开层的原生界面：" + closed);
                    auto_.phaseSince = now - 1000;
                    return;
                }
                auto& x = grow_.hexes;
                if (x.pending) {
                    // 4.0.0（用户）：自动爬塔不选拾荒者——打完 Boss 会弹原生装备界面，要手动关掉才能继续
                    auto usable = [&](int k) { return x.offers[k] >= 0 && (HexAvailable() & tower::hex::Bit(x.offers[k])); };
                    int slot = 0;
                    while (slot < 3 && (!usable(slot) || x.offers[slot] == tower::hex::Scavenger)) ++slot;
                    if (slot >= 3) { slot = 0; while (slot < 3 && !usable(slot)) ++slot; }
                    if (slot >= 3) { AutoEvent("海克斯三张都不可用"); return; }
                    AutoEvent(std::format("海克斯：选 {}", tower::hex::defs[x.offers[slot]].name));
                    AutoSend(std::format(R"({{"cmd":"hex_pick","slot":{},"hex_floor":"{}","hex_id":"{}"}})", slot, x.pending, x.offers[slot]));
                    return;
                }
                if (grow_.hexAttributes.remaining) { AutoSend(std::format(R"({{"cmd":"grow","action":"hex_attr","index":0,"to":{}}})", grow_.hexAttributes.claimed)); return; }
                if (grow_.forgeBatch.Active()) {  // 3.6.1 批量锻造：压测每组选第一项
                    std::string p; for (size_t k = 0; k < grow_.forgeBatch.rows.size(); ++k) p += k ? ",0" : "0";
                    AutoSend(std::format(R"({{"cmd":"forge_pick","picks":"{}"}})", p)); return;
                }
                if (!grow_.bonus.empty()) { AutoSend(R"({"cmd":"grow","action":"bonus","index":0})"); return; }
                if (!grow_.med.empty()) { AutoSend(R"({"cmd":"grow","action":"med","index":0})"); return; }
                if (!grow_.acc.empty()) { AutoSend(R"({"cmd":"grow","action":"acc","index":0})"); return; }
                if (!auto_.grantedAll && ShownNow() >= 25) { auto_.grantedAll = true; AutoGrantAll(); return; }
                if (AutoShop()) return;
                AutoSpend();
                if (grow_.syncOwed) return;  // 等获得界面补发完
                if (inPhase > 90) { AutoEvent("准备页 90 秒没能开下一层，再试"); auto_.phaseSince = now; }
                AutoSend(R"({"cmd":"next"})");
                auto_.phaseSince = now + 5000;  // 给入场 5 秒，不重复点
                return;
            }
            case tower::Phase::Reward:
                if (inPhase < 0.6) return;
                if (grow_.on && !grow_.offered) { if (inPhase > 20) AutoEvent("奖励页 20 秒没有选项"); return; }
                AutoSend(R"({"cmd":"pick","slot":0})");
                return;
            case tower::Phase::Fighting: {
                auto_.peakPeople = std::max(auto_.peakPeople, AutoPeople());
                AutoKill();
                if (towerentry::Holding() && inPhase > 4.0 && towerwidgets::MenuBusy() && now >= auto_.closeAt) {  // 4.0.0：读秒被原生界面挡住 → 关掉
                    auto_.closeAt = now + 2000;
                    if (const auto closed = towerwidgets::QaCloseBlocking(); !closed.empty()) AutoEvent("关闭挡住读秒的原生界面：" + closed);
                }
                // 4.0.0（用户：一层 1 分钟内）：卡关 300 → 60 秒；生存战按（压缩后的）时长 + 40 秒
                const double stallAt = SpecialOn() && special_.kind == tower::special::Kind::Survival ? special_.survivalSec + 40.0 : 60.0;
                if (inPhase > stallAt) {
                    ++auto_.stalls;
                    AutoEvent(std::format("卡关：第 {} 层战斗 {:.0f} 秒未结束（入场状态 {}，场上 {} 人）→ 跳过本层", ShownNow(), inPhase, towerentry::State(), AutoPeople()));
                    AutoSend(R"({"cmd":"skipfloor"})");
                    auto_.phaseSince = now;
                }
                return;
            }
            case tower::Phase::Lost:
                if (inPhase < 3) return;
                AutoEvent(std::format("第 {} 层殒命（无敌下不应出现）", ShownNow()));
                if (RebirthOffered()) AutoSend(R"({"cmd":"rebirth"})");
                else if (run_.endless) AutoSend(R"({"cmd":"checkpoint"})");
                else { auto_.on = false; qaGod_ = false; AutoEvent("无法继续，停止"); }
                auto_.phaseSince = now;
                return;
            case tower::Phase::Won:
                AutoEvent("本局结束（通关结算），停止"); auto_.on = false; qaGod_ = false; return;
            default: return;
        }
    }
#endif
    // 3.0.1（玩家反馈：某些层（如第 5 层）非常卡，重进游戏就好）：开层前（传送 / 读秒前，画面本来就在切换）记一行对象表大小；
    // 比上次回收多出 3 万个以上就主动做一次垃圾回收（原生 KismetSystemLibrary::CollectGarbage，引擎平时也会自己做，这里只是提前到开层时）。
    // 日志用来确认卡顿是不是随层数累积
    int gcNum_ = 0;
    void FloorGc() {
        const int num = pbz::ue::Num();
        const bool collect = gcNum_ > 0 && num - gcNum_ > 30000;
        if (!gcNum_) gcNum_ = num;
        pbz::log::Info("影塔第 {} 层开层：对象表 {} 个（上次回收时 {} 个）{}", run_.floor + 1, num, gcNum_, collect ? "，做一次垃圾回收" : "");
        if (!collect) return;
        auto* lib = towerwidgets::Default(towerperf::ClassNamed("KismetSystemLibrary"));
        auto* f = lib ? pe::FindFunction(ue::ClassOf(lib), "CollectGarbage") : nullptr;
        if (!f) { pbz::log::Info("影塔垃圾回收：找不到 CollectGarbage，跳过"); gcNum_ = num; return; }
        const uint64_t t0 = towerperf::Qpc();
        std::vector<uint8_t> args(0x20, 0);
        pe::Call(lib, f, args.data());
        gcNum_ = pbz::ue::Num();
        pbz::log::Info("影塔垃圾回收：用时 {:.0f} ms，对象表 {} → {} 个（回收要到下一帧才真正清掉）", towerperf::Ms(towerperf::Qpc() - t0), num, gcNum_);
    }
    void QaAssistTick() {
#if defined(PBZ_HEX_TEST) || defined(PBZ_PLAYTEST)
        MeasureTick();
#endif
#ifdef PBZ_HEX_TEST
        CapTick();
        const bool want = enabled && trip_.phase() == tower::trip::Phase::Inside && (run_.phase == tower::Phase::Ready || run_.phase == tower::Phase::Fighting);
        {  // qa_god：塔内持有无敌 + 秒杀（用户 2026-11-12 测无尽层要求）；出塔 / 关掉即放手
            const bool god = want && qaGod_;
            // 4.0.0 压力烟测：生存战敌方打不死（掉 10% 补满、死了补位），秒杀会让它反复死亡补位 → 生存战里只无敌、不秒杀，撑满时长过关
            const bool kill = god && !(SpecialOn() && special_.kind == tower::special::Kind::Survival);
            if (god != qaGodHeld_ || kill != qaKillHeld_) {
                auto* g = pbz::FindFeature("player.god"); auto* k = pbz::FindFeature("combat.one_hit");
                if (god != qaGodHeld_) { if (god) { if (g) pbz::HoldFeature(g, "tower-qa-god"); } else if (g) pbz::ReleaseFeature(g, "tower-qa-god"); }
                if (kill != qaKillHeld_) { if (kill) { if (k) pbz::HoldFeature(k, "tower-qa-god"); } else if (k) pbz::ReleaseFeature(k, "tower-qa-god"); }
                pbz::log::Info("影塔 QA 辅助：无敌 {}，秒杀 {}", god ? (g ? "开" : "不可用") : "关", kill ? (k ? "开" : "不可用") : "关");
                qaGodHeld_ = god; qaKillHeld_ = kill;
            }
        }
        if (want == qaAssist_) return;
        auto* dmg = pbz::FindFeature("combat.dmg_mul"); auto* stagger = pbz::FindFeature("player.no_stagger");
        if (want) {
            const bool a = dmg && pbz::HoldFeature(dmg, "tower-qa", 10.0), b = stagger && pbz::HoldFeature(stagger, "tower-qa");
            pbz::log::Info("影塔 QA 辅助：伤害 ×10 {}，不硬直 {}", a ? "已开" : "不可用", b ? "已开" : "不可用");
        } else {
            if (dmg) pbz::ReleaseFeature(dmg, "tower-qa");
            if (stagger) pbz::ReleaseFeature(stagger, "tower-qa");
            pbz::log::Info("影塔 QA 辅助：已关闭（离开塔内战斗流程）");
        }
        qaAssist_ = want;
#endif
    }
    bool qaAssist_ = false, qaGod_ = false, qaGodHeld_ = false, qaKillHeld_ = false;

    // ---- 武库扩建（用户 2026-11-11）：背包 4 主 + 4 影，原生仍是 2 格、右下角原生 HUD 不改 ----
    // 一开始原生两格是轮转里的第 1、2 把；每次切换（手上从 a 格换到 b 格）后，把刚换下的 a 格换成轮转里
    // 「手上那把」的下一把。于是按切换键依次轮到 1→2→3→4→1，HUD 始终显示「手上 + 下一把」。
    // 原生换格走 PBZBackPackSubsystem.ReplaceWeaponByIndex / ReplaceShaqiWeaponByIndex（只换已拥有的兵器）
    struct ArsenalRing { std::vector<uint64_t> list; std::vector<std::string> names; int lastIdx = -1, pendingSlot = -1; double pendingAt = 0, quietUntil = 0; bool warned = false; };
    ArsenalRing armMain_, armShaqi_;
    UObject *repMain_ = nullptr, *repShaqi_ = nullptr, *setCur_ = nullptr;
    void ArsenalTick() {
        const bool on = grow_.on && grow_.hexes.Has(tower::hex::Arsenal) && trip_.phase() == tower::trip::Phase::Inside &&
                        (run_.phase == tower::Phase::Ready || run_.phase == tower::Phase::Fighting);
        if (!on) { armMain_ = {}; armShaqi_ = {}; return; }
        UObject* bag = Bag();
        if (!bag || HasSnapshot(bag)) return;  // 剧情 / 试炼临时装备期间不动
        if (!repMain_) repMain_ = pe::FindFunction(bagCls_, "ReplaceWeaponByIndex");
        if (!repShaqi_) repShaqi_ = pe::FindFunction(bagCls_, "ReplaceShaqiWeaponByIndex");
        if (!setCur_) setCur_ = pe::FindFunction(bagCls_, "SetCurrentWeaponByIndex");
        if (!repMain_ || !repShaqi_) { if (!armMain_.warned) pbz::log::Info("影塔武库扩建：原生换格接口缺失，不可用"); armMain_.warned = true; return; }
        ArsenalStep(bag, armMain_, false);
        ArsenalStep(bag, armShaqi_, true);
    }
    void ArsenalBuild(UObject* bag, ArsenalRing& r, bool shaqi) {
        const int kind = shaqi ? 1 : 0;
        const auto equipped = Names(bag, shaqi ? offShaqi_ : offMain_);
        r.list.clear(); r.names.clear();
        auto nameOf = [&](uint64_t n) { for (const auto& w : grow_.profile.weapons) if (tower::growth::kWeapons[w.index].kind == kind && ToName(std::string(tower::growth::kWeapons[w.index].id)) == n) return std::string(tower::growth::kWeapons[w.index].name); return std::string("?"); };
        for (uint64_t n : equipped) if (n && std::find(r.list.begin(), r.list.end(), n) == r.list.end()) { r.list.push_back(n); r.names.push_back(nameOf(n)); }
        for (int w : ArsenalExtras(kind)) {  // 装备页第 3、4 格（玩家选的，或按获得顺序自动补的）
            if (w < 0 || r.list.size() >= 4) continue;
            const uint64_t n = ToName(std::string(tower::growth::kWeapons[w].id));
            if (std::find(r.list.begin(), r.list.end(), n) == r.list.end()) { r.list.push_back(n); r.names.push_back(std::string(tower::growth::kWeapons[w].name)); }
        }
        // 背包里其余已拥有的同类兵器按获得顺序补到 4 把
        for (const auto& w : grow_.profile.weapons) {
            if (r.list.size() >= 4) break;
            const auto& d = tower::growth::kWeapons[w.index];
            if (d.kind != kind || tower::rewards::RetiredWeapon(w.index)) continue;
            const uint64_t n = ToName(std::string(d.id));
            if (std::find(r.list.begin(), r.list.end(), n) != r.list.end()) continue;
            r.list.push_back(n); r.names.push_back(std::string(d.name));
        }
        std::string text; for (const auto& n : r.names) text += (text.empty() ? "" : " → ") + n;
        pbz::log::Info("影塔武库扩建：{}轮转 {} 把：{}{}", shaqi ? "影之武装" : "主武器", r.list.size(), text, r.list.size() < 3 ? "（不足 3 把，不轮转）" : "");
    }
public:
    // 装备页点第 3、4 格 → 打开原生兵器列表（借用原生第 2 格的点击）。原生选完会把兵器换进原生两格之一：
    // 这里记下原生两格与手上序号，看到变化就原样换回，并把新选的兵器放进第 3/4 格（等同 arsenal_set）
    struct NativePick {
        int slot = -1, kind = 0;
        UObject* bag = nullptr;
        int bagIndex = -1;
        std::vector<uint64_t> snap;
        int idx = -1;
        double until = 0;
    } nativePick_;
    bool ArsenalNativeBegin(int kind, int k) {
        nativePick_ = {};
        if (kind < 0 || kind > 1 || k < 0 || k > 1 || run_.phase != tower::Phase::Ready ||
            !grow_.on || !grow_.hexes.Has(tower::hex::Arsenal) ||
            trip_.phase() != tower::trip::Phase::Inside) return false;
        UObject* bag = Bag();
        const int off = kind ? offShaqi_ : offMain_, offIdx = kind ? offShaqiIdx_ : offMainIdx_;
        auto* replace = kind ? repShaqi_ : repMain_;
        if (!bag || !ue::Live(bag) || HasSnapshot(bag) || off < 0 || offIdx < 0 || !replace || !setCur_ ||
            ue::PropOffset(replace, kind ? "NewName" : "NewWeaponName") < 0 ||
            ue::PropOffset(replace, "ReplaceIndex") < 0 || ue::PropOffset(setCur_, "WeaponIndex") < 0 ||
            ue::PropOffset(setCur_, "IsNormalWeapon") < 0) return false;
        auto snapshot = Names(bag, off);
        const int current = ue::Get<int32_t>(bag + offIdx, -1);
        const int bagIndex = ue::Get<int>(bag + ue::Layout::ObjIndex, -1);
        if (snapshot.size() != 2 || current < 0 || current > 1 || bagIndex < 0) return false;
        nativePick_ = {kind * 2 + k, kind, bag, bagIndex, std::move(snapshot), current, now_ + 120.0};
        return true;
    }
private:
    void ArsenalPickWatch() {
        auto& np = nativePick_;
        if (np.slot < 0) return;
        if (now_ > np.until || run_.phase != tower::Phase::Ready || trip_.phase() != tower::trip::Phase::Inside ||
            !grow_.on || !grow_.hexes.Has(tower::hex::Arsenal)) { np = {}; return; }
        UObject* bag = Bag();
        if (!bag || bag != np.bag || !ue::Live(bag) || HasSnapshot(bag) ||
            ue::Get<int>(bag + ue::Layout::ObjIndex, -1) != np.bagIndex) { np = {}; return; }
        const bool shaqi = np.kind == 1;
        const int off = shaqi ? offShaqi_ : offMain_, offIdx = shaqi ? offShaqiIdx_ : offMainIdx_;
        const auto cur = Names(bag, off);
        if (cur == np.snap) return;
        const auto pick = std::exchange(np, NativePick{});
        const int changed = tower::arsenal::ReplacedSlot(pick.snap, cur);
        if (changed < 0) {
            pbz::log::Info("影塔武库扩建：原生装备发生非单槽变化，未保存新增槽");
            return;
        }
        const uint64_t chosen = cur[changed];
        UObject* fn = shaqi ? repShaqi_ : repMain_;
        std::vector<uint8_t> buf(0x80, 0);
        auto* oldName = Arg(fn, buf, shaqi ? "NewName" : "NewWeaponName");
        auto* oldSlot = Arg(fn, buf, "ReplaceIndex");
        std::memcpy(oldName, &pick.snap[changed], 8);
        std::memcpy(oldSlot, &changed, 4);
        pe::Call(bag, fn, buf.data());
        if (setCur_ && ue::Get<int32_t>(bag + offIdx) != pick.idx && pick.idx >= 0) {
            std::vector<uint8_t> c(0x40, 0); const int32_t keep = pick.idx; std::memcpy(Arg(setCur_, c, "WeaponIndex"), &keep, 4); *Arg(setCur_, c, "IsNormalWeapon") = shaqi ? 0 : 1;
            pe::Call(bag, setCur_, c.data());
        }
        const bool restored = Names(bag, off) == pick.snap && ue::Get<int32_t>(bag + offIdx, -1) == pick.idx;
        if (!restored) {
            pbz::log::Info("影塔武库扩建：原生两格复原失败，未保存新增槽");
            return;
        }
        int w = -1;
        for (const auto& pw : grow_.profile.weapons) if (tower::growth::kWeapons[pw.index].kind == pick.kind && ToName(std::string(tower::growth::kWeapons[pw.index].id)) == chosen) w = pw.index;
        if (w < 0 || tower::rewards::RetiredWeapon(w)) {
            pbz::log::Info("影塔武库扩建：原生两格已复原，所选兵器不在可用清单");
            return;
        }
        if (!BackupForSeed()) {
            pbz::log::Info("影塔武库扩建：塔档备份失败，未保存新增槽");
            return;
        }
        const auto previous = grow_.arsenal;
        for (int k = 0; k < 4; ++k) if (k != pick.slot && grow_.arsenal[static_cast<size_t>(k)] == w) grow_.arsenal[static_cast<size_t>(k)] = -1;
        grow_.arsenal[static_cast<size_t>(pick.slot)] = w;
        if (!SaveHex()) {
            grow_.arsenal = previous;
            pbz::log::Info("影塔武库扩建：塔档写入失败，已恢复新增槽");
            return;
        }
        armMain_ = {}; armShaqi_ = {};
        pbz::log::Info("影塔武库扩建：{}第 {} 格 = {}（原生列表选择，原生两格已复原）", shaqi ? "影之武装" : "主武器", pick.slot % 2 + 3, tower::growth::kWeapons[w].name);
    }
public:
    // 装备页第 3、4 格：玩家选过且仍持有、不是原生两格里的 → 用选的；否则按获得顺序自动补
    std::array<int, 2> ArsenalExtras(int kind) {
        std::array<int, 2> out{-1, -1};
        if (!grow_.on || !grow_.hexes.Has(tower::hex::Arsenal) || kind < 0 || kind > 1) return out;
        std::vector<int> taken;
        if (UObject* bag = Bag()) for (uint64_t n : Names(bag, kind ? offShaqi_ : offMain_))
            for (const auto& w : grow_.profile.weapons) if (tower::growth::kWeapons[w.index].kind == kind && ToName(std::string(tower::growth::kWeapons[w.index].id)) == n) taken.push_back(w.index);
        auto usable = [&](int w) { return w >= 0 && w < tower::growth::kWeaponCount && tower::growth::kWeapons[w].kind == kind && grow_.profile.Find(w) >= 0 &&
                                          !tower::rewards::RetiredWeapon(w) && std::find(taken.begin(), taken.end(), w) == taken.end(); };
        for (int k = 0; k < 2; ++k) if (const int w = grow_.arsenal[static_cast<size_t>(kind * 2 + k)]; usable(w)) { out[static_cast<size_t>(k)] = w; taken.push_back(w); }
        for (int k = 0; k < 2; ++k) {
            if (out[static_cast<size_t>(k)] >= 0) continue;
            for (const auto& w : grow_.profile.weapons) if (usable(w.index)) { out[static_cast<size_t>(k)] = w.index; taken.push_back(w.index); break; }
        }
        return out;
    }
private:
    void ArsenalStep(UObject* bag, ArsenalRing& r, bool shaqi) {
        const int offIdx = shaqi ? offShaqiIdx_ : offMainIdx_, offList = shaqi ? offShaqi_ : offMain_;
        if (r.list.empty()) { ArsenalBuild(bag, r, shaqi); r.lastIdx = -1; return; }
        if (r.list.size() < 3) return;
        int idx = ue::Get<int32_t>(bag + offIdx);
        if (shaqi) if (auto* p = Player()) if (UObject* comp = ArcaneWeapons(p->obj)) {  // 影之武装：背包序号不跟切换走，读武器组件
            const int ci = SlotIndex(comp, WeaponSlotTags(comp)[1]);
            if (ci >= 0 && ci <= 1) idx = ci;
        }
        if (now_ < r.quietUntil) { r.lastIdx = idx; return; }  // 刚换过格：原生回调可能改序号，这段时间只跟随
        // 用户 2026-11-11 实机日志：自动重开 / 传送时原生重设手上序号，被当成玩家切换、换掉了一格。只在正式战斗中跟随玩家切换
        if (run_.phase != tower::Phase::Fighting || fightSince_ <= 0 || towerentry::Holding()) { r.lastIdx = idx; r.pendingSlot = -1; return; }
        if (idx != r.lastIdx && idx >= 0 && idx <= 1 && r.lastIdx >= 0 && r.lastIdx <= 1) { r.pendingSlot = r.lastIdx; r.pendingAt = now_ + 0.35; }  // 等切换动作播完
        r.lastIdx = idx;
        if (r.pendingSlot < 0 || now_ < r.pendingAt) return;
        const int slot = r.pendingSlot; r.pendingSlot = -1;
        const auto cur = Names(bag, offList);
        if (cur.size() != 2 || idx < 0 || idx > 1 || slot == idx) return;
        const auto at = std::find(r.list.begin(), r.list.end(), cur[static_cast<size_t>(idx)]);
        if (at == r.list.end()) { ArsenalBuild(bag, r, shaqi); return; }  // 手上的不在轮转里（玩家在装备页换过）：按现在的装备重建
        const size_t next = (static_cast<size_t>(at - r.list.begin()) + 1) % r.list.size();
        if (r.list[next] == cur[static_cast<size_t>(slot)]) return;
        UObject* fn = shaqi ? repShaqi_ : repMain_;
        std::vector<uint8_t> buf(0x80, 0);
        std::memcpy(Arg(fn, buf, shaqi ? "NewName" : "NewWeaponName"), &r.list[next], 8);
        const int32_t s32 = slot; std::memcpy(Arg(fn, buf, "ReplaceIndex"), &s32, 4);
        const int before = ue::Get<int32_t>(bag + offIdx);
        pe::Call(bag, fn, buf.data());
        const int after = ue::Get<int32_t>(bag + offIdx);
        if (after != before && setCur_) {  // 换格把手上的也换掉了：切回原来手上那格
            std::vector<uint8_t> c(0x40, 0); const int32_t keep = before; std::memcpy(Arg(setCur_, c, "WeaponIndex"), &keep, 4); *Arg(setCur_, c, "IsNormalWeapon") = shaqi ? 0 : 1;
            pe::Call(bag, setCur_, c.data());
        }
        r.quietUntil = now_ + 0.5;
        pbz::log::Info("影塔武库扩建：{}切到第 {} 格「{}」，第 {} 格换成下一把「{}」{}", shaqi ? "影之武装" : "主武器", idx + 1,
                       r.names[static_cast<size_t>(at - r.list.begin())], slot + 1, r.names[next], after != before ? std::format("（原生改了手上序号 {}→{}，已切回）", before, after) : "");
    }
    static bool AssistOn() {
        for (const char* id : {"player.god", "player.demigod", "player.inf_hp"})
            if (auto* f = pbz::FindFeature(id); f && f->enabled) return true;
        return false;
    }            // 自己正常结束时为真：Disable 只关开关，不作废本局
    std::string controller_, resumeClient_, lastCode_;  // 控制这一局的界面；最近一条状态消息的代码
    uint64_t rev_ = 0;               // 状态版本，每次广播 +1
    bool clampHp_ = false;
    float hpMulSeen_ = 0;   // 上一帧的生命奖励倍率（升高 → 准备阶段补满当前生命）
    double hpFillAt_ = 0;
    tower::growthrun::Runner grow_;
    tower::trip::Trip trip_;                                 // 进出独立塔档（tower-session.txt）
    unsigned long long nextTripTick_ = 0;
    bool poolAudit_ = false;                                 // true：开发审计用全静态池（仍排除禁用）
    std::string resumePool_;                                 // 续局命令里显式写的 pool（空 = 沿用局存档）
    int fixedBoss_ = -1;                                     // 开发验收：每层固定这只 Boss
    std::string venuePath_;                                  // 本层固定场地（审计计划 @rongshu）；空 = 用 venueBoss_ 的原生场地
    // 无尽混编层（审计计划 @endless，tower_endless.h）：本层波次表、当前波、本层场地 Boss、小怪队列 / 在途 / 待回执、兜底次数、下一波到点
    std::vector<tower::endless::Wave> endless_;
    int endlessWave_ = 0, endlessAnchor_ = -1, endlessMobFlight_ = 0, endlessMobPending_ = 0, endlessRetries_ = 0, endlessMobRetries_ = 0;
    bool endlessRegionOff_ = false;  // 4.0.0：本层场内安全区放不下 Boss → 后面的 Boss 改回玩家附近生成
    int bossPlaced_ = 0;             // 4.0.0：本次入场把雾门外的 Boss 放回场内的次数
    double endlessWaveStart_ = -1;  // 3.0.1：本波开战时刻（到齐后 3 秒读秒）
    double endlessNextAt_ = -1, endlessMobAt_ = 0, endlessGuardUntil_ = -1, motianHoldUntil_ = -1;  // 换波入场 / 魔渊对话：玩家无敌、其他怪定身
    std::vector<int> endlessMobQ_;
    bool EndlessOn() const { return !endless_.empty(); }
    bool MobActor(const Actor& a) const { return run_.inMobWave || (EndlessOn() && a.mobIndex >= 0); }
    // 3.0.0（用户反馈"打完小兵后一直不弹结算，卡一会又刷出新的一轮"）：小怪被秒杀 / 尸体很快回收时，塔可能来不及采样到生命归零，
    // 战斗目标消失后按"异常消失"补位。观察原生 BP_DeathComponent.On Death（新状态 = 该组件自己的 Dead State Tag）记下真实死亡，
    // 作为补充死亡证据。按对象序号核对，防止指针被新 Actor 复用时误判
    struct NativeDeath { double at; int index; };
    std::unordered_map<UObject*, NativeDeath> nativeDeaths_;
    int deathObs_ = 0;
    void WatchNativeDeaths() {
        if (deathObs_) return;
        auto* cls = towerperf::ClassNamed("BP_DeathComponent_C");
        auto* fn = cls ? pe::FindFunction(cls, "On Death") : nullptr;
        const int tagArg = fn ? ue::PropOffset(fn, "NewState") : -1, deadTag = cls ? ue::PropOffset(cls, "Dead State Tag") : -1;
        if (!fn || tagArg < 0 || deadTag < 0) return;
        deathObs_ = pe::Observe(fn, [this, tagArg, deadTag](UObject* comp, void* params) {
            if (!comp || !params || !ue::Live(comp)) return;
            const uint64_t got = *reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(params) + tagArg);
            if (!static_cast<uint32_t>(got) || got != ue::Get<uint64_t>(comp + deadTag)) return;
            auto* owner = ue::Get<UObject*>(comp + ue::Layout::ObjOuter);
            if (!owner || !ue::Live(owner)) return;
            if (nativeDeaths_.size() > 512) nativeDeaths_.clear();
            nativeDeaths_[owner] = {now_, ue::Get<int>(owner + ue::Layout::ObjIndex, -1)};
        });
        pbz::log::Info("影塔死亡观察：已挂 BP_DeathComponent.On Death（小怪死亡补充证据）");
    }
    bool DiedNatively(const Actor& a) const {
        const auto it = nativeDeaths_.find(a.obj);
        if (it == nativeDeaths_.end() || it->second.at < fightSince_ - 1.0) return false;  // 只认本层开战后
        return !ue::Live(a.obj) || ue::Get<int>(a.obj + ue::Layout::ObjIndex, -1) == it->second.index;
    }
    int MobScaleFloor() const { return run_.inMobWave ? mob_.floor : std::max(1, run_.floor); }
    bool EndlessFormal() const { return run_.endless && run_.floor > 50 && EndlessOn(); }
    // 无尽层奖励交替：显示层为偶数（100、102 …）领奖励页，奇数（101、103 …）开驿站
    bool EndlessShopTurn() const { return ShownBoss(run_.floor) % 2 == 1; }
    // 无尽层数值：以第 49 层单 Boss 目标（419162 血 / 伤害 ×3.829，逐层表最后一个单挑层）为底，按本波 Boss 数分摊；
    // 层数增长交给 DiffHp / DiffDmg 里的 E(s)（显示 100 层后每层 +4%）
    double EndlessHpTarget() const { return 419162.0 * BossHpShare(); }
    // 多 Boss 时每只的血量份额：创意层（生存战除外）2 只及以上按本层单 Boss × 0.85（4.0.0 为 0.6；4.1.0 用户 2026-11-16 改），其余照旧按人数分摊
    // 4.1.0（用户：没秒杀 Boss、这层打完了也很久不弹结算）：
    //   1. 回执超时：场上已没有活着的敌人，但生成回执 20 秒还没到（无尽小怪 / 无尽补刷 Boss）→ 当这只没刷出来、跳过（原来没有超时，永远不结算）。
    //   2. 残余敌人：只剩 ≤3 只、15 秒没人掉血（没被唤醒站在大场地角落 / 卡在地形里）→ 补发"玩家进入领域"唤醒；小怪再拉到玩家身边（落点做地面检测）。
    //      Boss 原生机制不改，只唤醒不搬；主线 Boss 层不处理（屏幕有 Boss 血条）。HUD 计时器下显示"剩余敌人 N"，日志写名字 / 血量 / 距离便于定位。
    //   全部只处理"还活着 / 没刷出来"，不判任何敌人死亡、不白送击杀。
    double strayCheckAt_ = 0, strayQuietSince_ = -1, strayActAt_ = 0, receiptWaitSince_ = -1;
    float strayHpSum_ = -1;
    int stragglers_ = 0;
    void StragglerReset() { strayQuietSince_ = -1; strayHpSum_ = -1; stragglers_ = 0; receiptWaitSince_ = -1; strayActAt_ = 0; }
    void StragglerTick(bool anyAlive) {
        if (run_.phase != tower::Phase::Fighting || fightSince_ <= 0 || towerentry::Holding() || deathAt_ >= 0 || replay_ || fightPaused_ ||
            (SpecialOn() && special_.kind == tower::special::Kind::Survival)) { StragglerReset(); return; }
        if (now_ < strayCheckAt_) return;
        strayCheckAt_ = now_ + 0.5;
        // 1. 回执超时
        const bool waiting = (EndlessOn() && pending_ > 0) || endlessMobFlight_ > 0;
        if (!anyAlive && waiting && spawnQueue_.empty()) {
            if (receiptWaitSince_ < 0) receiptWaitSince_ = now_;
            else if (now_ - receiptWaitSince_ >= 20.0) {
                pbz::log::Info("影塔第 {} 层：场上已没有敌人，生成回执 20 秒未到（Boss {} / 小怪 {}），按没刷出来跳过", run_.floor, pending_, endlessMobFlight_);
                if (endlessMobFlight_ > 0) { endlessMobPending_ = std::max(0, endlessMobPending_ - endlessMobFlight_); endlessMobFlight_ = 0; endlessMobAt_ = now_; }
                if (EndlessOn() && pending_ > 0) pending_ = 0;
                receiptWaitSince_ = -1;
            }
        } else receiptWaitSince_ = -1;
        // 2. 残余敌人
        std::vector<Actor*> live; float sum = 0;
        for (auto& a : actors_) {
            if (a.dead || a.gone || !a.hasTarget || !attr::Alive(a.target) || !ue::Live(a.obj)) continue;
            const bool mob = a.mobHp > 0;
            if (!mob && !EndlessOn()) continue;  // 主线 Boss：不管
            const float hp = ue::Get<float>(attr::AttrCurrent(a.target, hp_), -1.0f);
            if (!std::isfinite(hp) || hp <= 0.5f) continue;
            live.push_back(&a); sum += hp;
        }
        const bool countdown = EndlessOn() && now_ < endlessWaveStart_;
        if (live.empty() || live.size() > 3 || countdown || towerstorygate::Cinematic() ||
            std::any_of(actors_.begin(), actors_.end(), [&](const Actor& x) { return !x.dead && !x.gone && x.mobHp <= 0 && !EndlessOn(); })) {
            strayQuietSince_ = -1; strayHpSum_ = -1; stragglers_ = 0; return;
        }
        if (strayQuietSince_ < 0 || strayHpSum_ < 0 || sum < strayHpSum_ - 0.5f) strayQuietSince_ = now_;  // 有人掉血 = 还在打
        strayHpSum_ = sum;
        const double quiet = now_ - strayQuietSince_;
        stragglers_ = quiet >= 15.0 ? static_cast<int>(live.size()) : 0;
        if (quiet < 15.0 || now_ < strayActAt_) return;
        strayActAt_ = now_ + 15.0;
        double pl[3] = {};
        const bool havePlayer = PawnLoc(pl);
        for (Actor* a : live) {
            const int r = towernative::EnterDomain(a->obj);
            uint8_t b[32] = {}; double v[3] = {};
            if (fnLoc_) { pe::Call(a->obj, fnLoc_, b); std::memcpy(v, b, 24); }
            const double dist = havePlayer ? std::hypot(v[0] - pl[0], v[1] - pl[1]) / 100.0 : -1;
            std::string moved = "不搬";
            if (a->mobHp > 0 && havePlayer && fnLoc_ && dist > 8.0) {
                const double dx = v[0] - pl[0], dy = v[1] - pl[1], d = std::max(1.0, std::hypot(dx, dy));
                const double nearAt[3] = {pl[0] + dx / d * 600, pl[1] + dy / d * 600, pl[2] + 50};
                double landing[3]; std::string why;
                const bool inWall = !wall_.ok || !tower::wall::Outside(wall_, {nearAt[0], nearAt[1], nearAt[2]});
                if (inWall && towerreach::Landing(a->obj, nearAt, landing, &why, true) && MoveActor(a->obj, {landing[0], landing[1], landing[2]})) moved = "已拉到玩家身边";
                else moved = "拉近失败" + (why.empty() ? std::string() : "：" + why);
            }
            pbz::log::Info("影塔第 {} 层：剩余敌人 {} 15 秒没掉血（生命 {:.0f}，离玩家 {:.1f} 米，位置 {:.0f}/{:.0f}/{:.0f}），唤醒{}，{}", run_.floor, a->name,
                           ue::Get<float>(attr::AttrCurrent(a->target, hp_), 0.0f), dist, v[0], v[1], v[2], r == 1 ? "已发" : "失败", moved);
        }
    }
    // 4.1.0（用户 2026-11-16）：前 50 层的创意层 Boss 基本属性（生命 / 攻击 / 杀气上限 / 破防）为该层的 50%；生存战敌方攻击 ×0.3（50 层后仍 ×2）
    // 4.1.0（用户 2026-11-16：创意层 Boss 血少攻击低、没威胁）：主线创意层（小兵段 F）数值并入成长曲线，当作第 F 层 Boss 层
    // （显示 2F−1）——取逐层表该层的单 Boss 目标（表值已含层型份额，除回去），再乘个别 Boss 修正；原来用的是早期调参公式（约 Boss 层 1/4 血、1/2 攻击）
    const tower::floorplan::Floor* SpecialMainDef() const {
        return specialMain_ && run_.specialStage && specialF_ > 0 && run_.floors >= 50 ? tower::floorplay::Find(specialF_) : nullptr;
    }
    int StatFloor() const { return SpecialMainDef() ? specialF_ : run_.floor; }
    double SpecialMainHp(int b) const { return SpecialMainDef() ? tower::floorplay::SoloHp(specialF_) * tower::floorplay::BossAdj(b).hp : 0.0; }
    double SpecialMainDmg(int b) const { return SpecialMainDef() ? tower::floorplay::SoloDmg(specialF_) * tower::floorplay::BossAdj(b).dmg * BossDmgShare() : 0.0; }
    double SurvivalEnemyAtk() const { return special_.early ? tower::special::kEarlySurvivalAtk : tower::special::kSurvivalEnemyAtk; }
    double BossHpShare() const {
        if (SpecialOn() && special_.kind != tower::special::Kind::Survival && count_ >= 2) return tower::special::kMultiBossHp;
        return tower::ShareHp(count_);
    }
    // 4.1.0：创意层 2 只及以上 Boss 每只攻击 ×0.85（生存战也按这个，再乘生存倍率），混战 ×1.2（4.1.1）；其他层照旧按人数分摊
    double BossDmgShare() const {
        if (SpecialOn() && special_.kind == tower::special::Kind::Melee) return tower::special::kMeleeDmg;  // 4.1.1 混战 ×1.2
        return SpecialOn() && count_ >= 2 ? tower::special::kMultiBossDmg : tower::ShareDmg(count_);
    }
    double EndlessDmgMul() const { return 3.829 * BossDmgShare(); }
    void EndlessReset() { SpecialReset(); endless_.clear(); endlessWave_ = 0; endlessAnchor_ = -1; endlessMobFlight_ = endlessMobPending_ = endlessRetries_ = endlessMobRetries_ = 0; endlessNextAt_ = endlessGuardUntil_ = motianHoldUntil_ = endlessWaveStart_ = -1; endlessMobQ_.clear(); endlessRegionOff_ = false; preloadedWave_ = -1; }
    std::string verifiedText_, deniedText_;                  // 本机准入增补 / 禁用表
    std::string lastTripError_;
    std::string settlement_;
    std::string bonusText_;                                  // 本层额外奖励说明（奖励页显示）
    std::array<tower::floorplay::Challenge, 51> chal_{};     // 本局附加挑战分布（按种子固定）
    std::string floorNote_, lastTravelWhy_;
    bool restartAfterExit_ = false;
    double storyWaitSince_ = -1;  // 开打前等剧情保护的起点
    bool storyDegraded_ = false;  // 本次运行剧情保护不可用、已降级（只提示一次）
    double deathAt_ = -1;  // 殒命表现开始时刻（-1 = 没有）
    int errSeq_ = 0; std::string errMsg_;  // 最近一条给玩家看的错误（游戏内界面弹框）
  public:
    std::string LastError(int* seq) const { *seq = errSeq_; return errMsg_; }
  private:
    int skippedFloors_ = 0;  // 本次运行跳过的坏层数（日志 / 诊断）  // 死亡结算点了"重新开始"：出塔回到原档后自动开新局
    double origin_[3] = {}, pushCooldown_ = 0;
    tower::arena::Boundary boundary_;
    bool originSet_ = false;                  // 上一次开层没成功的原因（准备页顶部显示）
  public:
    const std::string& FloorNote() const { return floorNote_; }
    bool CanSkipFailedStage() const { return FailedStageAvailable(); }
    bool FailedStageIsWave() const { return FailedStageAvailable() && failedStage_.wave; }
    std::string NextBossPreview() const {
        if (run_.items[static_cast<int>(tower::Item::Foresight)] <= 0 || run_.phase != tower::Phase::Ready) return {};
        const int nextFloor = run_.floor + 1;
        // 用户 2026-11-12：透视眼只透视下一层全部 Boss（不看小怪）；无尽层按同一种子预生成的波次列出每一波的 Boss
        if (run_.endless && nextFloor > 50 && plan_.empty() && fixedBoss_ < 0) {
            const auto waves = tower::endless::Generate(run_.seed, nextFloor, EndlessPools());
            std::string s;
            for (size_t w = 0; w < waves.size(); ++w) {
                std::string names;
                for (int b : waves[w].bosses) names += (names.empty() ? "" : "、") + std::string(tower::kBossPool[b].name);
                if (names.empty()) continue;
                s += (s.empty() ? "" : "；") + (waves.size() > 1 ? std::format("第 {} 波 ", w + 1) : std::string()) + names;
            }
            return s.empty() ? "下一场 Boss：暂无法确定" : "下一场 Boss：" + s;
        }
        if (run_.floor >= run_.floors) return {};
        std::vector<int> bosses;
        if (fixedBoss_ >= 0) bosses.push_back(fixedBoss_);
        else if (!plan_.empty() && static_cast<size_t>(run_.floor) < plan_.size()) bosses = plan_[run_.floor].bosses;
        else if (run_.floors >= 50 && tower::floorplay::Find(run_.floor + 1)) {
            tower::floorplan::Floor def{};
            const auto resolved = ResolveScheduled(run_.floor + 1, def, false);
            if (!resolved.ok) return "下一场 Boss：暂无法确定";
            bosses = resolved.bosses;
        } else return "下一场 Boss：暂无法确定";
        std::string names;
        for (int b : bosses) if (b >= 0 && b < kPool) names += (names.empty() ? "" : "、") + std::string(tower::kBossPool[b].name);
        return names.empty() ? "下一场 Boss：暂无法确定" : "下一场 Boss：" + names;
    }
  private:                                 // 通关 / 失败那一刻的结算快照（结算页显示，出塔前不变）
    tower::trip::Step lastStep_ = tower::trip::Step::None;     // 上次广播的切档步骤
    int lastFlushed_ = 0;
    unsigned long long nextSettleLog_ = 0;
    bool nativePoints_ = true, dustGate_ = true, resumeAuto_ = true;
    std::vector<tower::plan::Floor> plan_;                   // 开发审计计划（空 = 正式抽取）
    struct Queued { double at; int boss; };
    double staggerBase_ = 0;
    double stateLogUntil_ = 0, stateLogNext_ = 0;
    double storyNextAsk_ = 0, storySince_ = 0; bool storyLocked_ = false, storyTimedOut_ = false;  // 多 Boss 层开场台词定身（剧情镜头锁）                                 // 错峰队列的排队时刻（开战时按它重排）
    std::vector<Queued> spawnQueue_;                         // 错峰入场：还没进场的
    uint64_t lastTickQpc_ = 0, hitchWindow_ = 0; double lastTickMs_ = 0; int hitchLogs_ = 0;  // 卡顿记录
    bool staggerFloor_ = false;                         // 原生成长（v2）：塔档、进出塔会话、待选药 / 饰品
    std::string gains_;                                      // 最近一层的收获文字
    double normSince_ = -1, normRaw_[3] = {0, 0, 0};  // 统一起点实测：属性稳定起点时刻与上次读数
    double baseAtk_ = 0, baseHp_ = 0, growHpComp_ = 1, growDmgComp_ = 1, nextReconcile_ = 0;
    bool failed_ = false;
    std::vector<UObject*> before_;  // 开层时已存在的生成物（含修改器刷的），不算本层 Boss
    std::vector<int> taken_, dealt_, posture_;
    double now_ = 0, clearAt_ = 0, fightSince_ = 0;
    bool fightPaused_ = false;  // 战斗中暂停 / 菜单（时钟停走）
    bool healPending_ = false;
    double minHpFrac_ = 1.0;   // 本层玩家最低血量比例
    // 塔内换兵器
    UObject *bagCls_ = nullptr, *strLib_ = nullptr, *strCdo_ = nullptr, *bag_ = nullptr;
    UObject *pushFn_ = nullptr, *popFn_ = nullptr, *hasFn_ = nullptr, *toName_ = nullptr;
    int offMain_ = -1, offShaqi_ = -1, offMainIdx_ = -1, offShaqiIdx_ = -1, offSnap_ = -1;
    bool pushed_ = false, backedUp_ = false;
    bool sched_ = false, schedOff_ = false, venueArrived_ = false;  // 本层按 50 层方案逐层表（tower_floorplay.h）
    tower::floorplay::Resolved plan_f_;                    // 本层方案解析结果（层型 / Boss / 场地 / 份额 / 标准限时）
    tower::floorplay::RelayQueue relay_;                   // 车轮 / 连战 / 塔主的后续 Boss
    tower::floorplay::Fight fight_;                        // 本层战斗记录（用时 / 受伤 / 喝药）
    float lastHp_ = -1; double nextHpSample_ = 0;
    bool arenaOn_ = true, travelThenSpawn_ = false, noRoom_ = false, travelled_ = false;  // 场地传送：开局 "arena":"off" 关掉
    double spawnAt_ = 0, nextSweep_ = 0;                    // 到场地后延迟生成的时刻（0 = 没有）；下次清场地原有敌群
    UObject *npcCls_ = nullptr, *fnLoc_ = nullptr, *fnDestroy_ = nullptr;
    int offMonsterNpc_ = -1, swept_ = 0, offOwner_ = -1, offInstigator_ = -1;
    bool sweepWarned_ = false;
    int spawnTries_ = 0, repicks_ = 0, venueBoss_ = 0;
    int stayVenue_ = -1, stayFloor_ = -1;
    bool spawnerKicked_ = false;  // 本层已对契约刷怪点请求过原生刷出
    double replayWaitSince_ = -1;  // 重放入场条件未就绪时开始等待的时刻
    int medUses_ = -1, lastSubmitBoss_ = -1, relayRetries_ = 0;
    bool berserkFired_ = false, bonusEarned_ = false; int bonusFloor_ = -1; std::string bonusRun_;
    bool nativeTried_ = false, earlyHeld_ = false; double nativeAdoptUntil_ = 0; int nativeAdoptBoss_ = -1;  // 原生刷怪点兜底（每次入场一次）
    double phaseDeadAt_ = -1, phaseTravelAt_ = -1; int phaseTravelFor_ = -1; bool phaseKicked_ = false;  // 跨空间转阶段（荒行子）  // 同层换 Boss 重抽时留在已到的场地（35 层）
    unsigned long long diagAt_ = 0;
    std::unordered_set<UObject*> placeholderLogged_;     // 已记过的过场占位对象（F37）
    std::unordered_set<int> badBosses_, venueBad_;          // 本局生成失败 / 场地去不了的 Boss，不再抽
    std::vector<uint64_t> origMain_, origShaqi_;
    int origMainIdx_ = 0, origShaqiIdx_ = 0;
};

Tower* g_tower = nullptr;  // 所有权在运行库（AddFeature）；模块永不卸载，指针一直有效

}  // namespace

namespace towerapi {
const tower::Run& Run() { static const tower::Run idle; return g_tower ? g_tower->RunState() : idle; }
bool HasSave() { return Tower::SaveExists(); }
bool ClimbAvailable() { return Tower::ClimbAvailable(); }
int EndlessNext() { return Tower::EndlessNext(); }
bool EndlessImport(std::string* author, int* shown, int* difficulty) {
    tower::endlessshare::Share sh;
    if (!Tower::EndlessImportInfo(&sh)) return false;
    if (author) *author = sh.author; if (shown) *shown = sh.shown; if (difficulty) *difficulty = sh.difficulty;
    return true;
}
int ImportedSeed(uint64_t* seed, int* difficulty) { return Tower::ImportedSeed(seed, difficulty); }
void Command(const std::string& jsonLine) { if (g_tower) g_tower->Command(jsonLine); }
std::string Effects() { return g_tower ? g_tower->Effects() : std::string(); }
void Housekeep() { if (g_tower) g_tower->Housekeep(); }
const tower::growthrun::Runner& Growth() { static const tower::growthrun::Runner none; return g_tower ? g_tower->Growth() : none; }
std::string Gains() { return g_tower ? g_tower->Gains() : std::string(); }
int TripPhase() { return g_tower ? static_cast<int>(g_tower->TripPhase()) : 0; }
std::string TripText() { return g_tower ? g_tower->TripText() : std::string(); }
bool TripFailed() { return g_tower && g_tower->TripFailed(); }
std::string Settlement() { return g_tower ? g_tower->SettlementText() : std::string(); }
bool RebirthOffered() { return g_tower && g_tower->RebirthOffered(); }
int CheckpointShown() { return g_tower ? g_tower->CheckpointShown() : 0; }
int RebirthCount() { return g_tower ? g_tower->RebirthCount() : 0; }
int RebirthTarget() { return g_tower ? g_tower->RebirthTarget() : 0; }
int RebirthNext() { return g_tower ? g_tower->RebirthNext() : 0; }
int ShownNow() { return g_tower ? g_tower->ShownNow() : 0; }
std::vector<int> TradeEquipped() { return g_tower ? g_tower->TradeEquipped() : std::vector<int>{}; }
int TradeWeaponPrice() { return g_tower ? g_tower->TradeWeaponPrice() : 0; }
tower::trade::Prices TradePrices() { return g_tower ? g_tower->TradePrices() : tower::trade::Prices{}; }
bool TemperMasterHeld() { return g_tower && g_tower->Growth().on && g_tower->Growth().hexes.Has(tower::hex::TemperMaster); }
int TemperLevels(const std::string& id, int kind, int levels) { return g_tower ? g_tower->TemperLevels(id, kind, levels) : levels; }
float MeltMulOf(const std::string& id, int kind) {
    if (!g_tower || !g_tower->Growth().on) return 1.0f;
    int w = tower::growth::WeaponIndex(id, kind);
    if (w < 0) {  // N9m：原生有 HeiMoShangPrologue / XXX_Evolution 这类变体 ID，去掉后缀再找
        std::string base = id;
        for (const char* suf : {"_Evolution", "Evolution", "_Prologue", "Prologue"})
            if (base.size() > std::strlen(suf) && base.compare(base.size() - std::strlen(suf), std::string::npos, suf) == 0) { base.resize(base.size() - std::strlen(suf)); break; }
        w = tower::growth::WeaponIndex(base, kind);
    }
    const int n = w >= 0 ? g_tower->Growth().profile.MeltCount(w) : 0;
    return n > 0 ? static_cast<float>(tower::trade::MeltMul(n)) : 1.0f;
}
float MeltPowerMul(void* cfg) { return g_tower ? g_tower->MeltPowerMul(static_cast<UObject*>(cfg)) : 1.0f; }
float ShadowEdgeMul() { return g_tower ? g_tower->ShadowEdgeMul() : 1.0f; }
std::string TradeNote() { return g_tower ? g_tower->tradeNote_ : std::string(); }
int ShownNext() { return g_tower ? g_tower->ShownNext() : 0; }
std::string FloorNote() { return g_tower ? g_tower->FloorNote() : std::string(); }
bool FailedStageAvailable() { return g_tower && g_tower->CanSkipFailedStage(); }
bool FailedStageIsWave() { return g_tower && g_tower->FailedStageIsWave(); }
std::string NextBossPreview() { return g_tower ? g_tower->NextBossPreview() : std::string(); }
std::string LastError(int* seq) { if (!g_tower) { *seq = 0; return {}; } return g_tower->LastError(seq); }
std::string HudFloor() { return g_tower ? g_tower->HudFloor() : std::string(); }
std::string HudClock() { return g_tower ? g_tower->HudClock() : std::string(); }
std::string HudGoal() { return g_tower ? g_tower->HudGoal() : std::string(); }
std::string BonusText() { return g_tower ? g_tower->BonusText() : std::string(); }
std::string TimesText() { return g_tower ? g_tower->TimesText() : std::string(); }
bool ChargeWeapon(const std::string& id, int kind, int levels, std::string* why) { if (!g_tower) { *why = "影塔未就绪"; return false; } return g_tower->ChargeWeapon(id, kind, levels, why); }
bool ChargeNode(const std::string& id, std::string* why) { if (!g_tower) { *why = "影塔未就绪"; return false; } return g_tower->ChargeNode(id, why); }
bool ChargeSoul(const std::string& id, int kind, std::string* why) { if (!g_tower) { *why = "影塔未就绪"; return false; } return g_tower->ChargeSoul(id, kind, why); }
void RefundSoul(const std::string& id, int kind) { if (g_tower) g_tower->RefundSoul(id, kind); }
bool NativeDowngrade(const std::string& id, int kind, int target, std::string* why) { return g_tower && g_tower->NativeDowngrade(id, kind, target, why); }
bool NativeResetNodes(const std::string& id, bool all, std::string* why) { return g_tower && g_tower->NativeResetNodes(id, all, why); }
void Refund(const std::string& id, int kind, int levels) { if (g_tower) g_tower->Refund(id, kind, levels); }
std::array<int, 2> ArsenalExtras(int kind) { return g_tower ? g_tower->ArsenalExtras(kind) : std::array<int, 2>{-1, -1}; }
bool ArsenalNativeBegin(int kind, int k) { return g_tower && g_tower->ArsenalNativeBegin(kind, k); }
bool ChargeAccessory(const std::string& id, int levels, std::string* why) { if (!g_tower) { *why = "影塔未就绪"; return false; } return g_tower->ChargeAccessory(id, levels, why); }
void RefundAccessory(const std::string& id, int levels) { if (g_tower) g_tower->RefundAccessory(id, levels); }
void Notify(const std::string& code, const std::string& msg) { if (g_tower) g_tower->Notify(code, msg); }
bool InsideTower() { return g_tower && g_tower->TripPhase() == tower::trip::Phase::Inside; }
unsigned long long LastEncounterClear() { return g_lastEncounterClear; }
bool HiddenUnlocked() { return Tower::HiddenUnlocked(); }
}  // namespace towerapi

namespace towerui { void Register(); }

namespace pbz::module {

const char* Id() { return "tower"; }

// 本机失败记录随世界模板作废（用户 2026-11-06）：venue-unsafe.txt（场地判坏）/ boss-denied.txt（Boss 禁用）是在某个世界状态下
// 得出的。旧版本塔档用玩家自己的进度（打过三面 = 雪景伏极之林），在那里判坏的场地换成固定模板后已不成立，留着会让该层永远被跳过。
// 模板指纹（FNV-1a）变了就把旧记录改名成 *.stale-<指纹> 留档、不再生效，并记下新指纹
void ResetStaleRecords() {
    wchar_t base[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return;
    const std::wstring dir = std::wstring(base) + L"\\PBZTower\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::string text;
    uint64_t h = 1469598103934665603ull;
    const bool has = towernative::WorldTemplateText(&text);
    for (unsigned char c : text) { h ^= c; h *= 1099511628211ull; }
    const std::string fp = has ? std::format("{:016x}", h) : std::string("none");
    std::string old;
    { std::ifstream f(dir + L"records-world.txt", std::ios::binary); std::getline(f, old); }
    if (!old.empty() && old.back() == '\r') old.pop_back();
    if (old == fp) return;
    int moved = 0;
    for (const wchar_t* leaf : {L"venue-unsafe.txt", L"boss-denied.txt"}) {
        const std::wstring from = dir + leaf;
        if (GetFileAttributesW(from.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        const std::wstring to = from + L".stale-" + std::wstring(fp.begin(), fp.begin() + std::min<size_t>(8, fp.size()));
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) ++moved;
    }
    std::ofstream(dir + L"records-world.txt", std::ios::binary | std::ios::trunc) << fp << "\n";
    log::Info("影塔：世界模板 {}（上次 {}），本机旧的场地判坏 / Boss 禁用记录 {} 份已作废", fp, old.empty() ? "无记录" : old, moved);
}

// 3.2.0（玩家反馈：装 Mod 后多了好几 G）：运行库的异常诊断每次启动都可能写 minidump（每份约 7 MB）且从不清理，
// 修改器另有每次注入一份的核心副本。启动时只保留最新的若干份，其余删掉（只动诊断文件和核心副本，不动存档 / 存档备份）
void PruneDiagnostics() {
    wchar_t base[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return;
    namespace fs = std::filesystem;
    std::error_code ec;
    uint64_t freed = 0; int removed = 0;
    auto prune = [&](const fs::path& dir, const std::wstring& ext, size_t keep) {
        if (!fs::is_directory(dir, ec)) return;
        std::vector<std::pair<fs::file_time_type, fs::path>> files;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!e.is_regular_file(ec) || e.path().extension().wstring() != ext) continue;
            files.push_back({e.last_write_time(ec), e.path()});
        }
        if (files.size() <= keep) return;
        std::sort(files.begin(), files.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
        for (size_t i = keep; i < files.size(); ++i) {
            const auto size = fs::file_size(files[i].second, ec);
            if (fs::remove(files[i].second, ec)) { freed += ec ? 0 : size; ++removed; }  // 正在使用的核心副本删不掉，跳过
        }
    };
    const fs::path lab = fs::path(base) / L"PBZLab";
    for (const wchar_t* d : {L"crash", L"crash-v6"}) { prune(lab / d, L".dmp", 4); prune(lab / d, L".json", 40); }
    prune(lab / L"core", L".dll", 3);
    if (removed) log::Info("影塔：清理旧诊断文件 {} 个，释放 {:.1f} MB（异常转储留最新 4 份、记录留 40 份、核心副本留 3 份）", removed, freed / 1048576.0);
}

bool Start(std::string*) {
    PruneDiagnostics();
    ResetStaleRecords();  // 必须在任何读取 venue-unsafe / boss-denied 之前
    auto t = std::make_unique<Tower>();
    g_tower = t.get();
    g_tower->LoadTrip();
    AddFeature(std::move(t));
    ipc::RegisterOp("tower", [](const std::string& line) {
        gamethread::Post([line] { if (g_tower) g_tower->Command(line); });
    });
    towerui::Register();
    towertravel::Register();
    log::Info("影塔模块启动（Boss 池 {} 个，卡 {} 张）", kPool, tower::kCardCount);
    return true;
}

void Stop() { towermusic::Shutdown(); towergate::OnStop(); }  // 运行库随后会对 tower.run 调 Disable：清怪、还原；内存仍是塔内状态时保留原生存档锁

}  // namespace pbz::module
