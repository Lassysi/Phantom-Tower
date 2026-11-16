// L2：功能接口。所有功能都在游戏线程里 Enable/Disable/Tick；Disable 必须把游戏对象恢复原状。
#pragma once
#include <memory>
#include <string>
#include <vector>

namespace pbz {

class IFeature {
public:
    virtual ~IFeature() = default;
    virtual const char* Id() const = 0;
    virtual bool Resolve(std::string* why) = 0;  // 找类/属性；失败则该功能标为不可用
    virtual void Enable() = 0;
    virtual void Disable() = 0;
    virtual void Tick(float dt) = 0;
    virtual void SetValue(double v) { value = v; }
    virtual bool IsToggle() const { return false; }
    virtual bool DefaultOn() const { return false; }  // 核心就绪后自动开启（如地图传送：只在大地图按键时起作用），用户仍可关
    // 动作类功能（一次性执行，如一键清场）：游戏线程调用，返回是否成功，msg 给界面显示
    // slot：界面上的槽位输入（坐标存档等），没有时为 0
    virtual bool Run(std::string* msg, int slot) { (void)slot; *msg = "不是动作"; return false; }

    bool available = false;
    bool enabled = false;
    double value = 1.0;
    std::string reason;
};

std::vector<std::unique_ptr<IFeature>>& Features();
IFeature* FindFeature(const std::string& id);
void AddFeature(std::unique_ptr<IFeature> f);  // 游戏线程下一帧加入功能表（模块用；核心自带功能在启动时直接登记）
// 持有者（游戏线程）：有任何持有者就保持开启，最后一个放手才关闭并还原。"user" = 修改器界面
bool HoldFeature(IFeature* f, const std::string& holder, double value = 0);  // value>0 时设倍率（仅倍率类）；功能不可用返回 false
void ReleaseFeature(IFeature* f, const std::string& holder);
std::vector<std::string> FeatureHolders(IFeature* f);
bool IsModuleFeature(IFeature* f);  // 模块登记的功能：不进修改器的功能列表，管道 set / action / 全部还原都不碰
void RegisterAttributeFeatures();   // Claude：player / combat / time
void RegisterDeflectFeatures();     // Claude：格挡 / 闪避（ProcessEvent 拦截）
void RegisterAbilityFeatures();     // Claude：能力实例数值（影之武装消耗）、一键清场
void RegisterHealFeatures();        // Claude：回复道具
void RegisterProgressFeatures();    // Claude：武器满级、解锁全部武器 / 外观
std::string BellCommand(const std::string& cmd, const std::string& id, const double loc[3], double yaw);  // 传送点（管道 op "bell"）
// 传送（影塔等模块用，游戏线程调用）：点传送走地图传送的悬停等加载流程；钟所在关卡名；玩家位置 + 朝向 + 所在关卡
bool PointTeleport(double x, double y, double z, double yaw, std::string* msg);
bool TeleportBusy();                                   // 点传送还在悬停等加载
std::string BellLevel(const std::string& id);          // 钟所在关卡（世界）名，查不到返回空
bool PlayerWhere(double out[4], std::string* level);   // x y z yaw；level = 玩家所在世界名
std::string MapProbe();  // 研究用：地图控件状态（管道 op "mapprobe"）
std::string PositionCommand(const std::string& cmd, int id, const std::string& name);  // 坐标列表（管道 op "pos"，游戏线程调用）
void RegisterMoveFeatures();        // Claude：移动（坐标存读档、传送、跳跃、飞行；用户 2026-10-29 改由 Claude 负责）
void RegisterFactionBarFeatures();  // Claude：生成怪的血条按阵营上色（默认开）
void RegisterTrainFeatures();       // Claude：训练场（伤害统计、敌人数值）
void RegisterAutoResponseFeatures(); // Claude：蓝光/红光/普攻自动应对
// Codex：在 features/res|world|cam 里各写一个同名函数即可接入；没写时链接到 registry.cpp 里的空实现
void RegisterResFeatures();
void RegisterWorldFeatures();
void RegisterCamFeatures();

}  // namespace pbz
