// 驿站大面板（tower_shop_ui.cpp）。全部在游戏线程调用。
#pragma once
#include <array>
#include <functional>
#include <string>
#include <vector>

namespace towershop {
// 打开面板：send(cmd, extra) 发影塔命令；closed 在面板关闭后调用（回准备菜单）。搭不起来返回 false（调用方退回确认框驿站）
bool Open(std::function<void(const std::string&, const std::string&)> send, std::function<void()> closed);
// 背包面板（用户 2026-11-01：Ins → 背包，准备道具行右侧"使用（预装）"；准备阶段才开）
bool OpenBag(std::function<void(const std::string&, const std::string&)> send, std::function<void()> closed);
// 通用菜单面板（v14：主面板四按钮 / 整备 / 领奖第四位刷新令都超出原生确认框的三个按钮）：行文字 + 行右侧按钮 + 底栏四个按钮
struct MenuRow { std::string text, button; std::function<void()> act; bool enabled = true; };
struct MenuSpec {
    std::string what;                                   // 日志里的名字
    std::function<bool()> alive;                        // false = 自动关闭
    std::function<std::string()> title, banner;
    std::function<std::vector<MenuRow>()> rows;
    std::array<std::string, 4> foot{};                  // 底栏按钮文字（从左到右；空 = 隐藏）
    std::array<std::function<bool()>, 4> onFoot{};      // 返回 true = 关闭面板
};
bool OpenMenu(MenuSpec spec, std::function<void()> closed);
bool MenuOpen(const std::string& what);                 // 当前开着的是不是这个菜单
void Defer(std::function<void()> act);                  // 面板按钮里要换面板 / 打开原生界面：放到本帧面板处理完之后再做（不在按钮回调里销毁自己）
void Close();
bool IsOpen();
void Tick();  // 每帧：轮询按钮、刷新显示、商店关闭 / 世界切换时自动关
}  // namespace towershop

namespace towerhud {
void Tick();   // 每帧（内部 0.1 秒刷新）：进塔后左上显示层数 / 层型 / 计时 / 限时与附加挑战；不在塔里就隐藏
void Reset();
}  // namespace towerhud
