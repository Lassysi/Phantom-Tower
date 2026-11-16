# 由 Codex 的场地静态提取（mods/tower/data/venues.json）生成影塔内置场地表（src/venues.generated.h）。
# 只取运行时要用的：出生点与雾门的 actor 名（运行时按名字找，读实际位置）、出生点静态坐标（核对运行时偏移）、
# 激活雾门的门名、内缩后的凸包（静态 level 坐标，运行时按出生点实际偏移平移）、门控制点高度范围、出生点到门的最远距离。
# 坐标高于 50 万 uu 的标为独立战斗空间（sky）：可能只在剧情激活的数据层里，运行时找不到对象就判场地不可用。
# 用法：python -I mods/tower/tools/gen_venues.py [--check]
import hashlib
import json
import math
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC = ROOT / "data" / "venues.json"
OUT = ROOT / "src" / "venues.generated.h"


def esc(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def main():
    raw = SRC.read_bytes()
    data = json.loads(raw.decode("utf-8"))
    if "items" not in data:
        sys.exit("venues.json: no items")
    rows, hulls, doors = [], [], []
    for e in data["items"]:
        sp = e.get("spawner") or {}
        pose = sp.get("pose") or {}
        pos = pose.get("position_world") or pose.get("position_level")
        if not sp.get("name") or not pos:
            continue
        fd = (e.get("fog_doors") or {}).get("FogDoorsToActivate") or []
        names = [d["name"] for d in fd if d.get("name")]
        # 出生点到门控制点的最远水平距离（门点按 level 坐标）
        far = 0.0
        for d in fd:
            for q in d.get("spline_points_level") or []:
                x, y = (q["X"], q["Y"]) if isinstance(q, dict) else (q[0], q[1])
                far = max(far, math.hypot(x - pos[0], y - pos[1]))
        h = e.get("arena_hull") or {}
        poly = h.get("xy_polygon_world") or h.get("xy_polygon_level") or []
        zr = h.get("door_control_point_z_range_world") or h.get("door_control_point_z_range_level") or [0, 0]
        hull_at = len(hulls) // 2
        for q in poly:
            x, y = (q["X"], q["Y"]) if isinstance(q, dict) else (q[0], q[1])
            hulls += [x, y]
        door_at = len(doors)
        doors += names
        sky = max(abs(v) for v in pos) > 500000
        multi = bool((e.get("mob_group") or {}).get("multi_boss_identity"))
        rows.append((e["boss_class"], e.get("world") or "", sp["name"], pos, pose.get("yaw_degrees_level") or 0.0,
                     door_at, len(names), hull_at, len(poly), h.get("area_m2") or 0.0, zr, far, sky, multi,
                     bool(pose.get("world_coordinates_verified_static"))))
    digest = hashlib.sha256(raw).hexdigest()
    lines = [
        "// 生成文件，勿手改：python -I mods/tower/tools/gen_venues.py",
        f"// 来源 mods/tower/data/venues.json sha256={digest}（证据等级：静态；运行时按名字找 actor、读实际位置）",
        "#pragma once",
        "#include <string_view>",
        "namespace tower {",
        f'inline constexpr std::string_view kVenueSource = "{digest}";',
        "// 一个精确 Boss 类的原生场地：spawner = 原生出生点 actor 名；sx sy sz yaw = 出生点静态坐标（多数为 level 坐标，运行时核对偏移）；",
        "// doors = kVenueDoors 里从 doorAt 起 doorN 个激活雾门 actor 名；hull = kVenueHull 里从 hullAt 起 hullN 个 XY 点（内缩 60uu 的凸包）；",
        "// area = 凸包面积 m²；zmin zmax = 门控制点高度；reach = 出生点到门的最远水平距离；sky = 独立战斗空间（高空）；multi = 原生组内多 Boss 身份",
        "struct VenueDef { std::string_view path, world, spawner; double sx, sy, sz, yaw; int doorAt, doorN, hullAt, hullN; double area, zmin, zmax, reach; bool sky, multi, worldOk; };",
        "inline constexpr VenueDef kVenues[] = {",
    ]
    for (path, world, spawner, pos, yaw, da, dn, ha, hn, area, zr, far, sky, multi, ok) in rows:
        lines.append(f'    {{"{esc(path)}", "{esc(world)}", "{esc(spawner)}", {pos[0]:.1f}, {pos[1]:.1f}, {pos[2]:.1f}, {yaw:.1f}, {da}, {dn}, {ha}, {hn}, '
                     f'{area:.1f}, {zr[0]:.1f}, {zr[1]:.1f}, {far:.1f}, {str(sky).lower()}, {str(multi).lower()}, {str(ok).lower()}}},')
    lines.append("};")
    lines.append("inline constexpr std::string_view kVenueDoors[] = {")
    for i in range(0, len(doors), 2):
        lines.append("    " + " ".join(f'"{esc(n)}",' for n in doors[i:i + 2]))
    if not doors:
        lines.append('    "",')
    lines.append("};")
    lines.append("inline constexpr double kVenueHull[] = {")
    for i in range(0, len(hulls), 8):
        lines.append("    " + " ".join(f"{v:.1f}," for v in hulls[i:i + 8]))
    if not hulls:
        lines.append("    0,")
    lines.append("};")
    lines.append("}  // namespace tower")
    text = "\n".join(lines) + "\n"
    if "--check" in sys.argv:
        if not OUT.exists() or OUT.read_text(encoding="utf-8") != text:
            sys.exit(f"{OUT.name} is stale: run python -I mods/tower/tools/gen_venues.py")
        print(f"{OUT.name}: up to date ({len(rows)} venues)")
        return
    OUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"{OUT.name}: {len(rows)} venues, {len(doors)} doors, {len(hulls) // 2} hull points, sha256 {digest[:12]}")


if __name__ == "__main__":
    main()
