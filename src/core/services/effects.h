// L1：表驱动的通用功能。功能只描述"对谁、哪个字段、怎么乘"，实际写入交给 attr 仲裁。
// Codex 的 res/world 功能也用这里的类注册，不必另写写内存逻辑。
#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "../features/feature.h"
#include "attributes.h"

namespace pbz::fx {

using attr::Kind;
using attr::Who;

enum class Src { Attr, Actor, World };

struct Effect {
    Who who;
    Src src;
    std::string name;
    Kind kind;
    float fixed = NAN;    // 开关类用固定倍率；NAN 表示用滑条值
    bool invert = false;  // 用 1/值（如"敌人架势倍率"越大越难破防）
    bool base = false;    // 同时写 BaseValue（游戏按 Base 判定的属性，如 Health_Min）
    int off = -1;
};

inline Effect A(Who w, std::string n, Kind k, float fixed = NAN, bool inv = false) { return {w, Src::Attr, std::move(n), k, fixed, inv}; }
inline Effect AB(Who w, std::string n, Kind k, float fixed = NAN) { return {w, Src::Attr, std::move(n), k, fixed, false, true}; }
inline Effect ActorF(Who w, std::string n, Kind k, float fixed = NAN, bool inv = false) { return {w, Src::Actor, std::move(n), k, fixed, inv}; }
inline Effect WorldF(std::string n, Kind k, float fixed = NAN) { return {Who::World, Src::World, std::move(n), k, fixed}; }

// 同一组属性名展开（如各伤害类型）
inline std::vector<Effect> Each(Who w, const std::string& prefix, const std::vector<std::string>& suffixes, Kind k, float fixed = NAN, bool inv = false) {
    std::vector<Effect> v;
    for (auto& s : suffixes) v.push_back(A(w, prefix + s, k, fixed, inv));
    return v;
}

class EffectFeature : public IFeature {
public:
    EffectFeature(const char* id, bool toggle, std::vector<Effect> fx) : id_(id), toggle_(toggle), fx_(std::move(fx)) {}
    const char* Id() const override { return id_; }
    bool IsToggle() const override { return toggle_; }
    void Enable() override { enabled = true; }
    void Disable() override { enabled = false; }  // 停止声明，attr::Commit 自动还原

    bool Resolve(std::string* why) override {
        if (!attr::Resolve(why)) return false;
        int ok = 0;
        for (auto& e : fx_) {
            e.off = e.src == Src::Attr ? attr::AttrOffset(e.name) : e.src == Src::Actor ? attr::ActorOffset(e.name) : attr::WorldOffset(e.name);
            ok += e.off >= 0;
        }
        if (!ok) { *why = "字段均不存在：" + fx_.front().name; return false; }
        return true;
    }

    void Tick(float) override {
        if (!enabled) return;
        for (auto& t : attr::Targets()) {
            if (!attr::Alive(t)) continue;
            for (auto& e : fx_) {
                if (e.off < 0 || e.who != t.who) continue;
                float m = std::isnan(e.fixed) ? static_cast<float>(value) : e.fixed;
                if (e.invert) m = m > 0.0001f ? 1.0f / m : 10000.0f;
                float* field = e.src == Src::Attr ? attr::AttrCurrent(t, e.off) : reinterpret_cast<float*>(t.obj + e.off);
                attr::Want(t, field, e.kind, m);
                if (e.base && e.src == Src::Attr) attr::Want(t, attr::AttrBase(t, e.off), e.kind, m);
            }
        }
    }

private:
    const char* id_;
    bool toggle_;
    std::vector<Effect> fx_;
};

// 把玩家属性锁在上限（无限血、无限杀气、无限绝技）。
// 伤害/消耗改的是 BaseValue，所以 Base 和 Current 一起锁；关闭后保持当前值，不回退。
// floor：可选的"下限属性"（如 Health_Min），一并抬到上限，让游戏自己的钳制阻止扣血；关闭时下限会还原。
class LockFeature : public IFeature {
public:
    struct Item { std::string attr, max, floor; int o = -1, m = -1, f = -1; };
    LockFeature(const char* id, std::vector<Item> items) : id_(id), items_(std::move(items)) {}
    const char* Id() const override { return id_; }
    bool IsToggle() const override { return true; }
    void Enable() override { enabled = true; }
    void Disable() override { enabled = false; }

    bool Resolve(std::string* why) override {
        if (!attr::Resolve(why)) return false;
        int ok = 0;
        for (auto& it : items_) {
            it.o = attr::AttrOffset(it.attr);
            it.m = attr::AttrOffset(it.max);
            it.f = it.floor.empty() ? -1 : attr::AttrOffset(it.floor);
            ok += it.o >= 0 && it.m >= 0;
        }
        if (!ok) { *why = items_.front().attr + " 未找到"; return false; }
        return true;
    }

    void Tick(float) override {
        if (!enabled) return;
        for (auto& t : attr::Targets())
            if (t.who == Who::Player && attr::Alive(t))
                for (auto& it : items_) {
                    if (it.o < 0 || it.m < 0) continue;
                    const float mx = ue::Get<float>(attr::AttrCurrent(t, it.m));
                    if (!(mx > 0)) continue;
                    attr::Hold(t, attr::AttrBase(t, it.o), mx);
                    attr::Hold(t, attr::AttrCurrent(t, it.o), mx);
                    if (it.f >= 0) {
                        attr::Want(t, attr::AttrBase(t, it.f), Kind::Set, mx);
                        attr::Want(t, attr::AttrCurrent(t, it.f), Kind::Set, mx);
                    }
                }
    }

private:
    const char* id_;
    std::vector<Item> items_;
};

// 玩家整数字段不低于开启时的值（如剩余天数）。只阻止减少，关闭后不回退。
class FloorFeature : public IFeature {
public:
    FloorFeature(const char* id, std::string prop) : id_(id), prop_(std::move(prop)) {}
    const char* Id() const override { return id_; }
    bool IsToggle() const override { return true; }
    bool Resolve(std::string* why) override { return attr::Resolve(why); }  // 偏移在玩家出现后再查
    void Enable() override { enabled = true; floor_ = INT32_MIN; }
    void Disable() override { enabled = false; }

    void Tick(float) override {
        if (!enabled) return;
        const int off = attr::PlayerOffset(prop_);
        if (off < 0) return;
        for (auto& t : attr::Targets())
            if (t.who == Who::Player && attr::Alive(t)) {
                auto* p = reinterpret_cast<int32_t*>(t.obj + off);
                const int32_t v = ue::Get<int32_t>(p);
                if (floor_ == INT32_MIN) floor_ = v;
                if (v < floor_) *p = floor_;
            }
    }

private:
    const char* id_;
    std::string prop_;
    int32_t floor_ = INT32_MIN;
};

// 配置资产字段（如难度配置 PDA_GlobalCombatConfig_C 里的完美格挡帧数、破防时长）。
// 开启时对该类所有实例（各难度各一份）记原值并写入；滑条改值时按原值重算；关闭写回原值。
struct Field {
    std::string name;
    enum Type { Double, Int } type;
    bool set = false;        // true：直接设为 fixed；false：原值 × 倍率
    double fixed = 0;
    double minValue = 0;     // 结果下限（如帧数至少 1）
};

class ConfigFeature : public IFeature {
public:
    ConfigFeature(const char* id, bool toggle, std::string cls, std::vector<Field> fields)
        : id_(id), toggle_(toggle), cls_(std::move(cls)), fields_(std::move(fields)) {}
    const char* Id() const override { return id_; }
    bool IsToggle() const override { return toggle_; }

    bool Resolve(std::string* why) override {
        cls_obj_ = ue::FindClass(cls_);
        if (!cls_obj_) { *why = cls_ + " 未加载"; return false; }
        offs_.clear();
        for (auto& f : fields_) offs_.push_back(ue::PropOffset(cls_obj_, f.name));
        for (int o : offs_)
            if (o < 0) { *why = cls_ + " 缺字段"; return false; }
        return true;
    }

    void Enable() override {
        recs_.clear();
        ue::ForEach([&](int, ue::UObject* o) {
            if (ue::ClassOf(o) != cls_obj_ || (ue::Get<uint32_t>(o + 0x08) & 0x30)) return true;  // 跳过 CDO/原型
            for (size_t i = 0; i < fields_.size(); ++i) {
                auto* p = o + offs_[i];
                recs_.push_back({p, i, fields_[i].type == Field::Double ? ue::Get<double>(p) : ue::Get<int32_t>(p)});
            }
            return true;
        });
        enabled = true;
        Apply();
    }

    void SetValue(double v) override {
        value = v;
        if (enabled) Apply();
    }

    void Disable() override {
        for (auto& r : recs_) Write(r, r.orig);
        recs_.clear();
        enabled = false;
    }

    // 每秒复查一次：游戏可能重置配置资产（实测开启后一度被还原），被改回就重写
    void Tick(float dt) override {
        if (!enabled || (since_ += dt) < 1.0f) return;
        since_ = 0;
        for (auto& r : recs_) {
            const auto& f = fields_[r.field];
            const double want = std::max(f.minValue, f.set ? f.fixed : r.orig * value);
            const double cur = f.type == Field::Double ? ue::Get<double>(r.p) : ue::Get<int32_t>(r.p);
            if (std::abs(cur - (f.type == Field::Int ? std::lround(want) : want)) > 1e-6) Write(r, want);
        }
    }

private:
    struct Rec { uint8_t* p; size_t field; double orig; };
    float since_ = 0;

    void Write(const Rec& r, double v) {
        if (fields_[r.field].type == Field::Double) ue::Write(r.p, &v, sizeof v);
        else { int32_t i = static_cast<int32_t>(std::lround(v)); ue::Write(r.p, &i, sizeof i); }
    }

    void Apply() {
        for (auto& r : recs_) {
            const auto& f = fields_[r.field];
            Write(r, std::max(f.minValue, f.set ? f.fixed : r.orig * value));
        }
    }

    const char* id_;
    bool toggle_;
    std::string cls_;
    std::vector<Field> fields_;
    std::vector<int> offs_;
    ue::UObject* cls_obj_ = nullptr;
    std::vector<Rec> recs_;
};

}  // namespace pbz::fx
