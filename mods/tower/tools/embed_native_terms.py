"""Emit only verified game terms actually present in the tower's growth table.

Resolve collisions conservatively: one Chinese string must map to exactly one
English string. Do not modify keys, runtime logic, or use English as an ID.
"""
import argparse
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TABLE = ROOT / "mods/tower/src/growth_data.generated.h"
OUT = ROOT / "mods/tower/src/native_terms.generated.h"
PATTERN = re.compile(r'^\s*\{"([^"]+)",\s*"((?:\\.|[^"\\])*)"(?:,\s*"((?:\\.|[^"\\])*)")?', re.M)


def clean(s):
    return re.sub(r"\s+", " ", re.sub(r"<[^>]*>", " ", s)).strip().rstrip("。.")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--terms", type=Path, required=True)
    a = p.parse_args()
    data = json.loads(a.terms.read_text(encoding="utf-8-sig"))["categories"]
    source = TABLE.read_text(encoding="utf-8-sig")
    def section(name):
        body = source.split(f"{name}[] = {{", 1)[1].split("\n};", 1)[0]
        def unescape(value):
            return json.loads('"' + value + '"') if value is not None else None
        return [(unescape(m.group(1)), unescape(m.group(2)), unescape(m.group(3))) for m in PATTERN.finditer(body)]

    used = [("weapons", section("kWeapons")), ("abilities", section("kNodes")),
            ("medicines", section("kMedicines")), ("accessories", section("kAccessories"))]
    translations, missing = {}, []
    for kind, entries in used:
        for identity, name, desc in entries:
            cat = "phantom_edges" if kind == "weapons" and identity in data["phantom_edges"] and identity not in data["weapons"] else kind
            item = data[cat].get(identity)
            for field, visible in (("name", name), ("description", desc)):
                if visible is None:
                    continue
                pair = item.get(field, {}) if item else {}
                zh, en = pair.get("zh"), pair.get("en")
                same_text = bool(zh and re.sub(r"\s+", "", clean(zh["text"])) == re.sub(r"\s+", "", clean(visible)))
                # The growth generator displays the row name, not the localized
                # name, for weapons and medicine. A unique same-ID source row
                # with an exact row-name match proves the linkage even when the
                # row label and the game's localized Chinese name differ.
                same_unique_row = bool(field == "name" and item and len(item.get("rows", [])) == 1
                                       and item["rows"][0] == visible)
                if not zh or not en or not (same_text or same_unique_row):
                    missing.append((cat, identity, field))
                    continue
                translations.setdefault(visible, set()).add(clean(en["text"]))
    bosses = (ROOT / "mods/tower/src/boss_pool.generated.h").read_text(encoding="utf-8-sig")
    for path, visible in re.findall(r'^\s*\{"([^"]+)",\s*"([^"]+)"', bosses, re.M):
        item = data.get("enemies", {}).get(path, {})
        pair = item.get("name", {})
        zh, en = pair.get("zh"), pair.get("en")
        if not zh or not en or zh["text"] != visible:
            missing.append(("enemies", path, "name"))
            continue
        translations.setdefault(visible, set()).add(clean(en["text"]))
    translations = {k: next(iter(v)) for k, v in translations.items() if len(v) == 1}
    lines = ["// Generated from verified game-localized keys; see embed_native_terms.py.", "#pragma once", "#include <array>",
             "#include <string_view>", "#include <utility>", "namespace tower::language {",
             f"inline constexpr std::array<std::pair<std::string_view, std::string_view>, {len(translations)}> kNativeTerms{{{{"]
    for zh, en in sorted(translations.items()):
        lines.append(f"    {{{json.dumps(zh, ensure_ascii=False)}, {json.dumps(en, ensure_ascii=False)}}},")
    lines += ["}};", "}  // namespace tower::language", ""]
    OUT.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print(f"NATIVE_TERMS_EMBEDDED {len(translations)}; unresolved source fields {len(missing)}")
    for item in missing[:15]:
        print("UNRESOLVED", *item)


if __name__ == "__main__":
    main()
