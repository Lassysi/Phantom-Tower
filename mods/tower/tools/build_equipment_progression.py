"""Local static provenance inventory. Pending acquisition routes fail closed."""
from pathlib import Path
import collections, hashlib, json, re
ROOT=Path(__file__).resolve().parents[3]
BASE='notes/dump/tables/ProjectPBZ/Content/PBZContent/DataTable/'
OUT=ROOT/'mods/tower/data/equipment-progression.json'
def load(p):return json.loads((ROOT/p).read_text(encoding='utf-8-sig'))
def table(n):return load(BASE+n+'.json')[0]['Rows']
def ref(path,pointer,role,**kw):return dict(path=path,pointer=pointer,role=role,**kw)
def ptr(s):return str(s).replace('~','~0').replace('/','~1')
catalog=[x for x in load('shared/items.json') if x['kind'] in ['weapon','shaqi_weapon','accessory','medicine']]
assert len(catalog)==150 and len({x['id'].casefold() for x in catalog})==150
byid={x['id'].casefold():x for x in catalog};sources=collections.defaultdict(list);definitions={}
maps=table('UI/DT_MapData');chapters=table('UI/DT_ChapterData')
chapter_index={v['ChapterID']:(k,v) for k,v in chapters.items()}
map_index={}
for k,v in maps.items():
 for alias in [k,v['MapID']]:map_index[alias.casefold()]=(k,v)
def map_context(prefix):
 pair=map_index.get(prefix.casefold())
 if not pair:return None
 row,m=pair;ch=chapter_index.get(m['OwnChapterID'])
 if not ch:return None
 return {'map_id':m['MapID'],'map_row':row,'native_chapter_id':m['OwnChapterID'],'native_chapter_order':ch[1]['ChapterOrder'],
  'evidence':[ref(BASE+'UI/DT_MapData.json','/0/Rows/'+ptr(row),'map_definition'),ref(BASE+'UI/DT_ChapterData.json','/0/Rows/'+ptr(ch[0]),'native_chapter_order')]}
for name,kind in [('DT_WeaponData','weapon'),('DT_ShaqiWeaponData','shaqi_weapon'),('DT_MedicineTable','medicine'),('DT_Accessory','accessory')]:
 for row,d in table('UI/'+name).items():
  iid=d.get('ItemId') or d.get('ItemID') or d.get('MedicineID') or d.get('AccessoryID')
  if not iid:
   # Catalog provenance already supplies the exact native row name.
   matches=[x for x in catalog if x['kind']==kind and ('row='+row+'；') in x.get('note','')]
   if len(matches)==1:iid=matches[0]['id']
  if iid and iid.casefold() in byid:
   definitions.setdefault(iid.casefold(),[]).append((BASE+'UI/'+name+'.json',row,d))
for table_name in ['DT_LevelLootDataTable','DT_CombatLootDataTable']:
 for row,d in table(table_name).items():
  for ei,e in enumerate(d.get('EntryDatas',[])):
   for field,idfield in [('Weapons','WeaponID'),('Medicines','MedicineID'),('Accessories','AccessoryID')]:
    for ii,v in enumerate(e.get(field,[])):
     iid=v.get(idfield,'');key=iid.casefold()
     if key not in byid:continue
     ctx=map_context(row.split('.')[0]) if table_name=='DT_LevelLootDataTable' else None
     sources[key].append(ref(BASE+table_name+'.json',f'/0/Rows/{ptr(row)}/EntryDatas/{ei}/{field}/{ii}','native_loot_definition',
       row=row,probability=v.get('Probability'),native_context=ctx,route_verified=False,
       excluded_as_test=row.casefold().startswith('test')))
shop_items=table('UI/DT_ShopItemTable');shop_groups=table('UI/DT_ShopTable')
for row,d in shop_items.items():
 iid=d.get('SellItemID','');key=iid.casefold()
 if key not in byid:continue
 groups=[]
 for gr,g in shop_groups.items():
  for i,entry in enumerate(g.get('SellItems',[])):
   if entry.get('Key')==d.get('ShopItemID'):
    groups.append({'shop_row':gr,'unlock_tag':g.get('UnLockShopTag'),'evidence':ref(BASE+'UI/DT_ShopTable.json',f'/0/Rows/{ptr(gr)}/SellItems/{i}','native_shop_stock')})
 sources[key].append(ref(BASE+'UI/DT_ShopItemTable.json','/0/Rows/'+ptr(row),'native_shop_item_definition',row=row,native_shops=groups,route_verified=False,native_context=None))
boss=collections.defaultdict(list)
for r in load('mods/tower/data/rewards.json')['items']:
 if r['kind']!='weapon_drops':continue
 for item in r['items']:
  boss[item['id'].casefold()].append({'identity':r['identity'],'classes':r['bosses'],'association_status':r.get('association_status'),
   'association_verified':item.get('association_verified',False),'native_grant_verified':r.get('natural_grant_verified',False),
   'evidence':ref('mods/tower/data/rewards.json','/items/'+str(load('mods/tower/data/rewards.json')['items'].index(r)),'tower_design_association')})
starter={'Tang','IronBlade','Double','Long','UnnamedBow','BrutalAxe','Hulu','Medicine_Recover','Medicine_Duration','Medicine_AdditionalRecover','KuYe'}
story={'WoodenSword','WoodenSwordHun','Tang_HorseCartCombat','TangPrologue','HeiMoShangPrologue'}
excluded_acc={'AC_Loot'}
def proposed_zone(order):return 1 if order==1 else 2 if order==2 else 3 if order<=5 else 4 if order<=10 else 5
items=[]
for item in catalog:
 iid=item['id'];key=iid.casefold();defs=definitions.get(key,[]);src=sources[key]
 valid=[x for x in src if not x.get('excluded_as_test') and x.get('native_context')]
 earliest=min(valid,key=lambda x:x['native_context']['native_chapter_order']) if valid else None
 approved=iid in starter;excluded=iid in story or iid in excluded_acc or not item['name_zh']
 constraints=[]
 if iid in story:constraints.append('剧情/演出专用，不进塔池')
 if iid in excluded_acc:constraints.append('塔池排除：权益尘掉落/潜行/骑兵效果；聚宝盆按原档权益恢复')
 if not item['name_zh']:constraints.append('原生目录无可靠显示名称，不以内部ID凑奖励')
 if not defs:constraints.append('UI定义匹配待核')
 native_limits=[];snap=None;refine=None
 for path,row,d in defs:
  for k,v in d.items():
   if any(word in k.lower() for word in ['locktag','required','condition','addtagwhen']):native_limits.append({'field':k,'value':v,'evidence':ref(path,'/0/Rows/'+ptr(row)+'/'+ptr(k),'native_condition_field')})
  if item['kind']=='weapon':snap=bool(d.get('WeaponSnapSoulData'))
  if item['kind']=='shaqi_weapon':snap=bool(d.get('ShaChiWeaponSnapSoulData'))
 if snap is False:constraints.append('原生定义无铸魂数据，不提供铸魂选项')
 if not excluded and not approved:constraints.append('首次正式获取路径未穷尽：待核；暂不随机发放/售卖')
 items.append({'id':iid,'name_zh':item['name_zh'],'kind':item['kind'],
  'catalog_sources':[ref('shared/items.json','/'+str(load('shared/items.json').index(item)),'catalog_identity')]+[ref(p,'/0/Rows/'+ptr(r),'native_definition') for p,r,d in defs],
  'native_acquisition_sources':src,'native_first_obtain_status':'待核：有静态来源线索，未穷尽正式获取路径' if src else '待核：当前导出未定位正式获取路径',
  'earliest_located_native_chapter_order':earliest['native_context']['native_chapter_order'] if earliest else None,
  'first_tower_zone':1 if approved and not excluded else None,
  'first_zone_basis':'用户明确的2+2+2起手/备选药规则，非原生首次获取结论' if approved else '待核',
  'proposed_first_tower_zone':proposed_zone(earliest['native_context']['native_chapter_order']) if earliest else None,
  'proposal_is_approved':False,'boss_associated':bool(boss[key]),'boss_associations':boss[key],
  'allow_reward':approved and not excluded,'allow_shop':approved and not excluded and not bool(boss[key]),
  'eligibility_status':'排除' if excluded else '起手规则可用' if approved else '待核',
  'snap_soul_supported':snap,'native_condition_fields':native_limits,'special_restrictions':constraints,
  'usage_verified_in_tower':False})
assert len(items)==150
paths={BASE+'UI/'+x+'.json' for x in ['DT_MapData','DT_ChapterData','DT_WeaponData','DT_ShaqiWeaponData','DT_MedicineTable','DT_Accessory','DT_ShopTable','DT_ShopItemTable']}
paths|={BASE+x+'.json' for x in ['DT_LevelLootDataTable','DT_CombatLootDataTable']};paths|={'shared/items.json','mods/tower/data/rewards.json','mods/tower/src/tower_growth.h'}
doc={'schema':1,'generated':'2026-11-01','game_build':'26266538','evidence_level':'static; not live acquisition acceptance',
 'policy':{'pending_is_blocked':True,'null_zone_must_not_default_to_one':True,'first_tower_zone_is_approved_only':True,
  'chapter_to_zone_proposal':{'1':1,'2':2,'3-5':3,'6-10':4,'11-12':5},
  'mapping_warning':'塔旧地理配对与原生章节顺序不一致。庞镇原生第4章、梅谷湖第7章，不能因塔场地属于一区/二区就提前发其装备。章节分段只是待审提案。',
  'static_loot_row_is_not_proof_of_live_trigger':True},
 'source_fingerprints':{p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in sorted(paths)},'items':items}
OUT.write_text(json.dumps(doc,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print(json.dumps({'path':str(OUT),'items':len(items),'kinds':collections.Counter(x['kind'] for x in items),'status':collections.Counter(x['eligibility_status'] for x in items),
 'with_native_acquisition_refs':sum(bool(x['native_acquisition_sources']) for x in items),'proposed_zones':sum(x['proposed_first_tower_zone'] is not None for x in items)},ensure_ascii=False))
md=OUT.with_name('equipment-progression-review.md')
rows=['# 装备进度逐项审查（完整150项）\n\n先读 equipment-progression.md。正式来源与首次塔区未核实的条目，奖励/商店禁入；建议区不等于批准区。\n\n| ID | 名称 | 类别 | 批准区 | 来源建议区 | Boss关联 | 状态 | 静态来源数 |\n|---|---|---|---|---|---|---|---|\n']
for x in items:rows.append('| '+' | '.join(map(str,[x['id'],x['name_zh'] or '未命名',x['kind'],x['first_tower_zone'] or '待核',x['proposed_first_tower_zone'] or '待核','是' if x['boss_associated'] else '未见关联',x['eligibility_status'],len(x['native_acquisition_sources'])]))+' |\n')
md.write_text(''.join(rows),encoding='utf-8')
