// 能力实例上的数值：影之武装无限（消耗清零）。只作用于玩家自己的能力。
#include <cstring>
#include <format>
#include <unordered_set>

#include "../../services/attributes.h"
#include "../../services/fields.h"
#include "../../services/process_event.h"

namespace pbz {
namespace {

using fx::FieldOverride;
using T = FieldOverride::Type;

// 能力实例的 Outer 是拥有它的角色：只改玩家的，不动敌人
bool OwnedByPlayer(ue::UObject* ability) {
    static ue::UObject* playerCls = ue::FindClass("PBZCharacter_Player");
    auto* outer = ue::Get<ue::UObject*>(ability + ue::Layout::ObjOuter);
    return playerCls && outer && ue::IsA(outer, playerCls);
}

// 敌人不攻击：停掉怪物（BIsMonster）的 AIController 上 SGameAIComponent 的 AI（StopAI），关闭时对停过的重新 StartAI。
// 开启期间增量扫描对象表，新刷出来的怪也会被停
class EnemyPassive : public IFeature {
public:
    const char* Id() const override { return "combat.enemy_passive"; }
    bool IsToggle() const override { return true; }
    bool Resolve(std::string* why) override {
        aiCls_ = ue::FindClass("SGameAIComponent");
        npcCls_ = ue::FindClass("PBZCharacter_NPC");
        if (!aiCls_ || !npcCls_) { *why = "AI 类未加载"; return false; }
        fnStop_ = pe::FindFunction(aiCls_, "StopAI");
        fnStart_ = pe::FindFunction(aiCls_, "StartAI");
        offMonster_ = ue::PropOffset(npcCls_, "BIsMonster");
        auto* ctrlCls = ue::FindClass("Controller");
        offPawn_ = ctrlCls ? ue::PropOffset(ctrlCls, "Pawn") : -1;
        if (!fnStop_ || !fnStart_ || offMonster_ < 0 || offPawn_ < 0) { *why = "AI 接口缺失"; return false; }
        return true;
    }
    void Enable() override {
        enabled = true;
        cursor_ = 0;
        ue::ForEach([&](int, ue::UObject* o) { Consider(o); return true; });
    }
    void Disable() override {
        for (auto* c : stopped_)
            if (Valid(c)) pe::Call(c, fnStart_, nullptr);
        stopped_.clear();
        enabled = false;
    }
    void Tick(float) override {
        if (!enabled) return;
        ue::UObject* batch[2048];
        const int n = ue::ObjBatch(cursor_, 2048, batch);
        for (int i = 0; i < n; ++i) Consider(batch[i]);
        cursor_ = cursor_ + 2048 >= ue::Num() ? 0 : cursor_ + 2048;
    }

private:
    bool Valid(ue::UObject* c) const {
        if (!c || (ue::Get<uint32_t>(c + 0x08) & 0x30) || !ue::IsA(c, aiCls_)) return false;
        // AI 组件挂在 AIController 上：经 Controller.Pawn 找到怪物
        auto* ctrl = ue::Get<ue::UObject*>(c + ue::Layout::ObjOuter);
        auto* pawn = ctrl ? ue::Get<ue::UObject*>(ctrl + offPawn_) : nullptr;
        return pawn && ue::IsA(pawn, npcCls_) && ue::Get<uint8_t>(pawn + offMonster_);
    }
    void Consider(ue::UObject* c) {
        if (!c || stopped_.count(c) || !Valid(c)) return;
        pe::Call(c, fnStop_, nullptr);
        stopped_.insert(c);
    }

    ue::UObject *aiCls_ = nullptr, *npcCls_ = nullptr, *fnStop_ = nullptr, *fnStart_ = nullptr;
    int offMonster_ = -1, offPawn_ = -1, cursor_ = 0;
    std::unordered_set<ue::UObject*> stopped_;
};

}  // namespace

void RegisterAbilityFeatures() {
    auto& F = Features();
    // 影之武装：每个能力自带"杀气 / 弹药"的初始与持续消耗，全部清零
    F.push_back(std::make_unique<FieldOverride>("combat.phantom_inf", "BP_ShachiAbility_Base_C",
        std::vector<FieldOverride::Spec>{{"Shachi Initial Cost", T::Double, 0}, {"Shachi Constant Cost", T::Double, 0},
                                         {"Bullet Instant Cost", T::Double, 0}, {"Bullet Constant Cost", T::Double, 0}},
        OwnedByPlayer));
    // 一键清场已删除：Kill / 血量清零 / KillMonsters 实测都不触发死亡（死亡由 GAS 伤害流程判定）
    F.push_back(std::make_unique<EnemyPassive>());
    // 注：实测玩家 143 个能力都没有 CooldownGameplayEffectClass（本作无 GAS 技能冷却），不做"技能无冷却"
}

}  // namespace pbz
