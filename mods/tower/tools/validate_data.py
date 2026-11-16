"""Offline structural/provenance validation for tower preparation data.

Exit 0 means internally consistent preparation, not approved live content.
--strict-ready also requires all runtime evidence and pending features resolved.
"""
from __future__ import annotations

import argparse
import collections
import datetime
import json
import math
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
DATA = ROOT / "mods/tower/data"
FILES = ("bosses", "floors", "rewards", "cards", "shop", "events", "arenas")
STATS = {"DmgDealt", "DmgTaken", "MaxHp", "ShachiGain", "AtkSpeed", "PostureDealt", "HealOnFloor"}
SCHOOLS = {"刀势", "金钟", "杀气", "时机", "生命", "影卫", "诅咒"}
TYPES = {"1v1", "1v2", "team", "spectate", "seasoning", "zone_boss", "tower_lord"}


def load(path):
    def pairs(values):
        d = {}
        for key, value in values:
            if key in d:
                raise ValueError(f"duplicate JSON key: {key}")
            d[key] = value
        return d
    return json.loads(path.read_text(encoding="utf-8-sig"), object_pairs_hook=pairs,
                      parse_constant=lambda x: (_ for _ in ()).throw(ValueError("non-finite number: " + x)))


class Validator:
    def __init__(self, data_dir=DATA, root=ROOT):
        self.root, self.data_dir = Path(root), Path(data_dir)
        self.errors, self.pending = [], []
        self.docs, self.rows, self.index = {}, {}, {}
        self.native_items = {i["id"]: i for i in load(self.root / "shared/items.json")}
        self.native_enemies = {e["path"]: e for e in load(self.root / "shared/enemies.json")}
        self.native_bells = {b["id"]: b for b in load(self.root / "shared/bells.json")}
        self.features = {f["id"] for f in load(self.root / "shared/features.json")["features"]}
        self._source_cache = {}
        self._fragments = {}

    def need(self, condition, where, message):
        if not condition:
            self.errors.append(f"{where}: {message}")
        return bool(condition)

    def fields(self, obj, names, where):
        return self.need(isinstance(obj, dict) and all(k in obj for k in names), where, "missing fields: " + ", ".join(k for k in names if not isinstance(obj, dict) or k not in obj))

    def source(self, ref, where):
        if not self.need(isinstance(ref, str) and bool(ref), where, "invalid source"):
            return
        path, _, fragment = ref.partition("#")
        target = (self.root / path).resolve()
        if not self.need(target.is_relative_to(self.root.resolve()) and target.is_file(), where, "source missing/outside repository: " + path):
            return
        if path in ("shared/items.json", "shared/enemies.json", "shared/bells.json") and fragment:
            catalog = self.native_items if path.endswith("items.json") else self.native_bells if path.endswith("bells.json") else {e["id"]: e for e in self.native_enemies.values()}
            self.need(fragment in catalog, where, "source catalog ID missing: " + ref)
        if path.startswith("notes/dump/") and path.endswith(".json") and fragment:
            try:
                if path not in self._source_cache:
                    self._source_cache[path] = load(target)
                obj = self._source_cache[path]
                if isinstance(obj, list):
                    obj = obj[0]
                for part in fragment.split("/"):
                    obj = obj[int(part)] if isinstance(obj, list) else obj[part]
                self._fragments[ref] = obj
            except (KeyError, IndexError, ValueError, TypeError) as exc:
                self.need(False, where, f"source fragment missing: {ref}: {exc}")

    def metadata(self, item, where):
        if not self.fields(item, ("evidence", "source"), where):
            return
        self.need(item["evidence"] in ("static", "unverified"), where, "invalid evidence")
        if self.need(isinstance(item["source"], list) and bool(item["source"]), where, "source must be a nonempty list"):
            for src in item["source"]:
                self.source(src, where)
        if item["evidence"] == "unverified" or item.get("runtime_evidence") == "unverified" or item.get("balance_evidence") == "unverified":
            self.pending.append(where)

    def item(self, item_id, kinds, where):
        if not self.need(isinstance(item_id, str) and item_id in self.native_items, where, f"unknown native item {item_id!r}"):
            return False
        return self.need(self.native_items[item_id]["kind"] in kinds, where, f"wrong item family/DLC cosmetic: {item_id}")

    @staticmethod
    def number(value):
        return type(value) in (int, float) and math.isfinite(value)

    def validate(self):
        all_ids = set()
        for name in FILES:
            try:
                doc = load(self.data_dir / (name + ".json"))
            except (OSError, ValueError) as exc:
                self.errors.append(f"{name}: {exc}")
                continue
            if not self.fields(doc, ("schema", "source", "generated", "items"), name):
                continue
            self.need(type(doc["schema"]) is int and doc["schema"] == 1, name, "schema must be 1")
            self.need(isinstance(doc["source"], str) and bool(doc["source"]), name, "missing top-level source")
            try:
                datetime.date.fromisoformat(doc["generated"])
            except (ValueError, TypeError):
                self.errors.append(name + ": invalid generated date")
            if not self.need(isinstance(doc["items"], list), name, "items must be an array"):
                continue
            self.docs[name], self.rows[name] = doc, doc["items"]
            self.index[name] = {}
            for n, row in enumerate(doc["items"]):
                where = f"{name}[{n}]"
                if not self.fields(row, ("id", "source", "evidence"), where):
                    continue
                rid = row["id"]
                if not self.need(isinstance(rid, str) and bool(rid), where, "id must be a nonempty string"):
                    continue
                self.need(rid not in all_ids, where, "duplicate ID: " + rid)
                all_ids.add(rid)
                self.index[name][rid] = row
                self.metadata(row, where)
        if len(self.rows) != len(FILES):
            return self
        for name in FILES:
            for row in self.rows[name]:
                if not isinstance(row, dict):
                    continue
                try:
                    getattr(self, "check_" + name)(row)
                except (KeyError, TypeError, ValueError, AttributeError) as exc:
                    self.errors.append(f"{name}/{row.get('id', '?')}: invalid shape: {exc}")
        self.totals()
        return self

    def check_bosses(self, b):
        w = "bosses/" + b.get("id", "?")
        keys = ("path", "name", "identity", "class", "tier", "zone_map", "phase2", "group", "tags", "arena", "runtime_evidence", "spawn_verified", "role", "selectable")
        if not self.fields(b, keys, w):
            return
        self.need(b["path"] in self.native_enemies, w, "boss path absent from shared/enemies.json")
        if b["path"] in self.native_enemies:
            self.need(b["id"] == self.native_enemies[b["path"]]["id"], w, "boss ID/path mismatch")
        self.need(b["class"] in "ABCDEF" and len(b["class"]) == 1, w, "invalid class")
        self.need(type(b["tier"]) is int and 1 <= b["tier"] <= 5, w, "tier outside 1..5")
        self.need(isinstance(b["name"], str) and bool(b["name"]) and isinstance(b["identity"], str) and bool(b["identity"]), w, "missing name/identity")
        self.need(b["group"] in (None, 2), w, "group size must be 2")
        self.need(isinstance(b["tags"], list), w, "tags must be a list")
        if b["phase2"]:
            phase = next((x for x in self.rows["bosses"] if x.get("path") == b["phase2"]), None)
            self.need(phase is not None, w, "phase2 path missing")
            if phase:
                self.need(phase["identity"] == b["identity"] and phase["role"] == "phase2" and not phase["selectable"], w, "phase2 identity/role mismatch")
                self.need(phase["path"] != b["path"] and not phase.get("phase2"), w, "cyclic phase transition")
        if b["role"] == "phase2":
            self.need(not b["selectable"], w, "standalone phase2 is selectable")
        self.need(type(b["spawn_verified"]) is bool and type(b["selectable"]) is bool, w, "audit flags must be boolean")
        if b["class"] == "B":
            self.need(isinstance(b["arena"], dict), w, "class B requires native arena metadata")
            if isinstance(b["arena"], dict):
                self.check_location(b["arena"], w + "/arena")

    def check_location(self, a, w):
        self.metadata(a, w)
        if not self.fields(a, ("map", "center", "radius", "yaw", "nearest_bell", "enabled"), w):
            return
        unresolved = any(a[k] is None for k in ("map", "center", "radius", "yaw", "nearest_bell"))
        if unresolved:
            self.need(a["evidence"] == "unverified" and a["enabled"] is False and bool(a.get("verification")), w, "unknown location must be disabled/unverified with verification steps")
        else:
            self.need(isinstance(a["center"], dict) and set(a["center"]) == {"x", "y", "z"} and all(self.number(v) for v in a["center"].values()), w, "invalid center")
            self.need(self.number(a["radius"]) and a["radius"] > 0 and self.number(a["yaw"]), w, "invalid radius/yaw")
            self.need(a["nearest_bell"] in self.native_bells and self.native_bells[a["nearest_bell"]]["map"] == a["map"], w, "bell/map mismatch")

    def check_arenas(self, a):
        w = "arenas/" + a["id"]
        self.check_location(a, w)
        self.fields(a, ("zone", "approach_bell", "approach"), w)
        b = self.native_bells.get(a["approach_bell"])
        self.need(b is not None and a["map"] == b["map"], w, "approach bell/map mismatch")
        if b:
            self.need(a["approach"] == {k: b[k] for k in ("x", "y", "z", "yaw")}, w, "approach transform differs from shared bell; must not be invented")

    def check_cards(self, c):
        w = "cards/" + c["id"]
        if not self.fields(c, ("name", "name_en", "school", "rarity", "value", "text", "text_en", "weight", "enabled", "implemented"), w):
            return
        self.need(c["school"] in SCHOOLS and type(c["rarity"]) is int and 0 <= c["rarity"] <= 3, w, "invalid school/rarity")
        self.need(all(isinstance(c[k], str) and c[k].strip() for k in ("name", "name_en", "text", "text_en")), w, "empty bilingual text")
        self.need(self.number(c["value"]) and -.8 < c["value"] <= 1 and type(c["weight"]) is int and c["weight"] > 0, w, "invalid value/weight")
        self.need(("stat" in c) != ("trigger" in c), w, "exactly one of stat/trigger is required")
        if "stat" in c:
            self.need(c["stat"] in STATS, w, "unsupported stat")
        if "trigger" in c:
            self.need(not c["implemented"] and not c["enabled"] and c["evidence"] == "unverified", w, "unsupported trigger must be disabled/unverified")
            self.fields(c["trigger"], ("event", "target", "effect", "cooldown_seconds"), w)
        if c["school"] == "诅咒":
            self.need(c["value"] < 0 and c.get("random_offer") is False, w, "curse must have a cost and be excluded from random offers")

    def check_shop(self, x):
        w, kind = "shop/" + x["id"], x["kind"]
        self.need(kind in ("growth", "item", "shentong", "policy"), w, "unknown shop kind")
        if kind != "policy":
            self.need(all(isinstance(x.get(k), str) and x[k].strip() for k in ("name", "name_en")), w, "empty bilingual shop text")
        if kind == "shentong":
            self.need(x["duration"] == "one_floor" and x["consumable"] is True and x["max_stock_per_refresh"] == 1, w, "shentong must be a one-floor consumable")
            self.need(x["feature"] in self.features, w, "unknown feature ID")
            self.need(x["sold"] == (x["tier"] == "normal"), w, "rare/lingxi shentong cannot be sold")
            if x["tier"] != "normal":
                self.need(x["base_price"] is None and not x["enabled"], w, "rare/lingxi shop entry enabled")
        elif kind == "item":
            self.need(x["item_id"] in {"heal", "reroll", "freeze", "slow", "sunder", "shachi", "intel", "borrow", "revive", "guard", "edge", "break"}, w, "unknown tower item")
            self.need(x["consumable"] is True, w, "item must be consumable")
            if not x["implemented"]:
                self.need(not x["enabled"], w, "unimplemented item enabled")
        elif kind == "growth":
            self.need(x["stat"] in STATS and x["duration"] == "run" and x["purchase_multiplier"] > 1, w, "invalid growth definition")
        else:
            self.need(x["zone_multipliers"] == sorted(x["zone_multipliers"]) and len(x["zone_multipliers"]) == 5, w, "invalid zone prices")
            self.need(x["refresh"]["purchase_multiplier"] > 1 and x["refresh"]["base_price"] > 0, w, "invalid refresh curve")
            self.need(x["shentong"]["chance_per_refresh"] == .03 and x["shentong"]["tiers_sold"] == ["normal"] and x["shentong"]["max_per_refresh"] == 1, w, "wrong shentong shop probability/tier")
        if kind != "policy" and x.get("base_price") is not None:
            self.need(self.number(x["base_price"]) and x["base_price"] > 0, w, "invalid price")

    def check_events(self, e):
        w = "events/" + e["id"]
        self.fields(e, ("name", "name_en", "text", "text_en", "choices", "weight"), w)
        self.need(all(isinstance(e.get(k), str) and e[k].strip() for k in ("name", "name_en", "text", "text_en")), w, "empty bilingual event text")
        self.need(2 <= len(e["choices"]) <= 3 and e["weight"] > 0, w, "invalid event choices/weight")
        choice_ids = set()
        for choice in e["choices"]:
            self.fields(choice, ("id", "text", "text_en", "effects", "requires"), w)
            self.metadata(choice, w)
            self.need(all(isinstance(choice.get(k), str) and choice[k].strip() for k in ("text", "text_en")), w, "empty bilingual choice text")
            self.need(choice["id"] not in choice_ids, w, "duplicate choice ID")
            choice_ids.add(choice["id"])
            cost = 0
            for effect in choice["effects"]:
                kind = effect["kind"]
                self.need(kind in ("jade", "item", "card", "max_hp", "curse_card"), w, "unsupported event effect")
                if kind == "jade":
                    self.need(type(effect["amount"]) is int, w, "jade must be integer")
                    cost += max(0, -effect["amount"])
                elif kind == "item":
                    target = next((i for i in self.rows["shop"] if i.get("item_id") == effect["id"]), None)
                    self.need(target is not None and target["implemented"] and type(effect["amount"]) is int and effect["amount"] > 0, w, "unknown/unimplemented event item")
                elif kind in ("card", "curse_card"):
                    c = self.index["cards"].get(effect["id"])
                    self.need(c is not None and c["implemented"] and c["enabled"], w, "unknown/unsupported event card")
                    if c:
                        self.need((c["school"] == "诅咒") == (kind == "curse_card"), w, "curse effect/card mismatch")
                elif kind == "max_hp":
                    self.need(self.number(effect["value"]) and -.5 < effect["value"] <= .5, w, "invalid max_hp delta")
            self.need(choice["requires"].get("jade_at_least") == cost, w, "jade cost not preconditioned atomically")

    def check_rewards(self, r):
        w, kind = "rewards/" + r["id"], r["kind"]
        if kind == "weapon_drops":
            self.need(r["chance"] == 1.0, w, "boss weapon drop must be 100%")
            self.need(bool(r["bosses"]), w, "weapon drop has no boss")
            for path in r["bosses"]:
                b = next((x for x in self.rows["bosses"] if x["path"] == path), None)
                self.need(b is not None and b["identity"] == r["identity"], w, "drop boss identity/reference mismatch")
            for item in r["items"]:
                self.item(item["id"], {"weapon", "shaqi_weapon"}, w)
                self.metadata(item, w)
            if not r["verified"] or r["selection"] == "unverified":
                self.need(not r["enabled"], w, "unverified drop association/selection must be disabled")
        elif kind in ("zone_packs", "convert"):
            self.need(type(r["zone"]) is int and 1 <= r["zone"] <= 5 and type(r["dust"]) is int and r["dust"] > 0, w, "invalid zone/dust")
            for material in r["materials"]:
                self.item(material["id"], {"material"}, w)
                self.need(type(material["amount"]) is int and material["amount"] > 0, w, "invalid material amount")
                self.metadata(material, w)
                proven = False
                for ref in material["source"]:
                    if "DT_LevelLootDataTable.json#Rows/" in ref:
                        values = self._fragments.get(ref, [])
                        proven |= isinstance(values, list) and any(x.get("ItemId") == material["id"] and x.get("Probability", 0) > 0 for x in values if isinstance(x, dict))
                self.need(proven, w, "material has no matching positive native loot source")
            if kind == "convert":
                self.need(not r["enabled"] and r["evidence"] == "unverified", w, "conversion balance not ready")
        elif kind == "accessory_boxes":
            self.need(r["draw"] == 3 and r["choose"] == 1 and r["without_replacement"] is True, w, "accessory box must be 3 choose 1")
            seen = set()
            for candidate in r["candidates"]:
                iid = candidate["id"]
                self.item(iid, {"accessory"}, w)
                self.metadata(candidate, w)
                self.need(iid not in seen, w, "duplicate accessory candidate")
                seen.add(iid)
                proven = False
                for ref in candidate["source"]:
                    if "DT_LevelLootDataTable.json#Rows/" not in ref:
                        continue
                    path, fragment = ref.split("#", 1)
                    parts = fragment.split("/")
                    values = self._fragments.get(ref, [])
                    proven |= isinstance(values, list) and any(x.get("AccessoryID") == iid and x.get("Probability", 0) > 0 for x in values if isinstance(x, dict))
                    prefix = parts[1].split(".")[0].lower()
                    maps = {1: {"xigu", "pangzhen", "houjie"}, 2: {"fujizhilin", "meiguhu", "huanshushi"}, 3: {"lujiacun", "lujiaccun", "zhengdaodaying"}, 4: {"dengdingshandao", "ximeng"}, 5: {"duanhunya", "duanhunshan"}}
                    self.need(prefix in maps[r["zone"]], w, "accessory source map outside zone")
                self.need(proven, w, "accessory has no positive native loot source")
            if len(seen) < 3:
                self.need(not r["enabled"] and r["evidence"] == "unverified", w, "insufficient accessory candidates must be disabled")
        elif kind == "policy":
            s = r["shentong"]
            self.need(s["normal"] == {"floor": .005, "zone_boss": .02} and s["rare"] == {"floor": .001, "zone_boss": .005}, w, "wrong divine drop probabilities")
            self.need(s["lingxi"] == {"floor": 0.0, "zone_boss": .01, "eligible_floors": [30, 40, 50], "max_per_run": 1}, w, "wrong lingxi eligibility")
            self.need(s["roll_unit"] == "once_per_tier_per_cleared_floor" and s["capacity"] == 3 and s["consumable"] is True and s["duration"] == "one_floor", w, "divine scope/rate wrong")
            self.need(r["native_rewards"]["paid_dlc_cosmetics"] is False and r["native_rewards"]["backup_required"] is True and r["native_rewards"]["session_check_required"] is True, w, "native reward safety contract missing")
            self.need(len(r["rarity_by_zone"]) == 5 and all(len(x) == 4 and sum(x) == 100 for x in r["rarity_by_zone"]), w, "rarity weights invalid")
        else:
            self.need(False, w, "unknown reward kind")

    def check_floors(self, f):
        w = "floors/" + f["id"]
        rules = f.get("rules", {})
        self.need(rules.get("ordinary_mobs_allowed") is False, w, "ordinary mobs forbidden")
        self.need(rules.get("non_boss_enemy_kind") == "elite" and rules.get("non_boss_named_only") is True, w, "non-boss enemies must be named elites")
        self.need(type(rules.get("non_boss_default_count")) is int and rules.get("non_boss_default_count") == 4
                  and type(rules.get("non_boss_max_simultaneous")) is int and rules.get("non_boss_max_simultaneous") == 5, w, "elite count must default to 4 and cap at 5")
        self.need(rules.get("native_ambient_mobs") == "isolate_before_entry", w, "native ambient mobs must be isolated")
        self.need(rules.get("spawn_owner") == "tower" and rules.get("native_boss_autospawn") is False, w, "tower must own all enemy spawning")
        self.need(rules.get("load_scene_before_spawn") is True and rules.get("scene_dependencies") == "validate_and_bind_per_boss"
                  and rules.get("campaign_callbacks") == "isolate", w, "scene must load before controlled boss binding")
        self.source(rules.get("content_policy_source", ""), w)
        self.fields(f, ("floor", "zone", "type", "candidates", "rules", "encounters"), w)
        self.need(type(f["floor"]) is int and f["zone"] == (f["floor"] - 1) // 10 + 1 and f["type"] in TYPES, w, "invalid floor/zone/type")
        paths = {b["path"]: b for b in self.rows["bosses"]}
        self.need(bool(f["candidates"]) and len(set(f["candidates"])) == len(f["candidates"]), w, "empty/duplicate candidates")
        self.need(set(f["candidates"]) == {p for e in f["encounters"] for p in e["bosses"]}, w, "candidate/encounter mismatch")
        for path in f["candidates"]:
            self.need(path in paths, w, "unknown boss path")
        for enc in f["encounters"]:
            self.metadata(enc, w)
            bs = [paths[p] for p in enc["bosses"] if p in paths]
            reasons = []
            if f["type"] in ("1v2", "team", "spectate") and any(b["class"] != "A" for b in bs):
                reasons.append("nonnative_multi_requires_class_A")
            if f["floor"] % 10 != 0 and any("B" in b["classes"] for b in bs):
                reasons.append("scene_dependent_boss_outside_landmark_floor")
            if enc["mode"] != "group" and len({b["identity"] for b in bs}) != len(bs):
                reasons.append("duplicate_identity")
            if any(b["role"] == "phase2" for b in bs):
                reasons.append("standalone_phase2_forbidden")
            if sum("summoner" in b["tags"] for b in bs) > 1:
                reasons.append("multiple_summoners")
            self.need(set(reasons) <= set(enc["blocked_reasons"]), w, "unsafe encounter missing blocked reason")
            if reasons:
                self.need(not enc["enabled"], w, "unsafe encounter enabled")
            if enc["mode"] == "group":
                self.need(len(bs) == 2 and all(b["identity"] == "gold_mask_guard" for b in bs), w, "group must be gold mask x2")
            if f["type"] == "1v2":
                self.need(len(bs) == 2 and enc["mode"] == "simultaneous", w, "1v2 requires two concurrent distinct bosses")
            if enc.get("final_phase"):
                b = paths.get(enc["final_phase"])
                self.need(b is not None and b["role"] == "phase2", w, "invalid final phase")
                self.need(bs and bs[-1].get("phase2") == enc["final_phase"], w, "final phase not linked from previous boss")
        if f["type"] == "1v2":
            self.need(f["rules"]["hp_per_boss"] == .65 and f["rules"]["damage_per_boss"] == .75, w, "1v2 scaling wrong")
        if f["type"] == "spectate":
            self.need(f["rules"]["weapon_drops"] is False, w, "spectator floor drops weapons")
        if f["type"] == "tower_lord":
            self.need(f["rules"].get("stage_count") == 3 and f["rules"].get("heal_between_stages") is False and f["rules"].get("max_simultaneous_bosses") == 1, w, "tower lord must be three sequential stages without healing")
        self.need(f["rules"]["spawn_requires_audit"] is True and f["enabled"] is False, w, "offline preparation must not become a live spawn whitelist")

    def totals(self):
        numbers = [f.get("floor") for f in self.rows["floors"] if isinstance(f, dict)]
        self.need(all(type(n) is int for n in numbers) and sorted(numbers) == list(range(1, 51)), "floors", "must cover floors 1..50 exactly")
        types = collections.Counter(f.get("type") for f in self.rows["floors"])
        self.need(types == {"1v1": 25, "1v2": 5, "team": 10, "spectate": 2, "seasoning": 3, "zone_boss": 4, "tower_lord": 1}, "floors", "wrong type counts")
        self.need([f["floor"] for f in self.rows["floors"] if f["type"] == "1v2"] == [3, 13, 23, 33, 43], "floors", "wrong 1v2 floors")
        self.need(collections.Counter(c.get("school") for c in self.rows["cards"]) == {s: 6 for s in SCHOOLS}, "cards", "expected six cards per school")
        self.need(collections.Counter(r.get("kind") for r in self.rows["rewards"]) == {"weapon_drops": 32, "zone_packs": 5, "accessory_boxes": 5, "convert": 5, "policy": 1}, "rewards", "incomplete reward sections")
        self.need(10 <= len(self.rows["events"]) <= 15, "events", "expected 10..15 events")
        self.need(collections.Counter(a.get("zone") for a in self.rows["arenas"]) == {z: 2 for z in range(1, 6)}, "arenas", "expected two candidates per zone")
        expected = {e["path"] for e in self.native_enemies.values() if e["kind"] == "boss" and (e["hp"] or 0) >= 3000 and "Horse" not in e["id"]}
        self.need({b.get("path") for b in self.rows["bosses"]} == expected, "bosses", "audit catalog does not cover original pool")
        self.need(len(self.rows["bosses"]) == len(expected), "bosses", "duplicate boss paths")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, default=DATA)
    parser.add_argument("--strict-ready", action="store_true")
    args = parser.parse_args()
    result = Validator(args.data_dir).validate()
    for err in result.errors:
        print("ERROR " + err)
    print(f"TOWER_DATA {'FAIL' if result.errors else 'OK'}: {sum(len(v) for v in result.rows.values())} rows, {len(result.errors)} errors, {len(set(result.pending))} pending evidence/balance records")
    if args.strict_ready and result.pending:
        print("NOT RELEASE READY: offline evidence does not establish live acceptance")
    raise SystemExit(1 if result.errors or (args.strict_ready and result.pending) else 0)


if __name__ == "__main__":
    main()
