"""Compile reviewed tower-owned strings into a presentation-only dictionary."""
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
DATA = ROOT / "mods/tower/data/localization-tower-en.json"
OUT = ROOT / "mods/tower/src/tower_terms.generated.h"
SPECS = (("items", "tower_economy.h", "kItems"),
         ("powers", "tower_economy.h", "kShentong"),
         ("blessings", "tower_growth.h", "kBlessings"))


def main():
    data = json.loads(DATA.read_text(encoding="utf-8-sig"))["categories"]
    pairs = {}
    for category, filename, array in SPECS:
        src = (ROOT / "mods/tower/src" / filename).read_text(encoding="utf-8-sig")
        body = src.split(f"{array}[] = {{", 1)[1].split("\n};", 1)[0]
        for line in body.splitlines():
            values = re.findall(r'"((?:\\.|[^"\\])*)"', line)
            if len(values) < 3:
                continue
            identity, name, desc = values[:3]
            assert identity in data[category], (category, identity)
            translation = data[category][identity]
            for zh, en in ((name, translation["name_en"]), (desc, translation["description_en"])):
                pairs.setdefault(zh, set()).add(en)
    collisions = {key for key, values in pairs.items() if len(values) != 1}
    safe = {key: next(iter(values)) for key, values in pairs.items() if key not in collisions}
    lines = ["// Generated tower-owned bilingual text; see embed_tower_terms.py.", "#pragma once",
             "#include <array>", "#include <string_view>", "#include <utility>", "namespace tower::language {",
             f"inline constexpr std::array<std::pair<std::string_view, std::string_view>, {len(safe)}> kTowerTerms{{{{"]
    for zh, en in sorted(safe.items()):
        lines.append(f"    {{{json.dumps(zh, ensure_ascii=False)}, {json.dumps(en, ensure_ascii=False)}}},")
    lines += ["}};", "}  // namespace tower::language", ""]
    OUT.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print(f"TOWER_TERMS_EMBEDDED {len(safe)}; ambiguous strings omitted {len(collisions)}")


if __name__ == "__main__":
    main()
