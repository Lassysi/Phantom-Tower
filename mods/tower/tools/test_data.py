"""Adversarial offline checks; fixtures are temporary siblings of this script."""
from __future__ import annotations

import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from validate_data import DATA, FILES, ROOT, Validator, load
from gen_pool import select


class DataTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.base = {name: load(DATA / (name + ".json")) for name in FILES}

    def check_mutation(self, mutate, expected):
        changed = copy.deepcopy(self.base)
        mutate(changed)
        with tempfile.TemporaryDirectory(prefix=".data-test-", dir=HERE) as folder:
            path = Path(folder)
            for name, data in changed.items():
                (path / (name + ".json")).write_text(json.dumps(data, ensure_ascii=False), encoding="utf8")
            result = Validator(path).validate()
        self.assertTrue(any(expected in e for e in result.errors), result.errors)

    def test_baseline_is_preparation_not_live_acceptance(self):
        result = Validator().validate()
        self.assertEqual(result.errors, [])
        self.assertTrue(result.pending)
        self.assertEqual(select(result.rows["bosses"]), [])

    def test_missing_field(self):
        self.check_mutation(lambda d: d["bosses"]["items"][0].pop("identity"), "missing fields")

    def test_ordinary_mobs_forbidden(self):
        self.check_mutation(lambda d: d["floors"]["items"][0]["rules"].update(ordinary_mobs_allowed=True), "ordinary mobs forbidden")

    def test_named_elites_only(self):
        self.check_mutation(lambda d: d["floors"]["items"][0]["rules"].update(non_boss_named_only=False), "named elites")

    def test_elite_hard_cap(self):
        self.check_mutation(lambda d: d["floors"]["items"][0]["rules"].update(non_boss_max_simultaneous=6), "cap at 5")

    def test_native_ambient_isolation_required(self):
        self.check_mutation(lambda d: d["floors"]["items"][0]["rules"].update(native_ambient_mobs="preserve"), "ambient mobs must be isolated")

    def test_tower_owns_enemy_spawning(self):
        self.check_mutation(lambda d: d["floors"]["items"][0]["rules"].update(native_boss_autospawn=True), "tower must own all enemy spawning")

    def test_scene_load_before_binding(self):
        self.check_mutation(lambda d: d["floors"]["items"][0]["rules"].update(load_scene_before_spawn=False), "scene must load before")

    def test_duplicate_id(self):
        self.check_mutation(lambda d: d["cards"]["items"].append(copy.deepcopy(d["cards"]["items"][0])), "duplicate ID")

    def test_missing_boss_path(self):
        self.check_mutation(lambda d: d["bosses"]["items"][0].update(path="/Game/Fake.Fake_C"), "absent from shared/enemies")

    def test_native_boss_id_path_mismatch(self):
        self.check_mutation(lambda d: d["bosses"]["items"][0].update(path=d["bosses"]["items"][1]["path"]), "ID/path mismatch")

    def test_bad_phase_link(self):
        self.check_mutation(lambda d: next(b for b in d["bosses"]["items"] if b["phase2"]).update(phase2="/Game/Missing.Missing_C"), "phase2 path missing")

    def test_phase_identity(self):
        def mutate(d):
            path = next(b["phase2"] for b in d["bosses"]["items"] if b["phase2"])
            next(b for b in d["bosses"]["items"] if b["path"] == path)["identity"] = "WrongIdentity"
        self.check_mutation(mutate, "phase2 identity/role mismatch")

    def test_standalone_phase2(self):
        self.check_mutation(lambda d: next(b for b in d["bosses"]["items"] if b["role"] == "phase2").update(selectable=True), "standalone phase2")

    def test_missing_B_arena(self):
        self.check_mutation(lambda d: next(b for b in d["bosses"]["items"] if b["class"] == "B").update(arena=None), "class B requires")

    def test_unknown_arena_cannot_be_enabled(self):
        self.check_mutation(lambda d: d["arenas"]["items"][0].update(enabled=True), "unknown location must be disabled")

    def test_bell_approach_not_fabricated(self):
        self.check_mutation(lambda d: d["arenas"]["items"][0]["approach"].update(x=0), "differs from shared bell")

    def test_bilingual_card_required(self):
        self.check_mutation(lambda d: d["cards"]["items"][0].update(text_en=""), "empty bilingual")

    def test_unsupported_stat(self):
        self.check_mutation(lambda d: d["cards"]["items"][0].update(stat="PermanentGodMode"), "unsupported stat")

    def test_trigger_must_not_sneak_into_draw_pool(self):
        self.check_mutation(lambda d: next(c for c in d["cards"]["items"] if "trigger" in c).update(enabled=True), "trigger must be disabled")

    def test_curse_not_random_reward(self):
        self.check_mutation(lambda d: next(c for c in d["cards"]["items"] if c["school"] == "诅咒").update(random_offer=True), "curse must have")

    def test_weapon_probability(self):
        self.check_mutation(lambda d: d["rewards"]["items"][0].update(chance=.5), "must be 100%")

    def test_unknown_weapon(self):
        self.check_mutation(lambda d: d["rewards"]["items"][0]["items"][0].update(id="MissingBlade"), "unknown native item")

    def test_skin_reward_rejected_even_with_valid_native_id(self):
        skin = next(i["id"] for i in load(ROOT / "shared/items.json") if i["kind"] == "skin")
        self.check_mutation(lambda d: d["rewards"]["items"][0]["items"][0].update(id=skin), "wrong item family/DLC")

    def test_unverified_weapon_not_enabled(self):
        self.check_mutation(lambda d: next(r for r in d["rewards"]["items"] if r["kind"] == "weapon_drops" and not r["verified"]).update(enabled=True), "unverified drop association")

    def test_accessory_needs_actual_row_proof(self):
        def mutate(d):
            box = next(r for r in d["rewards"]["items"] if r["kind"] == "accessory_boxes" and r["candidates"])
            box["candidates"][0]["id"] = next(i["id"] for i in load(ROOT / "shared/items.json") if i["kind"] == "accessory" and i["id"] not in {c["id"] for c in box["candidates"]})
        self.check_mutation(mutate, "no positive native loot source")

    def test_material_row_name_is_not_item(self):
        def mutate(d):
            pack = next(r for r in d["rewards"]["items"] if r["kind"] == "zone_packs")
            pack["materials"][0]["id"] = "Weapon_Material_27"
        self.check_mutation(mutate, "no matching positive native loot source")

    def test_native_rewards_require_backup(self):
        self.check_mutation(lambda d: next(r for r in d["rewards"]["items"] if r["kind"] == "policy")["native_rewards"].update(backup_required=False), "safety contract")

    def test_shentong_one_floor(self):
        self.check_mutation(lambda d: next(x for x in d["shop"]["items"] if x["kind"] == "shentong").update(duration="permanent"), "one-floor consumable")

    def test_rare_shentong_not_sold(self):
        self.check_mutation(lambda d: next(x for x in d["shop"]["items"] if x.get("tier") == "rare").update(sold=True), "cannot be sold")

    def test_shop_three_percent_is_per_refresh(self):
        self.check_mutation(lambda d: next(x for x in d["shop"]["items"] if x["kind"] == "policy")["shentong"].update(chance_per_refresh=.3), "shop probability")

    def test_drop_rate_exact(self):
        self.check_mutation(lambda d: next(x for x in d["rewards"]["items"] if x["kind"] == "policy")["shentong"]["rare"].update(floor=.002), "wrong divine drop")

    def test_lingxi_only_three_floors(self):
        self.check_mutation(lambda d: next(x for x in d["rewards"]["items"] if x["kind"] == "policy")["shentong"]["lingxi"].update(eligible_floors=[10,20,30,40,50]), "lingxi eligibility")

    def test_event_unknown_item(self):
        self.check_mutation(lambda d: d["events"]["items"][0]["choices"][0].update(effects=[{"kind":"item","id":"infinite_hp","amount":1}]), "unknown/unimplemented event item")

    def test_event_english_text_required(self):
        self.check_mutation(lambda d: d["events"]["items"][0].update(text_en=""), "empty bilingual event text")

    def test_shop_english_text_required(self):
        self.check_mutation(lambda d: d["shop"]["items"][0].update(name_en=""), "empty bilingual shop text")

    def test_event_no_spend_before_prerequisite(self):
        self.check_mutation(lambda d: d["events"]["items"][1]["choices"][0]["requires"].update(jade_at_least=0), "not preconditioned atomically")

    def test_missing_floor(self):
        self.check_mutation(lambda d: d["floors"]["items"].pop(), "cover floors 1..50")

    def test_no_1v3(self):
        self.check_mutation(lambda d: d["floors"]["items"][2].update(type="1v3"), "invalid floor/zone/type")

    def test_gold_mask_only_two(self):
        self.check_mutation(lambda d: d["floors"]["items"][6]["encounters"][0]["bosses"].append(d["floors"]["items"][6]["encounters"][0]["bosses"][0]), "group must be gold mask x2")

    def test_disabled_incompatible_spectate_not_enabled(self):
        self.check_mutation(lambda d: d["floors"]["items"][16]["encounters"][0].update(enabled=True), "unsafe encounter enabled")

    def test_same_identity_in_pair(self):
        self.check_mutation(lambda d: d["floors"]["items"][2]["encounters"][0]["bosses"].__setitem__(1, d["floors"]["items"][2]["encounters"][0]["bosses"][0]), "missing blocked reason")

    def test_tower_lord_no_healing(self):
        self.check_mutation(lambda d: d["floors"]["items"][49]["rules"].update(heal_between_stages=True), "three sequential stages")

    def test_missing_source(self):
        self.check_mutation(lambda d: d["cards"]["items"][0].update(source=["notes/does-not-exist.md"]), "source missing")

    def test_source_fragment_is_real(self):
        self.check_mutation(lambda d: d["cards"]["items"][0].update(source=["notes/dump/Game.zh-Hans.json#Default/does-not-exist"]), "source fragment missing")

    def test_duplicate_json_keys_and_nan(self):
        with tempfile.TemporaryDirectory(prefix=".data-test-", dir=HERE) as folder:
            path = Path(folder) / "broken.json"
            for text in ('{"schema":1,"schema":2}', '{"value":NaN}'):
                path.write_text(text, encoding="utf8")
                with self.assertRaises(ValueError):
                    load(path)

    def test_generation_deterministic_and_does_not_write_engine(self):
        from prepare_data import build
        header = ROOT / "mods/tower/src/boss_pool.generated.h"
        before = header.read_bytes()
        self.assertEqual(build(), self.base)
        self.assertEqual(header.read_bytes(), before)

    def test_preview_pool_explicitly_excludes_stage2(self):
        pool = select(self.base["bosses"]["items"], include_unverified=True)
        self.assertTrue(pool)
        self.assertTrue(all(b["role"] == "primary" and b["selectable"] for b in pool))
        self.assertEqual(pool, sorted(pool, key=lambda b: (b["tier"], b["path"])))


if __name__ == "__main__":
    unittest.main(verbosity=2)
