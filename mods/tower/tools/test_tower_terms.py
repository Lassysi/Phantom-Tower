import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_tower_terms import BLESSINGS, ITEMS, POWERS

ROOT = Path(__file__).resolve().parents[3]


def ids(path, array):
    source = (ROOT / path).read_text(encoding="utf-8-sig")
    body = source.split(f"{array}[] = {{", 1)[1].split("\n};", 1)[0]
    if array == "kShentong":
        return set(re.findall(r'^\s*\{"([a-z0-9_]+)"\s*,', body, re.M))
    return set(re.findall(r'^\s*\{(?:Item|Bless)::\w+\s*,\s*"([a-z0-9_]+)"\s*,', body, re.M))


class TermsTests(unittest.TestCase):
    def test_ids_cover_actual_tower_content(self):
        self.assertEqual(ids("mods/tower/src/tower_economy.h", "kItems"), set(ITEMS))
        self.assertEqual(ids("mods/tower/src/tower_economy.h", "kShentong"), set(POWERS))
        self.assertEqual(ids("mods/tower/src/tower_growth.h", "kBlessings"), set(BLESSINGS))

    def test_no_blank_english(self):
        for table in (ITEMS, POWERS, BLESSINGS):
            for name, description in table.values():
                self.assertTrue(name.strip())
                self.assertTrue(description.strip())


if __name__ == "__main__":
    unittest.main()
