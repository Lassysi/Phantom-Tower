"""Reference-resolution and streaming regressions; no game access."""
import json
from pathlib import Path
import tempfile
import unittest
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from audit_native_arenas import ROOT, audit_file, exports


class NativeArenaTests(unittest.TestCase):
    def fixture(self, name='PBZ_Arena'):
        scratch = ROOT/'build/native-arena-tests'
        scratch.mkdir(parents=True, exist_ok=True)
        folder = Path(tempfile.mkdtemp(dir=scratch))
        path = folder/f'ProjectPBZ/Content/Maps/Arena/{name}.json'
        path.parent.mkdir(parents=True)
        return path, f'/Game/Maps/Arena/{name}'

    def test_stream_preserves_nested_strings_and_unicode(self):
        p, _ = self.fixture()
        objects = [{'Type': 'Example', 'Properties': {'text': '雾门 } [ \\"', 'arr': [1, {'x': 2}]}}, {'Type': 'Second'}]
        p.write_text(json.dumps(objects, ensure_ascii=False), encoding='utf-8')
        self.assertEqual(list(exports(p)), objects)

    def test_truncated_export_not_silently_complete(self):
        p, _ = self.fixture()
        p.write_text('[{"Type":"Incomplete"}', encoding='utf-8')
        with self.assertRaises(ValueError):
            list(exports(p))

    def test_repeated_scene_names_resolve_exact_export_indices(self):
        p, pkg = self.fixture()
        ref = lambda i: {'ObjectPath': f'{pkg}.{i}', 'ObjectName': "SceneComponent'Scene'"}
        objects = [
            {'Type': 'BP_AISpawner_C', 'Name': 'SpawnerA', 'Properties': {'Class': {'AssetPathName': '/Game/Boss.Boss_C'}, 'RootComponent': ref(4), 'MobGroup': ref(1)}},
            {'Type': 'MobGroup', 'Name': 'Group', 'Properties': {'Scripts': [ref(2)]}},
            {'Type': 'MobGroupScript_LinkFogDoor_C', 'Name': 'FogScript', 'Properties': {'FogDoorsToActivate': [ref(3)]}},
            {'Type': 'BP_FogDoor_C', 'Name': 'DoorA', 'Properties': {'RootComponent': ref(5)}},
            {'Type': 'SceneComponent', 'Name': 'Scene', 'Properties': {'RelativeLocation': {'X': 1, 'Y': 2, 'Z': 3}}},
            {'Type': 'SceneComponent', 'Name': 'Scene', 'Properties': {'RelativeLocation': {'X': 4, 'Y': 5, 'Z': 6}}},
            {'Type': 'SceneComponent', 'Name': 'Scene', 'Properties': {'RelativeLocation': {'X': 999, 'Y': 999, 'Z': 999}}},
        ]
        p.write_text(json.dumps(objects), encoding='utf-8')
        rows, count = audit_file(p, {'/Game/Boss.Boss_C': {'id': 'Boss_C', 'name': 'Boss', 'identity': 'Boss'}})
        self.assertEqual(count, 7)
        self.assertEqual(rows[0]['boss_transform']['position'], [1, 2, 3])
        self.assertEqual(rows[0]['fog_doors'][0]['transform']['position'], [4, 5, 6])
        self.assertEqual(rows[0]['fog_link'], 'explicit_group_script')
        self.assertIsNone(rows[0]['player_landing'])
        self.assertFalse(rows[0]['fog_runtime_verified'])

    def test_pso_map_never_a_challenge_candidate(self):
        p, pkg = self.fixture('PSOCombat')
        objects = [{'Type': 'BP_AISpawner_C', 'Name': 'Probe', 'Properties': {'Class': {'AssetPathName': '/Game/Boss.Boss_C'}, 'RootComponent': {'ObjectPath': pkg+'.1'}}},
                   {'Type': 'SceneComponent', 'Name': 'Scene', 'Properties': {'RelativeLocation': {'X': 1, 'Y': 2, 'Z': 3}}}]
        p.write_text(json.dumps(objects), encoding='utf-8')
        rows, _ = audit_file(p, {'/Game/Boss.Boss_C': {'id': 'Boss_C', 'name': 'Boss', 'identity': 'Boss'}})
        self.assertFalse(rows[0]['challenge_candidate'])


if __name__ == '__main__':
    unittest.main()
