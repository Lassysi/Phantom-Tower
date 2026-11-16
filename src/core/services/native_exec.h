// L1：UFunction 原生执行入口拦截。游戏脚本（Puerts）、蓝图虚拟机和反射调用最终都经过 UFunction 上的原生函数指针
// （本版实机：UFunction+0x128，Codex 2026-10-31 硬件断点确认 UpgradeWeapon / HasEnoughItems / RemovePropsFromBackPack 走这里；
// ProcessEvent 钩子看不到这些调用）。拦截 = 把该指针换成本服务的跳板，卸下即写回原值，完全可逆。
//
// 只在参数来自参数缓冲时（FFrame.Code 为空、FFrame.Node 就是本函数：反射 / 脚本调用）把参数交给处理函数；
// 蓝图字节码直接调用（参数还在字节码里）一律原样放行，不解析、不拦截。
// 处理函数返回 true = 已处理，不调原函数（需要返回值时自己写 result / 参数缓冲里的 ReturnValue）。
// 全部在游戏线程装卸。证据等级：静态 + 离线（假 UFunction / FFrame）；实机未验。
#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "ue.h"

namespace pbz::nx {

using Handler = std::function<bool(ue::UObject* self, uint8_t* params, void* result)>;
using Observer = std::function<void(ue::UObject* self)>;

bool Hook(ue::UObject* func, Handler fn, std::string* why);
void Unhook(ue::UObject* func);
void UnhookAll();
bool Hooked(ue::UObject* func);
// Read-only pre-call notification, including bytecode calls. No argument
// parsing or result replacement; multiple owners coexist with a handler.
int Observe(ue::UObject* func, Observer fn, std::string* why);
void Unobserve(int token);
uint64_t Calls(ue::UObject* func);     // 经过跳板的次数（含放行）
uint64_t Bytecode(ue::UObject* func);  // 其中蓝图字节码调用（原样放行）的次数

inline constexpr int kFuncOffset = 0x128;  // UFunction 原生函数指针（Build 26266538 实机）
inline constexpr int kSlots = 40;  // 1.0.1：影塔一局已用到 21 个，加免费回退的 GetItemNumInfo 后余量太小（只是内部跳板数，不是模块接口）

}  // namespace pbz::nx
