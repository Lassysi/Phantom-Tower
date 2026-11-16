#pragma once
#include <functional>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "../../services/ue.h"

namespace pbz::enemies {
struct Request {
    std::string command;        // status / spawn / clear
    std::string path;           // exact shared/enemies.json path, never a short id
    std::string requestId;      // echoed, caller supplies unique ID
    int count = 1;             // 1..8 per request; no session/lifetime total cap
    double distanceCm = 600;   // 200..2000; footprint can increase minimum distance
    std::string faction;        // enemy（默认）/ ally / chaos / enemy2 / enemy3 → 游戏阵营 Faction.Enemy / Player / Chaotic / Enemy2 / Enemy3
};
using Reply = std::function<void(std::string)>;
struct ShutdownResult { bool ok = false; std::string json; };

// Safe from an IPC thread. Always posts engine work to the existing GameThread
// queue. Callback executes on that queue (or immediately on validation failure).
// Host must stop accepting commands before unload; call StopOnGameThread before
// removing UE/PE services. Spawn callers must run existing SaveGuard first.
void Submit(Request request, Reply reply);
// GameThread only. Stops accepting commands, cancels earlier queued requests,
// destroys only owned spawns, and returns JSON including any cleanup failure.
ShutdownResult StopOnGameThread();
// 本工具生成且改过阵营（非 Faction.Enemy）的角色 → 阵营标签；游戏线程调用（血条按阵营上色用）
std::vector<std::pair<ue::UObject*, std::string>> OwnedFactions();
// 本工具生成、仍存活、名录里标为 boss 的角色；游戏线程调用（我方 Boss 主动挂 Boss 血条用）
std::vector<ue::UObject*> OwnedBosses();
// 本工具生成、仍存活的全部角色；游戏线程调用
std::vector<ue::UObject*> OwnedActors();
uint64_t NativeStatsRevision(ue::UObject* actor); // GameThread only; native stat rebuild invalidates HP presentation caches.
// 本工具生成、仍存活、request_id 不以 prefix 开头的角色（排除 Mod 自己管强度的，如影塔 "tower-"）；游戏线程调用
std::vector<ue::UObject*> OwnedUntagged(const std::string& prefix);
// 只清 request_id 以 prefix 开头的生成物（影塔用 "tower-"）；游戏线程调用，返回仍未销毁的个数
size_t ClearTagged(const std::string& prefix);
// 清掉除 prefix 开头之外的全部生成物（修改器"全部还原"保留影塔的 Boss）；游戏线程调用
size_t ClearExcept(const std::string& prefix);
}  // namespace pbz::enemies
