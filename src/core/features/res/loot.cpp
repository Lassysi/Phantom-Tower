// Source-preserving native inventory bridge. No UObject fields or map storage
// are written: only the by-value count entering the native grant is changed.
#include "runtime_contract.h"
#include "loot_policy.h"
#include "loot_catalog.h"
#include "native_loot_signatures.h"
#include "../../services/hooks.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <set>

namespace pbz::res::loot {
namespace {
using Exec = void(__fastcall*)(ue::UObject*, void*, void*);
using Add = void(__fastcall*)(ue::UObject*, int32_t, uint64_t, bool);
enum Index { Item, Money, Count };
constexpr const char* Ids[] = {"res.item_mul", "res.money_mul"};
constexpr size_t NativeExecCount = 5;
constexpr const char* HookNames[] = {"res.loot.exec.add", "res.loot.exec.map", "res.loot.exec.silent", "res.loot.exec.dust", "res.loot.exec.pickup", "res.loot.add"};

struct SourceContext {
    Origin origin = Origin::Unknown;
    runtime::Identity caller{nullptr};
    ue::UObject* node = nullptr;
    bool actorRoot = false;
    bool blocked = false;
};
thread_local SourceContext context;
thread_local unsigned grantDepth = 0, dustDepth = 0;

std::string ItemName(uint64_t item) {
    return FullItemName(ue::Name(uint32_t(item)), uint32_t(item >> 32));
}

uint64_t Hash(const uint8_t* bytes, size_t size) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i) h = (h ^ bytes[i]) * 1099511628211ull;
    return h;
}

// Resolve native registrations by name and full supported-thunk fingerprints.
// No sampled address or fixed RVA is used to locate an entry in a live image.
struct NativeBindings {
    std::array<void*, NativeExecCount> exec{};
    void* add = nullptr;
    bool Resolve(std::string* why) {
        auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
        IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
        if (!ue::Read(base, &dos, sizeof dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 || dos.e_lfanew > 4096 ||
            !ue::Read(base + dos.e_lfanew, &nt, sizeof nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt.FileHeader.NumberOfSections > 96) {
            *why = "奖励原生桥版本指纹不匹配"; return false;
        }
        const auto* profile=signature::ForImage(nt.FileHeader.TimeDateStamp,nt.OptionalHeader.SizeOfImage);
        if(!profile||profile->count!=NativeExecCount){*why="奖励原生桥版本指纹不匹配";return false;}
        std::vector<IMAGE_SECTION_HEADER> sections(nt.FileHeader.NumberOfSections);
        auto* sectionAt = base + dos.e_lfanew + 4 + sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
        if (!ue::Read(sectionAt, sections.data(), sections.size() * sizeof sections[0])) return false;
        auto executable = [&](const uint8_t* p, size_t size) {
            const uintptr_t address = reinterpret_cast<uintptr_t>(p), start = reinterpret_cast<uintptr_t>(base);
            if (address < start || address - start > nt.OptionalHeader.SizeOfImage || size > nt.OptionalHeader.SizeOfImage - (address - start)) return false;
            const auto rva = address - start;
            for (const auto& s : sections)
                if ((s.Characteristics & IMAGE_SCN_MEM_EXECUTE) && rva >= s.VirtualAddress &&
                    rva + size <= uint64_t(s.VirtualAddress) + s.Misc.VirtualSize) return true;
            return false;
        };
        std::array<unsigned, NativeExecCount> matches{};
        // Registration entries are name-VA + function-VA pairs; the name
        // strings and the pairs are not adjacent, so use two bounded passes:
        // collect the name addresses, then scan 8-byte slots for each pair.
        // The old implementation read whole read-only sections into one
        // vector; .rsrc alone is ~54 MB and low-memory VMs could stall.
        constexpr size_t kChunk = 1u << 20, kOverlap = 64;
        std::array<std::vector<uintptr_t>, NativeExecCount> names;
        std::vector<uint8_t> data(kChunk + kOverlap);
        for (const auto& section : sections) {
            if (!(section.Characteristics & IMAGE_SCN_MEM_READ) || (section.Characteristics & IMAGE_SCN_MEM_WRITE) ||
                (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) || section.Misc.VirtualSize > 128 * 1024 * 1024 ||
                uint64_t(section.VirtualAddress) + section.Misc.VirtualSize > nt.OptionalHeader.SizeOfImage) continue;
            const size_t size = section.Misc.VirtualSize;
            for (size_t offset = 0; offset < size; offset += kChunk) {
                const size_t starts = std::min(kChunk, size - offset);
                const size_t take = std::min(kChunk + kOverlap, size - offset);
                if (!ue::Read(base + section.VirtualAddress + offset, data.data(), take)) break;
                for (size_t slot = 0; slot < exec.size(); ++slot) {
                    const auto& spec = profile->execs[slot];
                    const size_t length = std::strlen(spec.name) + 1;
                    for (size_t i = 0; i < starts && i + length <= take; ++i)
                        if (data[i] == spec.name[0] && std::memcmp(data.data() + i, spec.name, length) == 0)
                            names[slot].push_back(reinterpret_cast<uintptr_t>(base + section.VirtualAddress + offset + i));
                }
            }
            if (std::all_of(names.begin(), names.end(), [](const auto& n) { return !n.empty(); })) break;
        }
        if (std::any_of(names.begin(), names.end(), [](const auto& n) { return n.empty(); })) {
            *why = "奖励原生注册表缺失/歧义或包装代码已变化"; return false;
        }
        bool allMatched = false;
        for (const auto& section : sections) {
            if (allMatched) break;
            if (!(section.Characteristics & IMAGE_SCN_MEM_READ) || (section.Characteristics & IMAGE_SCN_MEM_WRITE) ||
                (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) || section.Misc.VirtualSize > 128 * 1024 * 1024 ||
                uint64_t(section.VirtualAddress) + section.Misc.VirtualSize > nt.OptionalHeader.SizeOfImage) continue;
            const size_t size = section.Misc.VirtualSize;
            for (size_t offset = 0; offset < size; offset += kChunk) {
                const size_t starts = std::min(kChunk, size - offset);
                const size_t take = std::min(kChunk + 16, size - offset);
                if (!ue::Read(base + section.VirtualAddress + offset, data.data(), take)) break;
                for (size_t i = 0; i < starts && i + 16 <= take; i += 8) {
                    uintptr_t name = 0; uint8_t* function = nullptr;
                    std::memcpy(&name, data.data() + i, 8);
                    std::memcpy(&function, data.data() + i + 8, 8);
                    for (size_t slot = 0; slot < exec.size(); ++slot) {
                        const auto& spec = profile->execs[slot];
                        if (std::find(names[slot].begin(), names[slot].end(), name) == names[slot].end()) continue;
                        if (!executable(function, spec.bytes)) continue;
                        std::vector<uint8_t> bytes(spec.bytes);
                        if (!ue::Read(function, bytes.data(), bytes.size()) || Hash(bytes.data(), bytes.size()) != spec.hash) continue;
                        exec[slot] = function; ++matches[slot];
                    }
                }
                allMatched = std::all_of(matches.begin(), matches.end(), [](unsigned n) { return n >= 1; });
            }
        }
        for (unsigned n : matches) if (n != 1) { *why = "奖励原生注册表缺失/歧义或包装代码已变化"; return false; }
        auto* call = static_cast<uint8_t*>(exec[0]) + profile->addCallOffset;
        uint8_t opcode = 0; int32_t displacement = 0;
        if (!ue::Read(call, &opcode, 1) || opcode != 0xE8 || !ue::Read(call + 1, &displacement, 4)) return false;
        auto* target = call + 5 + displacement;
        std::array<uint8_t, 14> prologue{};
        if (!executable(target, prologue.size()) || !ue::Read(target, prologue.data(), prologue.size()) ||
            Hash(prologue.data(), prologue.size()) != profile->addPrologueHash) {
            *why = "实际背包入账入口的原始字节不匹配（或已有其他钩子）"; return false;
        }
        add = target; return true;
    }
};

struct Bridge {
    NativeBindings bindings;
    std::array<Exec, NativeExecCount> originalExec{};
    Add originalAdd = nullptr;
    std::array<double, Count> values{1, 1};
    std::array<bool, Count> active{};
    std::atomic<DWORD> gameThread{0};
    bool resolved = false, validated = false, hooksCreated = false, ownsDustIntercept = false, observing = false;
    // Shared by the three loot features; see Resolve(). One reflection walk
    // per second, not one per feature, while required classes are still loading.
    ULONGLONG nextResolve_ = 0;
    std::string lastWhy_;
    runtime::Function itemInfo, dust;
    ue::UObject *backpackClass = nullptr, *playerClass = nullptr, *worldClass = nullptr, *levelClass = nullptr;
    // Only classes actually used at runtime are resolved here. Source classes
    // (BP_Chest_C / BP_NPCLootComp_C / BP_RenewalNPC_C / BP_Rubbish_C) are
    // identified from each live caller's parent chain in Classify(); querying
    // them here would scan every object on every retry while an optional class
    // is absent. Actor-root bindings still look for them once on enable.
    ue::UObject* pickupClass = nullptr;
    ue::UObject* functionClass = nullptr;
    int dustOffset = -1, levelWorldOffset = -1;
    unsigned receipts = 0;
    unsigned rootReceipts = 0, unknownStacks = 0;
    std::array<uint64_t, NativeExecCount> execSeen{};
    std::set<ue::UObject*> actorRoots;
    std::set<ue::UObject*> exclusionRoots;
    std::vector<int> rootObservers;
    std::vector<int> noticeObservers;
    unsigned noticeReceipts = 0;
    // 获取提示跟随倍率：放大一笔奖励后记下"物品：原数 → 放大数"（尘埃 item=0），游戏随后把原数送进自己的提示控件时
    // 换成放大数——数字仍由游戏控件排版（原字体 / 动画），余额不再改动。只在 4 秒内、物品和原数都对上时替换
    struct TipRemap { uint64_t item; int32_t from, to; unsigned long long until; bool queued = false, dumped = false; };
    std::vector<TipRemap> tipRemaps;
    std::vector<int> tipObservers;
    void NoteTip(uint64_t item, int32_t from, int32_t to) {
        if (from == to || from <= 0) return;
        const auto now = GetTickCount64();
        std::erase_if(tipRemaps, [&](const TipRemap& r) { return r.until < now; });
        if (tipRemaps.size() < 64) tipRemaps.push_back({item, from, to, now + 4000});
        FixQueues();
    }
    // 游戏的获取提示从背包子系统的两个待显示队列取数：PushGetPropInfos {bIsAdd +0, ItemName +4, Num +0xC}（物品）、
    // UpdateDustNumData（尘埃，8 字节：数量在 +4 或 +0）。放大一笔后立即、并在 4 秒内每帧把队列里"同物品且等于原数"的条目改成放大数；
    // 每笔只改一次。提示控件照常读队列，用游戏自己的字体 / 动画显示
    ue::UObject* backpack = nullptr;
    unsigned long long backpackSearch = 0;
    int offPropQueue = -2, offDustQueue = -2;
    void FixQueues() {
        if (tipRemaps.empty() || !Thread()) return;
        const auto now = GetTickCount64();
        std::erase_if(tipRemaps, [&](const TipRemap& r) { return r.until < now; });
        if (std::none_of(tipRemaps.begin(), tipRemaps.end(), [](const TipRemap& r) { return !r.queued; })) return;
        if (!backpack || !ue::IsA(backpack, backpackClass) || !runtime::Identity(backpack).Alive()) {
            backpack = nullptr;
            if (now - backpackSearch < 5000) return;
            backpackSearch = now;
            ue::ForEach([&](int, ue::UObject* o) {
                if (!ue::IsDefault(o) && ue::IsA(o, backpackClass)) { backpack = o; return false; }
                return true;
            });
            if (!backpack) return;
        }
        if (offPropQueue == -2) { offPropQueue = ue::PropOffset(backpackClass, "PushGetPropInfos"); offDustQueue = ue::PropOffset(backpackClass, "UpdateDustNumData"); }
        auto each = [&](int off, int stride, auto&& fn) {
            if (off < 0) return;
            auto* data = ue::Get<uint8_t*>(backpack + off);
            const int n = ue::Get<int32_t>(backpack + off + 8);
            for (int i = 0; data && i < n && i < 256; ++i) fn(data + i * stride);
        };
        for (auto& r : tipRemaps) {
            if (r.queued) continue;
            if (r.item) {
                each(offPropQueue, 0x10, [&](uint8_t* e) {
                    if (!r.queued && ue::Get<uint64_t>(e + 4) == r.item && ue::Get<int32_t>(e + 0xC) == r.from) { ue::Write(e + 0xC, &r.to, 4); r.queued = true; }
                });
            } else {
                each(offDustQueue, 8, [&](uint8_t* e) {
                    for (int at : {4, 0})
                        if (!r.queued && ue::Get<int32_t>(e + at) == r.from) { ue::Write(e + at, &r.to, 4); r.queued = true; }
                });
            }
            if (r.queued && receipts < 256) log::Info("loot tip queue {} {} -> {}（游戏原生提示）", r.item ? ItemName(r.item) : "Dust", r.from, r.to);
            // 诊断：物品提示在队列里没找到 → 记录一次当时队列内容（实测材料拾取从未命中 PushGetPropInfos）
            if (!r.queued && r.item && !r.dumped && r.until - now < 3000 && tipDumps < 20) {
                r.dumped = true;
                ++tipDumps;
                std::string q;
                const int n = offPropQueue >= 0 ? ue::Get<int32_t>(backpack + offPropQueue + 8) : -1;
                each(offPropQueue, 0x10, [&](uint8_t* e) {
                    if (q.size() < 300) q += std::format(" [{} {} add={}]", ItemName(ue::Get<uint64_t>(e + 4)), ue::Get<int32_t>(e + 0xC), ue::Get<uint8_t>(e));
                });
                log::Info("loot tip miss {} {} -> {}：PushGetPropInfos 共 {} 条{}", ItemName(r.item), r.from, r.to, n, q);
            }
        }
    }
    unsigned tipDumps = 0;
    bool MapTip(uint64_t item, int32_t& n) const {
        const auto now = GetTickCount64();
        for (auto it = tipRemaps.rbegin(); it != tipRemaps.rend(); ++it)
            if (it->until >= now && it->item == item && it->from == n) { n = it->to; return true; }
        return false;
    }
    int dustObserver = 0;

    bool Thread() const { return GetCurrentThreadId() == gameThread.load(); }
    ue::UObject* WorldOf(ue::UObject* object) const {
        for (unsigned i = 0; object && i < 24; ++i) {
            if (ue::IsA(object, worldClass)) return object;
            // Streamed/World Partition actors can live in a Level whose Outer
            // is a package rather than the active UWorld. OwningWorld is the
            // typed authoritative relationship; never infer world from name.
            if (levelClass && levelWorldOffset >= 0 && ue::IsA(object, levelClass)) {
                auto* world = ue::Get<ue::UObject*>(object + levelWorldOffset);
                if (runtime::Identity(world).Alive() && ue::IsA(world, worldClass)) return world;
                return nullptr; // A present but invalid Level relationship fails closed.
            }
            object = ue::Get<ue::UObject*>(object + ue::Layout::ObjOuter);
        }
        return nullptr;
    }
    bool Local(ue::UObject* object) const {
        if (!runtime::Identity(object).Alive()) return false;
        auto* world = WorldOf(object);
        if (!world) return false;
        for (const auto& target : attr::Targets())
            if (target.who == attr::Who::Player && attr::Alive(target) && WorldOf(target.obj) == world) return true;
        return false;
    }
    bool Player(ue::UObject* object) const {
        for (const auto& target : attr::Targets())
            if (target.who == attr::Who::Player && target.obj == object && attr::Alive(target)) return true;
        return false;
    }
    Origin Classify(ue::UObject* object) const {
        // User-approved single-player policy: a live whitelisted caller in
        // the current synchronous grant stack is the source. World equality
        // remains a diagnostic, not a prerequisite for a streamed actor.
        if (!runtime::Identity(object).Alive()) return Origin::Unknown;
        const auto inherits = [](ue::UObject* instance, const char* name) {
            auto* cls = ue::ClassOf(instance);
            for (unsigned n = 0; cls && n < 64; ++n, cls = ue::Get<ue::UObject*>(cls + ue::Layout::StructSuper))
                if (ue::ObjName(cls) == name) return true;
            return false;
        };
        // Read the current hierarchy; a Blueprint class may be loaded only
        // after boot or replaced on a later map, so do not trust its old cache.
        if (inherits(object, "BP_Chest_C")) return Origin::Chest;
        if (inherits(object, "BP_NPCLootComp_C") || inherits(object, "BP_RenewalNPC_C") ||
            inherits(object, "MobGroupScript_AddItemAfterCombat_C")) return Origin::Enemy;
        if (inherits(object, "PBZPickupActor")) {
            // A pickup created/owned by an NPC belongs to enemy loot. If its
            // owner exists but is not understood, do not call it environment E.
            const int offset = ue::PropOffset(ue::ClassOf(object), "Owner");
            if (offset < 0) return Origin::Unknown;
            auto* owner = ue::Get<ue::UObject*>(object + offset);
            if (owner) return runtime::Identity(owner).Alive() && inherits(owner, "BP_RenewalNPC_C") ? Origin::Enemy : Origin::Unknown;
            return Origin::Pickup;
        }
        if (inherits(object, "BP_Rubbish_C")) return Origin::Pickup;
        return Origin::Unknown;
    }
    SourceContext Caller(void* frame) const {
        SourceContext found;
        // This FFrame.Object offset is independently present in all three
        // generated native wrappers (Step uses Stack.Object at +0x18). Node
        // at +0x10 is validated as a function of the actual caller hierarchy.
        if (!frame || !ue::Read(static_cast<uint8_t*>(frame) + 0x18, &found.caller.object, 8) ||
            !ue::Read(static_cast<uint8_t*>(frame) + 0x10, &found.node, 8)) return {};
        found.caller = runtime::Identity(found.caller.object);
        if (!found.caller.Alive() || !found.node || !ue::IsA(found.node, functionClass)) return {};
        auto* owner = ue::Get<ue::UObject*>(found.node + ue::Layout::ObjOuter);
        bool belongs = false;
        for (auto* cls = found.caller.cls; cls; cls = ue::Get<ue::UObject*>(cls + ue::Layout::StructSuper))
            if (cls == owner) { belongs = true; break; }
        if (!belongs) return {};
        const auto name = ue::ObjName(found.node);
        // Do not approve the actor's whole ubergraph: a load callback and an
        // acquisition can share that graph. Retain identity for observation,
        // but authorize only an actual named reward method at this boundary.
        const bool rewardMethod = name == "Collect Items" || name == "Pickup Items" ||
            name == "Loot Items" || name == "LootItems" || name == "CollectItems" ||
            name == "PickupItems" || name == "TryToAddDust" || name == "BoxCollisionOverlapBegin" ||
            name == "EndSwitchInteract" || name == "End Interact";
        if (rewardMethod) found.origin = Classify(found.caller.object);
        return found;
    }
    void InspectFrame(void* frame) const {
        ue::UObject *rawObject = nullptr, *rawNode = nullptr;
        if (!frame || !ue::Read(static_cast<uint8_t*>(frame) + 0x18, &rawObject, 8) ||
            !ue::Read(static_cast<uint8_t*>(frame) + 0x10, &rawNode, 8)) return;
        const bool objectAlive = runtime::Identity(rawObject).Alive();
        const bool nodeAlive = runtime::Identity(rawNode).Alive();
        log::Info("loot frame-reject object={} alive={} local={} node={} node-is-function={} node-owner={}",
            objectAlive ? ue::ObjName(rawObject) : "invalid", objectAlive, objectAlive && Local(rawObject),
            nodeAlive ? ue::ObjName(rawNode) : "invalid", nodeAlive && ue::IsA(rawNode, functionClass),
            nodeAlive ? ue::ObjName(ue::Get<ue::UObject*>(rawNode + ue::Layout::ObjOuter)) : "invalid");
    }
    void Receipt(const char* type, uint64_t item, int32_t before, int32_t after, int32_t current, int32_t acquired) {
        if (receipts++ >= 256) return;
        log::Info("loot request type={} origin={} caller={} node={} item={} request={} -> {} current={} acquired={} (not capacity)",
            type, int(context.origin), context.caller.Alive() ? ue::ObjName(context.caller.object) : "unknown",
            context.node ? ue::ObjName(context.node) : "unknown", std::strcmp(type, "dust") == 0 ? "Dust" : ItemName(item),
            before, after, current, acquired);
    }
    void UnknownStack() {
        if (!observing || context.origin != Origin::Unknown || unknownStacks++ >= 8) return;
        void* frames[16]{};
        const USHORT count = CaptureStackBackTrace(1, 16, frames, nullptr);
        std::string stack;
        for (USHORT i = 0; i < count; ++i) {
            HMODULE module = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(frames[i]), &module)) continue;
            char path[MAX_PATH]{}; GetModuleFileNameA(module, path, MAX_PATH);
            std::string name(path); const auto slash = name.find_last_of("/\\");
            if (slash != std::string::npos) name.erase(0, slash + 1);
            if (!stack.empty()) stack += " > ";
            stack += std::format("{}+0x{:x}", name, reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(module));
        }
        log::Info("loot unknown-source exec_seen={},{},{},{},{} stack={}", execSeen[0], execSeen[1], execSeen[2], execSeen[3], execSeen[4], stack);
    }

    static ue::UObject* GuidCallback(ue::UObject* cls) {
        // Prefix matching handles the corrupted Chinese display tail without
        // ever binding a replacement-character string. Require a unique actual
        // UFunction with one Guid-sized parameter in the most-derived class.
        for (unsigned depth = 0; cls && depth < 64; ++depth, cls = ue::Get<ue::UObject*>(cls + ue::Layout::StructSuper)) {
            ue::UObject* match = nullptr;
            auto* f = ue::Get<ue::UObject*>(cls + ue::Layout::StructChildren);
            for (unsigned n = 0; f && n < 4096; ++n, f = ue::Get<ue::UObject*>(f + ue::Layout::UFieldNext)) {
                if (!ue::ObjName(f).starts_with("CompletedDisplayUI_")) continue;
                auto* p = ue::Get<ue::UObject*>(f + ue::Layout::StructChildProps);
                unsigned count = 0; bool valid = true;
                for (unsigned k = 0; p && k < 64; ++k, p = ue::Get<ue::UObject*>(p + ue::Layout::FieldNext)) {
                    const auto flags = ue::Get<uint64_t>(p + 0x38);
                    if (!(flags & 0x80)) continue;
                    auto* type = ue::Get<ue::UObject*>(p + ue::Layout::FieldClass);
                    valid = valid && type && !(flags & 0x400) && ue::Name(ue::Get<uint32_t>(p + ue::Layout::FieldName)) == "Guid" &&
                        ue::Name(ue::Get<uint32_t>(type + ue::Layout::FieldClassName)) == "StructProperty" && ue::Get<int>(p + 0x34) == 16;
                    ++count;
                }
                if (p || !valid || count != 1) continue;
                if (match) return nullptr;
                match = f;
            }
            if (f) return nullptr;
            if (match) return match;
        }
        return nullptr;
    }
    bool ActorEvent(ue::UObject* object, ue::UObject* function, void* params) {
        if (!Thread()) return false;
        const auto origin = context.blocked ? Origin::Unknown : Classify(object);
        const auto previous = context;
        struct Restore { SourceContext saved; ~Restore() { context = saved; } } restore{previous};
        context = SourceContext{origin, runtime::Identity(object), function, origin != Origin::Unknown, previous.blocked};
        pe::Call(object, function, params);
        return true;
    }
    void ObserveActor(ue::UObject* object, ue::UObject* function) {
        if (!Thread() || !observing) return;
        const auto origin = context.blocked ? Origin::Unknown : Classify(object);
        if (observing && rootReceipts++ < 32) log::Info("loot actor-root caller={} fn={} origin={} (synchronous call only)",
            ue::ObjName(object), ue::ObjName(function), int(origin));
        if (rootReceipts <= 32) {
            std::string worlds;
            for (const auto& target : attr::Targets()) if (target.who == attr::Who::Player && attr::Alive(target)) {
                if (!worlds.empty()) worlds += ",";
                auto* world = WorldOf(target.obj);
                worlds += ue::ObjName(target.obj) + ":" + (world ? ue::ObjName(world) : "no-world");
            }
            auto* world = WorldOf(object);
            auto* outer = ue::Get<ue::UObject*>(object + ue::Layout::ObjOuter);
            log::Info("loot source-diagnostic identity={} class={} outer-class={} world={} player-worlds={} same-world={} (diagnostic only)",
                runtime::Identity(object).Alive(), ue::ObjName(ue::ClassOf(object)), outer ? ue::ObjName(ue::ClassOf(outer)) : "none",
                world ? ue::ObjName(world) : "none", worlds, Local(object));
        }
    }
    bool ExcludeEvent(ue::UObject* object, ue::UObject* function, void* params) {
        if (!Thread()) return false;
        const auto previous = context;
        struct Restore { SourceContext saved; ~Restore() { context = saved; } } restore{previous};
        context.origin = Origin::Unknown; context.actorRoot = false; context.blocked = true;
        pe::Call(object, function, params);
        return true;
    }
    void BindTipFix(bool on) {
        for (int token : tipObservers) pe::Unobserve(token);
        tipObservers.clear();
        tipRemaps.clear();
        if (!on) return;
        // 观察者在原函数执行前回调，参数缓冲就是原函数将要读的那份：在这里改数量即可
        auto bind = [&](const char* clsName, const char* method, std::initializer_list<runtime::Param> params, int quantity, int item, int isAdd) {
            auto* cls = ue::FindClass(clsName); runtime::Function fn; std::string why;
            if (!fn.Resolve(cls, method, params, &why)) { log::Info("loot tip-fix {}.{} unavailable: {}", clsName, method, why); return; }
            tipObservers.push_back(pe::Observe(fn.function, [this, cls, fn, quantity, item, isAdd](auto* object, void* args) {
                if (!Thread() || !args || tipRemaps.empty() || !ue::IsA(object, cls)) return;
                int32_t amount = 0; uint64_t id = 0; uint8_t add = 1;
                if (!fn.Read(args, quantity, amount) || (item >= 0 && !fn.Read(args, item, id)) || (isAdd >= 0 && !fn.Read(args, isAdd, add))) return;
                if (add != 1) return;
                const int32_t before = amount;
                if (MapTip(item >= 0 ? id : 0, amount) && fn.Write(args, quantity, amount) && receipts < 256)
                    log::Info("loot tip {} {} -> {}（游戏原生提示）", item >= 0 ? ItemName(id) : "Dust", before, amount);
            }));
        };
        bind("WB_ItemTips_C", "DisplayItem", {{"PropID", "NameProperty", 8}, {"bIsAdd", "BoolProperty", 1}, {"Num", "IntProperty", 4}}, 2, 0, 1);
        bind("WB_ItemTips_C", "UpdateDustNum_Event", {{"bIsAdd", "BoolProperty", 1}, {"Num", "IntProperty", 4}}, 1, -1, 0);
        // 升级界面的尘埃提示：W_Dust_Tips 收到的 BP_UpdateDustTipsObj.ChangeDustNum
        auto* event = ue::FindClass("GameplayEventData");
        auto* tip = ue::FindClass("BP_UpdateDustTipsObj_C");
        auto* ui = ue::FindClass("W_Dust_Tips_C");
        const int size = event ? ue::Get<int>(event + ue::Layout::StructSize, -1) : -1;
        const int optional = event ? ue::PropOffset(event, "OptionalObject") : -1;
        const int delta = tip ? ue::PropOffset(tip, "ChangeDustNum") : -1;
        if (!ui || !tip || size < 8 || size > 512 || optional < 0 || optional + 8 > size || delta < 0) return;
        for (const char* method : {"EventReceived_13C4885342CE314232EF92B7E1475574", "EventReceived_6B84224B4C1DF9EC5678C9805E444014", "EventReceived_4FDAF52548073837425C6685FE8CEBE4"}) {
            runtime::Function fn; std::string why;
            if (!fn.Resolve(ui, method, {{"Payload", "StructProperty", size}}, &why)) continue;
            tipObservers.push_back(pe::Observe(fn.function, [this, ui, tip, fn, optional, delta](auto* object, void* args) {
                if (!Thread() || !args || tipRemaps.empty() || !ue::IsA(object, ui)) return;
                auto* data = ue::Get<ue::UObject*>(static_cast<uint8_t*>(args) + fn.offsets[0] + optional);
                if (!runtime::Identity(data).Alive() || !ue::IsA(data, tip)) return;
                int32_t amount = 0;
                if (!ue::Read(data + delta, &amount, 4)) return;
                const int32_t before = amount;
                if (MapTip(0, amount) && ue::Write(data + delta, &amount, 4) && receipts < 256)
                    log::Info("loot tip Dust(W_Dust_Tips) {} -> {}（游戏原生提示）", before, amount);
            }));
        }
    }
    void BindNotices(bool on) {
        for (int token : noticeObservers) pe::Unobserve(token);
        noticeObservers.clear();
        if (!on) return;
        // Observe values entering the game's own formatting/animation path.
        // Never multiply again here or replace localized text with our string.
        auto bind = [&](const char* clsName, const char* method, std::initializer_list<runtime::Param> params,
                        int quantity, int item, int isAdd) {
            auto* cls = ue::FindClass(clsName); runtime::Function fn; std::string why;
            if (!fn.Resolve(cls, method, params, &why)) {
                log::Info("loot native-ui probe {}.{} unavailable: {}", clsName, method, why); return;
            }
            noticeObservers.push_back(pe::Observe(fn.function, [this, cls, fn, method, quantity, item, isAdd](auto* object, void* args) {
                if (!Thread() || !observing || noticeReceipts >= 64 || !runtime::Identity(object).Alive() || !ue::IsA(object, cls)) return;
                int32_t amount = 0; uint64_t id = 0; uint8_t add = 2;
                if (!fn.Read(args, quantity, amount) || (item >= 0 && !fn.Read(args, item, id)) ||
                    (isAdd >= 0 && !fn.Read(args, isAdd, add))) return;
                ++noticeReceipts;
                log::Info("loot native-ui fn={} item={} quantity={} is-add={} (observed only; native formatting preserved)",
                    method, item >= 0 ? ItemName(id) : "Dust", amount, int(add));
            }));
        };
        bind("WB_ItemTips_C", "DisplayItem", {{"PropID", "NameProperty", 8}, {"bIsAdd", "BoolProperty", 1}, {"Num", "IntProperty", 4}}, 2, 0, 1);
        bind("WB_ItemTips_C", "UpdateDustNum_Event", {{"bIsAdd", "BoolProperty", 1}, {"Num", "IntProperty", 4}}, 1, -1, 0);
        bind("BP_RenewalPlayer_C", "ChangeDustNumInternal", {{"ChangeDustNum", "IntProperty", 4}}, 0, -1, -1);

        auto* event = ue::FindClass("GameplayEventData");
        auto* tip = ue::FindClass("BP_UpdateDustTipsObj_C");
        auto* ui = ue::FindClass("W_Dust_Tips_C");
        const int size = event ? ue::Get<int>(event + ue::Layout::StructSize, -1) : -1;
        const int optional = event ? ue::PropOffset(event, "OptionalObject") : -1;
        const int current = tip ? ue::PropOffset(tip, "CurrentDustNum") : -1;
        const int delta = tip ? ue::PropOffset(tip, "ChangeDustNum") : -1;
        if (!ui || !tip || size < 8 || size > 512 || optional < 0 || optional + 8 > size || current < 0 || delta < 0) return;
        for (const char* method : {"EventReceived_13C4885342CE314232EF92B7E1475574", "EventReceived_6B84224B4C1DF9EC5678C9805E444014", "EventReceived_4FDAF52548073837425C6685FE8CEBE4"}) {
            runtime::Function fn; std::string why;
            if (!fn.Resolve(ui, method, {{"Payload", "StructProperty", size}}, &why)) {
                log::Info("loot native-ui probe W_Dust_Tips.{} unavailable: {}", method, why); continue;
            }
            noticeObservers.push_back(pe::Observe(fn.function, [this, ui, tip, fn, optional, current, delta](auto* object, void* args) {
                if (!Thread() || !observing || noticeReceipts >= 64 || !args || !runtime::Identity(object).Alive() || !ue::IsA(object, ui)) return;
                auto* data = ue::Get<ue::UObject*>(static_cast<uint8_t*>(args) + fn.offsets[0] + optional);
                if (!runtime::Identity(data).Alive() || !ue::IsA(data, tip)) return;
                int32_t balance = 0, amount = 0;
                if (!ue::Read(data + current, &balance, 4) || !ue::Read(data + delta, &amount, 4)) return;
                ++noticeReceipts;
                log::Info("loot native-ui fn=W_Dust_Tips item=Dust quantity={} current={} (observed only; native formatting preserved)", amount, balance);
            }));
        }
    }
    void BindActorRoots(bool on) {
        for (int token : rootObservers) pe::Unobserve(token);
        rootObservers.clear();
        if (!on) {
            for (auto* f : actorRoots) pe::RemoveIntercept(f);
            for (auto* f : exclusionRoots) pe::RemoveIntercept(f);
            actorRoots.clear(); exclusionRoots.clear(); return;
        }
        auto bind = [&](ue::UObject* f) {
            // The around-call interceptor is needed to restore a scoped source
            // after the native call. Pure logging uses tokenized observers.
            if (f && actorRoots.insert(f).second) pe::Intercept(f, [this, f](auto* o, void* p) { return ActorEvent(o, f, p); });
        };
        for (const char* name : {"BP_Rubbish_C", "BP_Chest_C", "BP_NPCLootComp_C", "BP_RenewalNPC_C", "MobGroupScript_AddItemAfterCombat_C"}) {
            auto* cls = ue::FindClass(name); if (!cls) continue;
            for (const char* method : {"OnInteract", "Collect Items", "Pickup Items", "Pickup", "CollectItems", "PickupItems", "TryToAddDust", "Loot Items", "LootItems", "BP_FinishChangeArmedAndMoveTo", "EndSwitchInteract", "End Interact"})
                bind(pe::FindFunction(cls, method));
            bind(GuidCallback(cls));
        }
        for (const char* clsName : {"PBZBackPackSubsystem", "BP_RenewalPlayer_C", "BP_Rubbish_C", "BP_Chest_C", "ShopSystem", "MobGroupScript_AddItemAfterCombat_C"}) {
            auto* cls = ue::FindClass(clsName); if (!cls) continue;
            for (const char* method : {"LoadGame", "PreLoadJsonGame", "InitItemList", "OnInitialize", "ResetGame", "ResetInventoryForNewCycle", "OnLoadedActorData", "ResetToDefaultState", "UserConstructionScript", "BuyItem", "SellShopItems", "UpgradeWeapon", "SnapSoulWeapon", "ResetWeaponToLevel", "ResetAllWeaponsToLevel1"}) {
                auto* f = pe::FindFunction(cls, method);
                if (f && !actorRoots.count(f) && exclusionRoots.insert(f).second)
                    pe::Intercept(f, [this, f](auto* o, void* p) { return ExcludeEvent(o, f, p); });
            }
        }
        if (observing) for (auto* f : actorRoots)
            rootObservers.push_back(pe::Observe(f, [this, f](auto* o, void*) { ObserveActor(o, f); }));
    }
    bool EligibleSource() const {
        // Origin is provenance, never a second multiplier. Do not borrow a
        // prior interaction for unknown grants, restoration or shop refunds.
        return !context.blocked && context.caller.Alive() &&
            (context.origin == Origin::Enemy || context.origin == Origin::Pickup || context.origin == Origin::Chest);
    }
    bool Counts(ue::UObject* object, uint64_t item, int32_t& current, int32_t& acquired) {
        std::vector<uint8_t> query(itemInfo.size);
        if (!itemInfo.Write(query.data(), 0, item)) return false;
        pe::Call(object, itemInfo.function, query.data());
        return itemInfo.Read(query.data(), 1, current) && itemInfo.Read(query.data(), 2, acquired);
    }
    void Grant(ue::UObject* object, int32_t amount, uint64_t item, bool pushWidget) {
        if (!Thread() || grantDepth || !ue::IsA(object, backpackClass) || !runtime::Identity(object).Alive()) {
            originalAdd(object, amount, item, pushWidget); return;
        }
        int32_t after = amount, current = -1, acquired = -1;
        if ((observing || (EligibleSource() && active[Item])) && !ItemName(item).empty() && amount > 0) {
            if (Counts(object, item, current, acquired) && EligibleSource() && active[Item] &&
                CatalogStackable(ItemName(item)))
                after = ScaleGrant(amount, values[Item], current, acquired).amount;
        }
        Receipt("item", item, amount, after, current, acquired);
        UnknownStack();
        if (after == 0 && amount > 0) return; // Explicit 0 multiplier: no zero-stack creation.
        const runtime::Identity owner(object);
        Depth depth(grantDepth);
        originalAdd(object, after, item, pushWidget);
        backpack = object;
        NoteTip(item, amount, after);
        if ((observing || after != amount) && receipts < 256) {
            int32_t post = -1, total = -1;
            const bool read = owner.Alive() && Counts(object, item, post, total);
            log::Info("loot balance item={} current={} -> {} acquired={} -> {} read={} (native post-read; acquisition/auto-compose acceptance separate)",
                ItemName(item), current, post, acquired, total, read);
        }
    }
    bool Dust(ue::UObject* object, void* params) {
        if (!Thread() || (!active[Money] && !observing) || dustDepth || !Player(object) || !ue::IsA(object, playerClass)) return false;
        uint8_t isAdd = 2; int32_t amount = -1, balance = -1;
        if (!dust.Read(params, 0, isAdd) || isAdd != 1 || !dust.Read(params, 1, amount) || amount <= 0) return false;
        if (dustOffset >= 0) ue::Read(object + dustOffset, &balance, 4);
        const bool identified = EligibleSource();
        // Never scale a mystery positive delta: it may be load/refund/editor.
        const auto scaled = identified && active[Money] ? ScaleGrant(amount, values[Money], balance, balance) : Quantity{amount, false};
        Receipt("dust", 0, amount, scaled.amount, balance, balance);
        UnknownStack();
        if (!scaled.changed && !observing) return false;
        if (scaled.amount == 0) return true;
        if (!dust.Write(params, 1, scaled.amount)) return false;
        const runtime::Identity owner(object);
        struct Restore { runtime::Function& fn; void* params; int32_t amount; ~Restore() { fn.Write(params, 1, amount); } } restore{dust, params, amount};
        Depth depth(dustDepth);
        if (scaled.changed) NoteTip(0, amount, scaled.amount);
        pe::Call(object, dust.function, params);
        FixQueues();
        if ((observing || scaled.changed) && receipts < 256) {
            int32_t post = -1;
            const bool read = owner.Alive() && Player(object) && ue::Read(object + dustOffset, &post, 4);
            log::Info("loot balance item=Dust current={} -> {} read={} (immediate native post-read; delayed BP/UI apply must be verified)", balance, post, read);
        }
        return true;
    }
    void ObserveDust(ue::UObject* object, void* params) {
        if (!Thread() || !observing || active[Money] || !Player(object) || !ue::IsA(object, playerClass)) return;
        uint8_t add = 2; int32_t amount = -1, balance = -1;
        if (!dust.Read(params, 0, add) || add != 1 || !dust.Read(params, 1, amount) || amount <= 0) return;
        ue::Read(object + dustOffset, &balance, 4);
        Receipt("dust", 0, amount, amount, balance, balance);
        UnknownStack();
        // Observer is before-call only. Actual final balance comes from items
        // readback; do not emulate an after-call hook by modifying parameters.
    }
    bool Resolve(std::string* why) {
        if (resolved) return true;
        // res.item_mul / money_mul / loot_probe share this bridge. While a
        // required class is still loading, RefreshUnavailableFeatures retries
        // every second; without a shared throttle all three features repeat
        // the same full reflection walk in one frame. A failed miss is cached
        // for one second (late-loaded classes still recover on the next retry).
        const auto now = GetTickCount64();
        if (now < nextResolve_) { if (why) *why = lastWhy_; return false; }
        nextResolve_ = now + 1000;
        const auto fail = [&] {
            if (why && !why->empty()) lastWhy_ = *why;
            else if (lastWhy_.empty()) lastWhy_ = "奖励反射/玩家服务未就绪";
            if (why) *why = lastWhy_;
            return false;
        };
        backpackClass = ue::FindClass("PBZBackPackSubsystem"); playerClass = ue::FindClass("BP_RenewalPlayer_C");
        worldClass = ue::FindClass("World"); functionClass = ue::FindClass("Function");
        levelClass = ue::FindClass("Level");
        levelWorldOffset = levelClass ? ue::PropOffset(levelClass, "OwningWorld") : -1;
        pickupClass = ue::FindClass("PBZPickupActor");
        if (!backpackClass || !playerClass || !worldClass || !functionClass || !attr::Resolve(why)) { if(why)*why = "奖励反射/玩家服务未就绪"; return fail(); }
        if (!itemInfo.Resolve(backpackClass, "GetItemNumInfo", {{"PropID", "NameProperty", 8}, {"CurrentNum", "IntProperty", 4}, {"MaxNum", "IntProperty", 4}}, why) ||
            !dust.Resolve(playerClass, "ChangeDustNum", {{"bIsAdd", "BoolProperty", 1}, {"Num", "IntProperty", 4}}, why)) return fail();
        dustOffset = ue::PropOffset(playerClass, "ConsciousnessDust");
        if (dustOffset < 0) return fail();
        // The native PE registration scan is deliberately deferred until a
        // loot feature is actually enabled. It reads the game's read-only
        // sections and must not run during core startup on low-memory VMs.
        lastWhy_.clear();
        resolved = true; return true;
    }
    bool Validate(std::string* why) {
        if (validated) return true;
        if (!bindings.Resolve(why)) return false;
        validated = true; return true;
    }
    bool Start();
    bool Rebind() {
        const bool any = observing || std::any_of(active.begin(), active.end(), [](bool on) { return on; });
        if (any && !hooksCreated) return false;
        bool okay = true;
        for (const char* name : HookNames) if (hooksCreated) okay = hooks::Enable(name, any) && okay;
        if (!okay && any) {
            active.fill(false);
            observing = false;
            for (const char* name : HookNames) hooks::Enable(name, false);
            for (auto& feature : Features()) {
                if (std::strcmp(feature->Id(), "res.loot_probe") == 0) feature->enabled = false;
                for (const char* id : Ids) if (std::strcmp(feature->Id(), id) == 0) feature->enabled = false;
            }
        }
        BindActorRoots(okay && any);
        BindNotices(okay && any && observing);
        BindTipFix(okay && any);
        if (dustObserver) { pe::Unobserve(dustObserver); dustObserver = 0; }
        if (active[Money]) {
            pe::Intercept(dust.function, [this](auto* o, void* p) { return Dust(o, p); });
            ownsDustIntercept = true;
        } else if (ownsDustIntercept) {
            pe::RemoveIntercept(dust.function); ownsDustIntercept = false;
        }
        if (observing && !active[Money]) dustObserver = pe::Observe(dust.function, [this](auto* o, void* p) { ObserveDust(o, p); });
        return okay;
    }
};
Bridge bridge;

template<unsigned Slot> void __fastcall ExecDetour(ue::UObject* object, void* frame, void* result) {
    if (!bridge.Thread()) { bridge.originalExec[Slot](object, frame, result); return; }
    const auto previous = context;
    struct Restore { SourceContext saved; ~Restore() { context = saved; } } restore{previous};
    auto caller = bridge.Caller(frame);
    // A native exec forwarding the already identified actor's call may expose
    // the callee as Stack.Object. Preserve only this exact synchronous root;
    // an explicitly different caller (quest/UI/load) cannot borrow it.
    const auto callerMethod = caller.node ? ue::ObjName(caller.node) : std::string{};
    bool restoring = false;
    for (const char* word : {"Load", "Init", "Reset", "Construction", "PreSave"})
        restoring = restoring || callerMethod.find(word) != std::string::npos;
    // FFrame is optional diagnostic/fallback. The actual live actor's current
    // around-call scope is authoritative even if no frame can be decoded.
    const bool forwarding = !restoring && previous.actorRoot && previous.caller.Alive();
    if (previous.blocked || restoring) {
        context.origin = Origin::Unknown; context.actorRoot = false; context.blocked = true;
    } else context = forwarding ? previous : caller;
    ++bridge.execSeen[Slot];
    if (bridge.observing && bridge.execSeen[Slot] <= 8) {
        log::Info("loot exec slot={} callee={} caller={} node={} origin={} inherited-root={}",
            Slot, ue::ObjName(object), caller.caller.Alive() ? ue::ObjName(caller.caller.object) : "unknown",
            caller.node ? ue::ObjName(caller.node) : "unknown", int(context.origin), forwarding);
        if (!caller.caller.Alive()) bridge.InspectFrame(frame);
    }
    if constexpr (Slot == 4) {
        // This entry is PBZPickupActor's actual native overlap implementation
        // wrapper. Its C++ body may call the grant directly, without a second
        // exec/PE entry. Scope the exact validated actor for that synchronous
        // handler, not for an overlap timer or an entire interaction sequence.
        if (!context.blocked && bridge.pickupClass && ue::IsA(object, bridge.pickupClass) && runtime::Identity(object).Alive()) {
            context.caller = runtime::Identity(object);
            context.node = pe::FindFunction(bridge.pickupClass, "BoxCollisionOverlapBegin");
            context.origin = bridge.Classify(object);
            context.actorRoot = context.origin != Origin::Unknown;
        }
    }
    bridge.originalExec[Slot](object, frame, result);
}
void __fastcall AddDetour(ue::UObject* object, int32_t amount, uint64_t item, bool pushWidget) {
    bridge.Grant(object, amount, item, pushWidget);
}
bool Bridge::Start() {
    if (hooksCreated) return true;
    constexpr Exec detours[] = {ExecDetour<0>, ExecDetour<1>, ExecDetour<2>, ExecDetour<3>, ExecDetour<4>};
    for (unsigned i = 0; i < NativeExecCount; ++i)
        if (!hooks::Create(HookNames[i], bindings.exec[i], reinterpret_cast<void*>(detours[i]), reinterpret_cast<void**>(&originalExec[i]))) return false;
    if (!hooks::Create(HookNames[NativeExecCount], bindings.add, reinterpret_cast<void*>(AddDetour), reinterpret_cast<void**>(&originalAdd))) return false;
    hooksCreated = true; return true;
}

class LootFeature : public IFeature {
public:
    explicit LootFeature(int slot) : slot_(slot) {}
    const char* Id() const override { return Ids[slot_]; }
    bool Resolve(std::string* why) override { return bridge.Resolve(why); }
    void SetValue(double v) override {
        accepted_ = ValidMultiplier(v);
        if (!accepted_) { log::Info("{} rejected value {}; expected integer 0..1000, previous value retained", Id(), v); return; }
        value = v; bridge.values[slot_] = v;
    }
    void Enable() override {
        std::string why;
        if (!accepted_ || !bridge.resolved || !bridge.Validate(&why) || !bridge.Start()) {
            if (!why.empty()) log::Info("{} 原生桥校验失败：{}", Id(), why);
            else log::Info("{} native bridge not enabled", Id());
            return;
        }
        bridge.gameThread = GetCurrentThreadId();
        bridge.active[slot_] = enabled = true;
        if (!bridge.Rebind()) { bridge.active[slot_] = enabled = false; log::Info("{} hook activation failed; all loot multipliers neutralized", Id()); return; }
        log::Info("{} enabled at native grant boundary; unknown sources/unique items unchanged; receipt logs are not gameplay acceptance", Id());
    }
    void Disable() override { bridge.active[slot_] = enabled = false; bridge.Rebind(); }
    void Tick(float) override { bridge.FixQueues(); }
private:
    int slot_; bool accepted_ = true;
};

// Development-only, deliberately absent from shared/features.json and the
// product UI. Its only purpose is source/quantity observation at multiplier 1
// before authorizing a candidate for live reward changes.
class LootProbe : public IFeature {
public:
    const char* Id() const override { return "res.loot_probe"; }
    bool IsToggle() const override { return true; }
    bool Resolve(std::string* why) override { return bridge.Resolve(why); }
    void Enable() override {
        std::string why;
        if (!bridge.resolved || !bridge.Validate(&why) || !bridge.Start()) {
            if (!why.empty()) log::Info("{} 原生桥校验失败：{}", Id(), why);
            return;
        }
        bridge.gameThread = GetCurrentThreadId(); bridge.receipts = bridge.rootReceipts = bridge.unknownStacks = bridge.noticeReceipts = 0; bridge.execSeen.fill(0);
        bridge.observing = enabled = true;
        if (!bridge.Rebind()) bridge.observing = enabled = false;
    }
    void Disable() override { bridge.observing = enabled = false; bridge.Rebind(); }
    void Tick(float) override {}
};
} // namespace
} // namespace pbz::res::loot

namespace pbz {
void RegisterLootFeatures() {
    for (int slot = 0; slot < res::loot::Count; ++slot) {
        auto feature = std::make_unique<res::loot::LootFeature>(slot);
        auto old = std::find_if(Features().begin(), Features().end(), [&](auto& f) { return std::strcmp(f->Id(), feature->Id()) == 0; });
        if (old == Features().end()) Features().push_back(std::move(feature));
        else *old = std::move(feature);
    }
    Features().push_back(std::make_unique<res::loot::LootProbe>());
}
}
