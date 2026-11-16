// Isolated UE/PE fixture used by native_loot_test.cxx. It owns no game
// process and includes no historical transaction implementation or test main.
#pragma once
#include <functional>
#include "../runtime_contract.h"
#include "../../../host/log.h"
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace fake {
using namespace pbz;
std::vector<std::unique_ptr<uint8_t[]>> memory;
std::vector<ue::UObject*> objects;
std::vector<std::string> names{"None"};
std::map<std::string, ue::UObject*> classes;
std::map<std::pair<ue::UObject*, std::string>, ue::UObject*> functions;
std::map<std::pair<ue::UObject*, std::string>, int> properties;
std::unordered_map<ue::UObject*, pe::Interceptor> interceptors;
std::unordered_map<ue::UObject*, std::function<void(ue::UObject*, void*)>> native;
std::vector<attr::Target> targets;
std::vector<std::string> classQueries;  // Resolve() retry scan accounting.
struct Wish { attr::Kind kind; float multiplier; };
std::vector<Wish> wishes;
int checks = 0, nativeAdds = 0, lastAdd = 0, lastRemove = 0, lastDust = 0, capacity = 1000, inventory = 20;
bool raiseOnAdd = false, nestedAdd = false;
template<class T> void put(void* base, int offset, T value) { std::memcpy(static_cast<uint8_t*>(base) + offset, &value, sizeof value); }
uint8_t* alloc() { memory.push_back(std::make_unique<uint8_t[]>(8192)); std::memset(memory.back().get(), 0, 8192); return memory.back().get(); }
uint32_t name(const std::string& s) {
    auto it = std::find(names.begin(), names.end(), s);
    if (it != names.end()) return static_cast<uint32_t>(it - names.begin());
    names.push_back(s); return static_cast<uint32_t>(names.size() - 1);
}
ue::UObject* object(const std::string& n, ue::UObject* cls = nullptr, ue::UObject* outer = nullptr) {
    auto* p = alloc(); put(p, ue::Layout::ObjIndex, static_cast<int>(objects.size()));
    put(p, ue::Layout::ObjName, name(n)); put(p, ue::Layout::ObjClass, cls); put(p, ue::Layout::ObjOuter, outer);
    objects.push_back(p); return p;
}
ue::UObject* cls(const std::string& n, ue::UObject* super = nullptr) {
    auto* p = object(n); put(p, ue::Layout::StructSuper, super); classes[n] = p; return p;
}
ue::UObject* function(ue::UObject* owner, const std::string& n, std::initializer_list<runtime::Param> params) {
    auto* f = object(n); int offset = 0; uint8_t* previous = nullptr;
    for (const auto& param : params) {
        const int align = (std::min)(param.size, 8); offset = (offset + align - 1) / align * align;
        auto* field = alloc(); auto* type = alloc();
        put(type, ue::Layout::FieldClassName, name(param.type)); put(field, ue::Layout::FieldClass, type);
        put(field, ue::Layout::FieldName, name(param.name)); put(field, ue::Layout::PropOffset, offset);
        put(field, 0x34, param.size); put(field, 0x38, uint64_t(0x80 | (param.result ? 0x400 : 0)));
        if (previous) put(previous, ue::Layout::FieldNext, field); else put(f, ue::Layout::StructChildProps, field);
        previous = field; properties[{f, param.name}] = offset; offset += param.size;
    }
    put(f, ue::Layout::StructSize, offset); functions[{owner, n}] = f; return f;
}
void dispatch(ue::UObject* o, ue::UObject* f, void* p) {
    if (auto it = interceptors.find(f); it != interceptors.end() && it->second(o, p)) return;
    pe::Call(o, f, p);
}
void check(bool condition, const char* message) {
    ++checks; if (!condition) throw std::runtime_error(message);
}
template<class T> T arg(ue::UObject* fn, void* params, const char* n) { return ue::Get<T>(static_cast<uint8_t*>(params) + properties.at({fn, n})); }
template<class T> void argPut(ue::UObject* fn, void* params, const char* n, T v) { put(params, properties.at({fn, n}), v); }
}  // namespace fake

namespace pbz {
std::vector<std::unique_ptr<IFeature>>& Features() { static std::vector<std::unique_ptr<IFeature>> v; return v; }
IFeature* FindFeature(const std::string& id) { for (auto& f : Features()) if (id == f->Id()) return f.get(); return nullptr; }
namespace ue {
bool Read(const void* p, void* out, size_t n) { if (!p) return false; std::memcpy(out, p, n); return true; }
bool Write(void* p, const void* in, size_t n) { if (!p) return false; std::memcpy(p, in, n); return true; }
std::string Name(uint32_t index) { return index < fake::names.size() ? fake::names[index] : ""; }
std::string ObjName(UObject* o) { return o ? Name(Get<uint32_t>(o + Layout::ObjName)) : ""; }
UObject* ClassOf(UObject* o) { return o ? Get<UObject*>(o + Layout::ObjClass) : nullptr; }
UObject* FindClass(std::string_view n) { fake::classQueries.emplace_back(n); auto it = fake::classes.find(std::string(n)); return it == fake::classes.end() ? nullptr : it->second; }
bool IsA(UObject* o, UObject* wanted) { for (auto* c = ClassOf(o); c; c = Get<UObject*>(c + Layout::StructSuper)) if (c == wanted) return true; return false; }
bool IsDefault(UObject* o) { return ObjName(o).starts_with("Default__"); }
UObject* Obj(int i) { return i >= 0 && i < static_cast<int>(fake::objects.size()) ? fake::objects[i] : nullptr; }
void ForEach(const std::function<bool(int, UObject*)>& fn) {
    for (int i = 0; i < static_cast<int>(fake::objects.size()); ++i)
        if (!fn(i, fake::objects[i])) break;
}
int PropOffset(UObject* owner, std::string_view name) {
    for (; owner; owner = Get<UObject*>(owner + Layout::StructSuper))
        if (auto it = fake::properties.find({owner, std::string(name)}); it != fake::properties.end()) return it->second;
    return -1;
}
}
namespace attr {
bool Resolve(std::string*) { return true; }
int AttrOffset(const std::string& n) { return n == "MindDustAdditiveMultiplier" ? 0 : n == "WalkSpeed" ? 32 : n == "JogSpeed" ? 64 : n == "SprintSpeed" ? 96 : -1; }
int ActorOffset(const std::string&) { return -1; }
int WorldOffset(const std::string&) { return -1; }
float* AttrCurrent(const Target& t, int off) { return reinterpret_cast<float*>(t.set + off + 12); }
float* AttrBase(const Target& t, int off) { return reinterpret_cast<float*>(t.set + off + 8); }
const std::vector<Target>& Targets() { return fake::targets; }
bool Alive(const Target& t) { return runtime::Identity(t.obj).Alive(); }
void Want(const Target&, float*, Kind kind, float multiplier) { fake::wishes.push_back({kind, multiplier}); }
void Hold(const Target&, float*, float) {}
}
namespace pe {
ue::UObject* FindFunction(ue::UObject* owner, std::string_view name) {
    for (; owner; owner = ue::Get<ue::UObject*>(owner + ue::Layout::StructSuper))
        if (auto it = fake::functions.find({owner, std::string(name)}); it != fake::functions.end()) return it->second;
    return nullptr;
}
void Call(ue::UObject* o, ue::UObject* f, void* p) { if (auto it = fake::native.find(f); it != fake::native.end()) it->second(o, p); }
void Intercept(ue::UObject* f, Interceptor h) { fake::interceptors[f] = std::move(h); }
void RemoveIntercept(ue::UObject* f) { fake::interceptors.erase(f); }
}
}  // namespace pbz
