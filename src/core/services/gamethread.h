// L1：游戏线程。把 GameEngine 实例的虚表指针换成副本，替换 Tick 一项，原 Tick 之后执行队列与功能 Tick。
// 只影响这一个对象，卸载时写回原虚表指针即可完全还原。
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace pbz::gamethread {

bool Install(std::string* why);
void Uninstall();
bool Ready();
bool Healthy();                                // 挂接仍然有效；被别人换掉或试用撤回时返回 false（Ready 同时变 false）
unsigned long long Frames();                   // HookTick 已执行的帧数（诊断用）
uint32_t CostUs();                             // 核心每帧耗时平滑值（微秒）
uint32_t PeakUs();                             // 上次查询以来的峰值（微秒），读后清零
void Post(std::function<void()> fn);          // 下一帧在游戏线程执行
void Post(std::function<void()> fn, const char* tag);  // tag：来源（静态字符串），异常报告里标明是谁排的队
void SetFrameCallback(std::function<void(float)> fn);  // 每帧执行（功能 Tick）

}  // namespace pbz::gamethread
