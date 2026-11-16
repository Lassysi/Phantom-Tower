import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_localization import boss_names, build, resolve


class ResolveTests(unittest.TestCase):
    def test_namespace_and_key_are_both_required(self):
        loc = {"Default": {"Weapon_1": "Official name"}, "Other": {"Weapon_1": "Wrong name"}}
        self.assertEqual(resolve({"Namespace": "Default", "Key": "Weapon_1"}, loc)["text"], "Official name")
        self.assertIsNone(resolve({"Namespace": "Missing", "Key": "Weapon_1"}, loc))

    def test_missing_or_placeholder_does_not_invent_a_translation(self):
        loc = {"Default": {"Empty": "", "Placeholder": "Placeholder"}}
        for ref in (None, {}, {"Namespace": "Default", "Key": "Empty"},
                    {"Namespace": "Default", "Key": "Placeholder"},
                    {"Namespace": "Default", "Key": "Absent"}):
            self.assertIsNone(resolve(ref, loc))

    def test_description_array_uses_first_level_without_losing_markup(self):
        ref = [{"Namespace": "Default", "Key": "Desc1"}, {"Namespace": "Default", "Key": "Desc2"}]
        loc = {"Default": {"Desc1": 'Boosts<InputAction id="HeavyAttack"/> damage by 20%.', "Desc2": "Different level"}}
        self.assertEqual(resolve(ref, loc)["text"], loc["Default"]["Desc1"])

    def test_boss_name_requires_matching_chinese_and_key(self):
        rows = [{"path": "/Boss", "name_zh": "原名", "note": "中文名=Game.zh-Hans.json:Default:EnemyName_1；"}]
        zh, en = {"Default": {"EnemyName_1": "原名"}}, {"Default": {"EnemyName_1": "Official Boss"}}
        self.assertEqual(boss_names(rows, zh, en)["/Boss"]["name"]["en"]["text"], "Official Boss")
        rows[0]["name_zh"] = "另一个名字"
        self.assertIsNone(boss_names(rows, zh, en)["/Boss"]["name"]["en"])

    def test_boss_fallback_requires_exact_unique_official_name(self):
        rows = [{"path": "/Boss", "name_zh": "首领", "note": ""}]
        zh = {"Default": {"BossName_1": "首领", "OtherKey": "首领"}}
        en = {"Default": {"BossName_1": "Official Boss", "OtherKey": "Unrelated"}}
        self.assertEqual(boss_names(rows, zh, en)["/Boss"]["name"]["en"]["text"], "Official Boss")
        zh["Default"]["BossName_2"] = "首领"
        en["Default"]["BossName_2"] = "Different Official Boss"
        self.assertIsNone(boss_names(rows, zh, en)["/Boss"]["name"]["en"])

    def test_ambiguous_same_id_retains_all_variants(self):
        import json
        import tempfile
        from pathlib import Path
        from gen_localization import SPECS
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for _, filename, _, _, _ in SPECS:
                (root / filename).write_text(json.dumps([{"Rows": {}}]), encoding="utf-8")
            rows = {"one": {"ItemId": "shared", "WeaponName": {"Namespace": "N", "Key": "A"}},
                    "two": {"ItemId": "shared", "WeaponName": {"Namespace": "N", "Key": "B"}}}
            (root / "DT_ShaqiWeaponData.json").write_text(json.dumps([{"Rows": rows}]), encoding="utf-8")
            loc = {"N": {"A": "First", "B": "Second"}}
            entry = build(root, loc, loc)["phantom_edges"]["shared"]
            self.assertIsNone(entry["name"]["en"])
            self.assertEqual(len(entry["variants"]), 2)

    def test_weapon_key_wins_over_character_name_for_same_id(self):
        import json
        import tempfile
        from pathlib import Path
        from gen_localization import SPECS
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for _, filename, _, _, _ in SPECS:
                (root / filename).write_text(json.dumps([{"Rows": {}}]), encoding="utf-8")
            rows = {"character": {"ItemId": "shared", "WeaponName": {"Namespace": "N", "Key": "Character_A"}},
                    "weapon": {"ItemId": "shared", "WeaponName": {"Namespace": "N", "Key": "PhantomEdges_015"}}}
            (root / "DT_ShaqiWeaponData.json").write_text(json.dumps([{"Rows": rows}]), encoding="utf-8")
            loc = {"N": {"Character_A": "Blaze", "PhantomEdges_015": "FIRE METEOR"}}
            entry = build(root, loc, loc)["phantom_edges"]["shared"]
            self.assertEqual(entry["name"]["en"]["text"], "FIRE METEOR")
            self.assertEqual(len(entry["variants"]), 2)


if __name__ == "__main__":
    unittest.main()
