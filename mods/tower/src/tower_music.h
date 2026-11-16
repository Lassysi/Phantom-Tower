// 影塔层内音乐（tower_music.cpp）：本机 mp3 循环播放，引擎在合适的时机开 / 停。只本地自用，不进公开包。
#pragma once
#include <string>

namespace towermusic {
void Play(const std::string& why);   // 开始循环（已在放同一首就不重来）
void Stop(const char* why);          // 停止（没在放什么也不做）
bool Playing();
void Shutdown();                     // 模块卸载：停掉并收线程
}  // namespace towermusic
