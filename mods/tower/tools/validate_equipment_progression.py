"""Verify evidence references and fail-closed progression data, not gameplay."""
from pathlib import Path
import collections,hashlib,json
ROOT=Path(__file__).resolve().parents[3]
data=json.loads((ROOT/'mods/tower/data/equipment-progression.json').read_text(encoding='utf-8'))
cache={};checks=0
def check(ok,msg):
 global checks
 checks+=1
 if not ok:raise AssertionError(msg)
def resolve(r):
 path=r['path'];check(path in data['source_fingerprints'], 'reference file not fingerprinted: '+path)
 if path not in cache:cache[path]=json.loads((ROOT/path).read_text(encoding='utf-8-sig'))
 obj=cache[path]
 for key in r['pointer'].split('/')[1:]:
  key=key.replace('~1','/').replace('~0','~');obj=obj[int(key)] if isinstance(obj,list) else obj[key]
 return obj
def refs(obj):
 if isinstance(obj,dict):
  if 'path' in obj and 'pointer' in obj:resolve(obj)
  for v in obj.values():refs(v)
 elif isinstance(obj,list):
  for v in obj:refs(v)
for path,digest in data['source_fingerprints'].items():check(hashlib.sha256((ROOT/path).read_bytes()).hexdigest()==digest,'source changed: '+path)
catalog=json.loads((ROOT/'shared/items.json').read_text(encoding='utf-8'))
expected={(x['kind'],x['id'].casefold()) for x in catalog if x['kind'] in ['weapon','shaqi_weapon','accessory','medicine']}
items=data['items'];keys=[(x['kind'],x['id'].casefold()) for x in items]
check(set(keys)==expected and len(keys)==len(expected)==150,'catalog completeness/duplicates')
for x in items:
 check(x['first_tower_zone'] is None or 1<=x['first_tower_zone']<=5,'bad approved zone')
 if x['first_tower_zone'] is None:check(not x['allow_reward'] and not x['allow_shop'],'pending zone opened')
 if x['eligibility_status'] in ['待核','排除']:check(not x['allow_reward'] and not x['allow_shop'],'unreviewed or excluded opened')
 if x['boss_associated']:check(not x['allow_shop'],'boss weapon opened in shop')
 check(not x['proposal_is_approved'],'unreviewed proposal auto approved')
 check(x['catalog_sources'] and x['special_restrictions'] is not None,'missing evidence/restrictions')
 refs(x)
print(json.dumps({'ok':True,'checks':checks,'items':len(items),'status':collections.Counter(x['eligibility_status'] for x in items)},ensure_ascii=False))
