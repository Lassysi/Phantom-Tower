// 固定场地（不属于任何 Boss 的原生场地，影塔自己刷怪）。只用场地模型、雾门和原生刷怪点的位置；
// 原生刷怪点照常静默，不会被 ClaimSeqSpawn 唤醒（tower.cpp TryNativeSpawner 对固定场地直接拒绝）。
// 坐标都是实机读回的世界坐标（2026-11-12 QA 0B9D1A9B / F100AB16，notes/rongshu-venues-20261112.json 第 2 战 interior_live_samples）。
#pragma once
#include <string_view>

namespace tower {

struct FixedVenue {
    std::string_view path;     // 场地键：审计计划 @rongshu 用它替代 Boss 原生场地
    std::string_view name, world, spawner, door;
    double sx, sy, sz;         // 参考刷怪点（世界坐标）：Boss 在这里生成
    double cx, cy, radius;     // 战斗区圆心与半径（软边另加 10 米）
    double px, py, pz, pyaw;   // 玩家落点：往场地里放，面向圆心
    double zmin, zmax;         // 雾门控制点高度
};

// 正道大营·人面榕树（夕阳小径）：入口外直接点传送进内场不触发原生遭遇（实机）；跨缺口线走进去才封门开战。
// 圆：落点 / 中心 / 四边拟合，圆心 (223, 103672)，左右边约 30 米、树根侧约 17 米，取 25 米 + 软边 10 米
inline constexpr FixedVenue kFixedVenues[] = {
    {"fixed:rongshu_zdy", "人面榕树（正道大营）", "PBZ_Zhengdaodaying_WP",
     "BP_AISpawner_C_UAID_E89C25A75120D40303_1620123456", "BP_FogDoor_C_UAID_BCFCE704F72519EA02_1539248210",
     -31.464, 103944.923, 2003410.439,
     223.0, 103672.0, 2500.0,
     -455.013, 103095.996, 2003508.589, 40.3,  // 入口内实测站立点，离圆心约 9 米（原缺口线内侧落点离圆心 20 米，仆从 / 多 Boss 绕玩家生成会挤到门外）
     2003340.189, 2003410.439},
};

inline const FixedVenue* FindFixedVenue(std::string_view path) {
    for (const auto& v : kFixedVenues) if (v.path == path) return &v;
    return nullptr;
}
inline const FixedVenue* FindFixedVenueById(std::string_view id) {  // 计划选项写短 id：rongshu → fixed:rongshu_zdy
    if (id == "rongshu") return &kFixedVenues[0];
    for (const auto& v : kFixedVenues) if (v.path.substr(6) == id) return &v;
    return nullptr;
}

}  // namespace tower
