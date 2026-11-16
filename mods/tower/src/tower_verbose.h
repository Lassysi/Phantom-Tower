// 开发诊断开关：帧耗时、每秒战斗状态、卡顿帧、原生模态、开战玩家状态等只在开发版记日志。
// 发布版（build_module 带 /DPBZ_RELEASE）不写这些，避免长时间游玩日志膨胀、低配机器逐行写盘卡顿。
#pragma once
namespace towerdev {
#ifdef PBZ_RELEASE
inline constexpr bool kVerbose = false;
#else
inline constexpr bool kVerbose = true;
#endif
// 性能取证构建（发布版 + /DPBZ_PERFLOG）：只打开帧耗时汇总，其余开发日志仍关
#ifdef PBZ_PERFLOG
inline constexpr bool kPerfLog = true;
#else
inline constexpr bool kPerfLog = kVerbose;
#endif
}  // namespace towerdev
