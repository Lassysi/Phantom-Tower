"""把修改器"坐标"里存的点导入成影塔的场地标定（%LOCALAPPDATA%\\PBZTower\\arenas.local.txt）。
在修改器里站好位置、按名字保存：
  擂台1 … 擂台5（或 擂台一 / 擂台·区一）→ 第 N 区的通用擂台
  Boss 名字里的一段（如 魔渊、花千笑、大师兄）→ 这只 Boss 的原生场地（只认要回原场地打的 Boss）
用法：python -I mods/tower/tools/import_marks.py [--list]    --list 只列出还要标定哪些地方
"""
import json
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
LOCAL = Path(os.environ["LOCALAPPDATA"])
POSITIONS = LOCAL / "PBZLab/positions.tsv"
MARKS = LOCAL / "PBZTower/arenas.local.txt"
ZONES = {"一": 1, "二": 2, "三": 3, "四": 4, "五": 5}


def scene_bosses():
    """要回原生场地打的 Boss：身份 → (中文名, 原生地图)；同一身份多个版本只列一次"""
    out = {}
    for b in json.loads((ROOT / "mods/tower/data/bosses.json").read_text(encoding="utf-8"))["items"]:
        if b["class"] == "B" or "native_scene_required" in (b.get("tags") or []):
            out.setdefault(b["identity"], (b["name"], b.get("native_map") or b.get("zone_map") or "?", b.get("chapter_order") or 99))
    return out


def arenas():
    return json.loads((ROOT / "mods/tower/data/arenas.json").read_text(encoding="utf-8"))["items"]


def load_marks():
    marks = {}
    if MARKS.exists():
        for line in MARKS.read_text(encoding="utf-8").splitlines():
            c = line.split()
            if len(c) >= 5 and not c[0].startswith("#"):
                marks[c[0]] = c[1:]
    return marks


def zone_of(name):
    m = re.fullmatch(r"擂台\s*[·.\-]?\s*(?:区)?\s*([一二三四五1-5])", name.strip())
    if not m:
        return None
    z = m.group(1)
    return ZONES.get(z) or int(z)


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    bosses = scene_bosses()
    marks = load_marks()
    if "--list" in sys.argv:
        first = {}
        for a in arenas():
            first.setdefault(a["zone"], a)
        print("通用擂台（每区一处，修改器坐标里存成 擂台1…擂台5）：")
        for z, a in sorted(first.items()):
            print(f"  擂台{z}  建议在 {a['map']}（钟 {a['approach_bell']} 附近）  {'已标定' if a['id'] in marks else '未标定'}")
        print("Boss 原生场地（存成 Boss 名字）：")
        for ident, (name, m, order) in sorted(bosses.items(), key=lambda kv: kv[1][2]):
            print(f"  {name:20} {m:16} {'已标定' if 'boss:' + ident in marks else '未标定'}")
        return
    if not POSITIONS.exists():
        raise SystemExit(f"没有 {POSITIONS}：先在修改器的坐标里保存位置")
    first = {}
    for a in arenas():
        first.setdefault(a["zone"], a["id"])
    added = []
    for line in POSITIONS.read_text(encoding="utf-8").splitlines():
        c = line.split("\t")
        if len(c) != 9:
            continue
        _, level, name, x, y, z, _, yaw, _ = c
        key = None
        if (zn := zone_of(name)) and zn in first:
            key = first[zn]
        else:
            hit = {i for i, (n, _, _) in bosses.items() if name.strip() and name.strip() in n}
            if len(hit) == 1:
                key = "boss:" + hit.pop()
            elif len(hit) > 1:
                print(f"  “{name}” 对应多只 Boss，写全一点：", "、".join(bosses[i][0] for i in hit))
        if key:
            marks[key] = [f"{float(x):.1f}", f"{float(y):.1f}", f"{float(z):.1f}", f"{float(yaw):.1f}", level]
            added.append(f"{name} → {key}（{level}）")
    MARKS.parent.mkdir(parents=True, exist_ok=True)
    MARKS.write_text("# 影塔擂台 / Boss 原生场地（游戏内标定）：id x y z yaw 关卡\n" +
                     "".join(f"{k} {' '.join(v)}\n" for k, v in sorted(marks.items())), encoding="utf-8")
    print(f"导入 {len(added)} 个：")
    for a in added:
        print("  " + a)
    print(f"写入 {MARKS}")


if __name__ == "__main__":
    main()
