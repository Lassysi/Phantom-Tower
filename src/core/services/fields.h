// L1：实例字段覆盖。把某类（含子类）所有活对象上的反射字段写成固定值，关闭时写回各自原值。
// 用于"每个能力实例自带的数值"（影之武装消耗、冷却 GE 等）：对象会随换武器 / 读档新建，
// 所以开启期间增量扫描对象表（每帧一段，不卡顿），新出现的对象也会被覆盖。
#pragma once
#include <cmath>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../features/feature.h"
#include "ue.h"

namespace pbz::fx {

class FieldOverride : public IFeature {
public:
    enum class Type { Double, Float, Int, Ptr };
    struct Spec { std::string name; Type type; double value; };
    using Filter = std::function<bool(ue::UObject*)>;

    FieldOverride(const char* id, std::string cls, std::vector<Spec> specs, Filter filter = nullptr)
        : id_(id), cls_(std::move(cls)), specs_(std::move(specs)), filter_(std::move(filter)) {}
    const char* Id() const override { return id_; }
    bool IsToggle() const override { return true; }

    bool Resolve(std::string* why) override {
        clsObj_ = ue::FindClass(cls_);
        if (!clsObj_) { *why = cls_ + " 未加载"; return false; }
        offs_.clear();
        for (auto& s : specs_) {
            offs_.push_back(ue::PropOffset(clsObj_, s.name));
            if (offs_.back() < 0) { *why = cls_ + " 缺字段 " + s.name; return false; }
        }
        return true;
    }

    void Enable() override {
        enabled = true;
        cursor_ = 0;
        // 先完整扫一遍，开启立即生效；之后靠 Tick 增量扫描补新对象
        ue::ForEach([&](int, ue::UObject* o) { Consider(o); return true; });
    }

    void Disable() override {
        for (auto& [o, orig] : objs_)
            if (Valid(o))
                for (size_t i = 0; i < specs_.size(); ++i) Write(o, i, orig[i]);
        objs_.clear();
        enabled = false;
    }

    void Tick(float dt) override {
        if (!enabled) return;
        ue::UObject* batch[kChunk];
        const int n = ue::ObjBatch(cursor_, kChunk, batch);
        for (int i = 0; i < n; ++i) Consider(batch[i]);
        cursor_ = cursor_ + kChunk >= ue::Num() ? 0 : cursor_ + kChunk;
        // 游戏可能把字段改回（能力重新初始化）：每秒复写一次，失效对象移除
        if ((since_ += dt) < 1.0f) return;
        since_ = 0;
        for (auto it = objs_.begin(); it != objs_.end();) {
            if (!Valid(it->first)) { it = objs_.erase(it); continue; }
            for (size_t i = 0; i < specs_.size(); ++i)
                if (Read(it->first, i) != specs_[i].value) Write(it->first, i, specs_[i].value);
            ++it;
        }
    }

private:
    static constexpr int kChunk = 2048;

    bool Valid(ue::UObject* o) const {
        return o && !(ue::Get<uint32_t>(o + 0x08) & 0x30) && ue::IsA(o, clsObj_) && (!filter_ || filter_(o));
    }

    void Consider(ue::UObject* o) {
        if (!o || objs_.count(o) || !Valid(o)) return;
        std::vector<double> orig;
        for (size_t i = 0; i < specs_.size(); ++i) orig.push_back(Read(o, i));
        objs_.emplace(o, std::move(orig));
        for (size_t i = 0; i < specs_.size(); ++i) Write(o, i, specs_[i].value);
    }

    double Read(ue::UObject* o, size_t i) const {
        auto* p = o + offs_[i];
        switch (specs_[i].type) {
            case Type::Double: return ue::Get<double>(p);
            case Type::Float: return ue::Get<float>(p);
            case Type::Int: return ue::Get<int32_t>(p);
            case Type::Ptr: return static_cast<double>(ue::Get<uint64_t>(p));
        }
        return 0;
    }

    void Write(ue::UObject* o, size_t i, double v) {
        auto* p = o + offs_[i];
        switch (specs_[i].type) {
            case Type::Double: ue::Write(p, &v, 8); break;
            case Type::Float: { const float f = static_cast<float>(v); ue::Write(p, &f, 4); break; }
            case Type::Int: { const int32_t n = static_cast<int32_t>(std::lround(v)); ue::Write(p, &n, 4); break; }
            case Type::Ptr: { const uint64_t q = static_cast<uint64_t>(v); ue::Write(p, &q, 8); break; }
        }
    }

    const char* id_;
    std::string cls_;
    std::vector<Spec> specs_;
    Filter filter_;
    ue::UObject* clsObj_ = nullptr;
    std::vector<int> offs_;
    std::unordered_map<ue::UObject*, std::vector<double>> objs_;  // 对象 → 各字段原值
    int cursor_ = 0;
    float since_ = 0;
};

}  // namespace pbz::fx
