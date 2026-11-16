// 进出独立影塔档（纯逻辑，游戏端经 IIo；离线测试接假的）：
//   进塔：备份真档 → 存档沙盒（记进塔基准）→ 停成就 → 用基准 + 塔档生成塔内存档 → 放进游戏内存 → 回标题继续载入
//   出塔：把进塔基准放回内存 → 回标题继续载入 → 核验游戏里真的是原档 → 才关沙盒、恢复成就
// 2026-10-31 实机反例（13f7e98）：出塔时还发了跨图回程，钟传送本身会存档再载入，把放回去的基准盖成塔内状态；
// 旧判据"背包换了新对象 + 玩家稳定 3 秒"在跨图时也成立，提前关了沙盒，回标题继续读到的仍是塔档。所以：
//   1. 只有回过标题（标题世界出现过）之后的新背包才算"重新载入"；
//   2. 到达标题时再放一次目标存档（此时塔内世界已卸载，不会再有塔内状态的保存把它盖掉）；
//   3. 载入后逐项核验（装备、兵器等级、能力、药、尘、材料），不一致就重新放回、保持保护，再回一次标题。
// 状态落盘（tower-session.txt）：上次没走完（崩溃 / 运行库热换）读回来是 Stale，由玩家选择"恢复原进度"或"确认已是原进度"。
//
// 自动切档（用户 2026-11-01：敲钟进塔 / 出塔不再手动回标题、点继续）：放好目标存档后，引擎自己走游戏原生的
// "返回标题"（TsHelper.returnStart：ChangeGameType(StartMenu) + OpenLevel 开始菜单图）和"继续"（PBZGameInstance.LoadGame
// 续玩槽 + OpenLevel 存档关卡）。玩家手动回标题 / 继续照样被接受（自动步骤失败时的退路）。
// 一次只有一个事务（txn）：进塔中再点进塔被拒；加载进行中要出塔（暂离 / 取消）时先受理，等原生加载落稳再放回原档。
// 每一步有上限：放存档 5 次、回标题 / 继续各 2 次、核验 3 次、整个事务 4 分钟；超了记失败原因，保护一直开着，不自动关、不宣称完成。
// 313987b 实机反例（03:01:55）：跨图加载中暂离，ReplaceSaveString 失败后仍回报"已暂离"——放存档失败现在不是完成，而是"待重试"。
// fa164d1 实机反例（05:05:45）：原档核验通过、同一刻关存档锁 + 关文件沙盒，1.28 秒后原生写了主档和轮转档（用户没操作）。
//   静态证据：存档锁开着时 TrySave 返回 Suspended、排队（PBZSaveLoadSubsystem.HandleSuspendedFireComplete），放开锁就补存；
//   还有 QuiescentAuto（空闲自动存）、ForceSave（绕过锁）。所以出塔的"完成"分两步（Step::Settle）：
//   先只放开存档锁、文件沙盒仍开着，让排队 / 空闲的原生存档落进沙盒；等原生存档不再脏、沙盒连续 5 秒没有写入、
//   没有加载，才关沙盒。90 秒内静不下来就停在失败（沙盒仍开，原档不会被写），不靠单纯延时、不忽略差异。
#pragma once
#include <cstdint>
#include <format>
#include <string>
#include <vector>
#include <string_view>

#include "tower_save.h"

namespace tower::trip {

enum class Phase { Outside, Entering, Inside, Exiting, Stale };
inline std::string_view PhaseName(Phase p) {
    switch (p) {
        case Phase::Outside: return "outside";
        case Phase::Entering: return "entering";
        case Phase::Inside: return "inside";
        case Phase::Exiting: return "exiting";
        case Phase::Stale: return "stale";
    }
    return "?";
}
// 事务内的步骤：Put 等放存档（加载中 / 放失败待重试）→ Title 等回标题 → Load 已点继续、等载入 → 核验；Failed 超限停住（保护保持）
enum class Step { None, Put, Title, Load, Settle, Failed };
inline std::string_view StepName(Step s) {
    switch (s) {
        case Step::None: return "none";
        case Step::Put: return "put";
        case Step::Title: return "title";
        case Step::Load: return "load";
        case Step::Settle: return "settle";
        case Step::Failed: return "failed";
    }
    return "?";
}

struct IIo {
    virtual ~IIo() = default;
    virtual bool Backup(std::string* why) = 0;                       // 真档备份（绕开沙盒）
    // 文件沙盒 + 存档锁；snapshot=true 时再记进塔基准（出塔时绝不能重记：内存里是塔内状态）
    virtual bool SandboxOn(bool snapshot, std::string* why) = 0;
    virtual bool Baseline(std::string* json) = 0;                    // 进塔基准（UTF-8；内存里没有就读落盘的 baseline.json）
    virtual bool Replace(const std::string& json, bool isBaseline, std::string* why) = 0;  // 放进游戏内存，并重新开始"等重新载入"
    virtual bool AtTitle() = 0;                                      // 现在在标题画面（标题世界存在、没有玩家角色）
    virtual bool Reloaded() = 0;                                     // 上次 Replace 之后回过标题、又进了档：新背包、玩家稳定
    virtual bool Verify(const std::string& expect, std::string* diff) = 0;  // 游戏里的实际内容与这份存档一致
    virtual bool SandboxOff(std::string* why) = 0;
    virtual void Achievements(bool blocked) = 0;
    // 自动切档（默认实现 = 不支持，只走手动）
    virtual bool Calm() { return true; }                             // 没有进行中的原生加载 / 传送（标题画面上也算）
    virtual bool ToTitle(std::string* why) { *why = "不支持自动回标题"; return false; }  // 原生返回标题
    virtual bool TitleReady() { return true; }                       // 标题画面已就绪、可以点继续
    virtual bool Continue(std::string* why) { *why = "不支持自动继续"; return false; }   // 原生"继续"：载入续玩槽
    virtual uint64_t Now() { return 0; }                             // 毫秒（墙钟）
    // 出塔收尾（默认 = 没有要等的原生存档，核验通过即关保护）
    virtual bool SettleSaves() { return false; }                     // 有原生存档引擎要等它落稳
    virtual bool ReleaseLock(std::string* why) { (void)why; return true; }  // 只放开存档锁（文件沙盒保持）
    virtual uint64_t SandboxActivity() { return 0; }                 // 沙盒累计改道的写 / 删 / 改名次数
    virtual bool SaveClean(std::string* why) { (void)why; return true; }    // 原生存档没有未落盘的改动
    // 世界模板（用户 2026-11-06）：塔档的世界 / 剧情 / 地图状态统一用随包的固定存档，不用玩家自己的进度——
    // 打过三面的存档伏极之林变成雪景、场地和刷怪点都变了，第 8 层白赤练就开不起来。没有模板 = 退回玩家原档
    virtual bool WorldTemplate(std::string* json) { (void)json; return false; }
};

class Trip {
public:
    static constexpr uint64_t kTxnMs = 240000, kPutGapMs = 2000, kTitleMs = 45000, kContinueMs = 40000, kTitleSettleMs = 1500;
    static constexpr uint64_t kQuietMs = 5000, kSettleMaxMs = 90000;
    static constexpr int kPuts = 5, kAsks = 2, kVerifies = 3;

    Phase phase() const { return phase_; }
    Step step() const { return step_; }
    bool failed() const { return step_ == Step::Failed; }
    bool busy() const { return phase_ == Phase::Entering || phase_ == Phase::Exiting; }
    bool pending() const { return step_ == Step::Put; }  // 已受理、目标存档还没放进去（等加载落稳 / 重试中）
    const std::string& error() const { return error_; }
    int retries() const { return retries_; }
    int txn() const { return txn_; }
    bool automatic() const { return auto_; }
    void SetAuto(bool on) { auto_ = on; }

    bool Enter(IIo& io, const growth::Profile& p, const save::Equip& eq, const std::string& stamp) {
        if (phase_ != Phase::Outside) return Fail(phase_ == Phase::Entering ? "正在进入塔档，不能再次进塔" : "已经在影塔会话里（或上次没正常结束）");
        if (!io.Calm()) return Fail("游戏正在加载或传送，稍后再进塔");
        std::string why, base;
        if (!io.Backup(&why)) return Fail("真档备份失败：" + why);
        if (!io.SandboxOn(true, &why)) return Fail("存档沙盒开不了：" + why);
        io.Achievements(true);
        if (!io.Baseline(&base)) return Undo(io, "读不到进塔基准");
        json::Value b;
        if (!json::Parse(base, &b)) return Undo(io, "进塔基准不是有效的存档 JSON");
        json::Value w;
        const json::Value& world = WorldBase(io, b, &w);
        auto built = save::Build(world, p, eq, stamp, usedTemplate_ ? &b : nullptr);  // 赠品取玩家自己的
        if (!built.ok) return Undo(io, "塔内存档生成失败：" + built.why);
        target_ = json::Dump(built.save);
        if (!io.Replace(target_, false, &why)) return Undo(io, "塔内存档放不进游戏：" + why);
        Arm(io, Phase::Entering, Step::Title);
        placed_ = true;
        return true;
    }
    // 塔内重开（用户 2026-11-06：死亡结算"重新开始"留在塔档里从第 0 层爬，不回原档）：已在塔档里时，用进塔基准 + 新塔档
    // 重新生成塔内存档，放进内存、回标题继续、照常核验。沙盒 / 存档锁 / 成就保持进塔时的状态，不再备份真档、不重记基准
    bool Rebuild(IIo& io, const growth::Profile& p, const save::Equip& eq, const std::string& stamp) {
        if (phase_ != Phase::Inside) return Fail("不在塔档里，不能塔内重开");
        if (!io.Calm()) return Fail("游戏正在加载或传送，稍后再重开");
        std::string why, base;
        if (!io.Baseline(&base) || base.size() < 64) return Fail("读不到进塔基准");
        json::Value b;
        if (!json::Parse(base, &b)) return Fail("进塔基准不是有效的存档 JSON");
        json::Value w;
        const json::Value& world = WorldBase(io, b, &w);
        auto built = save::Build(world, p, eq, stamp, usedTemplate_ ? &b : nullptr);
        if (!built.ok) return Fail("塔内存档生成失败：" + built.why);
        const std::string target = json::Dump(built.save);
        if (!io.Replace(target, false, &why)) return Fail("塔内存档放不进游戏：" + why);
        target_ = target;
        Arm(io, Phase::Entering, Step::Title);
        placed_ = true;
        return true;
    }
    bool usedTemplate() const { return usedTemplate_; }
    const std::vector<std::string>& droppedKeys() const { return droppedKeys_; }  // 原档有、模板没有、没带进塔档的顶层分区
    // 塔档的底：有兼容的世界模板就用模板（玩家原档的每个顶层分区模板里都有，且有背包 / 角色 / 能力），否则用玩家原档
    const json::Value& WorldBase(IIo& io, const json::Value& baseline, json::Value* holder) {
        usedTemplate_ = false;
        std::string text;
        if (!io.WorldTemplate(&text) || !json::Parse(text, holder) || holder->kind != json::Value::Object) return baseline;
        for (const char* need : {"Inventory", "Player", "AbilityNode_RenewalHun"}) if (!holder->Find(need)) return baseline;
        // 1.0.1 玩家反馈（二周目存档进塔后血量还是原档的）：原来玩家原档只要多一个模板没有的顶层分区就整份改用原档，
        // 二周目 / 通关后的存档正好多出分区，于是周目数、角色成长、能力全跟进了塔档。现在模板齐全就一律用模板；
        // 原档独有的分区不带进塔档（和新档一样没有这些系统），记下来写日志
        droppedKeys_.clear();
        for (const auto& [key, unused] : baseline.fields) if (!holder->Find(key)) droppedKeys_.push_back(key);
        usedTemplate_ = true;
        return *holder;
    }
    // 出塔（也用于进塔途中取消、上次没走完的恢复）：放回基准，回标题重新载入。
    // 返回 false = 拒绝（没有基准等），状态不变；true = 已受理：已放好（pending()==false），或等原生加载落稳 / 重试后再放（pending()==true）
    bool Exit(IIo& io) {
        if (phase_ == Phase::Outside) return Fail("不在影塔会话里");
        if (phase_ == Phase::Exiting && step_ != Step::Failed) return Fail("正在恢复原进度，不能重复出塔");
        std::string why, base;
        if (!io.SandboxOn(false, &why)) return Fail("存档沙盒开不了：" + why);  // Stale（热换后）时重新挂上钩子，不重记基准
        if (!io.Baseline(&base) || base.size() < 64) return Fail("读不到进塔基准");
        target_ = base;
        Arm(io, Phase::Exiting, Step::Put);
        TryPut(io);
        return true;
    }
    // 失败停住后再试一次（保护一直开着）：重新计时、重新放目标存档
    bool Retry(IIo& io) {
        if (!busy() || step_ != Step::Failed) return Fail("没有失败待重试的切档");
        if (settled_ && phase_ == Phase::Exiting) {  // 原档已核验、只是收尾没静下来：接着等，不重新载入
            step_ = Step::Settle;
            settleSince_ = quietSince_ = io.Now();
            activity_ = io.SandboxActivity();
            error_.clear();
            return true;
        }
        const Phase p = phase_;
        Arm(io, p, Step::Put);
        TryPut(io);
        return true;
    }

    // 每隔一会儿推进。返回 true 表示刚进塔；刚出塔看 phase()==Outside
    bool Tick(IIo& io) {
        if (!busy()) return false;
        const bool exiting = phase_ == Phase::Exiting;
        const uint64_t now = io.Now();
        if (step_ == Step::Settle) return Settle(io, now);
        if (step_ == Step::Failed && settled_) return false;  // 已核验、收尾超时停住：等玩家重试，不自己再来一轮
        if (step_ != Step::Failed && auto_ && now - begun_ > kTxnMs) Stop(std::string(exiting ? "恢复原进度" : "进入塔档") + "超过 4 分钟未完成：" + (error_.empty() ? std::string(StepName(step_)) : error_));
        if (step_ == Step::Put && !TryPut(io)) return false;
        // 到了标题：再放一次目标存档（塔内世界已卸载，不会再被塔内状态的保存盖掉）
        if (io.AtTitle()) {
            if (!titlePut_) {
                std::string why;
                titlePut_ = io.Replace(target_, exiting, &why);
                if (!titlePut_) error_ = "到标题时放回存档失败：" + why;
                placed_ = placed_ || titlePut_;
                titleSince_ = now;
                return false;
            }
            if (!auto_ || step_ == Step::Failed) return false;
            if (!io.TitleReady()) { titleSince_ = now; return false; }
            if (step_ != Step::Load) { if (now - titleSince_ >= kTitleSettleMs) Ask(io, Step::Load, now); }
            else if (now - askedAt_ > kContinueMs) Ask(io, Step::Load, now);  // 点了继续却还在标题：再点一次
            return false;
        }
        if (!placed_) return false;  // 本事务的目标存档一次也没放进去：载入的不可能是它，不核验（重新载入信号是旧的）
        if (!io.Reloaded()) {
            // 还在旧世界：自动请求回标题；请求了却一直没到标题就再请求一次（加载 / 传送中不请求）
            if (auto_ && step_ == Step::Title && io.Calm() && (!asks_ || now - askedAt_ > kTitleMs)) Ask(io, Step::Title, now);
            return false;
        }
        std::string diff;
        if (!io.Verify(target_, &diff)) {
            // 进了档但内容不对：重新放回目标存档、保持保护，再回一次标题
            ++retries_;
            error_ = std::string(exiting ? "原档" : "塔档") + "核验未通过（" + diff + "），保护保持开启";
            std::string why;
            if (!io.Replace(target_, exiting, &why)) error_ += "；重新放回失败：" + why;
            titlePut_ = false;
            if (auto_ && retries_ >= kVerifies) { Stop(error_); return false; }
            if (step_ != Step::Failed) { step_ = Step::Title; asks_ = 0; }
            if (!auto_) error_ += "；请再回标题继续一次";
            return false;
        }
        if (!exiting) { phase_ = Phase::Inside; step_ = Step::None; error_.clear(); return true; }
        if (io.SettleSaves()) {
            std::string why;
            if (!io.ReleaseLock(&why)) { error_ = "放开存档锁失败：" + why; return false; }  // 下次再试，保护全开着
            step_ = Step::Settle;
            settled_ = true;
            settleSince_ = quietSince_ = now;
            activity_ = io.SandboxActivity();
            flushed_ = 0;
            error_.clear();
            return false;
        }
        Finish(io);
        return false;
    }
    int flushed() const { return flushed_; }  // 收尾期间落进沙盒的原生写入次数
    // 玩家确认现在已经是原进度（例如游戏重启后读的是磁盘真档）：只清标记
    void Forget() { phase_ = Phase::Outside; step_ = Step::None; error_.clear(); target_.clear(); }

    std::string Save() const { return std::string("phase=") + std::string(PhaseName(phase_)) + "\n"; }
    static Trip Load(std::string_view text) {
        Trip t;
        if (text.find("phase=") != std::string_view::npos && text.find("phase=outside") == std::string_view::npos) t.phase_ = Phase::Stale;
        return t;
    }

private:
    // 收尾：存档锁已放开、沙盒仍开着。排队 / 空闲的原生存档落进沙盒；静下来才关沙盒
    bool Settle(IIo& io, uint64_t now) {
        if (const uint64_t a = io.SandboxActivity(); a != activity_) { activity_ = a; quietSince_ = now; ++flushed_; }
        std::string why;
        const bool clean = io.SaveClean(&why);
        if (!clean || !io.Calm()) quietSince_ = now;
        if (now - quietSince_ >= kQuietMs) {
            if (!io.SandboxOff(&why)) { error_ = "关沙盒被拒绝：" + why; return false; }
            Finish(io);
            return false;
        }
        if (now - settleSince_ > kSettleMaxMs)
            Stop(std::format("原档已核验，但原生存档 90 秒内没有安静下来（{}；落进沙盒 {} 次）：沙盒保持开启，原档未被写", clean ? "仍有写入或加载" : why, flushed_));
        return false;
    }
    void Finish(IIo& io) {
        std::string why;
        if (step_ != Step::Settle && !io.SandboxOff(&why)) { error_ = "关沙盒被拒绝：" + why; return; }  // 仍保持保护，下次再试
        io.Achievements(false);
        phase_ = Phase::Outside;
        step_ = Step::None;
        error_.clear();
        target_.clear();
    }
    void Arm(IIo& io, Phase p, Step s) {
        phase_ = p; step_ = s; titlePut_ = placed_ = settled_ = false; retries_ = puts_ = asks_ = 0; error_.clear();
        begun_ = io.Now(); askedAt_ = putAt_ = titleSince_ = 0; ++txn_;
    }
    // 放目标存档：原生加载 / 传送进行中不放（放了会被落地存档盖掉，或直接失败），落稳后再放；失败隔 2 秒重试，最多 5 次
    bool TryPut(IIo& io) {
        if (!io.Calm()) return false;  // 等原生加载落稳（不是错误；step()==Put 时界面显示"等待"）
        const uint64_t now = io.Now();
        if (puts_ && now - putAt_ < kPutGapMs) return false;
        std::string why;
        putAt_ = now;
        ++puts_;
        if (!io.Replace(target_, phase_ == Phase::Exiting, &why)) {
            error_ = std::format("{}放不进游戏（第 {} 次）：{}", phase_ == Phase::Exiting ? "原档基准" : "塔内存档", puts_, why);
            if (puts_ >= kPuts) Stop(error_);
            return false;
        }
        error_.clear();
        step_ = Step::Title;
        placed_ = true;
        titlePut_ = io.AtTitle();  // 在标题上放的：不用再放一次
        titleSince_ = now;
        return true;
    }
    void Ask(IIo& io, Step s, uint64_t now) {
        if (s != step_) asks_ = 0;
        step_ = s;
        if (asks_ >= kAsks) { Stop(s == Step::Title ? "自动返回标题没有反应（可手动回标题）" : "自动继续没有载入存档（可在标题手动点继续）"); return; }
        ++asks_;
        askedAt_ = now;
        std::string why;
        if (s == Step::Load) {
            // 点继续前再放一次：开始菜单初始化时会探测续玩槽
            if (!io.Replace(target_, phase_ == Phase::Exiting, &why)) { error_ = "点继续前放存档失败：" + why; return; }
            if (!io.Continue(&why)) error_ = "自动继续失败：" + why;
        } else if (!io.ToTitle(&why)) error_ = "自动返回标题失败：" + why;
    }
    void Stop(std::string why) { step_ = Step::Failed; error_ = std::move(why); }
    bool Fail(std::string why) { error_ = std::move(why); return false; }
    bool Undo(IIo& io, std::string why) {  // 进塔中途失败：内存没被换过，关掉刚开的保护
        std::string w;
        io.SandboxOff(&w);
        io.Achievements(false);
        return Fail(std::move(why));
    }
    Phase phase_ = Phase::Outside;
    Step step_ = Step::None;
    std::string error_, target_;
    bool titlePut_ = false, auto_ = false, placed_ = false, settled_ = false;
    bool usedTemplate_ = false;  // 本次塔档以世界模板为底
    std::vector<std::string> droppedKeys_;
    int retries_ = 0, puts_ = 0, asks_ = 0, txn_ = 0;
    uint64_t begun_ = 0, askedAt_ = 0, putAt_ = 0, titleSince_ = 0, settleSince_ = 0, quietSince_ = 0, activity_ = 0;
    int flushed_ = 0;
};

}  // namespace tower::trip
