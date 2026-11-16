#include "ue.h"

#include "diag.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace pbz::ue {
namespace {

uint8_t* g_objects = nullptr;   // 指向 Objects 字段（FUObjectItem** 所在处）
uint8_t** g_blocks = nullptr;   // FNamePool 块指针数组
std::mutex g_nameLock;
std::unordered_map<uint32_t, std::string> g_names;
std::unordered_map<std::string, UObject*> g_classes;

bool SafeCopyImpl(const void* p, void* out, size_t n) {
    __try {
        memcpy(out, p, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeCopy(const void* p, void* out, size_t n) {
    ++diag::t_probe;  // 探测失败是预期内的，异常诊断不记录
    const bool ok = SafeCopyImpl(p, out, n);
    --diag::t_probe;
    return ok;
}

struct Section { uint8_t* base; size_t size; };

template <class F> void ForEachWritableSection(F&& f) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        const auto ch = sec->Characteristics;
        if ((ch & IMAGE_SCN_MEM_WRITE) && !(ch & IMAGE_SCN_MEM_EXECUTE))
            f(Section{base + sec->VirtualAddress, sec->Misc.VirtualSize});
    }
}

bool InImage(uintptr_t p) {
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    return p >= base && p < base + nt->OptionalHeader.SizeOfImage;
}

bool FindObjects() {
    ForEachWritableSection([](Section s) {
        for (size_t o = 8; !g_objects && o + 0x18 < s.size; o += 8) {
            int32_t v[6];
            if (!SafeCopy(s.base + o - 8, v, sizeof v)) continue;
            const int maxChunks = v[0], numChunks = v[1], num = v[4], max = v[5];
            const auto objs = *reinterpret_cast<uintptr_t*>(&v[2]);
            if (max < 0x10000 || max > 0x4000000 || num < 0x8000 || num > max) continue;
            if (maxChunks != (max + 65535) / 65536 || numChunks != (num + 65535) / 65536) continue;
            if (objs < 0x10000 || objs > 0x7FFFFFFFFFFF || InImage(objs)) continue;
            g_objects = s.base + o;
        }
    });
    return g_objects != nullptr;
}

bool FindNames() {
    ForEachWritableSection([](Section s) {
        for (size_t o = 0; !g_blocks && o + 8 <= s.size; o += 8) {
            uintptr_t blk;
            if (!SafeCopy(s.base + o, &blk, 8) || blk < 0x10000 || blk > 0x7FFFFFFFFFFF || InImage(blk)) continue;
            char head[40];
            if (!SafeCopy(reinterpret_cast<void*>(blk), head, sizeof head)) continue;
            if (memcmp(head + 2, "None", 4) == 0 && std::string_view(head, sizeof head).find("ByteProperty") != std::string_view::npos)
                g_blocks = reinterpret_cast<uint8_t**>(s.base + o);
        }
    });
    return g_blocks != nullptr;
}

}  // namespace

bool Read(const void* p, void* out, size_t n) { return p && SafeCopy(p, out, n); }
bool Write(void* p, const void* in, size_t n) { return p && SafeCopy(in, p, n); }

bool Init(std::string* why) {
    if (!FindObjects()) { if (why) *why = "GObjects 未找到"; return false; }
    if (!FindNames()) { if (why) *why = "FNamePool 未找到"; return false; }
    return true;
}

int Num() { return Get<int32_t>(g_objects + 8); }

UObject* Obj(int i) {
    if (i < 0 || i >= Num()) return nullptr;
    auto** chunks = Get<uint8_t**>(g_objects);
    auto* chunk = chunks ? Get<uint8_t*>(chunks + (i >> 16)) : nullptr;
    return chunk ? Get<UObject*>(chunk + Layout::ItemSize * (i & 0xFFFF) + Layout::ItemObject) : nullptr;
}

int ObjBatch(int start, int count, UObject** out) {
    const int n = std::min(Num(), start + count);
    auto** chunks = Get<uint8_t**>(g_objects);
    int got = 0;
    uint8_t buf[Layout::ItemSize * 1024];
    for (int i = start; i < n;) {
        auto* chunk = chunks ? Get<uint8_t*>(chunks + (i >> 16)) : nullptr;
        const int inChunk = i & 0xFFFF;
        const int take = std::min({n - i, 0x10000 - inChunk, 1024});
        if (!chunk || !Read(chunk + Layout::ItemSize * inChunk, buf, Layout::ItemSize * take)) { i += take; continue; }
        for (int k = 0; k < take; ++k) out[got++] = *reinterpret_cast<UObject**>(buf + Layout::ItemSize * k + Layout::ItemObject);
        i += take;
    }
    return got;
}

std::string Name(uint32_t idx) {
    std::lock_guard lk(g_nameLock);
    if (auto it = g_names.find(idx); it != g_names.end()) return it->second;
    std::string out;
    if (auto* blk = Get<uint8_t*>(g_blocks + (idx >> 16))) {
        const uint8_t* e = blk + 2 * (idx & 0xFFFF);
        const auto hd = Get<uint16_t>(e);
        const int len = hd >> 6;
        if (hd & 1) {  // 宽字符名只取 ASCII 部分，足够做类/属性名匹配
            for (int i = 0; i < len; ++i) out.push_back(static_cast<char>(Get<uint16_t>(e + 2 + 2 * i)));
        } else {
            out.resize(len);
            Read(e + 2, out.data(), len);
        }
    }
    g_names.emplace(idx, out);
    return out;
}

void NameRaw(uint32_t idx, char* out, size_t cap) {
    if (!cap) return;
    out[0] = 0;
    auto* blk = Get<uint8_t*>(g_blocks + (idx >> 16));
    if (!blk) return;
    const uint8_t* e = blk + 2 * (idx & 0xFFFF);
    const auto hd = Get<uint16_t>(e);
    const size_t len = std::min<size_t>(hd >> 6, cap - 1);
    size_t i = 0;
    for (; i < len; ++i) out[i] = (hd & 1) ? static_cast<char>(Get<uint16_t>(e + 2 + 2 * i)) : static_cast<char>(Get<uint8_t>(e + 2 + i));
    out[i] = 0;
}

bool Live(UObject* o) {
    if (!o) return false;
    struct { void* vt; uint32_t flags; int32_t index; } h;
    if (!Read(o, &h, sizeof h)) return false;
    // RF_BeginDestroyed | RF_FinishDestroyed | RF_MirroredGarbage：已销毁或标为垃圾
    if (h.flags & (0x00008000u | 0x00010000u | 0x40000000u)) return false;
    // 对象表的槽位在释放时清空；旧指针（已释放、或内存被别的对象复用）读出的索引对不上同一个槽
    return Obj(h.index) == o;
}

std::string ObjName(UObject* o) {
    const auto idx = Get<uint32_t>(o + Layout::ObjName);
    const auto num = Get<uint32_t>(o + Layout::ObjName + 4);
    auto n = Name(idx);
    return num ? n + "_" + std::to_string(num - 1) : n;
}

UObject* ClassOf(UObject* o) { return Get<UObject*>(o + Layout::ObjClass); }
std::string ClassName(UObject* o) { auto* c = ClassOf(o); return c ? ObjName(c) : std::string(); }
// 类默认对象（CDO）：看对象标志 RF_ClassDefaultObject（0x10，与 Live 同一处 flags），不再逐个拼名字（巡检每帧上千次）
bool IsDefault(UObject* o) { return o && (Get<uint32_t>(o + 8) & 0x00000010u) != 0; }

bool IsA(UObject* o, UObject* cls) {
    int depth = 0;  // 父类链上限：失效类对象的链可能成环
    for (auto* c = ClassOf(o); c && depth < 64; c = Get<UObject*>(c + Layout::StructSuper), ++depth)
        if (c == cls) return true;
    return false;
}

void ForEach(const std::function<bool(int, UObject*)>& fn) {
    const int n = Num();
    for (int i = 0; i < n; ++i)
        if (auto* o = Obj(i); o && !fn(i, o)) return;
}

UObject* FindClass(std::string_view name) {
    std::string key(name);
    // 缓存命中先校验：蓝图类会随关卡卸载释放（跨区传送后实测），旧指针指向被复用的内存，
    // 在上面遍历函数链表会死循环。名字或"是类"对不上就丢掉缓存重新找
    if (auto it = g_classes.find(key); it != g_classes.end()) {
        auto* c = it->second;
        const auto cn = ClassName(c);
        // 还要在对象表里核对身份：已释放的类对象内存里名字往往还在（回标题 / 读档后蓝图类会重建）
        if (Live(c) && ObjName(c) == name && (cn.ends_with("Class") || cn == "ScriptStruct")) return c;
        g_classes.erase(it);
    }
    // 1.0.1（影塔"几秒一顿"：没加载的类每次整表扫描 24–36 ms）：先只比名字——直接读名字池条目，长度不同立即跳过，
    // 不拼字符串、不进名字缓存锁；名字对上（且没有 _N 后缀）才看它是不是类。约快一个数量级
    auto nameIs = [&](UObject* o) {
        if (Get<uint32_t>(o + Layout::ObjName + 4) != 0) return false;
        const auto idx = Get<uint32_t>(o + Layout::ObjName);
        auto* blk = Get<uint8_t*>(g_blocks + (idx >> 16));
        if (!blk) return false;
        const uint8_t* e = blk + 2 * (idx & 0xFFFF);
        const auto hd = Get<uint16_t>(e);
        const size_t len = hd >> 6;
        if (len != name.size()) return false;
        if (hd & 1) {
            for (size_t i = 0; i < len; ++i) if (Get<uint16_t>(e + 2 + 2 * i) != static_cast<uint8_t>(name[i])) return false;
            return true;
        }
        char buf[256];
        if (len > sizeof buf || !Read(e + 2, buf, len)) return false;
        return std::memcmp(buf, name.data(), len) == 0;
    };
    UObject* hit = nullptr;
    ForEach([&](int, UObject* o) {
        if (!nameIs(o)) return true;
        const auto cn = ClassName(o);
        // 原生类、结构，以及各种蓝图类（BlueprintGeneratedClass、LegoAbilityBlueprintGeneratedClass…）
        if ((cn.ends_with("Class") || cn == "ScriptStruct") && Live(o)) { hit = o; return false; }
        return true;
    });
    if (hit) g_classes.emplace(key, hit);
    return hit;
}

int PropOffset(UObject* st, std::string_view prop) {
    for (int depth = 0; st && depth < 64; st = Get<UObject*>(st + Layout::StructSuper), ++depth) {
        int guard = 0;
        for (auto* f = Get<uint8_t*>(st + Layout::StructChildProps); f && guard < 10000; f = Get<uint8_t*>(f + Layout::FieldNext), ++guard)
            if (Name(Get<uint32_t>(f + Layout::FieldName)) == prop) return Get<int32_t>(f + Layout::PropOffset);
    }
    return -1;
}

}  // namespace pbz::ue
