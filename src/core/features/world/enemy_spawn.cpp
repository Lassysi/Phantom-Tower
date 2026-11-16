// Static/isolated candidate. Host/IPC/UI wiring belongs to Claude; see
// tools/enemies/SPAWNER.md. Nothing runs merely by linking this translation unit.
#include "enemy_spawn.h"
#include "enemy_catalog.generated.h"
#include "spawn_policy.h"
#include "spawn_compat.h"
#include "../../services/gamethread.h"
#include "../../services/inventory.h"
#include "../../services/process_event.h"
#include "../../services/diag.h"
#include "../../host/log.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cctype>
#include <limits>
#include <cstring>
#include <format>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <unordered_map>
#include <unordered_set>

namespace pbz::enemies {
namespace {
using U = ue::UObject;
struct Failure : std::runtime_error { using std::runtime_error::runtime_error; };
void Need(bool ok, const std::string& message) { if (!ok) throw Failure(message); }
std::string Escape(const std::string& value) {
    std::string out;
    for (unsigned char c : value) {
        if (c == '\\' || c == '"') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32) out += std::format("\\u{:04x}", c);
        else out += static_cast<char>(c);
    }
    return out;
}
std::string Response(const Request& r, bool ok, const std::string& message, const std::string& extra = {}) {
    return std::format(R"({{"op":"enemy","cmd":"{}","request_id":"{}","ok":{},"message":"{}"{}}})",
                       Escape(r.command), Escape(r.requestId), ok ? "true" : "false", Escape(message), extra);
}
struct Identity {
    U *object = nullptr, *cls = nullptr, *outer = nullptr;
    int index = -1;
    uint64_t name = 0;
    Identity() = default;
    explicit Identity(U* p) : object(p) {
        if (!p) return;
        index = ue::Get<int>(p + ue::Layout::ObjIndex, -1);
        cls = ue::ClassOf(p); outer = ue::Get<U*>(p + ue::Layout::ObjOuter);
        name = ue::Get<uint64_t>(p + ue::Layout::ObjName);
    }
    bool Alive() const {
        return object && index >= 0 && ue::Obj(index) == object && ue::ClassOf(object) == cls &&
               ue::Get<U*>(object + ue::Layout::ObjOuter) == outer && ue::Get<uint64_t>(object + ue::Layout::ObjName) == name;
    }
};
struct Array { void* data = nullptr; int32_t count = 0, capacity = 0; };
struct Transform {
    double rotation[4]{0, 0, 0, 1};
    Vec translation; double padding0 = 0;
    Vec scale{1, 1, 1}; double padding1 = 0;
};
static_assert(sizeof(Vec) == 24 && sizeof(Array) == 16 && sizeof(Transform) == 96);
struct Spec { const char *name, *type; int bytes; bool result = false; const char* structure = nullptr; };
struct Buffer {
    alignas(16) std::array<uint8_t, 4096> data{};
    template<class T> void Put(int off, const T& value) {
        Need(off >= 0 && off <= 4096 - static_cast<int>(sizeof value), "参数写入越界");
        std::memcpy(data.data() + off, &value, sizeof value);
    }
    template<class T> T Get(int off) const {
        Need(off >= 0 && off <= 4096 - static_cast<int>(sizeof(T)), "参数读取越界");
        T value{}; std::memcpy(&value, data.data() + off, sizeof value); return value;
    }
};
struct Fn {
    Identity owner, function;
    std::vector<int> offsets, bytes;
    void Bind(U* receiver, U* cls, const char* name, std::initializer_list<Spec> specs) {
        auto* fn = cls ? pe::FindFunction(cls, name) : nullptr;
        Need(receiver && fn, std::string("缺少反射接口：") + name);
        owner = Identity(receiver); function = Identity(fn); offsets.clear(); bytes.clear();
        const int size = ue::Get<int>(fn + ue::Layout::StructSize, -1);
        Need(size >= 0 && size <= 4096, std::string("反射参数区异常：") + name);
        std::vector<U*> fields;
        U* field = ue::Get<U*>(fn + ue::Layout::StructChildProps);
        for (int n = 0; field && n < 128; ++n) {
            if (ue::Get<uint64_t>(field + 0x38) & 0x80) fields.push_back(field);
            field = ue::Get<U*>(field + ue::Layout::FieldNext);
        }
        Need(!field && fields.size() == specs.size(), std::string("参数数目不匹配：") + name);
        for (const auto& spec : specs) {
            U* found = nullptr;
            for (U* f : fields) if (ue::Name(ue::Get<uint32_t>(f + ue::Layout::FieldName)) == spec.name) {
                Need(!found, "重复参数名"); found = f;
            }
            Need(found != nullptr, std::string("缺少参数：") + name + "." + spec.name);
            U* type = ue::Get<U*>(found + ue::Layout::FieldClass);
            const auto typeName = type ? ue::Name(ue::Get<uint32_t>(type + ue::Layout::FieldClassName)) : "";
            const int off = ue::Get<int>(found + ue::Layout::PropOffset, -1);
            const int length = ue::Get<int>(found + 0x34, -1);
            const bool result = (ue::Get<uint64_t>(found + 0x38) & 0x400) != 0;
            // The SDK exporter aliases Class/ObjectPtr and SoftClass types.
            // Its enum<X>:u8 syntax is a ByteProperty with UEnum at +0x70.
            const bool objectAlias = std::string_view(spec.type) == "ObjectProperty" &&
                (typeName == "ClassProperty" || typeName == "ClassPtrProperty" || typeName == "ObjectPtrProperty");
            const bool softAlias = std::string_view(spec.type) == "SoftObjectProperty" && typeName == "SoftClassProperty";
            const bool byteEnum = std::string_view(spec.type) == "EnumProperty" && typeName == "ByteProperty" && ue::Get<U*>(found + 0x70);
            Need((typeName == spec.type || objectAlias || softAlias || byteEnum) && length == spec.bytes && result == spec.result && off >= 0 &&
                 off <= size - length, std::string("参数类型/长度不匹配：") + name + "." + spec.name);
            if (spec.structure) {
                const bool isEnum = std::string_view(spec.type) == "EnumProperty";
                U* structure = ue::Get<U*>(found + (isEnum && typeName == "EnumProperty" ? 0x78 : 0x70));
                Need((isEnum || typeName == "StructProperty") && structure && ue::ObjName(structure) == spec.structure,
                     std::string("参数结构/枚举类型不匹配：") + name + "." + spec.name);
            }
            for (size_t i = 0; i < offsets.size(); ++i)
                Need(off + length <= offsets[i] || offsets[i] + bytes[i] <= off, "反射参数区重叠");
            offsets.push_back(off); bytes.push_back(length);
        }
    }
    void Call(Buffer& b, U* overrideOwner = nullptr) const {
        Need(owner.Alive() && function.Alive(), "接口对象已失效，取消本次请求");
        pe::Call(overrideOwner ? overrideOwner : owner.object, function.object, b.data.data());
    }
    template<class T> void Set(Buffer& b, size_t index, const T& value) const {
        Need(index < offsets.size() && sizeof value == bytes[index], "参数尺寸不符"); b.Put(offsets[index], value);
    }
    template<class T> T Get(const Buffer& b, size_t index) const {
        Need(index < offsets.size() && sizeof(T) == bytes[index], "返回尺寸不符"); return b.Get<T>(offsets[index]);
    }
};
U* Cdo(U* cls) {
    static std::unordered_map<U*, Identity> cache;
    if (auto it = cache.find(cls); it != cache.end()) {
        if (it->second.Alive() && ue::ClassOf(it->second.object) == cls) return it->second.object;
        cache.erase(it);
    }
    // 第一次整表找到后记下 UClass 里默认对象指针的偏移，之后直接读并核对（每种新怪省一次全表扫描）
    static int cdoOffset = -1;
    U* result = nullptr;
    if (cdoOffset >= 0) {
        U* c = ue::Get<U*>(cls + cdoOffset);
        if (c && Identity(c).Alive() && ue::ClassOf(c) == cls && ue::ObjName(c) == "Default__" + ue::ObjName(cls)) result = c;
    }
    if (!result) {
        ue::ForEach([&](int, U* o) {
            if (ue::ClassOf(o) == cls && ue::ObjName(o) == "Default__" + ue::ObjName(cls)) { result = o; return false; }
            return true;
        });
        if (result && cdoOffset < 0) for (int o = 0x80; o <= 0x1C0; o += 8) if (ue::Get<U*>(cls + o) == result) { cdoOffset = o; break; }
    }
    if (result) { if (cache.size() >= 256) cache.clear(); cache[cls] = Identity(result); }
    return result;
}
void Library(Fn& fn, const char* clsName, const char* name, std::initializer_list<Spec> specs) {
    U* cls = ue::FindClass(clsName); fn.Bind(cls ? Cdo(cls) : nullptr, cls, name, specs);
}
void StructLayout(const char* name, int size, std::initializer_list<std::pair<const char*, int>> fields) {
    U* cls = ue::FindClass(name);
    Need(cls && ue::Get<int>(cls + ue::Layout::StructSize, -1) == size, std::string("结构布局不符：") + name);
    for (const auto& [field, offset] : fields)
        Need(ue::PropOffset(cls, field) == offset, std::string("结构字段不符：") + name + "." + field);
}
U* WorldOf(U* obj) {
    U *world = ue::FindClass("World"), *level = ue::FindClass("Level");
    const int owning = level ? ue::PropOffset(level, "OwningWorld") : -1;
    for (int n = 0; obj && n < 24; ++n) {
        if (world && ue::IsA(obj, world)) return obj;
        if (level && owning >= 0 && ue::IsA(obj, level)) {
            U* result = ue::Get<U*>(obj + owning);
            return result && ue::IsA(result, world) ? result : nullptr;
        }
        obj = ue::Get<U*>(obj + ue::Layout::ObjOuter);
    }
    return nullptr;
}
std::string ObjectPath(U* o) {
    std::string result;
    for (int n = 0; o && n < 32; ++n) {
        result = ue::ObjName(o) + (result.empty() ? "" : "." + result);
        o = ue::Get<U*>(o + ue::Layout::ObjOuter);
    }
    return result;
}
struct Owned { Identity actor, controller, world; std::string path; Landing landing; std::string faction; std::string tag; uint64_t statsRevision = 0; ULONGLONG deadSince = 0, zeroSince = 0; bool deathQueued = false; int storyLocksRemoved = 0; ULONGLONG compatNext = 0; int arenaRedirects = 0; bool coldBodyReady = false; };  // tag = 生成时的 request_id
std::vector<Owned> g_owned;
std::atomic<bool> g_stopping{false};
std::atomic<int> g_pending{0};
std::atomic<uint64_t> g_epoch{0};
std::set<std::string> g_requests; // requests are never replayed to double-spawn
std::atomic<DWORD> g_thread{0};
bool g_maintenance = false;

void ClaimSpawnRequest(const Request& request) {
    Need(!request.requestId.empty() && request.requestId.size() <= 128, "生成请求缺少唯一 request_id");
    Need(request.count >= 1 && request.count <= 8, "单批数量须为1–8只，可连续生成，不限制总数");
    // Keep all IDs until core shutdown so old/partial requests cannot replay.
    // Neither owned actor count nor lifetime request count is an admission cap.
    Need(g_requests.insert(request.requestId).second, "重复 request_id 已拒绝，未重复生成");
}

std::string OwnedStatsJson() {
    std::string rows;
    auto* basic = ue::FindClass("BasicAttributeSet");
    for (const auto& own : g_owned) {
        if (!own.actor.Alive()) continue;
        auto* actor = own.actor.object;
        const int setOffset = ue::PropOffset(own.actor.cls, "AttributeSetBase"), levelOffset = ue::PropOffset(own.actor.cls, "Level");
        auto* set = setOffset >= 0 ? ue::Get<U*>(actor + setOffset) : nullptr;
        auto number = [](double n) { return std::isfinite(n) ? std::format("{}", n) : "null"; };
        std::string attributes;
        for (const auto* name : {"Health", "Health_Max", "Health_Min", "Shachi", "Shachi_Max", "Attack", "ShachiAttack", "Infliction_Max"}) {
            const int off = basic ? ue::PropOffset(basic, name) : -1;
            if (!attributes.empty()) attributes += ",";
            const float invalid = std::numeric_limits<float>::quiet_NaN();
            const auto base = set && off >= 0 ? ue::Get<float>(set + off + 8, invalid) : invalid;
            const auto current = set && off >= 0 ? ue::Get<float>(set + off + 12, invalid) : invalid;
            attributes += std::format(R"("{}":{{"base":{},"current":{}}})", name, number(base), number(current));
        }
        if (!rows.empty()) rows += ",";
        const double invalid = std::numeric_limits<double>::quiet_NaN();
        rows += std::format(R"({{"name":"{}","path":"{}","tag":"{}","native_level":{},"stats_revision":{},"death_recovery_queued":{},"story_locks_removed":{},"arena_redirects":{},"attributes":{{{}}}}})",
            Escape(ue::ObjName(actor)), Escape(own.path), Escape(own.tag), number(levelOffset >= 0 ? ue::Get<double>(actor + levelOffset, invalid) : invalid), own.statsRevision, own.deathQueued ? "true" : "false", own.storyLocksRemoved, own.arenaRedirects, attributes);
    }
    return "[" + rows + "]";
}

void AdaptStandalone(Owned& own) {
    if (own.tag.starts_with("tower-")) return;
    const auto rule = StandaloneRuleFor(own.path);
    if (rule.effect.empty()) return;
    Need(own.actor.Alive() && own.world.Alive() && WorldOf(own.actor.object) == own.world.object &&
         ObjectPath(own.actor.cls) == own.path, "独立召唤适配的角色身份已变化");
    Need(std::any_of(g_owned.begin(), g_owned.end(), [&](const Owned& e) { return &e == &own; }), "仅允许适配自有召唤物");
    const int co = ue::PropOffset(own.actor.cls, "Actor Config"), ao = ue::PropOffset(own.actor.cls, "AbilitySystemComponent");
    U* config = co >= 0 ? ue::Get<U*>(own.actor.object + co) : nullptr;
    U* asc = ao >= 0 ? ue::Get<U*>(own.actor.object + ao) : nullptr;
    U* ac = ue::FindClass("AbilitySystemComponent");
    Need(config && Identity(config).Alive() && ObjectPath(config) == rule.config, "独立召唤适配的原生配置不匹配");
    Need(asc && ac && ue::IsA(asc, ac) && Identity(asc).Alive() && ue::Get<U*>(asc + ue::Layout::ObjOuter) == own.actor.object,
         "独立召唤适配不能修改其他角色的能力系统");
    const int av = ue::PropOffset(ac, "AvatarActor");
    Need(av >= 0 && ue::Get<U*>(asc + av) == own.actor.object, "召唤能力系统的 Avatar 不匹配");
    // The final snake's passive starts a player/level ending sequence; it is
    // not a combat move. Remove this exact granted spec via native ClearAbility.
    if (!rule.cinematicAbility.empty()) {
        StructLayout("GameplayAbilitySpecContainer", 0x120, {{"Items", 0x108}});
        StructLayout("GameplayAbilitySpec", 0xF0, {{"Handle", 0xC}, {"Ability", 0x10}});
        const int off = ue::PropOffset(ac, "ActivatableAbilities");
        Need(off >= 0, "原生能力目录尚未就绪");
        auto findHandles = [&] {
            const auto list = ue::Get<Array>(asc + off + 0x108);
            Need(list.count >= 0 && list.count <= list.capacity && list.capacity <= 4096 && (!list.count || list.data), "召唤能力目录损坏");
            std::vector<int32_t> handles;
            for (int i = 0; i < list.count; ++i) {
                auto* spec = static_cast<uint8_t*>(list.data) + i * 0xF0;
                auto* ability = ue::Get<U*>(spec + 0x10);
                if (!ability || !Identity(ability).Alive()) continue;
                const auto path = ObjectPath(ue::ClassOf(ability));
                const bool coldStory = own.path == kLengTu && std::find(std::begin(kLengTuStoryAbilities), std::end(kLengTuStoryAbilities), path) != std::end(kLengTuStoryAbilities);
                if (!coldStory && path != rule.cinematicAbility) continue;
                const int32_t handle = ue::Get<int32_t>(spec + 0xC);
                Need(handle > 0, "召唤剧情能力的原生编号无效"); handles.push_back(handle);
            }
            return handles;
        };
        const auto handles = findHandles();
        if (!handles.empty()) {
            Fn clear; clear.Bind(asc, ac, "ClearAbility", {{"Handle", "StructProperty", 4, false, "GameplayAbilitySpecHandle"}});
            for (int32_t handle : handles) { Buffer b; clear.Set(b, 0, handle); clear.Call(b); }
            Need(own.actor.Alive() && findHandles().empty(), "原生接口未移除召唤剧情能力");
            log::Info("独立召唤：移除结束剧情能力 {}（{}项），战斗能力保留", own.path, handles.size());
        }
    }
    const auto dot = rule.effect.rfind('.');
    Need(dot != std::string_view::npos, "召唤锁血规则路径无效");
    auto* effect = ue::FindClass(rule.effect.substr(dot + 1));
    Need(effect && Identity(effect).Alive() && ObjectPath(effect) == rule.effect, "召唤剧情锁血效果未解析");
    Fn count, remove;
    count.Bind(asc, ac, "GetGameplayEffectCount", {{"SourceGameplayEffect", "ObjectProperty", 8},
        {"OptionalInstigatorFilterComponent", "ObjectProperty", 8}, {"bEnforceOnGoingCheck", "BoolProperty", 1}, {"ReturnValue", "IntProperty", 4, true}});
    auto current = [&] { Buffer b; count.Set(b, 0, effect); count.Call(b); return count.Get<int32_t>(b, 3); };
    const int before = current(); Need(before >= 0 && before <= 4096, "锁血效果计数异常");
    if (own.path == kLengTu && !own.coldBodyReady) {
        // Exact own-instance terminal combat state from FinalPhase.OnEnd plus
        // ToBattle.Activate. No world lookup, quest save API or shared CDO edit.
        Fn set;
        Library(set, "AbilityUtils", "SetLooseGameplayTagCount", {{"Actor", "ObjectProperty", 8},
            {"GameplayTag", "StructProperty", 8, false, "GameplayTag"}, {"Count", "IntProperty", 4}});
        for (const auto* name : {"CharacterState.AbilityState.LengTu.Phase.P1", "CharacterState.AbilityState.LengTu.Phase.P2",
             "CharacterState.AbilityState.LengTu.Phase.P3", "CharacterState.AbilityState.LengTu.Phase.P4", "CharacterState.AbilityState.LengTu.Phase.P5",
             "CharacterState.Unselectable.Force.Perform", "CharacterState.AbilityState.LengTu.OffCombat", "AI.Hatred.CanNotBeHatred",
             "CharacterState.SoftCollision.CanNotBePush", "CharacterState.AbilityState.LengTu.FirstTimeSummon",
             "CharacterState.AbilityState.LengTu.IsInvulnerable", "CharacterState.AbilityState.LengTu.FinishedQuest"}) {
            const auto tag = inv::ToFName(name); Need(tag != 0, "冷荼独立阶段标签缺失");
            Buffer t; set.Set(t, 0, own.actor.object); set.Set(t, 1, tag); set.Set(t, 2, int32_t(0)); set.Call(t);
        }
        for (const auto* name : {"CharacterState.AbilityState.LengTu.Phase.Final", "CharacterState.AbilityState.LengTu.LeiDang",
                                "CharacterState.AbilityState.LengTu.EnableMetaAI"}) {
            const auto tag = inv::ToFName(name); Need(tag != 0, "冷荼独立战斗标签缺失");
            Buffer t; set.Set(t, 0, own.actor.object); set.Set(t, 1, tag); set.Set(t, 2, int32_t(1)); set.Call(t);
        }
        // Preserve the two effects authored on FinalPhase's native timeline.
        // Empty context delegates creation/ownership to the native ASC; no
        // shared-pointer-bearing context is returned to a raw host buffer.
        Fn apply;
        apply.Bind(asc, ac, "BP_ApplyGameplayEffectToSelf", {{"GameplayEffectClass", "ObjectProperty", 8},
            {"Level", "FloatProperty", 4}, {"EffectContext", "StructProperty", 24, false, "GameplayEffectContextHandle"},
            {"ReturnValue", "StructProperty", 8, true, "ActiveGameplayEffectHandle"}});
        for (const auto path : kLengTuBodyEffects) {
            auto* cls = ue::FindClass(path.substr(path.rfind('.') + 1));
            Need(cls && Identity(cls).Alive() && ObjectPath(cls) == path, "冷荼本体阶段效果缺失");
            Buffer c; count.Set(c, 0, cls); count.Call(c);
            if (count.Get<int32_t>(c, 3) > 0) continue;
            Buffer a; apply.Set(a, 0, cls); apply.Set(a, 1, 1.f); apply.Call(a);
            Buffer verify; count.Set(verify, 0, cls); count.Call(verify);
            Need(count.Get<int32_t>(verify, 3) > 0, "冷荼原生本体阶段效果未生效");
        }
        own.coldBodyReady = true;
        log::Info("独立冷荼：本体终阶段已初始化，保留原生战斗技能与阶段效果");
    }
    if (!before) return;
    remove.Bind(asc, ac, "RemoveActiveGameplayEffectBySourceEffect", {{"GameplayEffect", "ObjectProperty", 8},
        {"InstigatorAbilitySystemComponent", "ObjectProperty", 8}, {"StacksToRemove", "IntProperty", 4}});
    Buffer b; remove.Set(b, 0, effect); remove.Set(b, 2, int32_t(-1)); remove.Call(b);
    Need(current() == 0, "原生接口未移除独立召唤的剧情锁血");
    own.storyLocksRemoved += before;
    log::Info("独立召唤：原生接口移除剧情锁血 {}（{}层）；不改血量/杀气/攻击配置", own.path, before);
}

struct ArenaHook {
    Identity cls, function;
    Fn avatar, location, rotation, warp, base, end, removeTag, setTag;
    bool toBattle = false;
};
std::vector<ArenaHook> g_arenaHooks;
std::vector<Identity> g_coldStoryHooks;

bool OwnIndependentCold(U* actor) {
    return !g_stopping && std::any_of(g_owned.begin(), g_owned.end(), [&](const Owned& own) {
        return own.path == kLengTu && !own.tag.starts_with("tower-") && own.actor.object == actor &&
               own.actor.Alive() && own.world.Alive() && WorldOf(actor) == own.world.object;
    });
}

void BindColdStoryIsolation(U* cls) {
    Need(ObjectPath(cls) == kLengTu, "冷荼剧情隔离类型不匹配");
    // These four exact BP functions read/write the authored LengTu_Combat save
    // record. Independent bodies never participate in that global progression.
    for (const auto* name : {"InitTags", "UnlockTag", "RemoveUnLockTag", "ResetTags"}) {
        U* fn = pe::FindFunction(cls, name); Need(fn != nullptr, "冷荼剧情隔离接口缺失");
        if (std::any_of(g_coldStoryHooks.begin(), g_coldStoryHooks.end(), [&](const Identity& x) { return x.object == fn && x.Alive(); })) continue;
        g_coldStoryHooks.emplace_back(fn);
        pe::Intercept(fn, [](U* actor, void*) { try { return OwnIndependentCold(actor); } catch (...) { return false; } });
    }
}

bool RedirectArenaAbility(const ArenaHook& hook, U* ability) {
    try {
    if (g_stopping || !hook.cls.Alive() || !hook.function.Alive() || !Identity(ability).Alive() || !ue::IsA(ability, hook.cls.object)) return false;
    Buffer a; hook.avatar.Call(a, ability); U* actor = hook.avatar.Get<U*>(a, 0);
    auto own = std::find_if(g_owned.begin(), g_owned.end(), [&](const Owned& e) {
        return e.actor.object == actor && e.actor.Alive() && e.path == kLengTu && !e.tag.starts_with("tower-") &&
               e.world.Alive() && WorldOf(actor) == e.world.object;
    });
    if (own == g_owned.end()) return false; // Natural/other-module abilities retain their original blueprint.
    try {
        U* comp = nullptr; U* wc = ue::FindClass("MotionWarpingComponent");
        ue::ForEach([&](int, U* o) {
            if (o && ue::Get<U*>(o + ue::Layout::ObjOuter) == actor && wc && ue::IsA(o, wc) && Identity(o).Alive()) { comp = o; return false; }
            return true;
        });
        Need(comp, "冷荼自身的位移修正组件缺失");
        Buffer l, r; hook.location.Call(l, actor); hook.rotation.Call(r, actor);
        const auto point = hook.location.Get<Vec>(l, 0), rotation = hook.rotation.Get<Vec>(r, 0);
        Need(Finite(point) && Finite(rotation), "冷荼局部阶段落点无效");
        const uint64_t tag = inv::ToFName("SnapPoint"); Need(tag != 0, "局部阶段定位名解析失败");
        Buffer w; hook.warp.Set(w, 0, tag); hook.warp.Set(w, 1, point); hook.warp.Set(w, 2, rotation); hook.warp.Call(w, comp);
        // This exact derived BP only calls its parent and then looks up an
        // authored level marker without checking it. Keep the real timeline,
        // visibility and phase-end behavior; replace only that marker lookup.
        Buffer b; hook.base.Call(b, ability);
        if (hook.toBattle) {
            // The original return-to-battle BP also clears these exact loose
            // phase guards and reenables its meta AI. Preserve those consumers.
            for (const auto* name : {"CharacterState.Unselectable.Force.Perform", "CharacterState.AbilityState.LengTu.OffCombat",
                                     "AI.Hatred.CanNotBeHatred", "CharacterState.SoftCollision.CanNotBePush"}) {
                const uint64_t value = inv::ToFName(name); Need(value != 0, "冷荼回场阶段标签解析失败");
                Buffer t; hook.removeTag.Set(t, 0, actor); hook.removeTag.Set(t, 1, value); hook.removeTag.Call(t);
            }
            const uint64_t value = inv::ToFName("CharacterState.AbilityState.LengTu.EnableMetaAI");
            Need(value != 0, "冷荼回场AI标签解析失败");
            Buffer t; hook.setTag.Set(t, 0, actor); hook.setTag.Set(t, 1, value); hook.setTag.Set(t, 2, int32_t(1)); hook.setTag.Call(t);
        }
        ++own->arenaRedirects;
        log::Info("独立冷荼：场地转阶段位移改用自身当前位置，原生动作保留（{}）", ue::ObjName(actor));
    } catch (const std::exception& e) {
        log::Info("独立冷荼场地定位失败，结束本次动作而不传送到无效场地：{}", e.what());
        try { Buffer b; hook.end.Call(b, ability); } catch (...) { log::Info("独立冷荼本次场地动作无法结束，保留登记供清理"); }
    } catch (...) {
        log::Info("独立冷荼场地动作异常，不继续原关卡传送");
        try { Buffer b; hook.end.Call(b, ability); } catch (...) {}
    }
    return true;
    } catch (...) { return false; } // Never unwind through the engine's ProcessEvent boundary.
}

void BindArenaAbility(U* cls) {
    const auto path = ObjectPath(cls);
    Need(std::find(std::begin(kLengTuArenaAbilities), std::end(kLengTuArenaAbilities), path) != std::end(kLengTuArenaAbilities), "场地适配能力不在审核名单");
    for (auto it = g_arenaHooks.begin(); it != g_arenaHooks.end();) {
        if (it->cls.object == cls && it->cls.Alive() && it->function.Alive()) return;
        if (!it->cls.Alive() || !it->function.Alive()) { pe::RemoveIntercept(it->function.object); it = g_arenaHooks.erase(it); }
        else ++it;
    }
    U* parent = ue::Get<U*>(cls + ue::Layout::StructSuper);
    Need(parent && ue::ObjName(parent) == "BP_ActionAbilityBase_Simple_C", "场地适配能力父类已变化");
    ArenaHook h; h.cls = Identity(cls);
    Fn derived; derived.Bind(Cdo(cls), cls, "K2_ActivateAbility", {}); h.function = derived.function;
    h.base.Bind(Cdo(cls), parent, "K2_ActivateAbility", {});
    Need(h.base.function.object != h.function.object, "场地能力父入口与派生入口相同");
    h.avatar.Bind(Cdo(cls), cls, "GetAvatarActorFromActorInfo", {{"ReturnValue", "ObjectProperty", 8, true}});
    h.end.Bind(Cdo(cls), cls, "K2_EndAbility", {});
    auto* ac = ue::FindClass("Actor"); auto* wc = ue::FindClass("MotionWarpingComponent");
    h.location.Bind(Cdo(ac), ac, "K2_GetActorLocation", {{"ReturnValue", "StructProperty", 24, true, "Vector"}});
    h.rotation.Bind(Cdo(ac), ac, "K2_GetActorRotation", {{"ReturnValue", "StructProperty", 24, true, "Rotator"}});
    h.warp.Bind(Cdo(wc), wc, "AddOrUpdateWarpTargetFromLocationAndRotation", {{"WarpTargetName", "NameProperty", 8},
        {"TargetLocation", "StructProperty", 24, false, "Vector"}, {"TargetRotation", "StructProperty", 24, false, "Rotator"}});
    h.toBattle = path == kLengTuArenaAbilities[1];
    if (h.toBattle) {
        Library(h.removeTag, "AbilityUtils", "RemoveLooseGameplayTag", {{"Actor", "ObjectProperty", 8},
            {"GameplayTag", "StructProperty", 8, false, "GameplayTag"}, {"bShouldReplicate", "BoolProperty", 1}});
        Library(h.setTag, "AbilityUtils", "SetLooseGameplayTagCount", {{"Actor", "ObjectProperty", 8},
            {"GameplayTag", "StructProperty", 8, false, "GameplayTag"}, {"Count", "IntProperty", 4}});
    }
    g_arenaHooks.push_back(h);
    pe::Intercept(h.function.object, [h](U* ability, void*) { return RedirectArenaAbility(h, ability); });
}

void RemoveArenaHooks() {
    for (const auto& h : g_arenaHooks) pe::RemoveIntercept(h.function.object);
    g_arenaHooks.clear();
    for (const auto& fn : g_coldStoryHooks) pe::RemoveIntercept(fn.object);
    g_coldStoryHooks.clear();
}

// GetCombatLevel reads the Blueprint's double Level, not BasicAttributeSet.Level.
// Borrow only the read context for Init Level. Never register a summoned actor
// in the story group or keep its quest/domain association afterwards.
double InitializeNativeStats(const Identity& actor, const Identity& group) {
    Need(actor.Alive() && group.Alive() && WorldOf(actor.object) == WorldOf(group.object), "原生定级上下文已变化");
    const auto own = std::find_if(g_owned.begin(), g_owned.end(), [&](const Owned& o) { return o.actor.object == actor.object && o.actor.Alive(); });
    Need(own != g_owned.end() && !own->tag.starts_with("tower-"), "只允许初始化修改器登记的非影塔生成物");
    auto* cls = ue::ClassOf(actor.object);
    auto* basic = ue::FindClass("BasicAttributeSet");
    const int setOffset = ue::PropOffset(cls, "AttributeSetBase"), healthOffset = basic ? ue::PropOffset(basic, "Health") : -1;
    auto* set = setOffset >= 0 ? ue::Get<U*>(actor.object + setOffset) : nullptr;
    const float health = set && healthOffset >= 0 ? ue::Get<float>(set + healthOffset + 12, -1.f) : -1.f;
    Need(std::isfinite(health) && health > 0, "属性尚未就绪或生成物已死亡，不执行重建");
    const int groupOffset = ue::PropOffset(cls, "MobGroup"), levelOffset = ue::PropOffset(cls, "Level");
    const int componentOffset = ue::PropOffset(cls, "Attribute Component");
    const int configOffset = ue::PropOffset(cls, "Actor Config");
    auto* configClass = ue::FindClass("PDA_ActorConfig_C");
    auto* config = configOffset >= 0 ? ue::Get<U*>(actor.object + configOffset) : nullptr;
    Need(config && configClass && ue::IsA(config, configClass) && Identity(config).Alive(), "原生角色配置尚未就绪，不使用通用弱化属性代替");
    auto* componentClass = ue::FindClass("AttributeComponent");
    Need(groupOffset >= 0 && levelOffset >= 0 && componentOffset >= 0 && componentClass, "原生属性接口尚未就绪");
    auto* component = ue::Get<U*>(actor.object + componentOffset);
    Need(component && ue::IsA(component, componentClass) && ue::Get<U*>(component + ue::Layout::ObjOuter) == actor.object, "属性组件不属于当前生成物");
    Fn init, rebuild, combatLevel;
    init.Bind(actor.object, cls, "Init Level", {});
    rebuild.Bind(component, componentClass, "ReinitializeAttributes", {});
    combatLevel.Bind(actor.object, cls, "GetCombatLevel", {{"ReturnValue", "FloatProperty", 4, true}});
    auto* previousGroup = ue::Get<U*>(actor.object + groupOffset);
    const double previousLevel = ue::Get<double>(actor.object + levelOffset);
    Need(ue::Write(actor.object + groupOffset, &group.object, sizeof group.object), "无法提供原生定级上下文");
    try {
        Buffer b; init.Call(b);
        Need(actor.Alive() && group.Alive(), "原生定级期间对象已变化");
    } catch (...) {
        if (actor.Alive()) { ue::Write(actor.object + groupOffset, &previousGroup, sizeof previousGroup); ue::Write(actor.object + levelOffset, &previousLevel, sizeof previousLevel); }
        throw;
    }
    Need(ue::Write(actor.object + groupOffset, &previousGroup, sizeof previousGroup), "无法还原临时定级上下文");
    Buffer level; combatLevel.Call(level);
    const float result = combatLevel.Get<float>(level, 0);
    if (!std::isfinite(result) || result < 1 || result > 10000) {
        ue::Write(actor.object + levelOffset, &previousLevel, sizeof previousLevel);
        throw Failure("原生定级未产生有效等级，未保留弱化生成物");
    }
    // Native config computes every attribute, difficulty/global modifiers and
    // preserved current/max percentages. No universal HP/attack ratio patch.
    Buffer b; rebuild.Call(b);
    Need(actor.Alive(), "原生属性初始化后对象失效");
    ++own->statsRevision;
    log::Info("生成物原生属性初始化：{} 等级 {}，定级组 {}（只读借用，已解除关联）", ue::ObjName(actor.object), result, ue::ObjName(group.object));
    return result;
}

void RecheckNativeStats(Identity actor, Identity group, uint64_t epoch, ULONGLONG due, double expected) {
    if (g_stopping || g_epoch != epoch || !actor.Alive() || !group.Alive()) return;
    if (GetTickCount64() < due) {
        gamethread::Post([=] { RecheckNativeStats(actor, group, epoch, due, expected); }); return;
    }
    const int offset = ue::PropOffset(actor.cls, "Level");
    const double current = offset >= 0 ? ue::Get<double>(actor.object + offset, -1) : -1;
    if (current == expected) return; // Never reinitialize healthy combatants each tick.
    try { InitializeNativeStats(actor, group); }
    catch (const std::exception& e) { log::Info("生成物延迟定级未完成：{}", e.what()); }
}

class Engine : public SceneQuery {
public:
    Identity world, player;
    Vec location{}, forward{};
    Capsule playerSize{};
    Fn valid, loc, direction, capsuleSize, line, room, nav, begin, finish, destroy,
       controllerSpawn, activate, playerPawn, standalone, paused, name, soft, load, objectType;
    U *npcClass = nullptr, *capsuleClass = nullptr, *pawnClass = nullptr;
    std::array<uint8_t, 6> collisionTypes{};
    int capsuleOffset = -1, controllerOffset = -1;
    int hitImpact = -1, hitNormal = -1, hitBlock = -1, hitPenetrate = -1;
    uint8_t blockMask = 0, penetrateMask = 0;
    bool hasNav = false;
    Identity levelGroup;
    std::string levelGroupPath;
    bool originalEncounter = false;
    std::string levelSource = "current_area";

    Identity NativeLevelGroup(const std::string& path) {
        if (levelGroup.Alive() && levelGroupPath == path && WorldOf(levelGroup.object) == world.object) return levelGroup;
        auto* spawnerClass = ue::FindClass("AISpawner"); auto* groupClass = ue::FindClass("MobGroup");
        Need(spawnerClass && groupClass, "原生生成组尚未加载");
        const int groupOffset = ue::PropOffset(spawnerClass, "MobGroup"), domainOffset = ue::PropOffset(groupClass, "MobDomain");
        const int classOffset = ue::PropOffset(spawnerClass, "Class");
        Need(groupOffset >= 0 && domainOffset >= 0, "原生生成组结构不符");
        Fn info; info.Bind(Cdo(spawnerClass), spawnerClass, "GetClassLoadConfigAndPolicy", {{"IsLoadSync", "BoolProperty", 1}, {"OutSoftClass", "SoftObjectProperty", 40}});
        Identity best; double score = std::numeric_limits<double>::infinity();
        ue::ForEach([&](int, U* source) {
            if (ue::IsDefault(source) || !ue::IsA(source, spawnerClass) || WorldOf(source) != world.object) return true;
            auto* group = ue::Get<U*>(source + groupOffset);
            if (!group || !ue::IsA(group, groupClass) || WorldOf(group) != world.object) return true;
            // The callable policy can return an empty soft class for cleared
            // spawners. Its authored Class property survives the encounter.
            std::array<uint8_t, 40> ref{};
            if (classOffset < 0 || !ue::Read(source + classOffset, ref.data(), ref.size()) || !ue::Get<uint64_t>(ref.data() + 8)) {
                Buffer b; info.Call(b, source); ref = info.Get<std::array<uint8_t, 40>>(b, 1);
            }
            // FSoftObjectPtr: weak reference (8) then FSoftObjectPath (32),
            // whose first two fields are the package and asset FNames.
            uint64_t package = 0, asset = 0; std::memcpy(&package, ref.data() + 8, 8); std::memcpy(&asset, ref.data() + 16, 8);
            const auto sourcePath = ue::Name(static_cast<uint32_t>(package)) + "." + ue::Name(static_cast<uint32_t>(asset));
            const bool exact = sourcePath.size() == path.size() &&
                std::equal(sourcePath.begin(), sourcePath.end(), path.begin(), [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); }) &&
                !(package >> 32) && !(asset >> 32);
            const double distance = Distance2D(Location(source), location);
            bool inside = false;
            if (!exact) {
                // Roadways and a cleared arena edge may be outside every
                // MobDomain. Use only an authored enemy encounter within 100m,
                // never a friendly/story NPC's default level or another world.
                const bool knownEnemy = std::any_of(std::begin(kEnemyCatalog), std::end(kEnemyCatalog), [&](const auto& entry) {
                    return sourcePath.size() == entry.path.size() && std::equal(sourcePath.begin(), sourcePath.end(), entry.path.begin(), [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
                });
                if (!knownEnemy) return true;
                auto* domain = ue::Get<U*>(group + domainOffset);
                auto* fn = domain && WorldOf(domain) == world.object ? pe::FindFunction(ue::ClassOf(domain), "IsActorInMobDomain") : nullptr;
                if (fn) {
                    Fn contains; contains.Bind(domain, ue::ClassOf(domain), "IsActorInMobDomain", {{"ActorToCheck", "ObjectProperty", 8}, {"ReturnValue", "BoolProperty", 1, true}});
                    Buffer c; contains.Set(c, 0, player.object); contains.Call(c); inside = contains.Get<uint8_t>(c, 1) != 0;
                }
                if (!inside && distance > 10000) return true;
            }
            // Prefer the actual same-class encounter, then a native group in
            // the player's domain. No samples from dead/alive monsters needed.
            const double candidate = (exact ? 0.0 : inside ? 1e15 : 2e15) + distance;
            if (candidate < score) { score = candidate; best = Identity(group); }
            return true;
        });
        Need(best.Alive(), "无法确认本关卡原生定级组，取消生成以免出现零级属性");
        levelSource = score < 1e15 ? "original_encounter" : score < 2e15 ? "current_area" : "nearby_encounter";
        log::Info("生成物定级来源：{} {}", path, levelSource);
        levelGroup = best; levelGroupPath = path; originalEncounter = score < 1e15; return best;
    }

    void BindCleanup() {
        Need(pe::Ready(), "ProcessEvent 尚未就绪");
        Library(valid, "KismetSystemLibrary", "IsValid", {{"Object", "ObjectProperty", 8}, {"ReturnValue", "BoolProperty", 1, true}});
        U* actor = ue::FindClass("Actor");
        destroy.Bind(actor ? Cdo(actor) : nullptr, actor, "K2_DestroyActor", {});
        pawnClass = ue::FindClass("Pawn");
        controllerOffset = pawnClass ? ue::PropOffset(pawnClass, "Controller") : -1;
    }
    void Bind() {
        BindCleanup();
        StructLayout("Vector", 24, {{"X", 0}, {"Y", 8}, {"Z", 16}});
        StructLayout("Transform", 96, {{"Rotation", 0}, {"Translation", 32}, {"Scale3D", 64}});
        StructLayout("SoftClassPath", 32, {{"AssetPath", 0}, {"SubPathString", 16}});
        StructLayout("TopLevelAssetPath", 16, {{"PackageName", 0}, {"AssetName", 8}});
        StructLayout("HitResult", 256, {{"ImpactPoint", 40}, {"ImpactNormal", 88}});
        Library(standalone, "KismetSystemLibrary", "IsStandalone", {{"WorldContextObject", "ObjectProperty", 8}, {"ReturnValue", "BoolProperty", 1, true}});
        Library(paused, "GameplayStatics", "IsGamePaused", {{"WorldContextObject", "ObjectProperty", 8}, {"ReturnValue", "BoolProperty", 1, true}});
        Library(playerPawn, "GameplayStatics", "GetPlayerPawn", {{"WorldContextObject", "ObjectProperty", 8}, {"PlayerIndex", "IntProperty", 4}, {"ReturnValue", "ObjectProperty", 8, true}});
        Library(name, "KismetStringLibrary", "Conv_StringToName", {{"InString", "StrProperty", 16}, {"ReturnValue", "NameProperty", 8, true}});
        Library(soft, "KismetSystemLibrary", "Conv_SoftClassPathToSoftClassRef", {{"SoftClassPath", "StructProperty", 32, false, "SoftClassPath"}, {"ReturnValue", "SoftObjectProperty", 40, true}});
        Library(load, "KismetSystemLibrary", "LoadClassAsset_Blocking", {{"AssetClass", "SoftObjectProperty", 40}, {"ReturnValue", "ObjectProperty", 8, true}});
        Library(objectType, "PBZBlueprintFunctionLibrary", "ConvertToObjectType", {{"CollisionChannel", "EnumProperty", 1, false, "ECollisionChannel"}, {"ReturnValue", "EnumProperty", 1, true, "EObjectTypeQuery"}});
        // 只转换物体通道：ECC_Visibility(3) / ECC_Camera(4) 是射线通道，ConvertToObjectType 对它们返回无效值（实机 32）。
        // 物体通道：WorldStatic 0、WorldDynamic 1、Pawn 2、PhysicsBody 5、Vehicle 6、Destructible 7；[0] 仍是 WorldStatic（地面射线用）
        constexpr std::array<uint8_t, 6> kObjectChannels{0, 1, 2, 5, 6, 7};
        for (size_t i = 0; i < collisionTypes.size(); ++i) {
            Buffer b; objectType.Set(b, 0, kObjectChannels[i]); objectType.Call(b); collisionTypes[i] = objectType.Get<uint8_t>(b, 1);
            Need(collisionTypes[i] < 32, "碰撞通道映射失败");
        }
        Need(std::set<uint8_t>(collisionTypes.begin(), collisionTypes.end()).size() == collisionTypes.size(), "碰撞通道映射重复");
        Library(line, "KismetSystemLibrary", "LineTraceSingleForObjects", {
            {"WorldContextObject", "ObjectProperty", 8}, {"Start", "StructProperty", 24, false, "Vector"}, {"End", "StructProperty", 24, false, "Vector"},
            {"ObjectTypes", "ArrayProperty", 16}, {"bTraceComplex", "BoolProperty", 1}, {"ActorsToIgnore", "ArrayProperty", 16},
            {"DrawDebugType", "EnumProperty", 1, false, "EDrawDebugTrace"}, {"OutHit", "StructProperty", 256, false, "HitResult"}, {"bIgnoreSelf", "BoolProperty", 1},
            {"TraceColor", "StructProperty", 16, false, "LinearColor"}, {"TraceHitColor", "StructProperty", 16, false, "LinearColor"}, {"DrawTime", "FloatProperty", 4},
            {"ReturnValue", "BoolProperty", 1, true}});
        Library(room, "KismetSystemLibrary", "CapsuleTraceSingleForObjects", {
            {"WorldContextObject", "ObjectProperty", 8}, {"Start", "StructProperty", 24, false, "Vector"}, {"End", "StructProperty", 24, false, "Vector"},
            {"Radius", "FloatProperty", 4}, {"HalfHeight", "FloatProperty", 4}, {"ObjectTypes", "ArrayProperty", 16},
            {"bTraceComplex", "BoolProperty", 1}, {"ActorsToIgnore", "ArrayProperty", 16}, {"DrawDebugType", "EnumProperty", 1, false, "EDrawDebugTrace"},
            {"OutHit", "StructProperty", 256, false, "HitResult"}, {"bIgnoreSelf", "BoolProperty", 1}, {"TraceColor", "StructProperty", 16, false, "LinearColor"},
            {"TraceHitColor", "StructProperty", 16, false, "LinearColor"}, {"DrawTime", "FloatProperty", 4}, {"ReturnValue", "BoolProperty", 1, true}});
        Library(begin, "GameplayStatics", "BeginDeferredActorSpawnFromClass", {
            {"WorldContextObject", "ObjectProperty", 8}, {"ActorClass", "ObjectProperty", 8}, {"SpawnTransform", "StructProperty", 96, false, "Transform"},
            {"CollisionHandlingOverride", "EnumProperty", 1, false, "ESpawnActorCollisionHandlingMethod"}, {"Owner", "ObjectProperty", 8},
            {"TransformScaleMethod", "EnumProperty", 1, false, "ESpawnActorScaleMethod"}, {"ReturnValue", "ObjectProperty", 8, true}});
        Library(finish, "GameplayStatics", "FinishSpawningActor", {{"Actor", "ObjectProperty", 8},
            {"SpawnTransform", "StructProperty", 96, false, "Transform"}, {"TransformScaleMethod", "EnumProperty", 1, false, "ESpawnActorScaleMethod"}, {"ReturnValue", "ObjectProperty", 8, true}});
        Library(activate, "BP_SGameAIFunctionLibrary_C", "ActivateAI", {{"Pawn", "ObjectProperty", 8}, {"__WorldContext", "ObjectProperty", 8}});
        npcClass = ue::FindClass("PBZCharacter_NPC"); pawnClass = ue::FindClass("Pawn");
        capsuleClass = ue::FindClass("CapsuleComponent");
        U* character = ue::FindClass("Character"); U* actor = ue::FindClass("Actor");
        Need(npcClass && pawnClass && character && actor && capsuleClass, "NPC/胶囊体基础类缺失");
        capsuleOffset = ue::PropOffset(character, "CapsuleComponent");
        controllerOffset = ue::PropOffset(pawnClass, "Controller");
        Need(capsuleOffset >= 0 && controllerOffset >= 0, "胶囊体/AI 控制器字段缺失");
        loc.Bind(Cdo(actor), actor, "K2_GetActorLocation", {{"ReturnValue", "StructProperty", 24, true, "Vector"}});
        direction.Bind(Cdo(actor), actor, "GetActorForwardVector", {{"ReturnValue", "StructProperty", 24, true, "Vector"}});
        capsuleSize.Bind(Cdo(capsuleClass), capsuleClass, "GetScaledCapsuleSize", {{"OutRadius", "FloatProperty", 4}, {"OutHalfHeight", "FloatProperty", 4}});
        destroy.Bind(Cdo(actor), actor, "K2_DestroyActor", {});
        controllerSpawn.Bind(Cdo(pawnClass), pawnClass, "SpawnDefaultController", {});
        U* hit = ue::FindClass("HitResult");
        hitImpact = ue::PropOffset(hit, "ImpactPoint"); hitNormal = ue::PropOffset(hit, "ImpactNormal");
        BoolLayout(hit, "bBlockingHit", hitBlock, blockMask);
        BoolLayout(hit, "bStartPenetrating", hitPenetrate, penetrateMask);
        try {
            Library(nav, "NavigationSystemV1", "K2_ProjectPointToNavigation", {
                {"WorldContextObject", "ObjectProperty", 8}, {"Point", "StructProperty", 24, false, "Vector"}, {"ProjectedLocation", "StructProperty", 24, false, "Vector"},
                {"NavData", "ObjectProperty", 8}, {"FilterClass", "ObjectProperty", 8}, {"QueryExtent", "StructProperty", 24, false, "Vector"},
                {"ReturnValue", "BoolProperty", 1, true}});
            hasNav = true;
        } catch (const Failure&) { hasNav = false; }
    }
    static void BoolLayout(U* type, const char* name, int& offset, uint8_t& mask) {
        U* f = ue::Get<U*>(type + ue::Layout::StructChildProps);
        for (int n = 0; f && n < 128; ++n, f = ue::Get<U*>(f + ue::Layout::FieldNext)) {
            if (ue::Name(ue::Get<uint32_t>(f + ue::Layout::FieldName)) != name) continue;
            offset = ue::Get<int>(f + ue::Layout::PropOffset, -1) + ue::Get<uint8_t>(f + 0x71);
            mask = ue::Get<uint8_t>(f + 0x72);
            Need(offset >= 0 && offset < 256 && mask && !(mask & (mask - 1)), "HitResult BoolProperty 位布局不匹配");
            return;
        }
        throw Failure(std::string("HitResult 缺失布尔字段：") + name);
    }
    bool IsValid(U* object) {
        if (!object || !Identity(object).Alive()) return false;
        Buffer b; valid.Set(b, 0, object); valid.Call(b); return valid.Get<uint8_t>(b, 1) != 0;
    }
    U* CurrentWorld() {
        U* viewportClass = ue::FindClass("GameViewportClient");
        const int offset = viewportClass ? ue::PropOffset(viewportClass, "World") : -1;
        Need(offset >= 0, "当前视口 World 尚未就绪");
        // 视口对象整局不变：找到后记住，不用每次整表扫（换波卡顿）
        static Identity viewport;
        if (viewport.Alive() && ue::IsA(viewport.object, viewportClass)) {
            U* w = ue::Get<U*>(viewport.object + offset);
            Need(w && ue::IsA(w, ue::FindClass("World")), "当前游戏世界尚未就绪");
            return w;
        }
        std::set<U*> worlds; U* found = nullptr;
        ue::ForEach([&](int, U* o) {
            if (ue::IsA(o, viewportClass) && !ue::IsDefault(o)) {
                found = o;
                U* w = ue::Get<U*>(o + offset); if (w && ue::IsA(w, ue::FindClass("World"))) worlds.insert(w);
            }
            return true;
        });
        Need(worlds.size() == 1, "当前游戏世界不唯一或尚未就绪");
        viewport = Identity(found);
        return *worlds.begin();
    }
    void Context() {
        world = Identity(CurrentWorld()); Need(IsValid(world.object), "World 已失效");
        Buffer b; standalone.Set(b, 0, world.object); standalone.Call(b);
        Need(standalone.Get<uint8_t>(b, 1) != 0, "仅支持本地单人 Standalone");
        Buffer pause; paused.Set(pause, 0, world.object); paused.Call(pause);
        Need(paused.Get<uint8_t>(pause, 1) == 0, "游戏暂停中，请返回可操作场景");
        Buffer p; playerPawn.Set(p, 0, world.object); playerPawn.Set(p, 1, int32_t(0)); playerPawn.Call(p);
        player = Identity(playerPawn.Get<U*>(p, 2));
        Need(IsValid(player.object) && WorldOf(player.object) == world.object && ue::IsA(player.object, ue::FindClass("PBZCharacter_Player")), "没有当前世界的本地玩家");
        location = Location(player.object); Buffer f; direction.Call(f, player.object); forward = direction.Get<Vec>(f, 0);
        playerSize = Size(player.object);
        Need(Finite(location) && Finite(forward) && Valid(playerSize), "玩家位置/胶囊体无效");
    }
    void CheckContext() {
        Need(world.Alive() && player.Alive() && IsValid(world.object) && IsValid(player.object) &&
             WorldOf(player.object) == world.object, "切图/读档/玩家变化，取消生成");
        Need(CurrentWorld() == world.object, "活动视口已切换到其他世界，取消生成");
        Buffer pause; paused.Set(pause, 0, world.object); paused.Call(pause);
        Need(paused.Get<uint8_t>(pause, 1) == 0, "游戏已暂停，取消生成");
        Buffer p; playerPawn.Set(p, 0, world.object); playerPawn.Set(p, 1, int32_t(0)); playerPawn.Call(p);
        Need(playerPawn.Get<U*>(p, 2) == player.object, "玩家角色已变化，取消生成");
        Need(Distance2D(Location(player.object), location) < 100, "玩家已离开选点区域，取消生成");
    }
    Vec Location(U* actor) { Buffer b; loc.Call(b, actor); return loc.Get<Vec>(b, 0); }
    Capsule Size(U* actor) {
        U* capsule = ue::Get<U*>(actor + capsuleOffset);
        Need(capsule && ue::IsA(capsule, capsuleClass), "角色胶囊体缺失");
        Buffer b; capsuleSize.Call(b, capsule);
        Capsule result{capsuleSize.Get<float>(b, 0), capsuleSize.Get<float>(b, 1)};
        Need(Valid(result), "角色胶囊体尺寸超出有效范围"); return result;
    }
    std::vector<Landing> Reservations() {
        std::vector<Landing> out;
        for (const auto& own : g_owned) {
            if (own.world.object != world.object || !ActorAlive(own)) continue;
            const auto center = Location(own.actor.object);
            Need(Finite(center), "已生成角色的位置无效，请稍后重试");
            // Reserve current poses, not spawn-time positions that actors have
            // since left. Controller-only remnants do not occupy landing space.
            out.push_back({center, Size(own.actor.object), own.landing.navProjected});
        }
        return out;
    }
    U* LoadClass(const std::string& path) {
        const auto dot = path.rfind('.'); Need(dot != std::string::npos, "完整类路径无效");
        auto toName = [&](const std::string& text) {
            // Catalog paths are ASCII. Reject rather than corrupt a future UTF-8 path.
            Need(std::all_of(text.begin(), text.end(), [](unsigned char c) { return c < 128; }), "类路径编码尚不支持");
            std::wstring wide(text.begin(), text.end()); wide.push_back(L'\0');
            Array str{wide.data(), static_cast<int32_t>(wide.size()), static_cast<int32_t>(wide.size())};
            Buffer b; name.Set(b, 0, str); name.Call(b); return name.Get<uint64_t>(b, 1);
        };
        std::array<uint8_t, 32> softPath{};
        const uint64_t package = toName(path.substr(0, dot)), asset = toName(path.substr(dot + 1));
        std::memcpy(softPath.data(), &package, 8); std::memcpy(softPath.data() + 8, &asset, 8);
        Buffer ref; soft.Set(ref, 0, softPath); soft.Call(ref);
        const auto value = soft.Get<std::array<uint8_t, 40>>(ref, 1);
        Buffer b; load.Set(b, 0, value); load.Call(b); U* cls = load.Get<U*>(b, 1);
        Need(cls && Identity(cls).Alive() && ue::ClassName(cls).ends_with("Class") && ObjectPath(cls) == path, "指定完整路径加载失败或返回了不同类");
        return cls;
    }
    U* Load(const std::string& path) {
        U* cls = LoadClass(path);
        bool npc = false;
        U* p = cls;
        for (int n = 0; p && n < 64; ++n, p = ue::Get<U*>(p + ue::Layout::StructSuper)) if (p == npcClass) { npc = true; break; }
        Need(npc, "指定蓝图不是 PBZCharacter_NPC，拒绝生成"); return cls;
    }
    bool Project(Vec candidate, Vec& out) override {
        if (!hasNav) return false;
        Buffer b; nav.Set(b, 0, world.object); nav.Set(b, 1, candidate); nav.Set(b, 5, Vec{120, 120, 250}); nav.Call(b);
        out = nav.Get<Vec>(b, 2); return nav.Get<uint8_t>(b, 6) != 0;
    }
    bool Floor(Vec candidate, Ground& out) override {
        Buffer b; line.Set(b, 0, world.object);
        line.Set(b, 1, Vec{candidate.x, candidate.y, location.z + 250});
        line.Set(b, 2, Vec{candidate.x, candidate.y, location.z - playerSize.halfHeight - 250});
        // Only WorldStatic floor. Dynamic pawns/props are not landing surfaces.
        Array types{collisionTypes.data(), 1, 1}; line.Set(b, 3, types);
        U* ignore = player.object; Array ignores{&ignore, 1, 1}; line.Set(b, 5, ignores);
        line.Set(b, 8, uint8_t(1)); line.Call(b);
        const auto hit = line.Get<std::array<uint8_t, 256>>(b, 7);
        std::memcpy(&out.point, hit.data() + hitImpact, sizeof(Vec)); std::memcpy(&out.normal, hit.data() + hitNormal, sizeof(Vec));
        out.blocking = (hit[hitBlock] & blockMask) != 0; out.penetrating = (hit[hitPenetrate] & penetrateMask) != 0;
        return line.Get<uint8_t>(b, 12) != 0;
    }
    bool RoomIgnoring(Vec center, Capsule capsule, U* self) {
        // 胶囊底部离地只有几厘米，地面稍有起伏 / 坡度就会碰到地面被判"没有空间"（实测 34/36 个候选点因此被拒）。
        // 只检查离地一段之上到头顶的空间：底部抬高 lift，顶部不变，仍能挡住墙、柱子和其他角色
        const double lift = (std::min)(capsule.halfHeight * 0.5, capsule.radius * 0.6 + 15);
        center.z += lift / 2;
        capsule.halfHeight -= lift / 2;
        if (capsule.radius > capsule.halfHeight) capsule.radius = capsule.halfHeight;
        Buffer b; room.Set(b, 0, world.object); room.Set(b, 1, center); room.Set(b, 2, Vec{center.x, center.y, center.z + .1});
        room.Set(b, 3, static_cast<float>(capsule.radius)); room.Set(b, 4, static_cast<float>(capsule.halfHeight));
        // 只查静态地形（WorldStatic）和角色（Pawn）：按物体类型的扫掠也会命中只做重叠检测的触发体积
        // （事件区域等，多为 WorldDynamic），它们覆盖大片区域，实测导致几乎所有候选点都被判"没有空间"
        std::array<uint8_t, 2> roomTypes{collisionTypes[0], collisionTypes[2]};
        Array types{roomTypes.data(), 2, 2};
        room.Set(b, 5, types);
        U* ignoresData[]{self}; Array ignores{self ? ignoresData : nullptr, self ? 1 : 0, self ? 1 : 0}; room.Set(b, 7, ignores);
        room.Set(b, 10, uint8_t(1)); room.Call(b);
        return room.Get<uint8_t>(b, 14) == 0;
    }
    bool Room(Vec center, Capsule capsule) override { return RoomIgnoring(center, capsule, nullptr); }
    bool ActorAlive(const Owned& entry) {
        return entry.actor.Alive() && entry.world.Alive() && IsValid(entry.actor.object) && WorldOf(entry.actor.object) == entry.world.object;
    }
    bool ControllerAlive(const Owned& entry) {
        if (!entry.controller.Alive() || !entry.world.Alive() || !IsValid(entry.controller.object) || WorldOf(entry.controller.object) != entry.world.object) return false;
        const int off = ue::PropOffset(ue::ClassOf(entry.controller.object), "Pawn");
        U* pawn = off >= 0 ? ue::Get<U*>(entry.controller.object + off) : reinterpret_cast<U*>(1);
        return pawn == entry.actor.object || pawn == nullptr;
    }
    bool Alive(const Owned& entry) { return ActorAlive(entry) || ControllerAlive(entry); }
    void RecordController(Owned& entry) {
        if (!ActorAlive(entry) || controllerOffset < 0) return;
        U* controller = ue::Get<U*>(entry.actor.object + controllerOffset);
        U* aiClass = ue::FindClass("AIController");
        const int owner = controller ? ue::PropOffset(ue::ClassOf(controller), "Owner") : -1;
        const int pawn = controller ? ue::PropOffset(ue::ClassOf(controller), "Pawn") : -1;
        // Native SpawnDefaultController sets Owner to its pawn. A controller
        // borrowed from the level must never enter this cleanup journal.
        if (controller && aiClass && ue::IsA(controller, aiClass) && owner >= 0 && pawn >= 0 &&
            ue::Get<U*>(controller + owner) == entry.actor.object && ue::Get<U*>(controller + pawn) == entry.actor.object)
            entry.controller = Identity(controller);
    }
    void Destroy(Owned& entry) {
        RecordController(entry);
        // 3.6.0 诊断（05:19 修改器断开清怪时一次偶发闪退，未能复现）：删除前记下对象与阶段，崩溃报告的 where / 最近事件会直接写明崩在删谁
        diag::Scope scope("销毁生成物");
        if (ActorAlive(entry)) { diag::Event("enemy.destroy", entry.actor.object); log::Info("销毁生成物：{}（{}）", ue::ObjName(entry.actor.object), entry.tag); Buffer b; destroy.Call(b, entry.actor.object); }
        if (ControllerAlive(entry)) { Buffer b; destroy.Call(b, entry.controller.object); }
    }
    size_t Clear() {
        for (auto& entry : g_owned) Destroy(entry);
        std::erase_if(g_owned, [&](const Owned& e) { return !Alive(e); });
        return g_owned.size();
    }
    // 阵营：游戏自带阵营关系表（PBZ_FactionCalculator）—— Player 与 Enemy/Enemy2/Enemy3/Chaotic 敌对，
    // Enemy/Enemy2/Enemy3 两两敌对，Chaotic 与所有人敌对。生成后调游戏自己的 FactionComponent.SetFaction 改阵营，AI 按关系表自己找目标
    std::string factionTag;
    std::string requestTag;  // 本批次的 request_id，记进 Owned.tag，供 ClearTagged 按前缀清理
    void ApplyFaction(U* actor) {
        if (factionTag.empty() || factionTag == "Faction.Enemy") return;
        U* comp = nullptr;
        if (auto* get = pe::FindFunction(ue::ClassOf(actor), "GetFactionComponent")) {
            Buffer g; pe::Call(actor, get, g.data.data()); comp = g.Get<U*>(0);
        }
        auto* compCls = ue::FindClass("FactionComponent");
        if (!comp && compCls)  // 接口没导出时：在角色的子对象里找（只在生成时做一次）
            ue::ForEach([&](int, U* o) {
                if (ue::Get<U*>(o + ue::Layout::ObjOuter) == actor && ue::IsA(o, compCls)) { comp = o; return false; }
                return true;
            });
        Need(comp && compCls && ue::IsA(comp, compCls), "找不到阵营组件，未改阵营");
        auto* set = pe::FindFunction(compCls, "SetFaction");
        Need(set != nullptr, "阵营接口缺失");
        const uint64_t tag = inv::ToFName(factionTag);
        Need(tag != 0, "阵营标签无效");
        Buffer b; b.Put(0, tag); pe::Call(comp, set, b.data.data());
        log::Info("怪物阵营：{} → {}", ue::ObjName(actor), factionTag);
    }
    void Spawn(U* cls, const std::string& path, Landing landing) {
        CheckContext();
        Need(Room(landing.center, landing.capsule), "生成前落点被占用");
        Transform t; t.translation = landing.center;
        const double yaw = std::atan2(location.y - landing.center.y, location.x - landing.center.x);
        t.rotation[2] = std::sin(yaw * .5); t.rotation[3] = std::cos(yaw * .5);
        Buffer b; begin.Set(b, 0, world.object); begin.Set(b, 1, cls); begin.Set(b, 2, t);
        // AdjustIfPossibleButDontSpawnIfColliding：落点胶囊与地面略有接触时允许引擎微调位置（实测 DontSpawnIfColliding 在起伏地面频繁拒绝），
        // 仍不强行穿墙；后面会再校验实际位置偏移 < 1 米且不与场景重叠
        begin.Set(b, 3, uint8_t(3));
        begin.Set(b, 5, uint8_t(1)); // MultiplyWithRoot: preserve the class's authored root scale
        begin.Call(b); U* actor = begin.Get<U*>(b, 6);
        Need(actor && Identity(actor).Alive(), "原生生成拒绝：类依赖或碰撞不允许");
        g_owned.push_back({Identity(actor), {}, world, path, landing, {}, requestTag}); // journal before Finish/BeginPlay
        Owned& own = g_owned.back();
        try {
            Need(ue::ClassOf(actor) == cls && WorldOf(actor) == world.object && !ue::IsDefault(actor), "生成对象身份不匹配");
            Buffer f; finish.Set(f, 0, actor); finish.Set(f, 1, t); finish.Set(f, 2, uint8_t(1)); finish.Call(f);
            RecordController(own);
            Need(finish.Get<U*>(f, 3) == actor && ActorAlive(own), "原生初始化未返回同一对象");
            if (!ue::Get<U*>(actor + controllerOffset)) { Buffer c; controllerSpawn.Call(c, actor); }
            U* controller = ue::Get<U*>(actor + controllerOffset);
            RecordController(own);
            Need(controller && ue::IsA(controller, ue::FindClass("AIController")), "生成角色没有 AIController");
            Buffer a; activate.Set(a, 0, actor); activate.Set(a, 1, world.object); activate.Call(a);
            Need(ActorAlive(own), "AI 初始化后对象失效");
            const Vec actual = Location(actor); const Capsule size = Size(actor);
            Need(Finite(actual) && Distance2D(actual, landing.center) < 100 && std::abs(actual.z - landing.center.z) < 100,
                 "初始化后位置发生异常偏移");
            Need(RoomIgnoring(actual, size, actor), "初始化后实际胶囊体与场景重叠");
            own.landing = {actual, size, landing.navProjected};
            ApplyFaction(actor);
            own.faction = factionTag;
            if (!requestTag.starts_with("tower-") && ue::IsA(actor, ue::FindClass("BP_RenewalNPC_C"))) {
                const Identity group = NativeLevelGroup(path);
                const auto expected = InitializeNativeStats(own.actor, group);
                const Identity stable = own.actor;
                const auto epoch = g_epoch.load(); const auto due = GetTickCount64() + 3500;
                gamethread::Post([=] { RecheckNativeStats(stable, group, epoch, due, expected); });
            }
            AdaptStandalone(own);
        } catch (...) {
            Destroy(own);
            if (!Alive(own)) g_owned.pop_back();
            throw;
        }
    }
};

bool NativeDead(const Owned& own) {
    if (!own.actor.Alive()) return false;
    auto* actor = own.actor.object;
    auto* basic = ue::FindClass("BasicAttributeSet");
    const int so = ue::PropOffset(own.actor.cls, "AttributeSetBase"), ho = basic ? ue::PropOffset(basic, "Health") : -1;
    auto* set = so >= 0 ? ue::Get<U*>(actor + so) : nullptr;
    const float health = set && ho >= 0 ? ue::Get<float>(set + ho + 12, -1.f) : -1.f;
    if (!std::isfinite(health) || health < 0 || health > 0.01f) return false;
    const int off = ue::PropOffset(own.actor.cls, "Death Component");
    auto* death = off >= 0 ? ue::Get<U*>(actor + off) : nullptr;
    auto* cls = ue::FindClass("BP_DeathComponent_C");
    if (!death || !cls || !ue::IsA(death, cls) || ue::Get<U*>(death + ue::Layout::ObjOuter) != actor) return false;
    const int fake = ue::PropOffset(cls, "Fake Death Enabled");
    if (fake < 0 || ue::Get<uint8_t>(death + fake, 1)) return false; // Multiphase/respawn-owned bodies stay native.
    Fn dead; dead.Bind(death, cls, "Is Dead", {{"ReturnValue", "BoolProperty", 1, true}});
    Buffer b; dead.Call(b); return dead.Get<uint8_t>(b, 0) != 0;
}

void QueueMissingNativeDeath(Owned& own, ULONGLONG now) {
    // Reproduce BP_DeathComponent's real Health==0 path (AddPendingState),
    // never force a transition or destroy a still-living/protected boss.
    // Is Ready To Dead is InExactState OR InPendingState, not CanActivate.
    const auto reset = [&] { own.zeroSince = 0; own.deathQueued = false; };
    if (!own.actor.Alive() || own.tag.starts_with("tower-")) { reset(); return; }
    auto* actor = own.actor.object; auto* basic = ue::FindClass("BasicAttributeSet");
    const int so = ue::PropOffset(own.actor.cls, "AttributeSetBase");
    auto* set = so >= 0 ? ue::Get<U*>(actor + so) : nullptr;
    const int ho = basic ? ue::PropOffset(basic, "Health") : -1;
    const int mo = basic ? ue::PropOffset(basic, "Health_Min") : -1;
    const float invalid = std::numeric_limits<float>::quiet_NaN();
    const float health = set && ho >= 0 ? ue::Get<float>(set + ho + 12, invalid) : invalid;
    const float floor = set && mo >= 0 ? ue::Get<float>(set + mo + 12, invalid) : invalid;
    if (!std::isfinite(health) || health != 0 || !std::isfinite(floor) || floor > 0) { reset(); return; }
    const int off = ue::PropOffset(own.actor.cls, "Death Component");
    auto* death = off >= 0 ? ue::Get<U*>(actor + off) : nullptr;
    auto* dc = ue::FindClass("BP_DeathComponent_C");
    if (!death || !dc || !ue::IsA(death, dc) || !Identity(death).Alive() || ue::Get<U*>(death + ue::Layout::ObjOuter) != actor) { reset(); return; }
    const int fo = ue::PropOffset(dc, "Fake Death Enabled"), st = ue::PropOffset(dc, "Actor State Component"), dt = ue::PropOffset(dc, "Dead State Tag");
    if (fo < 0 || st < 0 || dt < 0 || ue::Get<uint8_t>(death + fo, 1)) { reset(); return; }
    auto* state = ue::Get<U*>(death + st); auto* sc = ue::FindClass("ActorStateComponent");
    const uint64_t tag = ue::Get<uint64_t>(death + dt);
    if (!state || !sc || !ue::IsA(state, sc) || !Identity(state).Alive() || ue::Get<U*>(state + ue::Layout::ObjOuter) != actor || !uint32_t(tag)) { reset(); return; }
    if (!own.zeroSince) { own.zeroSince = now; return; }
    if (own.deathQueued || now - own.zeroSince < 1000) return;
    Fn ready; ready.Bind(death, dc, "Is Ready To Dead", {{"ReturnValue", "BoolProperty", 1, true}});
    Buffer check; ready.Call(check);
    if (ready.Get<uint8_t>(check, 0)) return; // Native death/phase transition is already pending.
    Fn pending; pending.Bind(state, sc, "AddPendingState", {{"NewStateTag", "StructProperty", 8, false, "GameplayTag"}});
    Buffer b; pending.Set(b, 0, tag); pending.Call(b);
    own.deathQueued = true;
    log::Info("生成物真实零血未登记死亡：{} → 原生 AddPendingState（保留原生过渡条件）", ue::ObjName(actor));
}

void MaintainOwnedAt(ULONGLONG now) {
    Engine engine; engine.BindCleanup();
    for (auto& own : g_owned) {
        if (own.tag.starts_with("tower-")) continue;
        if (!engine.ActorAlive(own)) { if (engine.ControllerAlive(own)) engine.Destroy(own); continue; }
        if (now >= own.compatNext) {
            own.compatNext = now + 1000;
            try { AdaptStandalone(own); }
            catch (const std::exception& e) { log::Info("独立召唤适配待重试：{}", e.what()); }
        }
        if (!NativeDead(own)) { own.deadSince = 0; QueueMissingNativeDeath(own, now); continue; }
        if (!own.deadSince) { own.deadSince = now; continue; }
        const int off = ue::PropOffset(own.actor.cls, "Death Delay");
        const float delay = off >= 0 ? ue::Get<float>(own.actor.object + off) : 0;
        const auto grace = ULONGLONG(1000 * std::clamp(std::isfinite(delay) ? double(delay) + 2 : 12., 12., 180.));
        if (now - own.deadSince < grace) continue;
        log::Info("原生死亡后回收自有生成物：{}（保留死亡演出 {}ms）", ue::ObjName(own.actor.object), grace);
        engine.Destroy(own); // Actor and its proven-owned AIController; retain journal on delayed failure.
    }
    std::erase_if(g_owned, [&](const Owned& own) { return !engine.Alive(own); });
}

void MaintainOwned(uint64_t epoch, ULONGLONG due) {
    if (g_stopping || epoch != g_epoch) { g_maintenance = false; return; }
    const auto now = GetTickCount64();
    if (now >= due) {
        try { MaintainOwnedAt(now); } catch (const std::exception& e) { log::Info("生成物维护：{}", e.what()); }
        due = now + 500;
    }
    const bool any = std::any_of(g_owned.begin(), g_owned.end(), [](const Owned& own) { return !own.tag.starts_with("tower-"); });
    if (!any) { g_maintenance = false; return; }
    gamethread::Post([=] { MaintainOwned(epoch, due); });
}

void StartOwnedMaintenance() {
    if (g_maintenance || g_stopping) return;
    g_maintenance = true; const auto epoch = g_epoch.load(); const auto due = GetTickCount64() + 500;
    gamethread::Post([=] { MaintainOwned(epoch, due); });
}

std::string Execute(const Request& request) {
    const auto timingStart = GetTickCount64();
    const DWORD thread = GetCurrentThreadId();
    DWORD previous = 0; g_thread.compare_exchange_strong(previous, thread);
    Need(g_thread.load() == thread, "生成接口不在已绑定的游戏线程");
    Engine engine;
    if (request.command == "clear") engine.BindCleanup(); else engine.Bind();
    const auto timingBind = GetTickCount64();
    std::erase_if(g_owned, [&](const Owned& e) { return !engine.Alive(e); });
    if (request.command == "clear") {  // 修改器的清怪：影塔的 Boss（request_id 以 "tower-" 开头）不动
        size_t before = 0;
        for (const auto& e : g_owned) before += e.tag.rfind("tower-", 0) != 0;
        const size_t left = ClearExcept("tower-");
        return Response(request, left == 0, left ? "部分对象暂未销毁，请保留核心并重试清理" : "已清理本工具生成的对象",
                        std::format(",\"cleared\":{},\"remaining\":{}", before - left, left));
    }
    if (request.command == "status")
        return Response(request, true, "反射接口已解析；生成效果仍需实机验收",
                        std::format(",\"owned\":{},\"catalog_sha256\":\"{}\",\"nav_available\":{},\"actors\":{}", g_owned.size(), kCatalogSha256, engine.hasNav ? "true" : "false", OwnedStatsJson()));
    Need(request.command == "spawn", "未知怪物命令");
    ClaimSpawnRequest(request); // failed/partial requests cannot replay
    const auto entry = std::find_if(std::begin(kEnemyCatalog), std::end(kEnemyCatalog), [&](const auto& e) { return e.path == request.path; });
    Need(entry != std::end(kEnemyCatalog), "路径不在已审核怪物目录中，拒绝任意类生成");
    Need(std::isfinite(request.distanceCm) && request.distanceCm >= 200 && request.distanceCm <= 2000, "生成距离须为200–2000厘米");
    static const std::pair<const char*, const char*> kFactions[] = {
        {"", "Faction.Enemy"}, {"enemy", "Faction.Enemy"}, {"ally", "Faction.Player"}, {"chaos", "Faction.Chaotic"}, {"enemy2", "Faction.Enemy2"}, {"enemy3", "Faction.Enemy3"}};
    // Private tower-to-core convention carried in the existing request field:
    // it keeps the shared module ABI intact while constraining wave spawns.
    bool towerRegion = request.faction.starts_with("tower-region ");
    Vec regionCenter{}; double regionRadius = 0;
    if (towerRegion) {
        Need(request.requestId.starts_with("tower-") && request.requestId.find("-mob") != std::string::npos, "仅影塔波次能指定场地区域");
        std::istringstream spec(request.faction.substr(13));
        std::string extra;
        Need(bool(spec >> regionCenter.x >> regionCenter.y >> regionRadius) && !(spec >> extra) &&
             std::isfinite(regionCenter.x) && std::isfinite(regionCenter.y) && std::isfinite(regionRadius) &&
             regionRadius >= 300 && regionRadius <= 2000, "影塔场地区域无效");
    }
    const auto fac = std::find_if(std::begin(kFactions), std::end(kFactions), [&](const auto& f) { return (towerRegion ? std::string_view("enemy") : std::string_view(request.faction)) == f.first; });
    Need(fac != std::end(kFactions), "未知阵营");
    engine.factionTag = fac->second;
    engine.requestTag = request.requestId;
    const std::string actualPath(StandalonePath(request.path, request.requestId));
    Need(std::any_of(std::begin(kEnemyCatalog), std::end(kEnemyCatalog), [&](const auto& e) { return e.path == actualPath; }), "后续阶段不在已审核目录中");
    engine.Context(); U* cls = engine.Load(actualPath); engine.CheckContext();
    const auto timingContext = GetTickCount64();
    if (actualPath == kLengTu && !request.requestId.starts_with("tower-")) {
        BindColdStoryIsolation(engine.LoadClass(actualPath));
        for (const auto effect : kLengTuBodyEffects) engine.LoadClass(std::string(effect));
        for (const auto ability : kLengTuArenaAbilities) BindArenaAbility(engine.LoadClass(std::string(ability)));
    }
    U* cdo = Cdo(cls); Need(cdo != nullptr, "角色默认对象未加载");
    const Capsule size = engine.Size(cdo);
    auto reservations = engine.Reservations();
    Vec searchOrigin = engine.location;
    if (towerRegion) {
        regionCenter.z = engine.location.z;
        Need(Distance2D(regionCenter, engine.location) <= 10000, "影塔场地中心离玩家过远，拒绝刷怪");
        searchOrigin = regionCenter;
        Capsule clearance = engine.playerSize; clearance.radius += 150;
        reservations.push_back({engine.location, clearance, false});
    }
    RegionQuery bounded(engine, regionCenter, regionRadius);
    SceneQuery& query = towerRegion ? static_cast<SceneQuery&>(bounded) : static_cast<SceneQuery&>(engine);
    const auto points = FindLandings(query, searchOrigin, engine.forward, engine.playerSize, size,
                                    request.count, request.distanceCm, reservations);
    const auto timingLanding = GetTickCount64();
    if (static_cast<int>(points.size()) != request.count) {
        const auto& r = LastLandingReject();
        throw Failure(std::format("角色附近没有足够可落地且不重叠的位置，未生成（拒绝：导航{} 太近{} 无地面{} 太陡{} 偏移{} 高差{} 边缘{} 重叠{} 空间{}；玩家 z={:.0f} 半高={:.0f}）",
                                  r.nav, r.tooClose, r.floor, r.steep, r.offset, r.height, r.support, r.overlap, r.room, engine.location.z, engine.playerSize.halfHeight));
    }
#ifndef PBZ_MOD_ONLY
    if (towerRegion) for (const auto& point : points)
        log::Info("影塔波次落点：场地中心 {:.0f}/{:.0f} 安全半径 {:.0f}；玩家 {:.0f}/{:.0f}；怪物 {:.0f}/{:.0f}/{:.0f}",
                  regionCenter.x, regionCenter.y, regionRadius, engine.location.x, engine.location.y,
                  point.center.x, point.center.y, point.center.z);
#endif
    const size_t before = g_owned.size();
    try {
        for (const auto& point : points) engine.Spawn(cls, actualPath, point);
    } catch (const std::exception& e) {
        // Keep successful earlier members; report partial outcome and all owned
        // objects remain eligible for clear/Stop. Never retry a whole batch.
        const size_t created = g_owned.size() - before;
        throw Failure(std::format("生成批次中断；当前新增{}个登记对象可清理：{}", created, e.what()));
    }
    const auto timingSpawn = GetTickCount64();
    if (timingSpawn - timingStart >= 100 && request.requestId.starts_with("tower-"))
        log::Info("影塔生成分段耗时：绑定 {} ms、上下文/载入 {} ms、落点 {} ms、创建 {} ms（{}）",
                  timingBind - timingStart, timingContext - timingBind, timingLanding - timingContext,
                  timingSpawn - timingLanding, request.requestId);
    if (!engine.requestTag.starts_with("tower-")) StartOwnedMaintenance();
    const bool coldBody = actualPath == kLengTu && !request.requestId.starts_with("tower-");
    return Response(request, true, coldBody ? "已生成独立冷荼本体终阶段，跳过原剧情召唤流程" : "已按游戏原生属性生成",
                    std::format(",\"spawned\":{},\"owned\":{},\"path\":\"{}\",\"requested_path\":\"{}\",\"final_phase\":{},\"level_source\":\"{}\",\"actors\":{}", request.count, g_owned.size(), Escape(actualPath), Escape(request.path), actualPath != request.path || coldBody ? "true" : "false", engine.requestTag.starts_with("tower-") ? "module" : engine.levelSource, OwnedStatsJson()));
}
}  // namespace

void Submit(Request request, Reply reply) {
    auto respond = [](const Reply& callback, std::string value) { if (callback) { try { callback(std::move(value)); } catch (...) {} } };
    if (g_stopping || !gamethread::Ready()) { respond(reply, Response(request, false, "核心正在停用或游戏线程未就绪")); return; }
    if (request.command != "spawn" && request.command != "status" && request.command != "clear") {
        respond(reply, Response(request, false, "未知怪物命令")); return;
    }
    const int pending = ++g_pending;
    if (pending > 4) { --g_pending; respond(reply, Response(request, false, "请求队列已满")); return; }
    const auto epoch = g_epoch.load();
    gamethread::Post([request = std::move(request), reply = std::move(reply), epoch, respond] {
        struct Pending { ~Pending() { --g_pending; } } pendingGuard;
        if (g_stopping || epoch != g_epoch.load() || !gamethread::Ready()) {
            respond(reply, Response(request, false, "请求已随停用取消")); return;
        }
        try {
            const auto started = GetTickCount64();
            auto result = Execute(request);
            if (const auto ms = GetTickCount64() - started; ms >= 100 && request.command == "spawn") log::Info("怪物生成耗时 {} ms（{}）", ms, request.requestId);
            respond(reply, std::move(result));
        }
        catch (const std::exception& e) { respond(reply, Response(request, false, e.what(), std::format(",\"owned\":{}", g_owned.size()))); }
        catch (...) { respond(reply, Response(request, false, "生成调用异常；停止本批次，已登记对象可清理")); }
    });
}

std::vector<std::pair<ue::UObject*, std::string>> OwnedFactions() {
    std::vector<std::pair<ue::UObject*, std::string>> out;
    for (const auto& o : g_owned)
        if (o.actor.Alive() && !o.faction.empty() && o.faction != "Faction.Enemy") out.emplace_back(o.actor.object, o.faction);
    return out;
}

uint64_t NativeStatsRevision(ue::UObject* actor) {
    for (const auto& owned : g_owned) if (owned.actor.object == actor && owned.actor.Alive()) return owned.statsRevision;
    return 0;
}
std::vector<ue::UObject*> OwnedActors() {
    std::vector<ue::UObject*> out;
    for (const auto& o : g_owned)
        if (o.actor.Alive()) out.push_back(o.actor.object);
    return out;
}

std::vector<ue::UObject*> OwnedUntagged(const std::string& prefix) {
    std::vector<ue::UObject*> out;
    for (const auto& o : g_owned)
        if (o.actor.Alive() && (prefix.empty() || o.tag.rfind(prefix, 0) != 0)) out.push_back(o.actor.object);
    return out;
}

std::vector<ue::UObject*> OwnedBosses() {
    std::vector<ue::UObject*> out;
    for (const auto& o : g_owned) {
        if (!o.actor.Alive()) continue;
        for (const auto& e : kEnemyCatalog)
            if (e.path == o.path) { if (e.kind == "boss") out.push_back(o.actor.object); break; }
    }
    return out;
}

namespace {
// keep=false：只清前缀匹配的；keep=true：清掉前缀不匹配的（保留匹配的）
size_t ClearByTag(const std::string& prefix, bool keep) {
    auto hit = [&](const Owned& e) { return (e.tag.rfind(prefix, 0) == 0) != keep; };
    Engine engine;
    try { engine.BindCleanup(); } catch (...) { return g_owned.size(); }
    for (auto& entry : g_owned)
        if (hit(entry)) engine.Destroy(entry);
    std::erase_if(g_owned, [&](const Owned& e) { return !engine.Alive(e); });
    size_t left = 0;
    for (auto& entry : g_owned) left += hit(entry);
    return left;
}
}  // namespace

// 只清 request_id 以 prefix 开头的生成物（影塔只清自己的，不动修改器刷的）。游戏线程调用，返回仍未销毁的个数
size_t ClearTagged(const std::string& prefix) { return prefix.empty() ? 0 : ClearByTag(prefix, false); }
// 清掉除 prefix 开头之外的全部生成物（修改器"全部还原"不清影塔的 Boss）。游戏线程调用
size_t ClearExcept(const std::string& prefix) { return ClearByTag(prefix, true); }

ShutdownResult StopOnGameThread() {
    g_stopping = true; ++g_epoch;
    Request r; r.command = "clear"; r.requestId = "shutdown";
    if (g_owned.empty()) { RemoveArenaHooks(); return {true, Response(r, true, "无本工具生成的对象")}; }
    if (g_thread.load() != GetCurrentThreadId()) return {false, Response(r, false, "必须先在游戏线程清理生成对象，再停用核心")};
    try { Engine e; e.BindCleanup(); const size_t left = e.Clear(); if (!left) RemoveArenaHooks(); return {left == 0, Response(r, left == 0, left ? "仍有生成对象，暂勿拆除核心服务" : "生成对象已清理")}; }
    catch (const std::exception& e) { return {false, Response(r, false, e.what())}; }
    catch (...) { return {false, Response(r, false, "清理异常，暂勿拆除核心服务")}; }
}
}  // namespace pbz::enemies
