"""Produce an actionable, conservative inventory; never alters gameplay.

This enumerates candidate Chinese string literals in UI files. It is an
overestimate (e.g. debug messages and comparisons can be included), not a
proof that every runtime-generated string has been found or translated.
"""
import argparse
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
FILES = ("tower_ui.cpp", "tower_shop_ui.cpp", "tower_points_ui.cpp",
         "tower_widgets.cpp", "tower_title.cpp", "tower_menu_text.h",
         "tower_reward_text.h", "tower_floorplay.h", "tower_rewards.h")
LITERAL = re.compile(r'(?<![A-Za-z0-9_])(?:u8|u|U|L)?"((?:\\.|[^"\\])*)"')
HAN = re.compile(r"[\u3400-\u9fff]")


def inventory(root=ROOT):
    out = []
    for name in FILES:
        source = (root / "mods/tower/src" / name).read_text(encoding="utf-8-sig")
        for number, line in enumerate(source.splitlines(), 1):
            if line.lstrip().startswith("//"):
                continue
            for match in LITERAL.finditer(line):
                value = match.group(1)
                if HAN.search(value):
                    out.append({"file": name, "line": number, "literal": value,
                                "review": "unclassified: may be visible text, control flow, or diagnostics"})
    return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--out", type=Path, required=True)
    args = p.parse_args()
    rows = inventory()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps({"schema": 1, "limitations": "static candidates only; dynamic text and other modules need review",
                                    "count": len(rows), "items": rows}, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"LOCALIZATION_CANDIDATES {len(rows)} unique={len({x['literal'] for x in rows})}")


if __name__ == "__main__":
    main()
