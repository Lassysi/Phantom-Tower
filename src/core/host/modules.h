// L0：功能模块宿主。运行库（PBZCore.dll）按管道请求加载模块 DLL（影塔等独立 Mod），把服务函数表交给它。
// 模块复制成带时间戳的副本再加载（原文件不被锁，可随时重新编译）；同名模块一个进程只加载一次；模块永不释放。
#pragma once
#include <string>

namespace pbz::modules {

std::string Command(const std::string& cmd, const std::string& path);  // load / list，返回回执 JSON；管道线程调用
void StopAll();  // 运行库停用时在游戏线程调用（在关闭功能之前）
size_t Count();  // 已加载、未停止的模块数
void AutoLoad(const std::wstring& dir);  // 发布版：游戏线程就绪后载入 dir\\PBZTower.dll（原地，不复制）

}  // namespace pbz::modules
