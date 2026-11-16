// 格挡 / 闪避类功能（研究记录见 notes/deflect-research.md）。
// 闪避必定完美：每次闪避游戏都会问各武器的完美闪避能力 K2_CanActivateAbility，回答"是"就当场触发完美闪避；
// 这里把这个询问的返回值强制为 true。证据：记录器对照（2026-10-28 22:37/22:38 完美闪避 vs 22:26 普通闪避）。
#include <memory>
#include <vector>

#include "../../services/effects.h"
#include "../../services/process_event.h"
#include "../feature.h"

namespace pbz {
namespace {

class ForceReturnFeature : public IFeature {
public:
    ForceReturnFeature(const char* id, std::vector<std::string> classSubs, std::string func, bool value)
        : id_(id), subs_(std::move(classSubs)), func_(std::move(func)), mode_(value ? 2 : 3) {}
    const char* Id() const override { return id_; }
    bool IsToggle() const override { return true; }
    bool Resolve(std::string* why) override {
        if (!pe::Ready()) { *why = "ProcessEvent 不可用"; return false; }
        return true;
    }
    // 开关时才去找类：武器能力在装备后才加载
    void Enable() override { for (auto& s : subs_) pe::Watch(s, func_, mode_); enabled = true; }
    void Disable() override { for (auto& s : subs_) pe::Watch(s, func_, 0); enabled = false; }
    void Tick(float) override {}

private:
    const char* id_;
    std::vector<std::string> subs_;
    std::string func_;
    int mode_;
};

}  // namespace

void RegisterDeflectFeatures() {
    using fx::ConfigFeature;
    using fx::Field;
    auto& F = Features();
    // 完美闪避 / 完美格挡 已移到 auto_response.cpp（同一套来袭帧引擎）

    // 难度配置 PDA_GlobalCombatConfig_C（每个难度一份，全部同步改）。实机读值：Hard 完美格挡 8 帧（逻辑帧率 30），连点惩罚 0.4s
    const std::string combat = "PDA_GlobalCombatConfig_C";
    F.push_back(std::make_unique<ConfigFeature>("player.deflect_window_mul", false, combat, std::vector<Field>{
        {"Deflect Perfect Window Frames", Field::Int, false, 0, 1}, {"Deflect Perfect Window Frames On Break", Field::Int, false, 0, 1}}));
    F.push_back(std::make_unique<ConfigFeature>("combat.break_duration_mul", false, combat, std::vector<Field>{
        {"Mob Break Duration", Field::Double}, {"Mob SimpleExe Break Duration", Field::Double}}));
    F.push_back(std::make_unique<ConfigFeature>("combat.break_damage_mul", false, combat, std::vector<Field>{{"Mob Break Damage Multiplier", Field::Double}}));
    F.push_back(std::make_unique<ConfigFeature>("player.break_duration_mul", false, combat, std::vector<Field>{{"Player Break Duration", Field::Double}}));
    // 玩家配置 PDA_ActorConfig_C：影之武装切换冷却（实机 2.0s）
    F.push_back(std::make_unique<ConfigFeature>("combat.shachi_weapon_cd_mul", false, "PDA_ActorConfig_C", std::vector<Field>{
        {"Switch Shachi Weapon Cooldown", Field::Double}}));
}

}  // namespace pbz
