#pragma once
#include <array>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "../../services/effects.h"
#include "../../services/process_event.h"
#include "../../host/log.h"
#include "quantity_policy.h"

namespace pbz::runtime {
// Reflection metadata layout follows services/ue.h and the current SDK build.
// No UObject or function addresses are stored in configuration or constants.
struct Param { const char* name; const char* type; int size; bool result = false; };
struct Function {
    ue::UObject* function = nullptr;
    std::vector<int> offsets;
    int size = 0;

    bool Resolve(ue::UObject* owner, const char* name, std::initializer_list<Param> expected, std::string* why) {
        Function candidate;
        if (!candidate.ResolveChecked(owner, name, expected, why)) return false;
        *this = std::move(candidate);
        return true;
    }
    bool ResolveChecked(ue::UObject* owner, const char* name, std::initializer_list<Param> expected, std::string* why) {
        function = owner ? pe::FindFunction(owner, name) : nullptr;
        offsets.clear();
        if (!function) { *why = std::string("缺少函数：") + name; return false; }
        size = ue::Get<int>(function + ue::Layout::StructSize, -1);
        if (size < 0 || size > 4096) { *why = std::string("函数参数区无效：") + name; return false; }
        std::vector<ue::UObject*> properties;
        auto* field = ue::Get<ue::UObject*>(function + ue::Layout::StructChildProps);
        for (int n = 0; field && n < 64; ++n) {
            if (ue::Get<uint64_t>(field + 0x38) & 0x80) properties.push_back(field); // CPF_Parm
            field = ue::Get<ue::UObject*>(field + ue::Layout::FieldNext);
        }
        if (field || properties.size() != expected.size()) {
            *why = std::string("参数数量不匹配：") + name; return false;
        }
        for (const auto& spec : expected) {
            ue::UObject* found = nullptr;
            for (auto* p : properties)
                if (ue::Name(ue::Get<uint32_t>(p + ue::Layout::FieldName)) == spec.name) {
                    if (found) { *why = "参数名重复"; return false; }
                    found = p;
                }
            if (!found) { *why = std::string("缺少参数：") + name + "." + spec.name; return false; }
            auto* type = ue::Get<ue::UObject*>(found + ue::Layout::FieldClass);
            const auto typeName = type ? ue::Name(ue::Get<uint32_t>(type + ue::Layout::FieldClassName)) : "";
            const int offset = ue::Get<int>(found + ue::Layout::PropOffset, -1);
            const int bytes = ue::Get<int>(found + 0x34, -1); // FProperty.ElementSize
            const bool result = (ue::Get<uint64_t>(found + 0x38) & 0x400) != 0;
            if (typeName != spec.type || bytes != spec.size || offset < 0 || offset > size - bytes || result != spec.result) {
                *why = std::string("参数类型/范围不匹配：") + name + "." + spec.name; return false;
            }
            for (size_t i = 0; i < offsets.size(); ++i) {
                auto it = expected.begin(); std::advance(it, i);
                if (offset < offsets[i] + it->size && offsets[i] < offset + bytes) {
                    *why = "参数区重叠"; return false;
                }
            }
            offsets.push_back(offset);
        }
        return true;
    }
    template<class T> bool Read(void* buffer, size_t index, T& out) const {
        return buffer && index < offsets.size() && ue::Read(static_cast<uint8_t*>(buffer) + offsets[index], &out, sizeof out);
    }
    // Only ProcessEvent argument buffers are written here; never UObject fields.
    template<class T> bool Write(void* buffer, size_t index, const T& value) const {
        return buffer && index < offsets.size() && ue::Write(static_cast<uint8_t*>(buffer) + offsets[index], &value, sizeof value);
    }
};

struct Identity {
    ue::UObject* object = nullptr;
    ue::UObject* cls = nullptr;
    ue::UObject* outer = nullptr;
    int index = -1;
    explicit Identity(ue::UObject* o = nullptr) : object(o) {
        if (o) {
            cls = ue::ClassOf(o);
            outer = ue::Get<ue::UObject*>(o + ue::Layout::ObjOuter);
            index = ue::Get<int>(o + ue::Layout::ObjIndex, -1);
        }
    }
    bool Alive() const {
        return object && index >= 0 && ue::Obj(index) == object && ue::ClassOf(object) == cls &&
            ue::Get<ue::UObject*>(object + ue::Layout::ObjOuter) == outer && !ue::IsDefault(object);
    }
};

class BoundedEffect : public fx::EffectFeature {
public:
    BoundedEffect(const char* id, std::vector<fx::Effect> effects, std::vector<std::string> required)
        : EffectFeature(id, false, std::move(effects)), required_(std::move(required)) {}
    void SetValue(double v) override {
        // Slider min/max are presentation hints. The IPC layer owns the
        // protocol's 0..1000 range; this effect only requires a finite >0 value.
        accepted_ = std::isfinite(v) && v > 0;
        if (!accepted_) {
            log::Info("{} rejected value {}: multiplier must be finite and > 0; keeping {}", Id(), v, value);
            return;
        }
        value = v;
    }
    void Enable() override {
        // IPC still calls Enable after SetValue. A rejected initial request
        // must not turn on an otherwise disabled feature with its old value.
        if (accepted_) EffectFeature::Enable();
    }
    bool Resolve(std::string* why) override {
        if (!EffectFeature::Resolve(why)) return false;
        for (const auto& name : required_)
            if (attr::AttrOffset(name) < 0) { *why = "缺少必要属性：" + name; return false; }
        return true;
    }
private:
    bool accepted_ = true;
    std::vector<std::string> required_;
};

class Unavailable : public IFeature {
public:
    Unavailable(const char* id, const char* why, bool toggle = false) : id_(id), why_(why), toggle_(toggle) {}
    const char* Id() const override { return id_; }
    bool IsToggle() const override { return toggle_; }
    bool Resolve(std::string* why) override { *why = why_; return false; }
    void Enable() override {}
    void Disable() override { enabled = false; }
    void Tick(float) override {}
private:
    const char *id_, *why_;
    bool toggle_;
};
inline void Missing(const char* id, const char* why, bool toggle = false) {
    Features().push_back(std::make_unique<Unavailable>(id, why, toggle));
}
}  // namespace pbz::runtime
