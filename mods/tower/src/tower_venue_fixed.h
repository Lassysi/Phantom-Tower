// 固定场地运行时（见 tower_venue_fixed.cpp）。接口与 towervenue 对齐，tower.cpp 的 tvenue 门面按本层场地分派。
#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "tower_arena.h"

namespace towerfixed {
bool Is(std::string_view path);   // 这是固定场地键（fixed:...）
bool Active();                     // 本层正在用固定场地
void Arrive(std::string_view path);
void Tick();
bool Ready();
bool Found();
bool MissingRequired();
void* Spawner();                   // 参考刷怪点（只用位置，不唤醒）
bool CloseDoors();
bool DoorsClosed();
void OpenDoors();
bool Boundary(std::vector<tower::arena::P3>* hull, tower::arena::P3* center, double* radius, double* zmin, double* zmax);
void End();
std::string Status();
}  // namespace towerfixed
