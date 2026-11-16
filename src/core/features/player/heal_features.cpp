// 无限回复道具：每半秒把背包里每种药的剩余次数补到游戏自己算的上限（GetMedicineTotalMaxUsage，含额外次数）。
// 关闭后不再补，已有次数保留（与"无限生命"一致）。
#include <algorithm>
#include <cstring>
#include <vector>

#include "../../host/log.h"
#include "../../services/process_event.h"
#include "../feature.h"

namespace pbz {
namespace {

class InfHeal : public IFeature {
public:
    const char* Id() const override { return "player.inf_heal"; }
    bool IsToggle() const override { return true; }

    bool Resolve(std::string* why) override {
        bagCls_ = ue::FindClass("PBZBackPackSubsystem");
        infoCls_ = ue::FindClass("PBZMedicineInfo");
        if (!bagCls_ || !infoCls_) { *why = "背包类未加载"; return false; }
        offMap_ = ue::PropOffset(bagCls_, "MedicineInfos");
        offCur_ = ue::PropOffset(infoCls_, "CurrentTime");
        offDisp_ = ue::PropOffset(infoCls_, "DisplayTime");
        fnTotal_ = pe::FindFunction(bagCls_, "GetMedicineTotalMaxUsage");
        if (offMap_ < 0 || offCur_ < 0 || offDisp_ < 0 || !fnTotal_) { *why = "药品接口缺失"; return false; }
        for (auto* p = ue::Get<uint8_t*>(fnTotal_ + ue::Layout::StructChildProps); p; p = ue::Get<uint8_t*>(p + ue::Layout::FieldNext)) {
            const auto n = ue::Name(ue::Get<uint32_t>(p + ue::Layout::FieldName));
            const int off = ue::Get<int32_t>(p + ue::Layout::PropOffset);
            if (n == "MedicineID") argId_ = off;
            else if (n == "ReturnValue") argRet_ = off;
        }
        if (argId_ < 0 || argRet_ < 0) { *why = "药品接口参数不符"; return false; }
        return true;
    }

    void Enable() override { enabled = true; since_ = 1; }
    void Disable() override { enabled = false; }

    void Tick(float dt) override {
        if (!enabled || (since_ += dt) < 0.5f) return;
        since_ = 0;
        if (!bag_ || ue::ClassOf(bag_) != bagCls_) {
            bag_ = nullptr;
            if ((search_ -= 1) > 0) return;  // 找不到时每 5 秒才全表重找一次
            search_ = 20;
            ue::ForEach([&](int, ue::UObject* o) {
                if (ue::ClassOf(o) == bagCls_ && !ue::IsDefault(o)) { bag_ = o; return false; }
                return true;
            });
            if (!bag_) return;
        }
        // TMap<FName, PBZMedicineInfo*>：元素 = 键 8 + 值 8 + 哈希链 8
        auto* data = ue::Get<uint8_t*>(bag_ + offMap_);
        const int num = ue::Get<int32_t>(bag_ + offMap_ + 8);
        if (!data || num <= 0 || num > 256) return;
        std::vector<uint8_t> args(std::max(argId_, argRet_) + 16);
        for (int i = 0; i < num; ++i) {
            const auto key = ue::Get<uint64_t>(data + 0x18 * i);
            auto* info = ue::Get<ue::UObject*>(data + 0x18 * i + 8);
            if (!info || !ue::IsA(info, infoCls_)) continue;
            std::fill(args.begin(), args.end(), 0);
            memcpy(args.data() + argId_, &key, 8);
            pe::Call(bag_, fnTotal_, args.data());
            const int32_t total = *reinterpret_cast<int32_t*>(args.data() + argRet_);
            if (total <= 0 || total > 99) continue;
            if (ue::Get<int32_t>(info + offCur_) < total) {
                ue::Write(info + offCur_, &total, 4);
                ue::Write(info + offDisp_, &total, 4);
            }
        }
    }

private:
    ue::UObject *bagCls_ = nullptr, *infoCls_ = nullptr, *fnTotal_ = nullptr, *bag_ = nullptr;
    int offMap_ = -1, offCur_ = -1, offDisp_ = -1, argId_ = -1, argRet_ = -1;
    float since_ = 0;
    int search_ = 0;
};

}  // namespace

void RegisterHealFeatures() { Features().push_back(std::make_unique<InfHeal>()); }

}  // namespace pbz
