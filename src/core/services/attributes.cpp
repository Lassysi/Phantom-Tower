#include "attributes.h"

#include <algorithm>
#include <map>
#include <unordered_map>

#include "../host/log.h"

namespace pbz::attr {
namespace {

ue::UObject* g_charCls = nullptr;
ue::UObject* g_playerCls = nullptr;
ue::UObject* g_setCls = nullptr;
ue::UObject* g_actorCls = nullptr;
ue::UObject* g_worldCls = nullptr;
int g_setOff = -1;
std::vector<Target> g_targets;
std::map<std::string, int> g_attrOffs;

struct Wish { Target t; Kind kind; float m; bool set = false; float setv = 0; };  // set：有 Set 声明，结果 = setv × 同字段 Scale 的乘积
struct Rec { Target t; float orig, last; };
std::unordered_map<float*, Wish> g_wish;
std::unordered_map<float*, Rec> g_recs;
struct HoldW { Target t; float v; };
std::unordered_map<float*, HoldW> g_hold;

int Cached(std::map<std::string, int>& cache, ue::UObject* cls, const std::string& name) {
    if (auto it = cache.find(name); it != cache.end()) return it->second;
    const int o = cls ? ue::PropOffset(cls, name) : -1;
    cache.emplace(name, o);
    return o;
}

bool ReadF(float* p, float* out) { return ue::Read(p, out, 4); }

}  // namespace

bool Resolve(std::string* why) {
    if (g_setOff >= 0) return true;
    g_charCls = ue::FindClass("PBZCharacter");
    g_playerCls = ue::FindClass("PBZCharacter_Player");
    g_setCls = ue::FindClass("BasicAttributeSet");
    g_actorCls = ue::FindClass("Actor");
    g_worldCls = ue::FindClass("WorldSettings");
    if (!g_charCls || !g_playerCls || !g_setCls || !g_actorCls) { *why = "角色或属性集类未找到"; return false; }
    g_setOff = ue::PropOffset(g_charCls, "AttributeSetBase");
    if (g_setOff < 0) { *why = "AttributeSetBase 未找到"; return false; }
    log::Info("属性：PBZCharacter.AttributeSetBase +0x{:X}", g_setOff);
    return true;
}

int AttrOffset(const std::string& a) { return Cached(g_attrOffs, g_setCls, a); }
int ActorOffset(const std::string& p) { static std::map<std::string, int> c; return Cached(c, g_actorCls, p); }
int WorldOffset(const std::string& p) { static std::map<std::string, int> c; return Cached(c, g_worldCls, p); }

int PlayerOffset(const std::string& p) {
    // 蓝图变量在玩家的蓝图类上，按当前玩家实例的类查
    static std::map<std::string, int> c;
    if (auto it = c.find(p); it != c.end() && it->second >= 0) return it->second;
    for (auto& t : g_targets)
        if (t.who == Who::Player) {
            const int o = ue::PropOffset(t.cls, p);
            c[p] = o;
            return o;
        }
    return -1;
}

float* AttrCurrent(const Target& t, int off) { return reinterpret_cast<float*>(t.set + off + 0x0C); }
float* AttrBase(const Target& t, int off) { return reinterpret_cast<float*>(t.set + off + 0x08); }

// 目标扫描分摊到多帧：每帧只看 kPerFrame 个对象，一轮约十几帧，避免单帧卡顿。
// 每个类只判断一次（缓存），默认对象用对象标志排除，不拼名字字符串。
namespace {
constexpr int kPerFrame = 2048;  // 约 26 万对象 → 一轮约 128 帧（~2 秒）；实测 4096/帧约 0.34ms，主要是读对象头的缓存未命中
constexpr uint32_t kRfDefaultOrArchetype = 0x10 | 0x20;  // RF_ClassDefaultObject | RF_ArchetypeObject
constexpr uint32_t kRfGone = 0x00008000 | 0x00010000 | 0x40000000;  // 已开始 / 完成销毁、标为垃圾
enum : int8_t { kNone, kEnemy, kPlayer, kWorld };
std::unordered_map<ue::UObject*, int8_t> g_kind;
std::vector<Target> g_building;
int g_cursor = 0;

int8_t KindOf(ue::UObject* cls) {
    auto [it, fresh] = g_kind.try_emplace(cls, kNone);
    if (!fresh) return it->second;
    int depth = 0;  // 父类链上限：失效类对象的链可能成环
    for (auto* c = cls; c && depth < 64; c = ue::Get<ue::UObject*>(c + ue::Layout::StructSuper), ++depth) {
        if (c == g_playerCls) return it->second = kPlayer;
        if (c == g_charCls) return it->second = kEnemy;
        if (c == g_worldCls) return it->second = kWorld;
    }
    return kNone;
}
}  // namespace

void Refresh(float) {
    if (g_setOff < 0) return;
    const int n = ue::Num();
    static ue::UObject* batch[kPerFrame];
    const int got = ue::ObjBatch(g_cursor, kPerFrame, batch);
    const int end = std::min(n, g_cursor + kPerFrame);
    for (int i = 0; i < got; ++i) {
        auto* o = batch[i];
        struct { void* vt; uint32_t flags; int32_t index; ue::UObject* cls; } head;  // UObject 前 0x18 字节一次读
        if (!o || !ue::Read(o, &head, sizeof head) || (head.flags & (kRfDefaultOrArchetype | kRfGone))) continue;
        auto* cls = head.cls;
        switch (KindOf(cls)) {
            case kEnemy:
            case kPlayer:
                if (auto* set = ue::Get<uint8_t*>(o + g_setOff))
                    g_building.push_back({o, set, KindOf(cls) == kPlayer ? Who::Player : Who::Enemies, cls});
                break;
            case kWorld: g_building.push_back({o, nullptr, Who::World, cls}); break;
            default: break;
        }
    }
    g_cursor = end;
    if (g_cursor < n) return;
    g_cursor = 0;
    g_targets.swap(g_building);
    g_building.clear();
    g_kind.clear();  // 类可能被卸载后地址复用，每轮重新判断
}

const std::vector<Target>& Targets() { return g_targets; }

// 目标列表约 2 秒才重建一次：期间角色可能被销毁、GC 释放、内存被复用（切图、过场、塔清场时最常见）。
// 只比类指针和属性集指针不够——释放后的内存里旧值往往还在，写进去就是写坏别人的堆。
// 所以角色和属性集都要在对象表里核对身份（槽位仍指向它、未销毁、未标垃圾）。
bool Alive(const Target& t) {
    if (!ue::Live(t.obj) || ue::Get<ue::UObject*>(t.obj + ue::Layout::ObjClass) != t.cls) return false;
    return t.who == Who::World || (ue::Get<uint8_t*>(t.obj + g_setOff) == t.set && ue::Live(t.set));
}

// Set 定基准值，同字段的 Scale 照样乘在上面（影塔把生命上限钉成标准值，修改器的"生命上限 ×1.8"仍然生效；2026-10-30 用户要求）
void Want(const Target& t, float* field, Kind kind, float m) {
    const bool isSet = kind == Kind::Set;
    auto [it, fresh] = g_wish.try_emplace(field, Wish{t, isSet ? Kind::Scale : kind, isSet ? 1.0f : m, isSet, isSet ? m : 0.0f});
    if (fresh) return;
    if (isSet) { it->second.set = true; it->second.setv = m; }
    else it->second.m *= m;
}

void Hold(const Target& t, float* field, float v) { g_hold[field] = {t, v}; }

void Commit() {
    for (auto& [field, h] : g_hold)
        if (Alive(h.t)) ue::Write(field, &h.v, 4);
    g_hold.clear();
    for (auto& [field, w] : g_wish) {
        float cur;
        if (!Alive(w.t) || !ReadF(field, &cur)) continue;
        auto [it, fresh] = g_recs.try_emplace(field, Rec{w.t, cur, cur});
        if (!fresh && cur != it->second.last) it->second.orig = cur;  // 游戏改过：以新值为原值
        const float o = it->second.orig;
        const float v = w.set ? w.setv * w.m : w.kind == Kind::Scale ? o * w.m : w.kind == Kind::Additive ? (1.0f + o) * w.m - 1.0f : w.m;
        ue::Write(field, &v, 4);
        it->second.last = v;
    }
    for (auto it = g_recs.begin(); it != g_recs.end();) {
        if (g_wish.count(it->first)) { ++it; continue; }
        float cur;
        if (Alive(it->second.t) && ReadF(it->first, &cur) && cur == it->second.last) ue::Write(it->first, &it->second.orig, 4);
        it = g_recs.erase(it);
    }
    g_wish.clear();
}

void ReleaseAll() {
    g_wish.clear();
    Commit();
}

}  // namespace pbz::attr
