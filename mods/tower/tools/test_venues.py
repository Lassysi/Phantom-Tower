"""Offline extraction regressions and independent native-JS geometry oracle."""
import json
from pathlib import Path
import random
import shutil
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parent))
import build_venues as v


class VenueTests(unittest.TestCase):
    def fixture(self,objects,pkg='/Game/Maps/Fixture_WP'):
        scratch=v.ROOT/'build/tower-venues/tests';scratch.mkdir(parents=True,exist_ok=True)
        folder=Path(tempfile.mkdtemp(dir=scratch))
        path=folder/'ProjectPBZ/Content'/str(pkg.removeprefix('/Game/'))
        path=path.with_suffix('.json');path.parent.mkdir(parents=True,exist_ok=True)
        path.write_text(json.dumps(objects),encoding='utf-8')
        return v.Index([path])

    def test_negative_origin_yaw_and_child_scale(self):
        root=v.matrix({'RelativeLocation':{'X':-200,'Y':-300,'Z':40},'RelativeRotation':{'Yaw':90},'RelativeScale3D':{'X':2,'Y':3,'Z':1}})
        child=v.matrix({'RelativeLocation':{'X':10,'Y':20,'Z':5},'RelativeRotation':{'Yaw':90}})
        result=v.compose_trs(root,child)
        for got,want in zip(v.apply(result,[0,0,0]),[-260,-280,45]):self.assertAlmostEqual(got,want)
        # Quaternion/scale composition, not a sheared parent*child matrix.
        for got,want in zip(v.apply(result,[1,0,0]),[-262,-280,45]):self.assertAlmostEqual(got,want)

    def test_relative_attachment_never_used_as_world(self):
        p='/Game/Maps/Fixture_WP';ref=lambda i:{'ObjectPath':f'{p}.{i}'}
        idx=self.fixture([
            {'Type':'BP_AISpawner_C','Name':'A','Outer':{'ObjectName':"Level'Fixture_WP:PersistentLevel'"},'Properties':{'RootComponent':ref(1)}},
            {'Type':'SceneComponent','Name':'Scene','Properties':{'RelativeLocation':{'X':2,'Y':3,'Z':4},'AttachParent':ref(2)}},
            {'Type':'SceneComponent','Name':'Parent','Properties':{'RelativeLocation':{'X':100,'Y':200,'Z':300}}},
            {'Type':'Level','Name':'PersistentLevel','Properties':{'bIsPartitioned':True}},
        ])
        pose=idx.pose(p+'.0');self.assertEqual(pose['position_world'],[102,203,304])
        self.assertEqual(len(pose['transform_chain']),2)
        idx.nodes[p+'.1']['Properties']['AttachSocketName']='bone_missing'
        idx.pose.cache_clear();self.assertIsNone(idx.pose(p+'.0')['position_world'])

    def test_generated_cell_missing_transform_stays_pending(self):
        p='/Game/Maps/Fixture_WP/_Generated_/CELL';ref=lambda i:{'ObjectPath':f'{p}.{i}'}
        idx=self.fixture([
            {'Type':'BP_AISpawner_C','Name':'A','Properties':{'RootComponent':ref(1)}},
            {'Type':'SceneComponent','Name':'Scene','Properties':{'RelativeLocation':{'X':1,'Y':2,'Z':3}}},
            {'Type':'Level','Name':'PersistentLevel','Properties':{}},
        ],p)
        pose=idx.pose(p+'.0');self.assertEqual(pose['position_level'],[1,2,3]);self.assertIsNone(pose['position_world'])

    def test_soft_members_default_group_and_ambiguous_scene(self):
        p='/Game/Maps/Fixture_WP';ref=lambda i:{'ObjectPath':f'{p}.{i}'}
        sp={'AssetPathName':p+'.Fixture_WP','SubPathString':'PersistentLevel.Spawner_UAID_123'}
        dom={'AssetPathName':p+'.Fixture_WP','SubPathString':'PersistentLevel.Domain_UAID_456'}
        idx=self.fixture([
            {'Type':'BP_AISpawner_C','Name':'Spawner_UAID_123','Outer':{'ObjectName':"Level'Fixture_WP:PersistentLevel'"},'Properties':{'SpawnInfo':{'MobDomain':dom}}},
            {'Type':'BP_MobDomain_C','Name':'Domain_UAID_456','Outer':{'ObjectName':"Level'Fixture_WP:PersistentLevel'"},'Properties':{'MobDomainConfig':{'MobGroups':[{'Key':'DefaultGroup','Value':ref(2)}]}}},
            {'Type':'MobGroup','Name':'Group','Properties':{'Members':[sp]}},
            {'Type':'SceneComponent','Name':'Scene','Properties':{}},
            {'Type':'SceneComponent','Name':'Scene','Properties':{}},
        ])
        self.assertEqual(idx.group_for(p+'.0')[0],p+'.2')
        self.assertEqual(idx.group_members[p+'.0'],[p+'.2'])
        self.assertIsNone(idx.resolve({'AssetPathName':p+'.Fixture_WP','SubPathString':'Scene'}))

    def test_player_dead_field_exact_and_no_fake_empty_hull(self):
        self.assertIn('FogDoorsToDeactivatePlayerDead',v.EVENTS)
        self.assertNotIn('FogDoorsToDeactivateOnPlayerDead',v.EVENTS)
        self.assertEqual(v.convex_hull([[0,0],[0,0],[1,1],[2,2]]),[])

    def test_multiple_reverse_groups_remain_explicit(self):
        p='/Game/Maps/Fixture_WP';ref=lambda i:{'ObjectPath':f'{p}.{i}'}
        soft={'AssetPathName':p+'.Fixture_WP','SubPathString':'PersistentLevel.Spawner_UAID_123'}
        idx=self.fixture([
            {'Type':'BP_AISpawner_C','Name':'Spawner_UAID_123','Outer':{'ObjectName':"Level'Fixture_WP:PersistentLevel'"},'Properties':{'MobGroup':ref(1)}},
            {'Type':'MobGroup','Name':'G1','Properties':{'Members':[soft]}},
            {'Type':'MobGroup','Name':'G2','Properties':{'Members':[soft]}},
        ])
        g,_,_,_,conflicts=idx.group_for(p+'.0')
        self.assertEqual(g,p+'.1');self.assertEqual(conflicts,[p+'.2'])

    def test_native_js_is_independent_geometry_oracle(self):
        node=shutil.which('node')
        self.assertIsNotNone(node,'Node is needed to execute the actual native JS oracle')
        rng=random.Random(20261101)
        cases=[[[0,0],[1000,0],[1000,1000],[0,1000]], [[-10000,-5000],[-8000,-5000],[-8000,-2000],[-10000,-2000]],
               [[0,0],[1,1],[2,2]],[[0,0],[0,0],[400,0],[400,40],[0,40]]]
        cases += [[[rng.uniform(-20000,20000),rng.uniform(-20000,20000)] for _ in range(15)] for _ in range(40)]
        # Include every current extracted activation spline, with nonzero origins.
        if v.DATA.exists():
            data=json.loads(v.DATA.read_text(encoding='utf-8'))
            for item in data['items']:
                for script in item['fog_doors']['link_scripts']:
                    pts=[p[:2] for d in item['fog_doors']['FogDoorsToActivate'] if d.get('link_script_ref')==script['source_ref'] for p in d.get('spline_points_level') or []]
                    cases.append(pts)
        js="""
const fs=require('fs'),vm=require('vm');
const x=JSON.parse(fs.readFileSync(0,'utf8'));
const ctx={exports:{},require:()=>({}),console:{warn:()=>{}},setInterval:()=>0,clearInterval:()=>{}};
vm.runInNewContext(fs.readFileSync(x.source,'utf8'),ctx);
const obj=new ctx.exports.MobGroupScript_LinkFogDoorMixinInterface();
process.stdout.write(JSON.stringify(x.cases.map(pts=>{const raw=obj._convexHull2D(pts);return {raw,inset:raw.length>=3?obj._insetHull(raw,60):[]};})));
"""
        result=subprocess.run([node,'-e',js],input=json.dumps({'source':str(v.JS),'cases':cases}),text=True,capture_output=True,check=True)
        oracle=json.loads(result.stdout)
        for pts,expected in zip(cases,oracle):
            raw=v.convex_hull(pts);self.assertEqual(raw,expected['raw'])
            got=v.inset_hull(raw,60) if len(raw)>=3 else []
            for a,b in zip(got,expected['inset']):
                for x,y in zip(a,b):self.assertAlmostEqual(x,y,places=6)


if __name__=='__main__':unittest.main()
