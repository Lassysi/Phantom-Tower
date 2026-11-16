"""Build a provenance-preserving bilingual tower vocabulary from game data.

The localization export and unpacked tables stay local. Unknown names and
descriptions remain null; never fabricate an English name from an internal ID.
This tool does not alter gameplay, saves, or the shipping UI.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path


SPECS = (
    ("weapons", "DT_WeaponData.json", "ItemId", "WeaponName", None),
    ("phantom_edges", "DT_ShaqiWeaponData.json", "ItemId", "WeaponName", None),
    ("medicines", "DT_MedicineTable.json", "ItemId", "Name", "AbilityDesc"),
    ("accessories", "DT_Accessory.json", "ItemId", "AccessoryName", "AccessoryDesc"),
    ("abilities", "DT_AbilityNodesTable.json", "AbilityNodeItemID", "AbilityNodeName", "AbilityNodeDesc"),
)


def resolve(ref, localization):
    if isinstance(ref, list):
        ref = ref[0] if ref else None
    if not isinstance(ref, dict):
        return None
    namespace, key = ref.get("Namespace"), ref.get("Key")
    if not isinstance(namespace, str) or not isinstance(key, str) or not key:
        return None
    text = localization.get(namespace, {}).get(key)
    if not isinstance(text, str) or not text.strip() or text == key:
        return None
    return {"namespace": namespace, "key": key, "text": text}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def boss_names(enemies, zh, en):
    """Use an exact catalog key or a unique exact-match official name key."""
    result = {}
    for row in enemies:
        path, name = row.get("path"), row.get("name_zh")
        if not isinstance(path, str) or not path:
            continue
        match = re.search(r"中文名=Game\.zh-Hans\.json:([^:；]+):([^；\s]+)", row.get("note") or "")
        ref = {"Namespace": match.group(1), "Key": match.group(2)} if match else None
        chinese, english = resolve(ref, zh), resolve(ref, en)
        verified = chinese is not None and chinese["text"] == name
        if not verified and isinstance(name, str):
            candidates = []
            for namespace, entries in zh.items():
                for key, value in entries.items():
                    if (value == name and key.startswith(("BossName_", "EnemyName_", "Other_All_BossName_"))
                            and resolve({"Namespace": namespace, "Key": key}, en)):
                        candidates.append({"Namespace": namespace, "Key": key})
            # Multiple official keys are safe only if they give identical English.
            english_values = {resolve(candidate, en)["text"] for candidate in candidates}
            if candidates and len(english_values) == 1:
                chinese, english = resolve(candidates[0], zh), resolve(candidates[0], en)
                verified = True
        if path in result:
            raise ValueError(f"Duplicate enemy path: {path}")
        result[path] = {"catalog_name_zh": name, "name": {"zh": chinese if verified else None,
                                                             "en": english if verified else None}}
    return result


def build(tables, zh, en):
    result = {}
    for category, filename, id_key, name_key, description_key in SPECS:
        rows = json.loads((tables / filename).read_text(encoding="utf-8-sig"))[0]["Rows"]
        records = {}
        for row_name, row in rows.items():
            identity = row.get(id_key)
            if not isinstance(identity, str) or not identity:
                continue
            record = {"row": row_name}
            for field, ref in (("name", row.get(name_key)), ("description", row.get(description_key))):
                record[field] = {"zh": resolve(ref, zh), "en": resolve(ref, en)}
            records.setdefault(identity, []).append(record)
        # Preserve all table rows. A caller can select a proven variant; it
        # must not silently take the first row for an ambiguous identity.
        for identity, variants in records.items():
            fields = {}
            for field in ("name", "description"):
                fields[field] = {}
                for lang in ("zh", "en"):
                    values = [v[field][lang] for v in variants if v[field][lang] is not None]
                    keys = {(v["namespace"], v["key"], v["text"]) for v in values}
                    if len(keys) == 1:
                        fields[field][lang] = values[0]
                    elif category == "phantom_edges" and field == "name":
                        # A duplicated weapon ID can include a character name
                        # (e.g. Blaze) and the actual Phantom Edge name. Only
                        # select an unambiguous PhantomEdges_* UI name key.
                        weapon_values = [v for v in values if v["key"].startswith("PhantomEdges_")]
                        unique = {(v["namespace"], v["key"], v["text"]) for v in weapon_values}
                        fields[field][lang] = weapon_values[0] if len(unique) == 1 else None
                    else:
                        fields[field][lang] = None
            records[identity] = {"rows": [v["row"] for v in variants], **fields,
                                 "variants": variants if len(variants) > 1 else []}
        result[category] = records
    return result


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--tables", type=Path, required=True)
    p.add_argument("--zh", type=Path, required=True)
    p.add_argument("--en", type=Path, required=True)
    p.add_argument("--enemies", type=Path, help="Existing class/path catalog with localization-key provenance")
    p.add_argument("--out", type=Path, required=True)
    a = p.parse_args()
    zh = json.loads(a.zh.read_text(encoding="utf-8-sig"))
    en = json.loads(a.en.read_text(encoding="utf-8-sig"))
    data = build(a.tables, zh, en)
    source = {"zh": sha(a.zh), "en": sha(a.en),
              "tables": {spec[1]: sha(a.tables / spec[1]) for spec in SPECS}}
    if a.enemies:
        data["enemies"] = boss_names(json.loads(a.enemies.read_text(encoding="utf-8-sig")), zh, en)
        source["enemies"] = sha(a.enemies)
    payload = {"schema": 1, "provenance": source, "categories": data}
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    for category, entries in data.items():
        names = sum(1 for x in entries.values() if x["name"]["en"])
        descriptions = sum(1 for x in entries.values() if x.get("description", {}).get("en"))
        print(f"{category}: {len(entries)} IDs; English names {names}; descriptions {descriptions}")


if __name__ == "__main__":
    main()
