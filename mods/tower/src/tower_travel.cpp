// 影塔传送（游戏端）：tower.travel 每帧推进传送状态机（tower_travel.h），接运行库的传送接口（SDK v4）。
// 擂台中心由玩家在游戏里标定：站到想打的平地上，发 {"op":"tower","cmd":"arena","value":"mark","id":"arena_z1_1"}，
// 存进 %LOCALAPPDATA%\PBZTower\arenas.local.txt。没标定的擂台落在钟旁接近点。
// 进塔位置存 home.txt，出塔（通关 / 失败 / 停止 / 暂离）传回去，传回后删除；崩溃留下的在下次空闲时不自动传（避免玩家正在别处时被拉走），
// 只在下一局出塔时一并处理。
#include <windows.h>

#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

#include "../../../src/core/host/log.h"
#include "../../../src/core/ipc/pipe.h"
#include "../../../src/core/sdk/pbz_module.h"
#include "tower_travel.h"
#include "tower_travel_api.h"
#include "tower_arena.h"
#include "tower_diagmode.h"
#include "tower_entry.h"
#include "tower_perf.h"
#include "tower_widgets.h"
#include "tower_reach.h"
#include "tower_venue.h"

namespace {
bool g_directKnown = false;  // 本次目的地是实机直接放置成功过的落点（venue-direct.txt）

using namespace tower::travel;
using tower::kArenas;

std::wstring TowerFile(const wchar_t* name) {
    wchar_t base[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return {};
    std::wstring dir = std::wstring(base) + L"\\PBZTower";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\" + name;
}
std::string ReadFile(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}
void WriteFile(const std::wstring& path, const std::string& s) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << s;
}

struct GameMove : IMove {
    std::unordered_map<std::string, std::string> levels;  // 钟 → 关卡（查一次缓存）
    bool Where(Pose* p, std::string* level) override {
        double o[4];
        if (!pbz::PlayerWhere(o, level)) return false;
        *p = {o[0], o[1], o[2], o[3]};
        return !level->empty();
    }
    std::string BellLevel(const std::string& bell) override {
        auto it = levels.find(bell);
        if (it != levels.end()) return it->second;
        auto lv = pbz::BellLevel(bell);
        if (!lv.empty()) levels[bell] = lv;
        return lv;
    }
    bool Bell(const std::string& bell, const Pose& at) override {
        const double loc[3] = {at.x, at.y, at.z};
        const auto r = pbz::BellCommand("tp", bell, loc, at.yaw);
        pbz::log::Info("影塔传送：钟 {} → {}", bell, r);
        return r.find("\"ok\":true") != std::string::npos;
    }
    bool Point(const Pose& at, std::string* msg) override {
        const bool ok = pbz::PointTeleport(at.x, at.y, at.z, at.yaw, msg);
        pbz::log::Info("影塔传送：点 ({:.0f}, {:.0f}, {:.0f}) {}", at.x, at.y, at.z, *msg);
        return ok;
    }
    // 游戏线程：K2_SetActorLocation(bTeleport) 把玩家放到落点，读回 3 米内算放到（地面是否成立由塔的落地审计判断）
    bool Direct(const Pose& at, std::string* msg) override {
        auto* pawn = towerwidgets::Pawn();
        namespace ue = pbz::ue; namespace pe = pbz::pe;
        if (!pawn || !ue::Live(pawn)) { *msg = "没有玩家"; return false; }
        auto* fn = pe::FindFunction(ue::ClassOf(pawn), "K2_SetActorLocation");
        auto* get = pe::FindFunction(ue::ClassOf(pawn), "K2_GetActorLocation");
        if (!fn || !get) { *msg = "没有 K2_SetActorLocation"; return false; }
        const int loc = ue::PropOffset(fn, "NewLocation"), tp = ue::PropOffset(fn, "bTeleport"), sw = ue::PropOffset(fn, "bSweep");
        if (loc < 0 || loc+24>0x200 || tp < 0 || tp>=0x200 || sw < 0 || sw>=0x200) { *msg = "参数不对"; return false; }
        const double nearAt[3] = {at.x, at.y, at.z}; double v[3];
        if (!towerreach::Landing(pawn,nearAt,v,msg,g_directKnown)) {
            pbz::log::Info("影塔传送：拒绝直接放置 ({:.0f}, {:.0f}, {:.0f})：{}",at.x,at.y,at.z,*msg);
            return false;
        }
        uint8_t args[0x200] = {};
        if(!towerreach::StopMotion(pawn)){*msg="无法清除传送前速度，拒绝放置";return false;}
        std::memcpy(args + loc, v, 24); args[tp] = 1; args[sw] = 0;
        pe::Call(pawn, fn, args);
        uint8_t b[32] = {}; pe::Call(pawn, get, b); double q[3]; std::memcpy(q, b, 24);
        const bool ok = std::isfinite(q[0]) && std::isfinite(q[1]) && std::isfinite(q[2]) && std::hypot(q[0]-v[0],q[1]-v[1],q[2]-v[2])<30;
        *msg = ok ? "已放到经地面及胶囊净空验证的落点" : "直接放置三维读回不对";
        pbz::log::Info("影塔传送：点 ({:.0f}, {:.0f}, {:.0f}) {}", at.x, at.y, at.z, *msg);
        return ok;
    }
    bool Busy() override { return pbz::TeleportBusy(); }
    bool StandingAt(const Pose& p) override {
        const double at[3]={p.x,p.y,p.z};
        const bool supported=towerreach::StandingAt(towerwidgets::Pawn(),at);
        if(supported)pbz::log::Info("影塔传送：原生中心高度已按当前可站立地面解析 ({:.1f}, {:.1f}, {:.1f})",p.x,p.y,p.z);
        return supported;
    }
    bool Ready() override { return towerentry::LoadingClear(); }  // 查不到加载状态也算未就绪（同入场门）

    // ---- 剧情数据层租用（39 层寒岁：DL_3721Unload 初始 Unloaded，场地单元只在这层里）----
    // 只在游戏线程（Travel::Tick）调用。激活前记下原状态，离开场地后按原状态还原；对象失效（换了世界）就放弃租约，
    // 不去碰新世界里的同名层（新世界由游戏按存档重新决定层状态）
    struct LayerLease { pbz::ue::UObject* mgr = nullptr; pbz::ue::UObject* inst = nullptr; pbz::ue::UObject* instCls = nullptr; std::string name; uint8_t original = 0; bool held = false; };
    LayerLease layer;

    static pbz::ue::UObject* PlayerWorld() {
        namespace ue = pbz::ue;
        auto* pawn = towerwidgets::Pawn();
        auto* level = pawn && ue::Live(pawn) ? ue::Get<ue::UObject*>(pawn + ue::Layout::ObjOuter) : nullptr;
        if (!level) return nullptr;
        const int owning = ue::PropOffset(ue::ClassOf(level), "OwningWorld");
        if (owning >= 0) if (auto* w = ue::Get<ue::UObject*>(level + owning); w && ue::Live(w)) return w;
        return ue::Get<ue::UObject*>(level + ue::Layout::ObjOuter);
    }
    static bool InWorld(pbz::ue::UObject* o, pbz::ue::UObject* world) {
        for (int i = 0; i < 12 && o; ++i) { o = pbz::ue::Get<pbz::ue::UObject*>(o + pbz::ue::Layout::ObjOuter); if (o == world) return true; }
        return false;
    }
    // GetDataLayerInstanceRuntimeState / ...Effective... ；失败返回 -1
    static int LayerState(pbz::ue::UObject* mgr, pbz::ue::UObject* inst, const char* fnName) {
        namespace ue = pbz::ue; namespace pe = pbz::pe;
        auto* fn = pe::FindFunction(ue::ClassOf(mgr), fnName);
        if (!fn) return -1;
        const int in = ue::PropOffset(fn, "InDataLayerInstance"), ret = ue::PropOffset(fn, "ReturnValue"), size = ue::Get<int>(fn + ue::Layout::StructSize);
        if (in < 0 || ret < 0 || size <= ret || size > 64 || in + 8 > size) return -1;
        uint8_t b[64] = {};
        std::memcpy(b + in, &inst, 8);
        pe::Call(mgr, fn, b);
        return b[ret];
    }
    static bool SetLayer(pbz::ue::UObject* mgr, pbz::ue::UObject* inst, uint8_t state) {
        namespace ue = pbz::ue; namespace pe = pbz::pe;
        auto* fn = pe::FindFunction(ue::ClassOf(mgr), "SetDataLayerInstanceRuntimeState");
        if (!fn) return false;
        const int in = ue::PropOffset(fn, "InDataLayerInstance"), st = ue::PropOffset(fn, "InState"), rec = ue::PropOffset(fn, "bInIsRecursive"),
                  ret = ue::PropOffset(fn, "ReturnValue"), size = ue::Get<int>(fn + ue::Layout::StructSize);
        if (in < 0 || st < 0 || rec < 0 || size > 64 || in + 8 > size || st >= size || rec >= size) return false;
        uint8_t b[64] = {};
        std::memcpy(b + in, &inst, 8); b[st] = state; b[rec] = 0;
        pe::Call(mgr, fn, b);
        return ret < 0 || b[ret] != 0;
    }
    static constexpr uint8_t kActivated = 2;  // EDataLayerRuntimeState: Unloaded 0 / Loaded 1 / Activated 2

    bool EnsureLayer(const std::string& name, std::string* msg) override {
        namespace ue = pbz::ue;
        if (layer.held && layer.name == name && ue::Live(layer.inst) && ue::Live(layer.mgr) && ue::ClassOf(layer.inst) == layer.instCls &&
            LayerState(layer.mgr, layer.inst, "GetDataLayerInstanceEffectiveRuntimeState") == kActivated) return true;
        if (layer.held) RestoreLayer("换目的地");
        auto* world = PlayerWorld();
        if (!world) { *msg = "找不到玩家所在世界"; return false; }
        ue::UObject *inst = nullptr, *mgr = nullptr;
        int seen = 0;
        ue::ForEach([&](int, ue::UObject* o) {
            if (ue::IsDefault(o) || !ue::Live(o)) return true;
            if (!inst && ue::ObjName(o) == name && ue::ClassName(o).find("DataLayerInstance") != std::string::npos && InWorld(o, world)) { inst = o; ++seen; }
            else if (!mgr && ue::ClassName(o) == "DataLayerManager" && InWorld(o, world)) mgr = o;
            return !(inst && mgr);
        });
        if (!inst || !mgr) { *msg = std::format("当前世界里找不到数据层 {}（实例 {}，管理器 {}）", name, inst ? "有" : "无", mgr ? "有" : "无"); return false; }
        const int before = LayerState(mgr, inst, "GetDataLayerInstanceRuntimeState");
        const int effBefore = LayerState(mgr, inst, "GetDataLayerInstanceEffectiveRuntimeState");
        if (before < 0 || effBefore < 0) { *msg = "数据层状态读不出"; return false; }
        if (effBefore == kActivated) { pbz::log::Info("影塔数据层：{} 已是激活状态，不改", name); return true; }
        if (!SetLayer(mgr, inst, kActivated)) { *msg = std::format("激活 {} 被拒（原状态 {}）", name, before); return false; }
        const int after = LayerState(mgr, inst, "GetDataLayerInstanceRuntimeState");
        const int eff = LayerState(mgr, inst, "GetDataLayerInstanceEffectiveRuntimeState");
        layer = {mgr, inst, ue::ClassOf(inst), name, static_cast<uint8_t>(before), true};
        pbz::log::Info("影塔数据层：激活 {}（原状态 {} / 生效 {} → {} / 生效 {}），离开场地后还原", name, before, effBefore, after, eff);
        if (eff != kActivated) { *msg = std::format("激活后读回生效状态 {}（父层未激活？）", eff); RestoreLayer("激活读回不对"); return false; }
        return true;
    }
    void RestoreLayer(const char* why) {
        namespace ue = pbz::ue;
        if (!layer.held) return;
        if (ue::Live(layer.inst) && ue::Live(layer.mgr) && ue::ClassOf(layer.inst) == layer.instCls && ue::ObjName(layer.inst) == layer.name) {
            const bool ok = SetLayer(layer.mgr, layer.inst, layer.original);
            pbz::log::Info("影塔数据层：{}，还原 {} → 状态 {}（{}，读回 {}）", why, layer.name, layer.original, ok ? "成功" : "被拒",
                           LayerState(layer.mgr, layer.inst, "GetDataLayerInstanceRuntimeState"));
        } else {
            pbz::log::Info("影塔数据层：{}，{} 所在世界已卸载，不再还原（新世界按存档决定）", why, layer.name);
        }
        layer = {};
    }
};

class Travel : public pbz::IFeature {
public:
    const char* Id() const override { return "tower.travel"; }
    bool IsToggle() const override { return true; }
    bool DefaultOn() const override { return true; }
    bool Resolve(std::string*) override { return true; }
    void Enable() override { enabled = true; pbz::HoldFeature(this, "tower"); }  // 同 tower.ui：自己持有，修改器"全部还原"不关它
    void Disable() override { enabled = false; t_.Reset(); HoldMove(false); }

    // 悬停等加载靠 world.map_tp 的每帧 Tick：旅行期间影塔自己持有它，修改器关窗 / "全部还原"关掉也不影响塔的传送；
    // 到了 / 失败就放手，用户在修改器里的开关照旧（2026-10-31 实测关窗后点传送"未等待加载"）
    void HoldMove(bool on) {
        if (on == holdingMove_) return;
        auto* f = pbz::FindFeature("world.map_tp");
        if (!f) { if (on) pbz::log::Info("影塔传送：没有 world.map_tp，远处落点无法悬停等加载"); return; }
        if (on) holdingMove_ = pbz::HoldFeature(f, "tower");
        else { pbz::ReleaseFeature(f, "tower"); holdingMove_ = false; }
    }

    void Tick(float dt) override {
#ifdef PBZ_HEX_TEST
        towervenue::SampleTick();
#endif
        towerperf::Section _("影塔传送");
        if (towerdiag::SandboxOnly()) { if (t_.Active()) { t_.Reset(); pbz::log::Info("影塔传送：诊断模式，取消进行中的旅行"); } HoldMove(false); return; }
        if (!t_.Active()) { if (holdingMove_ && !pbz::TeleportBusy()) HoldMove(false); return; }
        HoldMove(true);
        const auto s = t_.Step(game_, dt);
        if (s == State::Done) {
            pbz::log::Info("影塔传送：已到 {}", t_.dest().name);
            // 1.0.1（玩家反馈"每层未处于目标场地被跳过"）：传送可能按"站在可站立地面上"判到达（AcceptSurfaceHeight），
            // 这时人和数据里的目标点会差好几米高度。到达点记实际站位，开打前复核按它比
            { Pose here; std::string lvHere; arrival_ = game_.Where(&here, &lvHere) ? here : t_.dest().target; }
            // 已离开租用数据层的场地（到了别的落点、站稳了）才还原：还原时人若还站在那层的地面上会坠空
            if (game_.layer.held && t_.dest().dataLayer != game_.layer.name) game_.RestoreLayer("已离开该场地");
            if (goingHome_) { DeleteFileW(TowerFile(L"home.txt").c_str()); goingHome_ = false; }
        } else if (s == State::Failed) {
            pbz::log::Info("影塔传送：去 {} 失败：{}{}", t_.dest().name, t_.why(), t_.stop() ? "（停机，不再自动重试）" : "");
            goingHome_ = false;
            HoldMove(false);  // 失败立即放开地图传送持有（运行库已结束自己的悬停）
            // 去租用层场地失败：运行库已把人传回原处（不在那层地面上）才还原；人仍在落点附近就保持，等下一次到别处再还原
            if (game_.layer.held && t_.dest().dataLayer == game_.layer.name) {
                Pose p; std::string lv;
                if (game_.Where(&p, &lv) && DistXY(p, t_.dest().target) > Traveler::kArrive) game_.RestoreLayer("去场地失败，人已回原处");
            }
        }
    }

    // 运行库地面查询拒绝过、直接放置成功的落点（本机 venue-direct.txt："关卡 x y"，一行一个；10 米内算同一落点）
    bool DirectKnown(const std::string& level, const Pose& at) {
        // 发布版内置（开发机 venue-direct.txt 实测记下的，2026-11-05）：玩家机器上没有这份文件，首次到这些场地也不用悬空等
        struct Known { std::string_view level; double x, y; };
        static constexpr Known kBuiltin[] = {{"PBZ_Duanhun_WP", -194658, -97353}, {"PBZ_Pangzhen_WP", -179165, -1401}};
        for (const auto& k : kBuiltin) if (k.level == level && std::hypot(k.x - at.x, k.y - at.y) < 1000) return true;
        std::istringstream in(ReadFile(TowerFile(L"venue-direct.txt")));
        std::string lv; double x, y;
        while (in >> lv >> x >> y) if (lv == level && std::hypot(x - at.x, y - at.y) < 1000) return true;
        return false;
    }
    void RememberDirect(const std::string& level, const Pose& at) {
        if (level.empty() || DirectKnown(level, at)) return;
        WriteFile(TowerFile(L"venue-direct.txt"), ReadFile(TowerFile(L"venue-direct.txt")) + std::format("{} {:.0f} {:.0f}\n", level, at.x, at.y));
        pbz::log::Info("影塔传送：记下直接放置落点 {} ({:.0f}, {:.0f})，下次不再悬空等待", level, at.x, at.y);
    }
    std::string directLevel_;
    Centers LoadCenters() { return ParseCenters(ReadFile(TowerFile(L"arenas.local.txt"))); }

    int Arena(int zone, uint64_t seed) { return ChooseArena(zone, seed, LoadCenters()); }

    // 本层去哪（用户 2026-10-31：Boss 层用 Boss 自己的原生场地）：玩家手动标定的 boss:<身份> 优先，
    // 其次游戏数据里的原生刷怪点，都没有才去本区通用擂台。已在那里（同关卡 40 米内）返回 0，开始传送返回 1，没有目的地返回 -1
    int GoFloor(int zone, uint64_t seed, std::string_view path, std::string_view identity, std::string_view name, bool scene) {
        const auto centers = LoadCenters();
        Dest d;
        const char* kind = "通用擂台";
        if (FixedDest(path, &d)) kind = "固定场地";
        else if (BossDest(identity, name, centers, &d)) kind = "标定场地";
        else if (NativeDest(path, name, &d)) kind = "原生场地";
        else {
            if (scene) pbz::log::Info("影塔传送：{}（{}）要回原生场地打，但没有原生刷怪点也没标定，先用本区通用擂台", name, identity);
            const int arena = ChooseArena(zone, seed, centers);
            if (arena < 0) return -1;
            d = ArenaDest(arena, centers);
        }
        return Go(d, kind);
    }

    int GoArena(int index) { return index < 0 ? -1 : Go(ArenaDest(index, LoadCenters()), "通用擂台"); }

    int Go(Dest d, const char* kind) {
        if (towerdiag::SandboxOnly()) return -1;
        FillBell(&d, [this](const std::string& id) { return game_.BellLevel(id); });
        Pose p; std::string lv;
        const auto targetLevel = !d.level.empty() ? d.level : game_.BellLevel(d.bell);
        arrival_=d.target; arrivalLevel_=targetLevel; haveArrival_=true;
        // 已在场内起点才不动：6 米内、高度差 3 米内。313987b 第 4 层反例：同图上一层结束时玩家走到了雾门边（40 米内），
        // 旧判据视为已就位、原地开层，Boss 在玩家面前 8 米生成到了雾门另一侧。现在每层都回到场内起点
        if (game_.Where(&p, &lv) && SameLevel(lv, targetLevel) && AtSpot(p, d.target, 600)) return 0;
        goingHome_ = false;
        pbz::log::Info("影塔传送：前往{} {}（{}，经钟 {}）", kind, d.name, targetLevel.empty() ? "关卡未知" : targetLevel, d.bell.empty() ? "无" : d.bell);
        // 34 层力者断魂冰面（ui10 回归）：缓存曾只当证据、不再先直接放 → 运行库悬空 20 秒后传回原处，之后直接放又被冰面动态体积挡住。
        // 实机放置成功过的落点：先直接放（落点仍做地面审计，胶囊只信静态通道）
        d.directFirst = g_directKnown = DirectKnown(targetLevel, d.target);
        if (d.directFirst) pbz::log::Info("影塔传送：{} 的落点曾实机直接放置成功，到后直接放置（不再悬空等待）", d.name);
        HoldMove(true);
        t_.Go(d);
        directLevel_ = targetLevel;
        return 1;
    }

    // 第一次离开时记下进塔位置；已有记录（同一局多次进出 / 上次没传回）保留最早的
    bool RecordHome() {
        const auto path = TowerFile(L"home.txt");
        if (ParseHome(ReadFile(path)).valid) return true;
        Home h;
        if (!game_.Where(&h.at, &h.level)) return false;
        h.bell = NearestBell(h.at, h.level, [this](const std::string& id) { return game_.BellLevel(id); });
        h.valid = true;
        WriteFile(path, FormatHome(h));
        pbz::log::Info("影塔传送：记下进塔位置 {} ({:.0f}, {:.0f}, {:.0f})，回程钟 {}", h.level, h.at.x, h.at.y, h.at.z, h.bell.empty() ? "无" : h.bell);
        return true;
    }

    void GoHome() {
        if (towerdiag::SandboxOnly()) return;
        const auto h = ParseHome(ReadFile(TowerFile(L"home.txt")));
        if (!h.valid) return;
        pbz::log::Info("影塔传送：出塔，传回进塔位置");
        goingHome_ = true;
        HoldMove(true);
        t_.Go(HomeDest(h));
    }

    void Abort(const char* why) {
        if (!t_.Active()) return;
        pbz::log::Info("影塔传送：取消前往 {}（{}）——后续传送不再执行，已发出的原生加载交给游戏落地", t_.dest().name, why);
        t_.Reset();
        goingHome_ = false;  // 地图传送持有在悬停结束后由 Tick 放开
    }
    State Status() const { return t_.Active() ? State::Level : t_.state(); }
    bool Busy() const { return t_.Active(); }
    std::string Why() const { return t_.why(); }
    bool Stop() const { return t_.stop(); }
    bool AtDestination() {
        Pose p;std::string lv;
        // 水平仍按 15 米；高度放宽到 6 米（读秒前角色可能刚落地、上下台阶）
        return haveArrival_ && game_.Where(&p,&lv) && SameLevel(lv,arrivalLevel_) && Finite(p) && Finite(arrival_) &&
               DistXY(p,arrival_) <= Traveler::kArrive && std::abs(p.z-arrival_.z) <= 600;
    }

    bool ArrivalHere(double out[4]) {
        Pose p; std::string lv;
        if (!haveArrival_ || !Finite(arrival_) || !game_.Where(&p, &lv) || !SameLevel(lv, arrivalLevel_)) return false;
        out[0] = arrival_.x; out[1] = arrival_.y; out[2] = arrival_.z; out[3] = arrival_.yaw;
        return true;
    }

    std::string Command(const std::string& value, const std::string& id) {
        auto centers = LoadCenters();
        if (value == "mark") {  // id：擂台 id，或 boss:<身份>（Boss 原生场地）
            bool known = id.rfind("boss:", 0) == 0 && id.size() > 5;
            for (auto& a : kArenas) known |= a.id == id;
            if (!known) return R"("ok":false,"msg":"未知 id：擂台 id 或 boss:<身份>")";
            Pose p; std::string lv;
            if (!game_.Where(&p, &lv)) return R"("ok":false,"msg":"找不到玩家")";
            centers[id] = {p, lv};
            WriteFile(TowerFile(L"arenas.local.txt"), FormatCenters(centers));
            pbz::log::Info("影塔擂台标定：{} = {} ({:.0f}, {:.0f}, {:.0f}) 朝向 {:.0f}", id, lv, p.x, p.y, p.z, p.yaw);
            return std::format(R"("ok":true,"msg":"已标定","id":"{}","level":"{}","x":{:.0f},"y":{:.0f},"z":{:.0f})", id, lv, p.x, p.y, p.z);
        }
        if (value == "unmark") {
            centers.erase(id);
            WriteFile(TowerFile(L"arenas.local.txt"), FormatCenters(centers));
            return R"("ok":true,"msg":"已清除")";
        }
        if (value == "go") {
            for (int i = 0; i < static_cast<int>(std::size(kArenas)); ++i)
                if (kArenas[i].id == id) { GoArena(i); return R"("ok":true,"msg":"前往擂台")"; }
            if (auto it = centers.find(id); it != centers.end()) {
                Dest d; d.name = id; d.target = it->second.at; d.level = it->second.level;
                Go(d, "标定点");
                return R"("ok":true,"msg":"前往标定点")";
            }
            if (id == "home") { GoHome(); return R"("ok":true,"msg":"传回进塔位置")"; }
            return R"("ok":false,"msg":"未知 id")";
        }
        // list
        std::string s = R"("ok":true,"arenas":[)";
        for (size_t i = 0; i < std::size(kArenas); ++i) {
            const auto& a = kArenas[i];
            auto it = centers.find(std::string(a.id));
            s += std::format(R"({}{{"id":"{}","zone":{},"map":"{}","bell":"{}","marked":{}}})", i ? "," : "", a.id, a.zone, a.map, a.bell, it != centers.end() ? "true" : "false");
        }
        s += R"(],"spots":[)";
        bool first = true;
        for (auto& [k, v] : centers) {
            s += std::format(R"({}{{"id":"{}","level":"{}","x":{:.0f},"y":{:.0f},"z":{:.0f}}})", first ? "" : ",", pbz::ipc::Esc(k), pbz::ipc::Esc(v.level), v.at.x, v.at.y, v.at.z);
            first = false;
        }
        return s + std::format(R"(],"travel":"{}","why":"{}")", t_.Active() ? "busy" : t_.state() == State::Done ? "done" : t_.state() == State::Failed ? "failed" : "idle", pbz::ipc::Esc(t_.why()));
    }

private:
    GameMove game_;
    Traveler t_;
    Pose arrival_; std::string arrivalLevel_; bool haveArrival_=false;
    bool goingHome_ = false, holdingMove_ = false;
};

Travel* g_travel = nullptr;

}  // namespace

namespace towertravel {
bool HasVenue(std::string_view path, std::string_view identity) {
    Dest d;
    return FixedDest(path, &d) || NativeDest(path, "", &d) || (g_travel && g_travel->LoadCenters().count(BossKey(identity)) > 0);
}
void Register() { auto t = std::make_unique<Travel>(); g_travel = t.get(); pbz::AddFeature(std::move(t)); }
int ZoneArena(int zone, uint64_t seed) { return g_travel ? g_travel->Arena(zone, seed) : -1; }
int GoArena(int index) { return g_travel ? g_travel->GoArena(index) : -1; }
bool ArenaVerified(int index) {
    if (index < 0 || index >= static_cast<int>(std::size(kArenas))) return false;
    return tower::arena::ParseVerified(ReadFile(TowerFile(L"arena-verified.txt"))).count(std::string(kArenas[index].id)) > 0;
}
int GoFloor(int zone, uint64_t seed, std::string_view path, std::string_view identity, std::string_view name, bool scene) {
    return g_travel ? g_travel->GoFloor(zone, seed, path, identity, name, scene) : -1;
}
bool RecordHome() { return g_travel && g_travel->RecordHome(); }
void GoHome() { if (g_travel) g_travel->GoHome(); }
void Abort(const char* why) { if (g_travel) g_travel->Abort(why); }
void ForgetHome() { DeleteFileW(TowerFile(L"home.txt").c_str()); }
bool Busy() { return g_travel && g_travel->Busy(); }
bool Failed() { return g_travel && !g_travel->Busy() && g_travel->Status() == tower::travel::State::Failed; }
std::string Why() { return g_travel ? g_travel->Why() : std::string(); }
bool Stopped() { return Failed() && g_travel->Stop(); }
bool AtDestination() { return g_travel && g_travel->AtDestination(); }
bool ArrivalHere(double out[4]) { return g_travel && g_travel->ArrivalHere(out); }
std::string Command(const std::string& value, const std::string& id) { return g_travel ? g_travel->Command(value, id) : R"("ok":false,"msg":"未就绪")"; }
}  // namespace towertravel
