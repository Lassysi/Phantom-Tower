// L1：远跳钩子（MinHook 的后备）。游戏 exe 周围的空闲内存被切成 <64KB 的碎片，MinHook 申请不到 ±2GB 内的跳板，
// 这里改在目标处写 14 字节绝对跳转 FF 25 00000000 <addr64>，跳板放在任意地址。
// 只接受开头 ≥14 字节内没有 RIP 相对寻址 / 相对跳转 / ret 的函数（UE 函数序言一般是 push + sub rsp，满足）。
#pragma once
#include <cstdint>
#include <string>

namespace pbz::abshook {

struct Hook {
    uint8_t* target = nullptr;
    uint8_t* tramp = nullptr;   // 原指令 + 跳回，作为"原函数"调用
    uint8_t orig[32] = {};
    int len = 0;
    void* detour = nullptr;
    bool on = false;
    // 3.0.0 兼容（UE4SS 等先挂了同一函数）：入口已是别人的绝对跳转（FF 25 / mov rax,imm64; jmp rax / mov r11,imm64; jmp r11）时
    // 不改指令，只换跳转槽里的 8 字节地址；"原函数" = 换下来的旧地址（别人的钩子照样接着跑）。slot 非空即叠加模式，len = 0
    uint8_t* slot = nullptr;
    void* prev = nullptr;
};
// 入口是不是别的 Mod 写的跳转（E9 / EB / FF 25 / mov rax|r11 + jmp）。用于"序言不匹配"时区分游戏改版和被其他 Mod 挂接
bool LooksHooked(const void* target);
// 跳转最终落到哪个模块（文件名；不在任何模块里返回"动态内存"）。日志用
std::string JumpOwner(const void* target);

bool Prepare(Hook& h, void* target, void* detour, std::string* why);  // 建跳板，不改目标
bool Apply(Hook& h, bool on);                                           // 写入/还原目标字节（冻结其他线程）

}  // namespace pbz::abshook
