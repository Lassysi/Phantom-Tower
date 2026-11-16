// L1：UObject::ProcessEvent —— 反射函数调用的总入口。
// 地址取 Default__Object 虚表第 76 项（实机核对：UE5.7 序言，AActor::ProcessEvent 内部调用它），不写死地址。
// 提供：调用 UFunction、按函数拦截、调用记录器（玩家按鼠标前侧键打标记，导出标记前 5 秒的调用序列）。
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "ue.h"

namespace pbz::pe {

bool Init(std::string* why);
bool Ready();
std::string ForeignHook();  // 先挂了 ProcessEvent 的其他模块文件名（没有为空）                          // 钩子已建好（可拦截）
int InFlight();                        // 当前停留在拦截代码里的调用数（卸载等待用）
ue::UObject* FindFunction(ue::UObject* cls, std::string_view name);  // 沿 Super 链找 UFunction
std::vector<ue::UObject*> FunctionsWithPrefix(ue::UObject* cls, std::string_view prefix);  // 本类（不含父类）名字以 prefix 开头的函数
void Call(ue::UObject* obj, ue::UObject* func, void* params);

// 拦截：返回 true 表示吞掉这次调用（不执行原函数）。只在游戏线程回调。
using Interceptor = std::function<bool(ue::UObject* obj, void* params)>;
void Intercept(ue::UObject* func, Interceptor fn);
void RemoveIntercept(ue::UObject* func);

// 观察：原函数执行前回调，不能改结果；同一函数可有多个观察者，互不覆盖（Intercept 每个函数只能有一个）。
using Observer = std::function<void(ue::UObject* obj, void* params)>;
int Observe(ue::UObject* func, Observer fn);  // 返回令牌
void Unobserve(int token);

// 研究工具：观察名字含 classSub 的所有类（含蓝图）上的函数 func。
// mode 0 = 取消；1 = 记录每次调用和返回值；2 = 强制返回 true；3 = 强制返回 false（仅 bool 返回值）
std::string Watch(const std::string& classSub, const std::string& func, int mode);

// 记录器
void Trace(bool on);
bool Tracing();
void Mark(const std::string& label);   // 导出标记前 5 秒的调用
std::string DumpCounts();              // 导出全部函数调用次数，返回文件路径
void Shutdown();                       // 卸载前：停记录器并等后台写文件结束
void PollHotkey();                     // 游戏线程每帧调用：鼠标前侧键 = 打标记（core.ini 可改）

}  // namespace pbz::pe
