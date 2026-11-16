#include "tower_entry.h"
#include "tower_widgets.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <format>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>
#include "../../../src/core/host/log.h"
#include "tower_perf.h"

namespace towerapi { bool InsideTower(); }

namespace towerentry {
namespace ue = pbz::ue;
namespace pe = pbz::pe;
namespace {
using ue::UObject;
struct Args {
    UObject* fn; std::array<uint8_t, 0x200> bytes{};
    uint8_t* At(const char* name) { const int o = fn ? ue::PropOffset(fn, name) : -1; return o >= 0 && o < 0x180 ? bytes.data() + o : nullptr; }
};
UObject* Fn(UObject* o, const char* f) { return o && ue::Live(o) ? pe::FindFunction(ue::ClassOf(o), f) : nullptr; }
struct Held { UObject* actor; UObject* asc; bool wasInhibited; };
std::vector<Held> g_held;
tower::entry::Gate g_gate;
UObject* g_pc = nullptr;
bool g_move = false, g_look = false;
std::string g_why;
int g_shown = 0;
bool g_faced = false;
bool g_earlyTopUp = false;  // 本次入场已在画面可见时尝试过提前补满  // 本次入场已把镜头朝向 Boss
// 一次性把玩家镜头（控制器朝向）转向 Boss：只改 Yaw，Pitch 略微向下，不每帧强制
bool FaceBoss(UObject* boss) {
    auto* pc = towerwidgets::Controller();
    auto* pawn = towerwidgets::Pawn();
    auto* actorCls = towerperf::ClassNamed("Actor");
    auto* loc = actorCls ? pe::FindFunction(actorCls, "K2_GetActorLocation") : nullptr;
    auto* set = Fn(pc, "SetControlRotation");
    if (!pc || !pawn || !loc || !set || !ue::Live(boss)) return false;
    double a[3], b[3];
    { Args x{loc}; if (!x.At("ReturnValue")) return false; pe::Call(pawn, loc, x.bytes.data()); std::memcpy(a, x.At("ReturnValue"), 24); }
    { Args x{loc}; pe::Call(boss, loc, x.bytes.data()); std::memcpy(b, x.At("ReturnValue"), 24); }
    const double dx = b[0] - a[0], dy = b[1] - a[1];
    if (std::abs(dx) + std::abs(dy) < 1) return false;
    const double rot[3] = {-10.0, std::atan2(dy, dx) * 180.0 / 3.14159265358979, 0.0};  // Pitch / Yaw / Roll（FRotator，双精度）
    Args r{set}; if (!r.At("NewRotation")) return false;
    std::memcpy(r.At("NewRotation"), rot, 24);
    pe::Call(pc, set, r.bytes.data());
    pbz::log::Info("影塔入场：镜头朝向 Boss（Yaw {:.0f}），读秒期间可自由转视角", rot[1]);
    return true;
}
// 开层满状态（用户 2026-11-01：每层开打前满状态是免费塔规则）：一次性，不是常驻无敌 / 无限药
enum class TopUp { None, Done, Failed };
TopUp g_topUp = TopUp::None;
bool g_fullState = true;
uint64_t g_replayTopUpAt = 0;
bool SetBool(UObject* o, const char* name, const char* param, bool value) {
    auto* f = Fn(o, name); if (!f) return false;
    Args a{f}; if (!a.At(param)) return false;
    *a.At(param) = value; pe::Call(o, f, a.bytes.data()); return true;
}
bool ReadBool(UObject* o, const char* name, bool* value) {
    auto* f = Fn(o, name); if (!f) return false;
    Args a{f}; if (!a.At("ReturnValue")) return false;
    pe::Call(o, f, a.bytes.data()); *value = *a.At("ReturnValue") != 0; return true;
}
void Release() {
    // Paired increments: never reset another system's input locks.
    if (g_move) SetBool(g_pc, "SetIgnoreMoveInput", "bNewMoveInput", false);
    if (g_look) SetBool(g_pc, "SetIgnoreLookInput", "bNewLookInput", false);
    g_move = g_look = false; g_pc = nullptr;
    for (auto& h : g_held) if (ue::Live(h.asc)) SetBool(h.asc, "SetUserAbilityActivationInhibited", "NewInhibit", h.wasInhibited);
    g_held.clear(); g_shown = 0; towerwidgets::Countdown(0);
    // Dilation is owned by attribute arbitration: no direct writes or saved scaled baselines.
}
// 玩家所在世界的那份世界子系统（跨图期间旧世界的还在：不能取对象表里第一个，见 Core move 的 Ground）
UObject* WorldSubsystem(const char* cls, UObject* pawn) {
    auto* c = towerperf::ClassNamed(cls);
    auto* level = pawn ? ue::Get<UObject*>(pawn + ue::Layout::ObjOuter) : nullptr;
    auto* world = level ? ue::Get<UObject*>(level + ue::Layout::ObjOuter) : nullptr;
    UObject* found = nullptr;
    if (c && world) ue::ForEach([&](int, UObject* o) {
        if (ue::IsDefault(o) || !ue::IsA(o, c) || ue::Get<UObject*>(o + ue::Layout::ObjOuter) != world || !ue::Live(o)) return true;
        found = o; return false;
    });
    return found;
}
// 开层满状态（塔规则，每层开打前一次）。6ede7eb 实测：激活 Ability.System.Player.Dead.ResetAttribute 五次都失败
// （死亡复位能力活着时不可激活），药次数补满了但生命 / 杀气没有复位。现在不再依赖那个能力：
//   1. 背包子系统 RestoreQuickItems（药次数）、RestoreShaqiWeaponBullet（影之武装弹药）——原生复活同款；
//   2. 清掉上一层的临时药效：只移除原生药表里持续型（HasDuration）的药 GE，瞬发和常驻（装备着的药被动）不碰；
//      整局祝福 / 成长不是 GE（影塔属性仲裁 + 原生兵器 / 能力 / 饰品等级），不受影响；
//   3. 生命 = 生命上限、杀气 = 杀气上限：倒计时冻结期间每帧经属性仲裁 Hold（基值与当前值），放行前读回两项都满才算成功。
// 只在独立塔档里（不碰原档角色）。按当前上限补（能力 / 配装 / 祝福改过上限后自然按新上限）
// 原生药表 Prototypes/GameplayEffects/Medicine 下 DurationPolicy=HasDuration 的 GE（解包数据，2026-11-01）
constexpr const char* kMedicineBuffs[] = {
    "GE_Medicine_Attack_C", "GE_Medicine_Buff_01_C", "GE_Medicine_Buff_02_C", "GE_Medicine_Buff_03_Sub_01_C", "GE_Medicine_Buff_04_C",
    "GE_Medicine_Buff_05_C", "GE_Medicine_Buff_06_C", "GE_Medicine_Buff_07_C", "GE_Medicine_Buff_08_Sub_01_C", "GE_Medicine_Medicine_02_C",
    "GE_Medicine_Medicine_04_C", "GE_Medicine_Ghost_Ghost_C", "GE_Medicine_KuYeLu_Effect_Duration_Health_C", "GE_Medicine_KuYeLu_Effect_Duration_Shachi_C",
    "GE_Medicine_KuYeLu_HealthRegen_C", "GE_Medicine_LingYao_ShachiRegen_C", "GE_Medicine_DurationRecover_ShachiRegen_C", "GE_Medicine_Duration_Health_Effect_C",
    "GE_Medicine_RemoveDebuff_HealthRegen_C", "GE_Medicine_ResistanceElement_DecreaseDebuff_Effect_C", "GE_Medicine_ResistanceElement_Health_Effect_C",
    "GE_Medicine_RecoverMedicineOnKillMonster_IncreaseShachiAttack_C", "GE_Medicine_RecoverUltimateShachi_IncreaseAttack_C",
    "GE_Medicine_RecoverUltimateShachi_IncreaseAttackAttribute_C", "GE_Medicine_ReductionDamage_HealthRegen_C", "GE_Medicine_RegenShachi_HealthRegen_C",
    "GE_Medicine_RegenWithInterupt_DurationEffect_C", "GE_Medicine_RegenWithInterupt_GrantPassive_C",
};
const pbz::attr::Target* PlayerTarget() {
    for (auto& t : pbz::attr::Targets()) if (t.who == pbz::attr::Who::Player && t.obj == towerwidgets::Pawn() && pbz::attr::Alive(t)) return &t;
    return nullptr;
}
// 移除上一层的临时药效：返回调用了几个已加载的药 GE 类（没加载的类不可能有生效实例）；-1 = 接口不可用
int ClearMedicineBuffs(UObject* asc) {
    auto* f = Fn(asc, "RemoveActiveGameplayEffectBySourceEffect");
    if (!f) return -1;
    // 一次扫描找出全部已加载的药效类（旧做法逐个 FindClass：没加载的每个都整表扫描一遍，28 次，读秒开头卡约 780ms）。
    // 只看蓝图生成类对象（比指针，便宜），再比名字；结果缓存，下一层先核对还活着
    // 1.1.1（自测扫描第 22 层：9 次 0xC0000005 于 RemoveActiveGameplayEffectBySourceEffect，满状态没补上）：
    // 跳层 / 进塔会回标题重载世界，旧药效类卸载后对象槽位被复用，缓存的指针"还活着"却已是别的对象。
    // 用前逐个核对：槽位还活着、名字仍是药效类、仍是蓝图生成类；任何一个不对就整表重扫。
    // 1.3.0 实测：原先"玩家角色换了就重扫"让每层扫两次（钟传送跨图也换角色），每次约 38 ms 卡一帧；药效类是资源，跨图仍有效，核名足够
    // 新用过的药会新加载它的药效类：缓存 3 分钟后下一次开层再整表扫一遍补上
    static std::vector<UObject*> found;
    static uint64_t scannedAt = 0;
    bool stale = found.empty() || GetTickCount64() - scannedAt > 180000;
    for (auto* c : found) {
        if (stale) break;
        if (!ue::Live(c)) { stale = true; break; }
        const auto name = ue::ObjName(c);
        stale = std::none_of(std::begin(kMedicineBuffs), std::end(kMedicineBuffs), [&](const char* m) { return name == m; }) ||
                !ue::ClassName(c).ends_with("GeneratedClass");
    }
    if (stale) {
        scannedAt = GetTickCount64();
        towerperf::Section _("药效类扫描");
        found.clear();
        std::unordered_map<UObject*, bool> generated;  // 元类 → 是不是蓝图生成类（每个元类只取一次名字）
        ue::ForEach([&](int, UObject* o) {
            UObject* meta = ue::ClassOf(o);
            auto [it, fresh] = generated.try_emplace(meta, false);
            if (fresh) it->second = ue::ClassName(o).ends_with("GeneratedClass");
            if (!it->second) return true;
            const auto name = ue::ObjName(o);
            for (const char* m : kMedicineBuffs) if (name == m) { found.push_back(o); break; }
            return true;
        });
    }
    int n = 0;
    for (auto* cls : found) {
        if (!ue::Live(cls)) continue;
        Args a{f}; if (!a.At("GameplayEffect") || !a.At("StacksToRemove")) return -1;
        std::memcpy(a.At("GameplayEffect"), &cls, 8);
        const int32_t all = -1; std::memcpy(a.At("StacksToRemove"), &all, 4);  // InstigatorAbilitySystemComponent 留空 = 任何来源
        pe::Call(asc, f, a.bytes.data());
        ++n;
    }
    return n;
}
bool DoTopUp(std::string* why) {
    auto* pawn = towerwidgets::Pawn();
    if (!pawn) { *why = "玩家未就绪"; return false; }
    auto* bag = WorldSubsystem("PBZBackPackSubsystem", pawn);
    auto* quick = Fn(bag, "RestoreQuickItems");
    auto* bullet = Fn(bag, "RestoreShaqiWeaponBullet");
    auto* lib = towerwidgets::Default(towerperf::ClassNamed("AbilitySystemBlueprintLibrary"));
    auto* getAsc = Fn(lib, "GetAbilitySystemComponent");
    if (!quick || !bullet || !getAsc) { *why = "原生补满接口不可用"; return false; }
    if (!PlayerTarget() || pbz::attr::AttrOffset("Health") < 0 || pbz::attr::AttrOffset("Health_Max") < 0 || pbz::attr::AttrOffset("Shachi") < 0 || pbz::attr::AttrOffset("Shachi_Max") < 0) {
        *why = "玩家属性不可读"; return false;
    }
    Args a{getAsc}; if (!a.At("Actor") || !a.At("ReturnValue")) { *why = "能力组件参数变化"; return false; }
    std::memcpy(a.At("Actor"), &pawn, 8); pe::Call(lib, getAsc, a.bytes.data());
    auto* asc = ue::Get<UObject*>(a.At("ReturnValue"));
    if (!asc || !ue::Live(asc)) { *why = "能力组件不可用"; return false; }
    pe::Call(bag, quick, nullptr);
    pe::Call(bag, bullet, nullptr);
    const int cleared = ClearMedicineBuffs(asc);
    if (cleared < 0) { *why = "清除临时药效的接口不可用"; return false; }
    pbz::log::Info("影塔入场：已补药次数 / 影武弹药，清除上一层临时药效（检查 {} 类已加载的持续型药效）", cleared);
    return true;
}
// 冻结期间每帧把生命 / 杀气钉在当前上限（基值与当前值一起写）；放行后不回写
void HoldFull() {
    const auto* p = PlayerTarget();
    if (!p) return;
    for (auto [cur, max] : {std::pair{"Health", "Health_Max"}, {"Shachi", "Shachi_Max"}}) {
        const int c = pbz::attr::AttrOffset(cur), m = pbz::attr::AttrOffset(max);
        if (c < 0 || m < 0) continue;
        const float top = ue::Get<float>(pbz::attr::AttrCurrent(*p, m));
        if (top <= 0) continue;
        pbz::attr::Hold(*p, pbz::attr::AttrBase(*p, c), top);
        pbz::attr::Hold(*p, pbz::attr::AttrCurrent(*p, c), top);
    }
}
// 读回：生命、杀气都满才算成功
void ReadBack() {
    const auto* p = PlayerTarget();
    auto v = [&](const char* n) { const int o = pbz::attr::AttrOffset(n); return p && o >= 0 ? ue::Get<float>(pbz::attr::AttrCurrent(*p, o)) : -1.0f; };
    const float hp = v("Health"), hpMax = v("Health_Max"), sq = v("Shachi"), sqMax = v("Shachi_Max");
    const bool hpFull = hpMax > 0 && hp >= hpMax - 0.5f, sqFull = sqMax > 0 && sq >= sqMax - 0.5f;
    pbz::log::Info("影塔入场：满状态读回 生命 {:.0f}/{:.0f}{} 杀气 {:.0f}/{:.0f}{}", hp, hpMax, hpFull ? "（满）" : "（未满）", sq, sqMax, sqFull ? "（满）" : "（未满）");
    if (!hpFull || !sqFull) g_topUp = TopUp::Failed;
}
bool Declare(UObject* actor) {
    if (!actor || !ue::Live(actor)) return false;
    const int setOff = ue::PropOffset(towerperf::ClassNamed("PBZCharacter"), "AttributeSetBase");
    const int dilation = pbz::attr::ActorOffset("CustomTimeDilation");
    if (setOff < 0 || dilation < 0) return false;
    pbz::attr::Target t{actor, ue::Get<uint8_t*>(actor + setOff), actor == towerwidgets::Pawn() ? pbz::attr::Who::Player : pbz::attr::Who::Enemies, ue::ClassOf(actor)};
    if (!pbz::attr::Alive(t)) return false;
    pbz::attr::Want(t, reinterpret_cast<float*>(actor + dilation), pbz::attr::Kind::Scale, 0.0f);
    return true;
}
// ---- 画面可见门（6ede7eb 实测：跨图走 PBZSaveLoadSubsystem.TeleportTo，盖的是 GameInstance 的加载屏，
// 不是目的地遮罩；只查 IsDestinationLoadingActive 时日志读完 3/2/1，用户还在黑屏）。四个信号全清才算看得见：
//   目的地遮罩 / GameInstance.IsLoadingLevel / 活动中的 WB_TempLoadingScreen / 相机淡入淡出量
// 任何一项查不到（类 / 函数缺失）按"看不见"处理，由入场 90 秒上限兜底取消，不在黑屏后开打。
// 例外：IsLoadingLevel 是读字段（可能是位域），其他三项全清后它单独卡住超过 15 秒时记日志并忽略，避免永远卡死
struct Cover { const char* why = nullptr; uint64_t since = 0, onlyLevel = 0, lastScan = 0; bool screen = false; int logs = 0; std::string last; };
Cover g_cover;
UObject* GameInstance() {
    static UObject* cached = nullptr;
    if (cached && ue::Live(cached) && !ue::IsDefault(cached)) return cached;
    cached = nullptr;
    auto* c = towerperf::ClassNamed("PBZGameInstance");
    if (c) ue::ForEach([&](int, UObject* o) { if (!ue::IsDefault(o) && ue::IsA(o, c) && ue::Live(o)) { cached = o; return false; } return true; });
    return cached;
}
// 活动中的原生加载屏控件（激活态；淡出动画结束后才会失活）。每 250ms 扫一次对象表
bool LoadingScreenUp(uint64_t now, bool* known) {
    auto* c = towerperf::ClassNamed("WB_TempLoadingScreen_C");
    *known = true;
    if (!c) return false;  // 还没加载过这个蓝图 = 没有这块加载屏
    if (now - g_cover.lastScan < 250) return g_cover.screen;
    g_cover.lastScan = now;
    // 1.3.0（性能取证构建实测：整表找实例每次约 40 ms，每层 1–2 次，读秒前后看得见卡一下）：改为分帧扫描，
    // 每次调用（250 ms 一次）扫 8192 个对象，一整轮扫完才换上新结果。已找到的实例都还活着时 10 秒才开始下一轮；
    // 第一轮还没扫完又没有旧结果 = 状态未知，入场按"加载屏状态不可读"继续等，不会提前放行
    static std::vector<UObject*> screens, building;
    static std::unordered_map<UObject*, bool> hit;
    static bool scanning = false, haveResult = false;
    static int cursor = 0;
    static uint64_t fullAt = 0;
    bool alive = !screens.empty();
    for (auto* o : screens) alive = alive && ue::Live(o);
    if (!scanning && (!haveResult || now - fullAt >= (alive ? 10000u : 2000u))) { scanning = true; cursor = 0; building.clear(); hit.clear(); }
    if (scanning) {
        towerperf::Section _("加载屏实例扫描");
        constexpr int kStep = 8192;
        static UObject* batch[kStep];
        const int got = ue::ObjBatch(cursor, kStep, batch);
        for (int i = 0; i < got; ++i) {
            UObject* o = batch[i];
            UObject* k = o ? ue::ClassOf(o) : nullptr;
            if (!k) continue;
            auto [it, fresh] = hit.try_emplace(k, false);
            if (fresh) it->second = ue::IsA(o, c);
            if (it->second && !ue::IsDefault(o) && ue::Live(o)) building.push_back(o);
        }
        cursor += got ? got : kStep;
        if (cursor >= ue::Num()) { scanning = false; haveResult = true; fullAt = now; screens.swap(building); building.clear(); }
        else {
            g_cover.lastScan = 0;  // 一轮扫描进行中：不受 250 ms 节流，下一帧接着扫（约 20 帧扫完一轮）
            if (!haveResult) { *known = false; g_cover.screen = false; return false; }
        }
    }
    bool up = false;
    for (auto* o : screens) {
        if (!ue::Live(o)) continue;
        bool active = true;
        if (!ReadBool(o, "IsActivated", &active)) { *known = false; break; }
        if (active) { up = true; break; }
    }
    g_cover.screen = up;
    return up;
}
// 相机淡入淡出量（0 = 全清，1 = 全黑）；引擎自带的测试库函数，读的是 PlayerCameraManager 的淡入状态
float FadeAmount(bool* known) {
    auto* pc = towerwidgets::Controller();
    const int off = pc ? ue::PropOffset(ue::ClassOf(pc), "PlayerCameraManager") : -1;
    auto* pcm = off >= 0 ? ue::Get<UObject*>(pc + off) : nullptr;
    auto* lib = towerwidgets::Default(towerperf::ClassNamed("MovieSceneFadeTrackTestLibrary"));
    auto* f = Fn(lib, "GetManualFadeAmount");
    *known = false;
    if (!pcm || !ue::Live(pcm) || !f) return 1.0f;
    Args a{f}; if (!a.At("PlayerCameraManager") || !a.At("ReturnValue")) return 1.0f;
    std::memcpy(a.At("PlayerCameraManager"), &pcm, 8); pe::Call(lib, f, a.bytes.data());
    *known = true;
    return *reinterpret_cast<float*>(a.At("ReturnValue"));
}
void CoverLog(const std::string& what) {
    if (what == g_cover.last) return;
    g_cover.last = what;
    if (g_cover.logs++ < 40) pbz::log::Info("影塔入场：画面门 {}", what.empty() ? "可见（四项全清）" : what);
}
bool ScreenVisible(uint64_t now) {
    std::string block;
    if (!LoadingClear()) block = g_why;
    bool levelLoading = false;
    if (auto* gi = GameInstance()) {
        const int off = ue::PropOffset(ue::ClassOf(gi), "IsLoadingLevel");
        if (off < 0) block += block.empty() ? "GameInstance 加载标记不可读" : "";
        else levelLoading = ue::Get<uint8_t>(gi + off) != 0;
    } else if (block.empty()) block = "GameInstance 未就绪";
    bool known = true;
    if (LoadingScreenUp(now, &known)) { if (block.empty()) block = "原生加载屏仍在"; }
    else if (!known && block.empty()) block = "加载屏状态不可读";
    const float fade = FadeAmount(&known);
    if (!known) { if (block.empty()) block = "相机淡入状态不可读"; }
    else if (fade > 0.02f && block.empty()) block = std::format("相机仍在淡入（{:.2f}）", fade);
    if (block.empty() && levelLoading) {
        if (!g_cover.onlyLevel) g_cover.onlyLevel = now;
        if (now - g_cover.onlyLevel < 15000) block = "GameInstance 仍在加载关卡";
        else if (g_cover.last != "忽略卡住的 IsLoadingLevel") { CoverLog("忽略卡住的 IsLoadingLevel"); }
    } else if (!levelLoading) g_cover.onlyLevel = 0;
    if (block.empty()) { if (g_cover.last != "忽略卡住的 IsLoadingLevel") CoverLog(""); return true; }
    CoverLog(block);
    g_why = block;
    return false;
}
bool Paused() {
    auto* lib = towerwidgets::Default(towerperf::ClassNamed("GameplayStatics"));
    auto* f = Fn(lib, "IsGamePaused"); auto* pawn = towerwidgets::Pawn();
    if (!f || !pawn) return true;
    Args a{f}; if (!a.At("WorldContextObject") || !a.At("ReturnValue")) return true;
    std::memcpy(a.At("WorldContextObject"), &pawn, 8); pe::Call(lib, f, a.bytes.data()); return *a.At("ReturnValue") != 0;
}
} // namespace

void Begin(bool quick) { Release(); g_faced = false; g_earlyTopUp = false; g_why.clear(); g_topUp = TopUp::None; g_cover = {}; g_gate.Begin(GetTickCount64(), quick); pbz::log::Info("影塔入场：等待真实加载结束及双方初始化"); }
void BeginReplay() {
    Begin(); g_gate.Reset(); g_replayTopUpAt = 0;
    pbz::log::Info("影塔重放：原生遭遇入场不设普通生成等待 / 倒计时，保留开层补满规则");
}
// 先于交互开放，不冻结角色；属性仲裁在本帧结束提交，下帧读回，不把等待玩家或初见CG算入生成超时。
tower::entry::State PrepareReplay() {
    if (!g_fullState || !towerapi::InsideTower()) return tower::entry::State::Running;
    if (g_topUp == TopUp::Failed) return tower::entry::State::Failed;
    if (g_topUp == TopUp::None) {
        if (!DoTopUp(&g_why)) { g_topUp = TopUp::Failed; return tower::entry::State::Failed; }
        g_topUp = TopUp::Done; g_replayTopUpAt = GetTickCount64(); HoldFull();
        return tower::entry::State::Waiting;
    }
    HoldFull();
    const auto* player = PlayerTarget();
    bool full = player != nullptr;
    for (auto [cur, max] : {std::pair{"Health", "Health_Max"}, {"Shachi", "Shachi_Max"}}) {
        const int c = pbz::attr::AttrOffset(cur), m = pbz::attr::AttrOffset(max);
        if (!player || c < 0 || m < 0) { full = false; break; }
        const float top = ue::Get<float>(pbz::attr::AttrCurrent(*player, m));
        full = full && top > 0 && ue::Get<float>(pbz::attr::AttrCurrent(*player, c)) >= top - 0.5f;
    }
    if (!full) {
        if (GetTickCount64() - g_replayTopUpAt < 3000) return tower::entry::State::Waiting;
        g_topUp = TopUp::Failed; g_why = "重放开层满状态读回失败"; return tower::entry::State::Failed;
    }
    ReadBack();
    FullHealVisible("重放开层");
    pbz::log::Info("影塔重放：开层满状态读回通过，不锁输入、不读秒，等玩家开遭遇");
    return tower::entry::State::Running;
}
void SetFullState(bool on) { g_fullState = on; }
bool FullStateEnabled() { return g_fullState; }
void FullHealVisible(const char* why) {
    auto* pawn = towerwidgets::Pawn();
    bool native = false;
    auto* lib = towerwidgets::Default(towerperf::ClassNamed("PBZRestoreQueryLibrary"));
    if (auto* fn = Fn(lib, "ResetHealthToMax"); fn && pawn) {
        Args a{fn};
        if (a.At("Target") && a.At("ReturnValue")) { std::memcpy(a.At("Target"), &pawn, 8); pe::Call(lib, fn, a.bytes.data()); native = *a.At("ReturnValue") != 0; }
    }
    if (const auto* p = PlayerTarget()) {
        const int c = pbz::attr::AttrOffset("Health"), m = pbz::attr::AttrOffset("Health_Max");
        if (c >= 0 && m >= 0) {
            const float top = ue::Get<float>(pbz::attr::AttrCurrent(*p, m));
            if (top > 0) { ue::Write(pbz::attr::AttrBase(*p, c), &top, 4); ue::Write(pbz::attr::AttrCurrent(*p, c), &top, 4); }
        }
    }
    const int bars = towerwidgets::RefreshHpBar();
    static int logs = 0;
    if (logs++ < 20) pbz::log::Info("影塔回满（{}）：原生 ResetHealthToMax {}，血条重画 {} 个", why, native ? "成功" : "未生效", bars);
}
bool TopUpNow() {
    if (!g_fullState || !towerapi::InsideTower()) return false;
    std::string why;
    const bool ok = DoTopUp(&why);
    if (const auto* p = PlayerTarget())
        for (auto [cur, max] : {std::pair{"Health", "Health_Max"}, {"Shachi", "Shachi_Max"}}) {
            const int c = pbz::attr::AttrOffset(cur), m = pbz::attr::AttrOffset(max);
            if (c < 0 || m < 0) continue;
            const float top = ue::Get<float>(pbz::attr::AttrCurrent(*p, m));
            if (top <= 0) continue;
            ue::Write(pbz::attr::AttrBase(*p, c), &top, 4);
            ue::Write(pbz::attr::AttrCurrent(*p, c), &top, 4);
        }
    if (!ok) pbz::log::Info("影塔开层：药 / 影武弹药提前补满未成功（{}），读秒时再补", why);
    FullHealVisible("点下一层");
    return ok;
}
const char* TopUpState() { return g_topUp == TopUp::Done ? "done" : g_topUp == TopUp::Failed ? "failed" : "none"; }
void End() { Release(); g_gate.Reset(); }
// 1.3.4（用户 2026-11-07：重生符退后 10 层，倒计时结束人动不了）：致死那一下原生可能已经进了死亡处理
// （停移动 / 忽略移动输入 / 能力抑制），塔把血拉回来后这些不会自己恢复；入场释放又是"还原成冻结前的样子"，
// 把残留的锁原样还了回去。只在重生符路径调用：入场冻结没在持有时，逐项读、卡着就解开，并记下是哪一项（实机定位用）
int Unstick(const char* why) {
    if (Holding()) return 0;
    auto* pawn = towerwidgets::Pawn();
    auto* pc = towerwidgets::Controller();
    if (!pawn || !ue::Live(pawn)) return 0;
    std::string fixed;
    bool ignored = false;
    if (pc && ReadBool(pc, "IsMoveInputIgnored", &ignored) && ignored)
        if (auto* f = Fn(pc, "ResetIgnoreMoveInput")) { pe::Call(pc, f, nullptr); fixed += " 忽略移动输入"; }
    if (pc && ReadBool(pc, "IsLookInputIgnored", &ignored) && ignored)
        if (auto* f = Fn(pc, "ResetIgnoreLookInput")) { pe::Call(pc, f, nullptr); fixed += " 忽略视角输入"; }
    if (auto* lib = towerwidgets::Default(towerperf::ClassNamed("AbilitySystemBlueprintLibrary")))
        if (auto* f = Fn(lib, "GetAbilitySystemComponent")) {
            Args a{f};
            if (a.At("Actor") && a.At("ReturnValue")) {
                std::memcpy(a.At("Actor"), &pawn, 8); pe::Call(lib, f, a.bytes.data());
                auto* asc = ue::Get<UObject*>(a.At("ReturnValue"));
                bool inhibited = false;
                if (asc && ue::Live(asc) && ReadBool(asc, "GetUserAbilityActivationInhibited", &inhibited) && inhibited &&
                    SetBool(asc, "SetUserAbilityActivationInhibited", "NewInhibit", false)) fixed += " 能力抑制";
            }
        }
    if (const int off = ue::PropOffset(ue::ClassOf(pawn), "CharacterMovement"); off >= 0)
        if (auto* move = ue::Get<UObject*>(pawn + off); move && ue::Live(move))
            if (const int mode = ue::PropOffset(ue::ClassOf(move), "MovementMode"); mode >= 0 && ue::Get<uint8_t>(move + mode) == 0)  // MOVE_None
                if (auto* f = Fn(move, "SetMovementMode")) {
                    Args a{f};
                    if (auto* m = a.At("NewMovementMode")) { *m = 1; pe::Call(move, f, a.bytes.data()); fixed += " 移动模式（None → Walking）"; }  // MOVE_Walking
                }
    pbz::log::Info("影塔重生符：{}，检查玩家行动锁：{}", why, fixed.empty() ? "无残留" : "解开了" + fixed);
    return fixed.empty() ? 0 : 1;
}
bool Holding() { return g_gate.Holding(); }
bool GamePaused() { return Paused(); }
const char* Why() { return g_why.c_str(); }
const char* State() {
    switch (g_gate.state) {
        case tower::entry::State::Idle: return "idle";
        case tower::entry::State::Waiting: return "waiting";
        case tower::entry::State::Countdown: return "countdown";
        case tower::entry::State::Running: return "running";
        default: return "failed";
    }
}
bool LoadingClear() {
    auto* pawn = towerwidgets::Pawn();
    auto* lib = towerwidgets::Default(towerperf::ClassNamed("PBZDestinationLoadingSubsystem"));
    auto* f = Fn(lib, "IsDestinationLoadingActive");
    if (!pawn || !towerwidgets::Controller() || !f) { g_why = "原生加载状态或玩家未就绪"; return false; }
    Args a{f}; if (!a.At("WorldContext") || !a.At("ReturnValue")) { g_why = "原生加载状态参数变化"; return false; }
    std::memcpy(a.At("WorldContext"), &pawn, 8); pe::Call(lib, f, a.bytes.data());
    if (*a.At("ReturnValue")) { g_why = "原生加载遮罩仍活动"; return false; }
    return true;
}
bool Hold(UObject* actor, bool faceBoss) {
    if (!Holding() || !actor || !ue::Live(actor)) return false;
    for (auto& h : g_held) if (h.actor == actor) return Declare(actor);
    auto* lib = towerwidgets::Default(towerperf::ClassNamed("AbilitySystemBlueprintLibrary"));
    auto* f = Fn(lib, "GetAbilitySystemComponent");
    if (!f) { g_why = "无法取得入场能力组件"; return false; }
    Args a{f}; if (!a.At("Actor") || !a.At("ReturnValue")) return false;
    std::memcpy(a.At("Actor"), &actor, 8); pe::Call(lib, f, a.bytes.data());
    auto* asc = ue::Get<UObject*>(a.At("ReturnValue")); bool inhibited = false;
    if (!ReadBool(asc, "GetUserAbilityActivationInhibited", &inhibited) || !Declare(actor)) { g_why = "无法安全冻结入场角色"; return false; }
    if (!SetBool(asc, "SetUserAbilityActivationInhibited", "NewInhibit", true)) return false;
    g_held.push_back({actor, asc, inhibited});
    if (actor == towerwidgets::Pawn()) {
        g_pc = towerwidgets::Controller();
        // 32ffb7d 用户：读秒期间允许自由转视角——只锁移动（攻击 / 技能由能力抑制锁住），不锁视角
        g_move = SetBool(g_pc, "SetIgnoreMoveInput", "bNewMoveInput", true);
        if (!g_move) { g_why = "入场输入锁接口不可用"; return false; }
    } else if (faceBoss && !g_faced) {
        g_faced = FaceBoss(actor);  // 第一只 Boss 就位：镜头先朝向它一次，之后不再掰回
    }
    return true;
}
void ForgetEndedWaveActor(UObject* actor) {
    if (!actor || actor == towerwidgets::Pawn()) return;
    for (auto it = g_held.begin(); it != g_held.end();) {
        if (it->actor != actor) { ++it; continue; }
        if (ue::Live(it->asc)) SetBool(it->asc, "SetUserAbilityActivationInhibited", "NewInhibit", it->wasInhibited);
        it = g_held.erase(it);
    }
}
tower::entry::State Tick(bool actorsReady) {
    if (!Holding()) return g_gate.state;
    bool valid = true;
    for (auto& h : g_held) valid = Declare(h.actor) && valid;
    bool ready = false, visible = false;
    if (!actorsReady) g_why = "等待 Boss 与属性初始化";
    else if (!valid) g_why = "入场角色已失效";
    else if ((visible = ScreenVisible(GetTickCount64()))) {
        if (Paused()) g_why = "游戏仍处于暂停状态";
        else if (towerwidgets::MenuBusy()) g_why = "整备面板尚未关闭";
        else { ready = true; g_why.clear(); }
    }
    {   // 读秒前在等什么（发布版也记，原因变化才写，每层最多 12 条）：2026-11-05 第 3 层准入后 45 秒才读秒，日志看不出原因
        static std::string lastWait; static int waitLogs = 0;
        if (g_gate.state == tower::entry::State::Waiting && g_why != lastWait) {
            lastWait = g_why;
            if (waitLogs++ < 12 && !g_why.empty()) pbz::log::Info("影塔入场：读秒前等待（{}）", g_why);
        }
        if (g_gate.state != tower::entry::State::Waiting) { lastWait.clear(); waitLogs = 0; }
    }
    const int number = g_gate.Step(GetTickCount64(), ready);
    // 原生属性稳定、加载结束、倒计时第一次开始时补满（放行前读回）
    // 2026-11-05 发布版实测：原来到读秒才补，读秒前一旦要等（暂停 / 面板），玩家一直看着上一层打完的血和药。
    // 现在场景加载完、画面可见就补；读秒中才补到的（极少）仍按原来重算读秒
    const bool counting = g_gate.state == tower::entry::State::Countdown;
    if (((visible && !g_earlyTopUp) || counting) && g_topUp == TopUp::None && g_fullState && towerapi::InsideTower()) {
        std::string why;
        if (!counting) g_earlyTopUp = true;  // 提前只试一次，失败退回读秒时再补（不判失败）
        const bool ok = DoTopUp(&why);
        g_topUp = ok ? TopUp::Done : counting ? TopUp::Failed : TopUp::None;
        if (counting) g_gate.Restart(GetTickCount64());  // 313987b 实测"3"只显示约 0.22 秒：补满（扫对象表、移除药效）吃掉了读秒，读秒从补完这一刻重新算
        pbz::log::Info("影塔入场：开层满状态 {}", g_topUp == TopUp::Done ? "开始（药 / 影武弹药 / 清上层药效，冻结期间生命与杀气钉满）" : (counting ? "失败：" : "提前补满未成功，读秒时再补：") + why);
    }
    if ((g_gate.state == tower::entry::State::Countdown || g_gate.state == tower::entry::State::Waiting) && g_topUp == TopUp::Done) HoldFull();
    if (number != g_shown) {
        if (!towerwidgets::Countdown(number)) { g_why = "原生倒计时控件不可用，取消开战"; g_gate.state = tower::entry::State::Failed; }
        g_shown = number;
        if (number) pbz::log::Info("影塔入场：倒计时 {}（双方冻结，画面可见已连续 {}ms）", number, GetTickCount64() - g_gate.readySince);
    }
    if (g_gate.state == tower::entry::State::Running) {
        if (g_topUp == TopUp::Done) ReadBack();
        if (g_fullState && towerapi::InsideTower()) FullHealVisible("开战");
        Release(); pbz::log::Info("影塔入场：开战，释放本层冻结与输入锁（满状态 {}）", TopUpState());
    }
    if (g_gate.state == tower::entry::State::Failed) { if (g_why.empty()) g_why = "入场等待超过 90 秒"; Release(); }
    return g_gate.state;
}
int AddSpiritShadow(int n) {
    if (n <= 0) return 0;
    auto* pawn = towerwidgets::Pawn();
    if (!pawn) return -1;
    static UObject* ge = nullptr;
    if (!ge || !ue::Live(ge)) ge = towerperf::ClassNamed("GE_Talent_RestoreSpiritShadow_C");
    if (!ge || !ue::Live(ge)) ge = towerwidgets::LoadClass("/PBZCombatAsset/Prototypes/GameplayEffects/Talent/GE_Talent_RestoreSpiritShadow.GE_Talent_RestoreSpiritShadow_C");
    auto* lib = towerwidgets::Default(towerperf::ClassNamed("AbilitySystemBlueprintLibrary"));
    auto* getAsc = Fn(lib, "GetAbilitySystemComponent");
    if (!ge || !getAsc) return -1;
    Args a{getAsc}; if (!a.At("Actor") || !a.At("ReturnValue")) return -1;
    std::memcpy(a.At("Actor"), &pawn, 8); pe::Call(lib, getAsc, a.bytes.data());
    auto* asc = ue::Get<UObject*>(a.At("ReturnValue"));
    if (!asc || !ue::Live(asc)) return -1;
    auto* apply = Fn(asc, "BP_ApplyGameplayEffectToSelf");
    if (!apply) return -1;
    int done = 0;
    for (int i = 0; i < n; ++i) {
        Args b{apply}; if (!b.At("GameplayEffectClass") || !b.At("Level")) return done ? done : -1;
        std::memcpy(b.At("GameplayEffectClass"), &ge, 8);
        const float lv = 1.0f; std::memcpy(b.At("Level"), &lv, 4);  // EffectContext 留空：引擎自己补
        pe::Call(asc, apply, b.bytes.data());
        ++done;
    }
    return done;
}
} // namespace towerentry
