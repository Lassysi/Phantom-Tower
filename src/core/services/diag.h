// L1：异常诊断。记录首处被吞的异常和可能致命的异常：异常码、地址、访问地址、线程、可符号化栈、
// 当时在做什么（功能 / 队列 / ProcessEvent 回调）、最近回调事件、全部实际开关与影塔阶段，另写标准 minidump。
// 出口 %LOCALAPPDATA%\PBZLab\crash\<日期-时间>-<序号>.json / .dmp。
// 异常路径（过滤器）里不分配内存、不访问 UE 对象（只做受保护的内存读）、不等游戏线程；
// 文件由独立写入线程完成，过滤器最多等它 8 秒。致命类异常一律继续交给游戏原有处理（崩溃报告照常生成）。
#pragma once
#include <string>
#include <type_traits>

namespace pbz::diag {

inline thread_local int t_probe = 0;  // >0：本线程正在做受保护的内存探测（ue::Read），探测失败不算异常

void Install();    // 启动早期调用（log::Open 之后）
void Uninstall();  // 核心停用时摘掉向量异常处理
void MarkGameThread();  // 游戏线程每帧调用一次，报告里标明异常是否发生在游戏线程

// 当前在做什么（本线程）；what 必须是静态或长期存在的字符串
const char* SwapWhere(const char* what);  // 设置本线程当前在做什么，返回旧值（模块经函数表用）
struct Scope {
    explicit Scope(const char* what);
    ~Scope();
    const char* prev;
};
// 最近事件环（所有线程共享）：tag 静态字符串，obj/func 可为 UObject 指针（报告时受保护地解析名字）
void Event(const char* tag, const void* obj = nullptr, const void* func = nullptr);
// 现场状态段（任意线程，覆盖同名段）：core 写 "features"，影塔写 "tower"
void Section(const char* key, const std::string& text);

// 受保护调用：fn 抛出 C++ 异常或 SEH 异常时记录并吞掉，返回 false
bool Guard(const char* what, void (*fn)(void*), void* ctx);
template <class F> bool Guarded(const char* what, F&& f) {
    return Guard(what, [](void* p) { (*static_cast<std::remove_reference_t<F>*>(p))(); }, &f);
}

std::string Summary();  // 管道 op "diag"：本进程已记录的异常（JSON）

}  // namespace pbz::diag
