#include "feature.h"

#include <map>
#include <set>

#include "../host/log.h"
#include "../services/gamethread.h"

namespace pbz {

std::vector<std::unique_ptr<IFeature>>& Features() {
    static std::vector<std::unique_ptr<IFeature>> v;
    return v;
}

IFeature* FindFeature(const std::string& id) {
    for (auto& f : Features())
        if (id == f->Id()) return f.get();
    return nullptr;
}

// 核心与模块通用：放到游戏线程下一帧加入功能表。功能表只在游戏线程增长，管道线程不直接读它（见 ipc set/action）；
// 加入后由 RefreshUnavailableFeatures 每秒尝试 Resolve，和核心自带的功能一样。
namespace {
std::set<IFeature*>& ModuleFeatures() {
    static std::set<IFeature*> s;
    return s;
}
}  // namespace

// 模块（影塔等）登记的功能：修改器看不到、不能开关、"全部还原"不碰——Mod 和修改器互不影响（用户 2026-10-30）
bool IsModuleFeature(IFeature* f) { return ModuleFeatures().count(f) > 0; }

void AddFeature(std::unique_ptr<IFeature> f) {
    IFeature* p = f.release();
    gamethread::Post([p] {
        if (FindFeature(p->Id())) { log::Info("功能 {} 重名，未加入", p->Id()); delete p; return; }
        p->available = false;
        p->reason = "等待资源加载";
        Features().emplace_back(p);
        ModuleFeatures().insert(p);  // AddFeature 只给模块用：核心自带的功能启动时直接登记
    });
}

// ---- 持有者：同一个功能可以被多方开启（"user" = 修改器界面；模块用自己的名字，如 "tower"）。
// 有任何持有者就保持开启；最后一个持有者放手才关闭并还原。全部在游戏线程调用。
namespace {
std::map<IFeature*, std::set<std::string>>& Holders() {
    static std::map<IFeature*, std::set<std::string>> m;
    return m;
}
}  // namespace

bool HoldFeature(IFeature* f, const std::string& holder, double value) {
    if (!f || !f->available) return false;
    if (value > 0 && !f->IsToggle()) f->SetValue(value);
    if (!f->enabled) f->Enable();
    Holders()[f].insert(holder);
    return true;
}

void ReleaseFeature(IFeature* f, const std::string& holder) {
    if (!f) return;
    auto it = Holders().find(f);
    if (it != Holders().end()) {
        it->second.erase(holder);
        if (!it->second.empty()) return;  // 还有别人在用
        Holders().erase(it);
    }
    if (f->enabled) f->Disable();
    // 最后一个持有者放手：数值类一律回默认 1（动作类从不进入 enabled，旧写法会把值卡在上次设的数）
    if (!f->IsToggle()) f->SetValue(1.0);
}

std::vector<std::string> FeatureHolders(IFeature* f) {
    auto it = Holders().find(f);
    return it == Holders().end() ? std::vector<std::string>{} : std::vector<std::string>(it->second.begin(), it->second.end());
}

// 默认空实现：Codex 的目录里定义了真正的 RegisterXxxFeatures 时，链接器优先用那个
void RegisterResFeaturesDefault() {}
void RegisterWorldFeaturesDefault() {}
void RegisterCamFeaturesDefault() {}

}  // namespace pbz

#pragma comment(linker, "/alternatename:?RegisterResFeatures@pbz@@YAXXZ=?RegisterResFeaturesDefault@pbz@@YAXXZ")
#pragma comment(linker, "/alternatename:?RegisterWorldFeatures@pbz@@YAXXZ=?RegisterWorldFeaturesDefault@pbz@@YAXXZ")
#pragma comment(linker, "/alternatename:?RegisterCamFeatures@pbz@@YAXXZ=?RegisterCamFeaturesDefault@pbz@@YAXXZ")
