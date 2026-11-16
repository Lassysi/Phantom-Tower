#pragma once
#include <string>
#include <vector>
#include "../../../src/core/services/ue.h"
namespace towerinteractions {
void Begin();
void End();
void Tick();
void AllowNames(const std::vector<std::string>& actorNames);  // 原生遭遇重放：契约里的交互物件放行（空 = 清掉）
void AllowActors(const std::vector<pbz::ue::UObject*>& actors); // 本层存活阶段 / 已核实机制角色的快照；不授予奖励或任务权限
}
