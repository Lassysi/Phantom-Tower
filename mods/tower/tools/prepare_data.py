"""Rebuild offline tower preparation tables. Never opens a process or writes engine sources.

Design decisions are proposals unless present in tower-floors.md; static catalog
evidence does not establish spawnability, safe arenas, or native reward grants.
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import json
import re
import statistics
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / "mods/tower/data"
DATE = "2026-10-30"
TABLE = "notes/dump/tables/ProjectPBZ/Content/PBZContent/DataTable/"
PLAN = "notes/tower-floors.md"
ZONES = {
    1: ("XiGu", "PangZhen", "HouJie"),
    2: ("FuJiZhiLin", "MeiGuHu", "MeiGu_YiJi"),
    3: ("LuJiaCun", "ZhengDaoDaYing"),
    4: ("DengDingShanDao", "XiMeng"),
    5: ("DuanHunYa", "DuanHunShan"),
}
MAP_ZONE = {m: z for z, maps in ZONES.items() for m in maps}
LOOT_MAP = {"xigu": "XiGu", "lujiaccun": "LuJiaCun", "huanshushi": "MeiGu_YiJi"}
LOOT_MAP.update({m.lower(): m for m in MAP_ZONE})


def read(path):
    return json.loads((ROOT / path).read_text(encoding="utf-8-sig"))


def table(name):
    path = TABLE + name + ".json"
    return path, read(path)[0]["Rows"]


def provenance(*sources, evidence="static", **fields):
    return {**fields, "evidence": evidence, "source": list(dict.fromkeys(sources))}


def document(items, source):
    return {"schema": 1, "source": source, "generated": DATE, "items": items}


def boss_stem(path):
    return path.rsplit("/", 1)[-1].split(".")[0]


def identity(stem):
    short = stem.removeprefix("BP_Monster_")
    if short.startswith("MoTian_LongZhu_"):
        return "gold_mask_guard"
    if short.startswith("YuCong"):
        return "YuCong"
    if short.startswith("EMei_Sword"):
        return "EMei_Sword"
    if short in ("HuaShan_Blue", "HuaShan_JianOu"):
        return "HuaShan_Blue"
    if short == "MBossVariants":
        return "MBoss"
    return re.sub(r"_(P2|FirstTime|Ending|JYX|LengTu|Base)$", "", short)


class Sources:
    def __init__(self):
        self.enemies = read("shared/enemies.json")
        self.items = {x["id"]: x for x in read("shared/items.json")}
        self.bells = read("shared/bells.json")
        self.classes = {f.stem: f for f in (ROOT / "notes/dump/enemies").rglob("*.json")}
        self.maps_path, self.maps = table("UI/DT_MapData")
        self.chapter_path, chapters = table("UI/DT_ChapterData")
        self.chapters = {v["ChapterID"]: {"row": k, **v} for k, v in chapters.items()}
        self.loot_path, self.loot = table("DT_LevelLootDataTable")
        self.combat_path, self.combat = table("DT_CombatLootDataTable")
        self.loc = read("notes/dump/Game.zh-Hans.json")["Default"]
        self.weapon_path, weapons = table("UI/DT_WeaponData")
        self.weapon_rows = {v["ItemId"]: (k, v) for k, v in weapons.items()}
        self.shaqi_path, shaqi = table("UI/DT_ShaqiWeaponData")
        self.shaqi_rows = {v["ItemId"]: (k, v) for k, v in shaqi.items()}
        self.map_by_id = {v["MapID"]: v for v in self.maps.values()}

    def class_source(self, stem):
        return self.classes[stem].relative_to(ROOT).as_posix()

    def path(self, short):
        stem = short if short.startswith("BP_") else "BP_Monster_" + short
        hits = [e["path"] for e in self.enemies if boss_stem(e["path"]) == stem]
        if len(hits) != 1:
            raise ValueError(f"ambiguous or missing boss {short}: {hits}")
        return hits[0]


def prepare_bosses(s):
    pool = [e for e in s.enemies if e["kind"] == "boss" and (e["hp"] or 0) >= 3000 and "Horse" not in e["id"]]
    by_stem = {boss_stem(e["path"]): e for e in pool}
    phase_pairs = {stem: stem + "_P2" for stem in by_stem if stem + "_P2" in by_stem}
    phase_pairs.update({"BP_Monster_MBoss": "BP_Monster_MBossVariants", "BP_Monster_HuaShan_Blue": "BP_Monster_HuaShan_JianOu", "BP_Monster_ZuoShang_Ending": "BP_Monster_ZuoShang_P2", "BP_Monster_ChangHong_Spear": "BP_Monster_ChangHong_P2"})
    secondary = set(phase_pairs.values())
    scene_ids = {"MBoss", "MoTian", "HuaQianXIao", "HookMan", "PowerMan", "GuiChai_Red", "MuXiaoKui", "ZuoShang", "UltimateBoss", "HuaShan_Blue"}
    large_ids = {"JingYe", "ChangHong_Spear", "ChangHong"}
    summoners = {"Danghui", "QiShu_Du", "QiShu_Lei", "LengTu"}
    # Explicit identity aliases where the phase has a different stem.
    aliases = {"ChangHong": "ChangHong_Spear"}
    result = []
    for e in sorted(pool, key=lambda v: v["path"]):
        stem = boss_stem(e["path"])
        ident = aliases.get(identity(stem), identity(stem))
        map_id = (e.get("chapter_or_map") or "").split("（")[0] or None
        map_inferred = False
        if not map_id:
            relatives = [x for x in pool if aliases.get(identity(boss_stem(x["path"])), identity(boss_stem(x["path"]))) == ident and x.get("chapter_or_map")]
            if relatives:
                map_id = relatives[0]["chapter_or_map"].split("（")[0]
                map_inferred = True
        # Content placement for uncatalogued actors; do not call this a native map.
        placement = {"UltimateBoss": "DuanHunYa", "Hun": "DuanHunYa", "IronMaskMan_Boss": "DuanHunYa"}
        if not map_id and ident in placement:
            map_id, map_inferred = placement[ident], True
        if ident == "ZuoShang":
            map_id = "DuanHunYa"  # final-tower identity, prologue variant remains D
            map_inferred = e.get("chapter_or_map", "") != "DuanHunYa（断魂山）"
        variant = bool(re.search(r"_(FirstTime|Ending|JYX|LengTu|Base)$", stem)) or "YuCong_" in stem or "EMei_Sword_Top" in stem
        traits = []
        if ident in scene_ids:
            traits.append("B")
        if stem in phase_pairs or stem in secondary:
            traits.append("C")
        if variant:
            traits.append("D")
        if ident == "gold_mask_guard":
            traits.append("E")
        if ident in large_ids:
            traits.append("F")
        if not traits:
            traits = ["A"]
        primary = next((x for x in ("B", "E", "F", "C", "D", "A") if x in traits))
        tags = []
        if ident in summoners:
            tags.append("summoner")
        if "F" in traits:
            tags.append("large_arena")
        if ident in {"DuYanBoss", "YanQianSui", "LeiDang", "GuiChai_Red", "ChangHong_Spear"}:
            tags.append("ranged")
        if "B" in traits:
            tags.append("native_scene_required")
        if stem in secondary:
            tags.append("phase2_only")
        if "D" in traits:
            tags.append("variant")
        if ident == "gold_mask_guard":
            tags.append("group_only")
        class_file = s.class_source(stem)
        text = (ROOT / class_file).read_text(encoding="utf8")
        clues = sorted(set(re.findall(r'"(?:ObjectName|Name|Value)":\s*"([^"\n]*(?:Anchor|HangPoint|Hanging|LevelSequence|SpawnPillar|DespawnPillar)[^"\n]*)"', text)) - {"AnchorBone"})
        m = s.map_by_id.get(map_id, {})
        chapter = s.chapters.get(m.get("OwnChapterID"), {})
        phase = by_stem.get(phase_pairs.get(stem, ""), {}).get("path")
        role = "phase2" if stem in secondary else "variant" if variant else "primary"
        arena = None
        if "B" in traits:
            arena = provenance(class_file, "shared/bells.json", evidence="unverified", map=map_id, level=m.get("LevelName"), center=None, yaw=None, nearest_bell=None, radius=None, enabled=False, verification="ARENA-NATIVE in notes/codex-tower-zones.md")
        # Each original pool actor is retained for audit; none is a live whitelist yet.
        sources = ["shared/enemies.json#" + e["id"], class_file, "notes/bossrush-plan.md"]
        if m:
            sources.append(s.maps_path)
        if chapter:
            sources.append(s.chapter_path + "#Rows/" + chapter["row"])
        if phase:
            sources.append("shared/enemies.json#" + by_stem[phase_pairs[stem]]["id"])
        result.append(provenance(*sources, id=e["id"], path=e["path"], name=e["name_zh"], identity=ident, **{"class": primary}, classes=traits, tier=MAP_ZONE.get(map_id, 5), tier_evidence="unverified" if map_inferred or map_id not in MAP_ZONE else "static", zone_map=map_id, native_map=(e.get("chapter_or_map") or "").split("（")[0] or None, chapter=chapter.get("ChapterID"), chapter_order=chapter.get("ChapterOrder"), phase2=phase, phase2_evidence="unverified" if phase else None, group=2 if ident == "gold_mask_guard" else None, tags=tags, arena=arena, role=role, selectable=role == "primary" and ident != "gold_mask_guard", spawn_verified=False, runtime_evidence="unverified", classification_evidence="unverified", clues=clues, hp_reference=e["hp"], verification="BOSS-AUDIT in notes/codex-tower-phase.md"))
    return result


def prepare_arenas(s):
    # Bell transforms are approach points, never claimed as arena centers.
    seeds = {1: ["Bell_NLES_004", "Bell_SZ_003"], 2: ["Bell_LZSJ_002", "Bell_MGH_007"], 3: ["Bell_YXZR_009", "Bell_SXC_02_03"], 4: ["Bell_DDSD_005", "Bell_XM_012"], 5: ["Bell_DH_002", "Bell_DH_003"]}
    bells = {b["id"]: b for b in s.bells}
    out = []
    for zone, ids in seeds.items():
        for n, bell_id in enumerate(ids, 1):
            b = bells[bell_id]
            out.append(provenance("shared/bells.json#" + bell_id, PLAN + "#二、五区总览", evidence="unverified", id=f"arena_z{zone}_{n}", zone=zone, name=b["name_zh"] + "附近（候选）", map=b["map"], level=s.map_by_id[b["map"]]["LevelName"], center=None, radius=None, yaw=None, nearest_bell=None, approach_bell=bell_id, approach={k: b[k] for k in ("x", "y", "z", "yaw")}, approach_evidence="static", enabled=False, verification="ARENA-GENERIC in notes/codex-tower-zones.md"))
    return out


# The 32 design rows, not the 44 stem aliases in the old engine fallback.
WEAPONS = [
    ("PengMeng_Axe", ["BrutalAxe"]), ("Danghui", ["CaoOuSiXian"]), ("YanQianSui", ["CuiXinPiPa"]), ("DuYanBoss", ["DuYanGong"]),
    ("GuiChai_Red", ["HuoLiuXing"]), ("WanJun_JinGua", ["JinGua"]), ("LionDancer", ["LionHead"]), ("HookMan", ["ShachiHook"]),
    ("ChangHong_Spear", ["ThunderSpear"]), ("LeiDang", ["TigerCannon"]), ("LuanShi", ["TieGongZhuQuanTao"]), ("YouLong_Harpoon", ["YouLongCha", "YouLongTou"]),
    ("Zhuobi", ["ZhuoBiDun"]), ("AHua_GuiTouDao", ["GuiTouBlade"]), ("Yuu", ["YuuBlade"]), ("ChiLian", ["SnakeDual"]),
    ("JingYe", ["ZhanMaBlade"]), ("MobMan_Elite_Sword", ["IronBlade"]), ("FemaleVessel_YuDing", ["RenOuYanYueDao"]), ("MengHua", ["FangHuaXieShan"]),
    ("HuaQianXIao", ["ChiZheCanJian"]), ("GuiChai_Cyan_ShuangYue", ["TianShaShuangYue"]), ("GuiChai_Yellow_ShuangGou", ["DualHook"]),
    ("GreatBladeMan", ["HorizontalBlade"]), ("HuoDaoDuiZhang", ["XieHuo"]), ("YuCong_HunDun", ["CiGuHunDun"]),
    ("PowerMan", ["Jigsaw"]), ("BiYuan", ["RenOuQingTongJian"]), ("HuaShan_Blue", ["BaiShanShuangJian", "JianKeZhaoJiLing"]),
    ("EMei_Sword_JianZhen", ["LuanYing"]), ("JadeMaskMan", ["JadeDualChakram"]), ("ZuoShang", ["ZiMingJian"]),
]


def prepare_rewards(s, bosses):
    drops = []
    lore_positive = {"HorizontalBlade": "毕峰", "CiGuHunDun": "玉琮", "RenOuQingTongJian": "毕远", "JadeDualChakram": "玉面郎君", "ZiMingJian": "左殇", "Jigsaw": "锯者"}
    for n, (short, ids) in enumerate(WEAPONS, 1):
        ident = identity("BP_Monster_" + short)
        family = [b for b in bosses if b["identity"] == ident]
        sources = [PLAN + "#四、Boss → 兵器关联表"]
        for item_id in ids:
            if item_id not in s.items:
                raise ValueError(f"unknown weapon {item_id}")
            sources.append("shared/items.json#" + item_id)
        weak = n >= 24
        confirmed = not weak
        lore_keys = []
        if ids[0] in lore_positive:
            row_key, row = s.weapon_rows[ids[0]]
            text_key = row["Simple"]["Key"]
            assert lore_positive[ids[0]] in s.loc[text_key]
            sources += [s.weapon_path + "#Rows/" + row_key + "/Simple", "notes/dump/Game.zh-Hans.json#Default/" + text_key]
            lore_keys.append(text_key)
            confirmed = True
        if ids[0] == "LuanYing":
            row_key, row = s.weapon_rows[ids[0]]
            text_key = row["Simple"]["Key"]
            sources += [s.weapon_path + "#Rows/" + row_key + "/Simple", "notes/dump/Game.zh-Hans.json#Default/" + text_key]
            lore_keys.append(text_key)
        rec = provenance(*sources, evidence="static" if confirmed else "unverified", id=f"weapon_drop_{n:02}", kind="weapon_drops", identity=ident, bosses=[b["path"] for b in family], items=[provenance("shared/items.json#" + item_id, id=item_id, name=s.items[item_id]["name_zh"], kind=s.items[item_id]["kind"]) for item_id in ids], chance=1.0, verified=confirmed, enabled=confirmed, original_confidence="medium" if weak else "strong", association_basis="localized_lore" if lore_keys else "approved_design_name_match" if confirmed else "unresolved_design", lore_keys=lore_keys, natural_grant_verified=False, selection="all" if len(ids) == 1 else "choose_one", selection_evidence="static" if len(ids) == 1 else "unverified", scope="run_then_optional_save", verification="REWARD-AUDIT in notes/codex-tower-rewards.md")
        if ids[0] == "Jigsaw":
            rec["unverified_candidates"] = [{"identity": "MBoss", "path": s.path("MBoss"), "evidence": "unverified", "source": [PLAN + "#四、Boss → 兵器关联表"]}]
        if ids[0] == "LuanYing":
            rec["conflict"] = "Localized Simple names MuXiaoKui, not EMei_Sword; do not remap automatically."
        if ids[0] == "BaiShanShuangJian":
            rec["conflict"] = "Localized Simple names headmaster BiTaotao; no grant/identity link to HuaShan_Blue in current exports."
            row_key, row = s.weapon_rows[ids[0]]
            text_key = row["Simple"]["Key"]
            rec["source"] += [s.weapon_path + "#Rows/" + row_key + "/Simple", "notes/dump/Game.zh-Hans.json#Default/" + text_key]
            rec["lore_keys"].append(text_key)
        if len(ids) > 1:
            rec["enabled"] = False
            rec["selection"] = "unverified"
        for item in rec["items"]:
            iid = item["id"]
            item["association_verified"] = confirmed
            if iid == "BaiShanShuangJian":
                item["association_verified"] = False
            if iid == "JianKeZhaoJiLing":
                row_key, row = s.shaqi_rows[iid]
                key = row["SimpleDesc"]["Key"]
                assert "大师兄" in s.loc[key]
                refs = [s.shaqi_path + "#Rows/" + row_key + "/SimpleDesc", "notes/dump/Game.zh-Hans.json#Default/" + key]
                item["source"] += refs
                rec["source"] += refs
                item["association_verified"] = True
                item["evidence"] = "static"
            elif not item["association_verified"]:
                item["evidence"] = "unverified"
        rec["association_status"] = "confirmed" if all(i["association_verified"] for i in rec["items"]) else "partial" if any(i["association_verified"] for i in rec["items"]) else "unverified"
        drops.append(rec)

    loot_by_zone = collections.defaultdict(list)
    for row, data in s.loot.items():
        map_id = LOOT_MAP.get(row.split(".")[0].lower())
        if map_id in MAP_ZONE:
            loot_by_zone[MAP_ZONE[map_id]].append((row, data))
    packs, boxes, conversions = [], [], []
    last_dust = 0
    material_choices = {1: ["Weapon_Material_01", "Weapon_Material_10"], 2: ["Weapon_Material_07", "Ac_Material_1"], 3: ["Weapon_Material_08", "Ac_Material_1"], 4: ["Weapon_Material_02", "Ac_Material_4"], 5: ["Weapon_Material_03", "Ac_Material_4"]}
    for zone in range(1, 6):
        material_rows, accessory_rows = collections.defaultdict(list), collections.defaultdict(list)
        for row, data in loot_by_zone[zone]:
            for entry_index, entry in enumerate(data["EntryDatas"]):
                for item in entry.get("LootItems", []):
                    if item["ItemId"] in s.items and s.items[item["ItemId"]]["kind"] == "material":
                        material_rows[item["ItemId"]].append((row, entry_index, item))
                for a in entry.get("Accessories", []):
                    if a["AccessoryID"] in s.items and a["Probability"] > 0:
                        accessory_rows[a["AccessoryID"]].append((row, entry_index))
        dust_refs = []
        paths = {b["path"] for b in bosses if b["tier"] == zone and b["role"] == "primary"}
        for row, data in s.combat.items():
            if data.get("NPCClass", {}).get("AssetPathName") in paths:
                for dust in data["EntryDatas"][0].get("LootDusts", []):
                    dust_refs.append((row, (dust["MinDust"] + dust["MaxDust"]) / 2))
        reference = statistics.median(v for _, v in dust_refs) if dust_refs else None
        amount = max(last_dust + 1000, int(reference or zone * 1000))
        last_dust = amount
        materials = []
        for item_id in material_choices[zone]:
            rows = material_rows.get(item_id)
            if not rows:
                continue  # no provenance, no item
            row, ei, v = rows[0]
            materials.append(provenance(s.loot_path + f"#Rows/{row}/EntryDatas/{ei}/LootItems", "shared/items.json#" + item_id, id=item_id, amount=max(1, min(2 * zone, int(v["MinAmount"])))))
        packs.append(provenance(PLAN + "#5.5 区结算", *[s.combat_path + "#Rows/" + row + "/EntryDatas/0/LootDusts" for row, _ in dust_refs], id=f"zone_pack_{zone}", kind="zone_packs", zone=zone, materials=materials, dust=amount, reference_dust_median=reference, balance_evidence="unverified", enabled=bool(materials), verification="PACK-BALANCE in notes/codex-tower-rewards.md"))
        candidates = []
        for item_id, rows in sorted(accessory_rows.items()):
            row, ei = rows[0]
            assert s.items[item_id]["kind"] == "accessory"
            candidates.append(provenance(s.loot_path + f"#Rows/{row}/EntryDatas/{ei}/Accessories", "shared/items.json#" + item_id, id=item_id, name=s.items[item_id]["name_zh"], weight=1))
        boxes.append(provenance(PLAN + "#5.5 区结算", s.loot_path, evidence="static" if candidates else "unverified", id=f"accessory_box_{zone}", kind="accessory_boxes", zone=zone, candidates=candidates, draw=3, choose=1, without_replacement=True, enabled=len(candidates) >= 3, verification="ACCESSORY-SOURCE in notes/codex-tower-rewards.md"))
        conversions.append(provenance(PLAN + "#5.5 区结算", evidence="unverified", id=f"convert_{zone}", kind="convert", zone=zone, target="already_owned_weapon", dust=max(100, amount // 4), materials=[{**m, "amount": 1} for m in materials[:1]], enabled=False, dedupe="save_identity+reward_id", verification="PACK-BALANCE in notes/codex-tower-rewards.md"))
    policy = provenance(PLAN + "#5.4 神通", "mods/tower/src/tower_economy.h", id="reward_policy", kind="policy", jade={"base": 20, "per_floor": 5, "jitter": 0.2, "zone_boss_multiplier": 3}, rarity_by_zone=[[70, 25, 5, 0], [58, 30, 11, 1], [45, 35, 17, 3], [35, 38, 22, 5], [25, 40, 28, 7]], shentong={"normal": {"floor": 0.005, "zone_boss": 0.02}, "rare": {"floor": 0.001, "zone_boss": 0.005}, "lingxi": {"floor": 0.0, "zone_boss": 0.01, "eligible_floors": [30, 40, 50], "max_per_run": 1}, "roll_unit": "once_per_tier_per_cleared_floor", "capacity": 3, "consumable": True, "duration": "one_floor"}, native_rewards={"default_enabled": True, "backup_required": True, "session_check_required": True, "paid_dlc_cosmetics": False, "dedupe": "save_identity+reward_id"}, spectate_weapon_drops=False)
    return drops + packs + boxes + conversions + [policy]


def build():
    # Delayed import also permits direct execution with python -I.
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from tower_content import cards, events, floors, shop
    s = Sources()
    bosses = prepare_bosses(s)
    tables = {name: document(items, source) for name, items, source in [
        ("bosses", bosses, "shared/enemies.json; notes/bossrush-plan.md static preclassification"),
        ("arenas", prepare_arenas(s), "shared/bells.json approach transforms; arena centers unverified"),
        ("rewards", prepare_rewards(s, bosses), PLAN + " sections 4 and 5; native item and loot tables"),
        ("cards", cards(), "mods/tower/src/tower_logic.h; notes/bossrush-plan.md; Codex proposed extensions"),
        ("shop", shop(), "mods/tower/src/tower_economy.h; notes/tower-floors.md; Codex proposed prices"),
        ("events", events(), "notes/bossrush-plan.md; Codex proposed bilingual encounters"),
        ("floors", floors(s, bosses), PLAN + " section 3; conflicts retained as disabled encounters"),
    ]}
    tables["floors"]["generated"] = "2026-10-31"
    return tables


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    tables = build()
    if not args.check:
        OUT.mkdir(parents=True, exist_ok=True)
    stale = []
    for name, data in tables.items():
        raw = (json.dumps(data, ensure_ascii=False, indent=2) + "\n").encode("utf8")
        dest = OUT / (name + ".json")
        if args.check:
            if not dest.exists() or dest.read_text(encoding="utf-8-sig") != raw.decode("utf8"):
                stale.append(name)
        else:
            dest.write_bytes(raw)
        print(f"{name}: {len(data['items'])} rows, canonical-UTF8-LF-sha256 {hashlib.sha256(raw).hexdigest()}")
    if stale:
        raise SystemExit("stale preparation tables: " + ", ".join(stale))


if __name__ == "__main__":
    main()
