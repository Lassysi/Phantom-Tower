// 训练场：伤害统计（train.dps）、敌人数值（train.enemy_info）、格挡统计（train.parry_stats）。
// 不挂钩伤害计算，只看血量变化：每帧比较玩家 30 米内角色的 Health，减少即记一次命中 / 受击，归零记击败。
// 结果通过管道推给修改器界面（事件流 + 每秒一次汇总），游戏里不画任何东西。
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "../../host/log.h"
#include "../../ipc/pipe.h"
#include "../../services/attributes.h"
#include "../../services/process_event.h"
#include "../feature.h"

namespace pbz {
namespace {

std::string ShortName(ue::UObject* cls) {
    auto n = ue::ObjName(cls);
    for (const char* p : {"BP_Monster_", "BP_Boss_", "BP_NPC_", "BP_"})
        if (n.starts_with(p)) { n = n.substr(strlen(p)); break; }
    if (n.ends_with("_C")) n.resize(n.size() - 2);
    return n;
}

// 两个功能共用一份观察：谁开着谁驱动
class Watcher {
public:
    bool dps = false, info = false;

    bool Resolve(std::string* why) {
        offHp_ = attr::AttrOffset("Health");
        offHpMax_ = attr::AttrOffset("Health_Max");
        offSh_ = attr::AttrOffset("Shachi");
        offShMax_ = attr::AttrOffset("Shachi_Max");
        auto* actor = ue::FindClass("Actor");
        fnLoc_ = actor ? pe::FindFunction(actor, "K2_GetActorLocation") : nullptr;
        if (offHp_ < 0 || offHpMax_ < 0 || !fnLoc_) { *why = "属性未就绪"; return false; }
        return true;
    }

    void Tick(float dt) {
        clock_ += dt;
        const attr::Target* player = nullptr;
        for (auto& t : attr::Targets())
            if (t.who == attr::Who::Player && attr::Alive(t)) player = &t;
        if (!player) return;
        // 位置要调游戏函数（每个敌人一次），每 0.5 秒才算一次距离；平时只比较缓存范围内敌人的血量
        const bool refresh = (distSince_ += dt) >= 0.5f;
        if (refresh) distSince_ = 0;
        double pl[3] = {};
        if (refresh && !Loc(player->obj, pl)) return;

        // 受击
        const float php = *attr::AttrCurrent(*player, offHp_);
        if (dps && lastPlayerHp_ > 0 && php < lastPlayerHp_ - 0.5f) {
            const double amount = lastPlayerHp_ - php;
            taken_ += amount;
            ipc::Send(std::format(R"({{"op":"event","type":"damage_taken","data":{{"amount":{}}}}})", std::lround(amount)));
        }
        lastPlayerHp_ = php;

        // 敌人：只看 30 米内（远处的血量变化与玩家无关）
        const attr::Target* nearest = nullptr;
        double nearestD = 1e18;
        for (auto& t : attr::Targets()) {
            if (t.who != attr::Who::Enemies || !attr::Alive(t)) continue;
            const float maxHp = *attr::AttrCurrent(t, offHpMax_);
            if (maxHp <= 1) continue;
            double d;
            if (refresh) {
                double p[3];
                if (!Loc(t.obj, p)) continue;
                d = (p[0] - pl[0]) * (p[0] - pl[0]) + (p[1] - pl[1]) * (p[1] - pl[1]) + (p[2] - pl[2]) * (p[2] - pl[2]);
                if (d > 3000.0 * 3000.0) { near_.erase(t.obj); hp_.erase(t.obj); continue; }
                near_[t.obj] = d;
            } else {
                auto n = near_.find(t.obj);
                if (n == near_.end()) continue;
                d = n->second;
            }
            const float hp = *attr::AttrCurrent(t, offHp_);
            auto it = hp_.find(t.obj);
            if (it != hp_.end() && dps && hp < it->second - 0.5f) {
                const double amount = it->second - hp;
                total_ += amount;
                ++hits_;
                hitLog_.push_back({clock_, amount});
                lastTarget_ = t.obj;
                ipc::Send(std::format(R"({{"op":"event","type":"damage_dealt","data":{{"amount":{},"target":"{}"}}}})", std::lround(amount), ShortName(t.cls)));
                if (hp <= 0 && it->second > 0) {
                    ++kills_;
                    ipc::Send(std::format(R"({{"op":"event","type":"enemy_killed","data":{{"target":"{}"}}}})", ShortName(t.cls)));
                }
            }
            hp_[t.obj] = hp;
            if (hp > 0 && d < nearestD) { nearestD = d; nearest = &t; }
        }

        if ((since_ += dt) < 0.25f) return;
        since_ = 0;
        if (dps) {
            while (!hitLog_.empty() && hitLog_.front().first < clock_ - 5) hitLog_.pop_front();
            double recent = 0;
            for (auto& h : hitLog_) recent += h.second;
            ipc::Send(std::format(R"({{"op":"stats","dps":{},"total":{},"hits":{},"kills":{},"taken":{}}})",
                                  std::lround(recent / 5), std::lround(total_), hits_, kills_, std::lround(taken_)));
        }
        if (info) {
            // 优先显示最近打过的敌人，否则最近的敌人
            const attr::Target* show = nearest;
            for (auto& t : attr::Targets())
                if (t.obj == lastTarget_ && attr::Alive(t) && *attr::AttrCurrent(t, offHp_) > 0) show = &t;
            if (!show) { ipc::Send(R"({"op":"enemy_info","none":true})"); return; }
            auto v = [&](int off) { return off >= 0 ? std::lround(*attr::AttrCurrent(*show, off)) : -1L; };
            ipc::Send(std::format(R"({{"op":"enemy_info","name":"{}","hp":{},"hp_max":{},"shachi":{},"shachi_max":{}}})",
                                  ShortName(show->cls), v(offHp_), v(offHpMax_), v(offSh_), v(offShMax_)));
        }
    }

    void Reset() { total_ = taken_ = 0; hits_ = kills_ = 0; hitLog_.clear(); hp_.clear(); near_.clear(); lastPlayerHp_ = -1; lastTarget_ = nullptr; }

private:
    bool Loc(ue::UObject* a, double out[3]) const {
        uint8_t ret[32] = {};
        pe::Call(a, fnLoc_, ret);
        memcpy(out, ret, 24);
        return out[0] == out[0];
    }

    int offHp_ = -1, offHpMax_ = -1, offSh_ = -1, offShMax_ = -1;
    ue::UObject* fnLoc_ = nullptr;
    std::unordered_map<ue::UObject*, float> hp_;
    std::unordered_map<ue::UObject*, double> near_;  // 30 米内的敌人 → 距离平方（每 0.5 秒更新）
    float distSince_ = 1;
    std::deque<std::pair<double, double>> hitLog_;
    ue::UObject* lastTarget_ = nullptr;
    float lastPlayerHp_ = -1, since_ = 0;
    double clock_ = 0, total_ = 0, taken_ = 0;
    int hits_ = 0, kills_ = 0;
};

Watcher& W() {
    static Watcher w;
    return w;
}

class StatsFeature : public IFeature {
public:
    StatsFeature(const char* id, bool Watcher::*flag) : id_(id), flag_(flag) {}
    const char* Id() const override { return id_; }
    bool IsToggle() const override { return true; }
    bool Resolve(std::string* why) override { return W().Resolve(why); }
    void Enable() override {
        if (!W().dps && !W().info) W().Reset();
        W().*flag_ = enabled = true;
    }
    void Disable() override { W().*flag_ = enabled = false; }
    void Tick(float dt) override {
        // 两个开关共用：只由排在前面的已启用者驱动
        for (auto& f : Features())
            if (auto* s = dynamic_cast<StatsFeature*>(f.get()); s && s->enabled) {
                if (s == this) W().Tick(dt);
                return;
            }
    }

private:
    const char* id_;
    bool Watcher::*flag_;
};

// 格挡统计：观察所有已加载能力类的 K2_ActivateAbility（只看不改），玩家身上激活的格挡 / 闪避 / 反制 / 处决类能力按类型计数。
// 实测格挡被动的事件回调只收到"格挡请求"，结果（完美 / 反制）要从激活了哪个能力看出来。
// 研究期：每种第一次出现的能力（含敌人一方）都写日志，便于找出红蓝光反制对应的能力。
class ParryStats : public IFeature {
public:
    const char* Id() const override { return "train.parry_stats"; }
    bool IsToggle() const override { return true; }
    bool Resolve(std::string* why) override {
        abilityCls_ = ue::FindClass("GameplayAbility");
        playerCls_ = ue::FindClass("PBZCharacter_Player");
        if (!abilityCls_ || !playerCls_) { *why = "能力类未加载"; return false; }
        // 来袭帧（区分红 / 蓝光）：与自动格挡相同的数据来源
        passiveCls_ = ue::FindClass("BP_PlayerDeflectPassive_C");
        auto* deflect = ue::FindClass("DeflectComponent");
        durCls_ = ue::FindClass("LGAAction_DurationBulletWithDeflectBase");
        projCls_ = ue::FindClass("GAA_DelayDeflectProjectileBullet_C");
        if (passiveCls_) offComp_ = ue::PropOffset(passiveCls_, "Deflect Component");
        if (deflect) offFrames_ = ue::PropOffset(deflect, "ReceiveFrames");
        if (durCls_) offDur_ = ue::PropOffset(durCls_, "DeflectBulletType");
        if (projCls_) offProj_ = ue::PropOffset(projCls_, "Deflect Bullet Type");
        offShachi_ = attr::AttrOffset("Shachi");
        return true;
    }
    void Enable() override {
        counts_.clear();
        seen_.clear();
        dirty_ = true;
        std::unordered_set<ue::UObject*> fns;
        ue::ForEach([&](int, ue::UObject* o) {
            // 能力的 UClass：本身是类对象（类名以 Class 结尾）、继承自 GameplayAbility
            if (!ue::ClassName(o).ends_with("Class") || ue::IsDefault(o)) return true;
            bool ability = false;
            for (auto* c = o; c && !ability; c = ue::Get<ue::UObject*>(c + ue::Layout::StructSuper)) ability = c == abilityCls_;
            if (ability)
                if (auto* f = pe::FindFunction(o, "K2_ActivateAbility")) fns.insert(f);
            return true;
        });
        for (auto* f : fns) tokens_.push_back(pe::Observe(f, [this](ue::UObject* obj, void*) { OnActivate(obj); }));
        log::Info("格挡统计：观察 {} 个能力激活函数", fns.size());
        enabled = true;
    }
    void Disable() override {
        for (int t : tokens_) pe::Unobserve(t);
        tokens_.clear();
        enabled = false;
    }
    void Tick(float dt) override {
        clock_ += dt;
        // 格挡结果：激活后 0.6 秒内杀气没掉 = 完美格挡（完美格挡不耗杀气），掉了 = 普通格挡
        if (pendingBlock_ && clock_ >= blockAt_ + 0.6) {
            pendingBlock_ = false;
            const float now = PlayerShachi();
            if (blockHit_) { ++counts_[now >= blockShachi_ - 0.5f ? (blockBlue_ ? "蓝光完美格挡" : "完美格挡") : (blockBlue_ ? "蓝光普通格挡" : "普通格挡")]; dirty_ = true; }
        }
        if (pendingBlock_ && !blockHit_) blockHit_ = FrameHitSince(blockAt_);
        if (!dirty_ || (since_ += dt) < 0.5f) return;
        since_ = 0;
        dirty_ = false;
        std::string body;
        for (auto& [k, v] : counts_) body += std::format(R"({}"{}":{})", body.empty() ? "" : ",", k, v);
        ipc::Send(std::format(R"({{"op":"parry","tags":{{{}}}}})", body));
    }

private:
    // 玩家能力类名 → 统计项（空 = 不统计）
    static std::string Label(const std::string& cn) {
        if (cn.find("Counter") != std::string::npos) return "反制";
        if (cn.find("PerfectDodge") != std::string::npos) return cn.find("ForProjectile") != std::string::npos ? "完美闪避（飞行物）" : "完美闪避";
        if (cn.find("HoldDeflect") != std::string::npos) return "长按格挡";
        if (cn.find("Stance") != std::string::npos) return "格挡";
        if (cn.find("AdvancedDash") != std::string::npos) return "闪避";
        if (cn.find("DeflectEscape") != std::string::npos) return "格挡后闪避";
        if (cn.find("Execution") != std::string::npos) return "处决";
        return {};
    }
    void OnActivate(ue::UObject* ability) {
        const auto cn = ue::ClassName(ability);
        auto* owner = ue::Get<ue::UObject*>(ability + ue::Layout::ObjOuter);
        const bool mine = owner && ue::IsA(owner, playerCls_);
        if (seen_.insert(cn).second && seen_.size() < 400) log::Info("格挡统计：{}激活 {}", mine ? "玩家" : "其他 ", cn);
        if (!mine) return;
        auto label = Label(cn);
        if (label.empty()) return;
        const int type = NearestType();  // 2 = 蓝光，3 = 红光
        if (label == "格挡") {  // 结果延后判定；没有来袭攻击时的空举不计
            if (type < 0) return;
            pendingBlock_ = true; blockHit_ = false; blockAt_ = clock_; blockShachi_ = PlayerShachi(); blockBlue_ = type == 2;
            hitSeen_ = HitCount();
            return;
        }
        if (label == "完美闪避" && type == 3) label = "红光反制";
        ++counts_[label];
        dirty_ = true;
    }

    ue::UObject* Passive() {
        if (passive_ && ue::ClassOf(passive_) == passiveCls_) return passive_;
        passive_ = nullptr;
        if (!passiveCls_ || (lookups_++ % 600)) return nullptr;  // 找不到时约 10 秒才重找
        ue::ForEach([&](int, ue::UObject* o) {
            if (ue::ClassOf(o) == passiveCls_ && !ue::IsDefault(o)) { passive_ = o; return false; }
            return true;
        });
        return passive_;
    }
    // 遍历当前来袭帧：fn(帧数据)
    template <class Fn> void EachFrame(Fn&& fn) {
        auto* p = Passive();
        if (!p || offComp_ < 0 || offFrames_ < 0) return;
        auto* comp = ue::Get<uint8_t*>(p + offComp_);
        if (!comp) return;
        auto* data = ue::Get<uint8_t*>(comp + offFrames_);
        const int num = ue::Get<int32_t>(comp + offFrames_ + 8);
        for (int i = 0; data && i < num && i < 64; ++i) {
            uint8_t e[0x60];
            if (ue::Read(data + 0x60 * i, e, sizeof e)) fn(e + 8);
        }
    }
    int NearestType() {
        int type = -1;
        EachFrame([&](const uint8_t* f) {
            if (f[0x39]) return;
            const int idx = *reinterpret_cast<const int32_t*>(f);
            if (idx <= 0 || idx >= ue::Num()) return;
            auto* a = ue::Obj(idx);
            if (!a) return;
            int t = 1;
            if (durCls_ && offDur_ >= 0 && ue::IsA(a, durCls_)) t = ue::Get<uint8_t>(a + offDur_);
            else if (projCls_ && offProj_ >= 0 && ue::IsA(a, projCls_)) t = ue::Get<uint8_t>(a + offProj_);
            if (t == 2 || t == 3) type = t;  // 红 / 蓝光优先
            else if (type < 0) type = t;
        });
        return type;
    }
    int HitCount() {
        int n = 0;
        EachFrame([&](const uint8_t* f) { n += f[0x39] ? 1 : 0; });
        return n;
    }
    bool FrameHitSince(double) { return HitCount() > hitSeen_ || NearestType() < 0; }
    float PlayerShachi() {
        if (offShachi_ < 0) return 0;
        for (auto& t : attr::Targets())
            if (t.who == attr::Who::Player && attr::Alive(t)) return *attr::AttrCurrent(t, offShachi_);
        return 0;
    }

    ue::UObject *abilityCls_ = nullptr, *playerCls_ = nullptr;
    ue::UObject *passiveCls_ = nullptr, *durCls_ = nullptr, *projCls_ = nullptr, *passive_ = nullptr;
    int offComp_ = -1, offFrames_ = -1, offDur_ = -1, offProj_ = -1, offShachi_ = -1, hitSeen_ = 0;
    unsigned lookups_ = 0;
    bool pendingBlock_ = false, blockHit_ = false, blockBlue_ = false;
    double clock_ = 0, blockAt_ = 0;
    float blockShachi_ = 0;
    std::vector<int> tokens_;
    std::map<std::string, int> counts_;
    std::unordered_set<std::string> seen_;
    bool dirty_ = false;
    float since_ = 0;
};

}  // namespace

void RegisterTrainFeatures() {
    Features().push_back(std::make_unique<ParryStats>());
    Features().push_back(std::make_unique<StatsFeature>("train.dps", &Watcher::dps));
    Features().push_back(std::make_unique<StatsFeature>("train.enemy_info", &Watcher::info));
}

}  // namespace pbz
