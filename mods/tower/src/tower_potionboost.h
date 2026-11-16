// Game-thread-only potion coefficient adjustment while the tower run is active.
#pragma once
namespace towerpotion {
bool Sync(float lifeMultiplier, bool allowLoad);  // allowLoad：允许本帧阻塞加载药效资源（战斗中传 false）
void Clear();
}
