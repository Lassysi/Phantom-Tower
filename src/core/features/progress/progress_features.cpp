// 养成：升级无消耗（数据表消耗清零），解锁全部武器 / 外观（由修改器界面按物品目录逐件调用物品编辑接口，核心这里只登记为可用）。
// 资源类的这几项由用户 2026-10-29 改由 Claude 负责（倍率类仍归 Codex）。
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <stdexcept>

#include "../../host/log.h"
#include "../../services/inventory.h"
#include "../../services/ue.h"
#include "../../services/process_event.h"
#include "../../services/hooks.h"
#include "upgrade_native.h"
#include "../feature.h"

namespace pbz {
namespace {

class ProgressAction : public IFeature {
public:
    ProgressAction(const char* id, bool core) : id_(id), core_(core) {}
    const char* Id() const override { return id_; }
    bool Resolve(std::string*) override { return true; }
    void Enable() override {}
    void Disable() override {}
    void Tick(float) override {}
    bool Run(std::string* msg, int) override {
        if (!core_) { *msg = "请在修改器里执行"; return false; }
        const auto r = inv::Apply("weapons", "weapon_max", 0);  // {"op":"item","ok":…,"msg":"…"}
        const auto m = r.find("\"msg\":\"");
        *msg = m == std::string::npos ? r : r.substr(m + 7, r.size() - m - 9);
        return r.find("\"ok\":true") != std::string::npos;
    }

private:
    const char* id_;
    bool core_;
};

#include "upgrade_free.h"

}  // namespace

void RegisterProgressFeatures() {
    auto& F = Features();
    F.push_back(std::make_unique<UpgradeFree>());
    F.push_back(std::make_unique<ProgressAction>("res.unlock_weapons", false));
    F.push_back(std::make_unique<ProgressAction>("res.unlock_cosmetics", false));
}

}  // namespace pbz
