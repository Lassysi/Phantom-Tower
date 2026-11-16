#include "abshook.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cstring>
#include <vector>

extern "C" {
#include "../third_party/minhook/src/hde/hde64.h"
}

namespace pbz::abshook {
namespace {

constexpr int kJmp = 14;  // FF 25 00 00 00 00 + 8 字节地址

void WriteAbsJmp(uint8_t* at, const void* to) {
    at[0] = 0xFF; at[1] = 0x25;
    memset(at + 2, 0, 4);
    memcpy(at + 6, &to, 8);
}

// 冻结本进程除当前线程外的所有线程；若有线程正停在被改写的字节里，把它挪到跳板对应位置
std::vector<HANDLE> Freeze(const Hook& h, bool installing) {
    std::vector<HANDLE> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    THREADENTRY32 te{sizeof te};
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) == static_cast<DWORD>(-1)) { CloseHandle(t); continue; }
        CONTEXT c{};
        c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(t, &c)) {
            auto ip = reinterpret_cast<uint8_t*>(c.Rip);
            if (installing && ip >= h.target && ip < h.target + h.len) c.Rip = reinterpret_cast<DWORD64>(h.tramp + (ip - h.target));
            else if (!installing && ip >= h.tramp && ip < h.tramp + h.len) c.Rip = reinterpret_cast<DWORD64>(h.target + (ip - h.tramp));
            SetThreadContext(t, &c);
        }
        out.push_back(t);
    }
    CloseHandle(snap);
    return out;
}

void Thaw(std::vector<HANDLE>& ts) {
    for (HANDLE t : ts) { ResumeThread(t); CloseHandle(t); }
    ts.clear();
}

// 绝对跳转槽：FF 25 disp32（槽在 rip+disp）、48 B8 imm64 FF E0、49 BB imm64 41 FF E3
uint8_t* JumpSlot(uint8_t* p) {
    if (p[0] == 0xFF && p[1] == 0x25) return p + 6 + *reinterpret_cast<int32_t*>(p + 2);
    if (p[0] == 0x48 && p[1] == 0xB8 && p[10] == 0xFF && p[11] == 0xE0) return p + 2;
    if (p[0] == 0x49 && p[1] == 0xBB && p[10] == 0x41 && p[11] == 0xFF && p[12] == 0xE3) return p + 2;
    return nullptr;
}
bool Readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION m{};
    if (!VirtualQuery(p, &m, sizeof m) || m.State != MEM_COMMIT || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    return static_cast<const uint8_t*>(p) + n <= static_cast<const uint8_t*>(m.BaseAddress) + m.RegionSize;
}

}  // namespace

bool LooksHooked(const void* target) {
    auto* p = static_cast<const uint8_t*>(target);
    if (!p || !Readable(p, 16)) return false;
    return p[0] == 0xE9 || p[0] == 0xEB || JumpSlot(const_cast<uint8_t*>(p)) != nullptr;
}

std::string JumpOwner(const void* target) {
    auto* p = static_cast<uint8_t*>(const_cast<void*>(target));
    for (int hop = 0; p && hop < 6 && Readable(p, 16); ++hop) {
        if (p[0] == 0xE9) p = p + 5 + *reinterpret_cast<int32_t*>(p + 1);
        else if (p[0] == 0xEB) p = p + 2 + static_cast<int8_t>(p[1]);
        else if (auto* slot = JumpSlot(p); slot && Readable(slot, 8)) p = *reinterpret_cast<uint8_t**>(slot);
        else break;
    }
    HMODULE m = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (!p || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(p), &m) ||
        !GetModuleFileNameW(m, path, MAX_PATH)) return "动态内存";
    std::wstring w(path);
    w = w.substr(w.find_last_of(L"\\/") + 1);
    std::string out(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), static_cast<int>(out.size()), nullptr, nullptr);
    if (!out.empty()) out.pop_back();
    return out;
}

bool Prepare(Hook& h, void* target, void* detour, std::string* why) {
    auto* p = static_cast<uint8_t*>(target);
    // 跳转桩（函数开头就是 jmp rel32 / rel8，后面是填充或混淆字节，实测尘埃入账函数如此）：顺着跳到真正的函数体再钩。
    // 桩只做跳转，钩目标处与钩桩等价；最多跟 4 层
    for (int hop = 0; hop < 4; ++hop) {
        if (p[0] == 0xE9) p = p + 5 + *reinterpret_cast<int32_t*>(p + 1);
        else if (p[0] == 0xEB) p = p + 2 + static_cast<int8_t>(p[1]);
        else break;
    }
    if (uint8_t* slot = JumpSlot(p)) {  // 别人的绝对跳转：叠加模式，只换槽
        if (!Readable(slot, 8)) { *why = "其他 Mod 的跳转槽不可读"; return false; }
        h.target = p; h.slot = slot; h.prev = *reinterpret_cast<void**>(slot); h.tramp = static_cast<uint8_t*>(h.prev);
        h.len = 0; h.detour = detour;
        return h.prev != nullptr;
    }
    int len = 0;
    while (len < kJmp) {
        hde64s d{};
        const unsigned n = hde64_disasm(p + len, &d);
        if (!n || (d.flags & F_ERROR)) { *why = "反汇编失败"; return false; }
        const bool ripRel = (d.flags & F_MODRM) && d.modrm_mod == 0 && d.modrm_rm == 5;
        const bool branch = (d.flags & F_RELATIVE) || d.opcode == 0xC3 || d.opcode == 0xC2 || d.opcode == 0xCC;
        if (ripRel || branch) { *why = "函数开头含 RIP 相对指令或跳转，不能远跳"; return false; }
        len += static_cast<int>(n);
    }
    if (len > static_cast<int>(sizeof h.orig)) { *why = "序言过长"; return false; }
    auto* tr = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tr) { *why = "跳板内存申请失败"; return false; }
    memcpy(tr, p, len);
    WriteAbsJmp(tr + len, p + len);
    FlushInstructionCache(GetCurrentProcess(), tr, 64);
    h.target = p;
    h.tramp = tr;
    h.len = len;
    h.detour = detour;
    memcpy(h.orig, p, len);
    return true;
}

bool Apply(Hook& h, bool on) {
    if (!h.target || h.on == on) return true;
    if (h.slot) {
        void* const want = on ? h.detour : h.prev;
        void* const expect = on ? h.prev : h.detour;
        if (*reinterpret_cast<void* volatile*>(h.slot) != expect) return false;  // 槽已被别人改过：不动，保留现场
        DWORD old;
        if (!VirtualProtect(h.slot, 8, PAGE_EXECUTE_READWRITE, &old)) return false;
        if ((reinterpret_cast<uintptr_t>(h.slot) & 7) == 0) InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(h.slot), want);
        else { auto ts = Freeze(h, on); memcpy(h.slot, &want, 8); Thaw(ts); }
        VirtualProtect(h.slot, 8, old, &old);
        FlushInstructionCache(GetCurrentProcess(), h.slot, 8);
        h.on = on;
        return true;
    }
    uint8_t patch[32];
    if (on) {
        WriteAbsJmp(patch, h.detour);
        memset(patch + kJmp, 0x90, h.len - kJmp);
    } else {
        memcpy(patch, h.orig, h.len);
    }
    auto ts = Freeze(h, on);
    DWORD old;
    bool ok = VirtualProtect(h.target, h.len, PAGE_EXECUTE_READWRITE, &old) != 0;
    if (ok) {
        memcpy(h.target, patch, h.len);
        VirtualProtect(h.target, h.len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), h.target, h.len);
        h.on = on;
    }
    Thaw(ts);
    return ok;
}

}  // namespace pbz::abshook
