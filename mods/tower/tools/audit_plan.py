# 50 层 / 77 场地结构化审计（notes/tower-repair-33-38.md 第 F 节）。用法：python -X utf8 -I mods/tower/tools/audit_plan.py
# 输入：floors_plan.py 的逐层表与闸门（DEPS / NATIVE_ONLY / LANDING_BAD）、venues.generated.h、Codex 的放置审查与两轮实机记录。
# 输出：notes/tower-audit-v6.json（逐项结构化）与 notes/tower-audit-v6.md（表）。未知一律写"未知"，不升级证据等级。
import json, pathlib, runpy, sys, io, contextlib

ROOT = pathlib.Path(__file__).resolve().parents[3]
CODEX = pathlib.Path(r'E:\Phantom_Blade_Zero')
with contextlib.redirect_stdout(io.StringIO()):
    plan = runpy.run_path(str(ROOT / 'mods/tower/tools/floors_plan.py'), run_name='audit')
F, boss, CLASSIC, ZONE_CLASSIC, VEN = plan['F'], plan['boss'], plan['CLASSIC'], plan['ZONE_CLASSIC'], plan['VEN']
DEPS, NATIVE_ONLY, LANDING_BAD, ALLIES, MODE = plan['DEPS'], plan['NATIVE_ONLY'], plan['LANDING_BAD'], plan['ALLIES'], plan['MODE']

def load(p):
    try: return json.loads(pathlib.Path(p).read_text(encoding='utf-8'))
    except Exception: return None
review = load(CODEX / 'mods/tower/data/venue_placement_review_ed47e1f.json') or {'items': []}
hist32 = load(CODEX / 'mods/tower/data/floor_live_coverage_30324ba.json') or {'items': []}
live38 = load(CODEX / 'build/tower-ed47e1f/FLOOR_RECORDS_insert-fix.json') or {'items': []}

path_of = {k: v['path'] for k, v in boss.items()}
stem_of = {v: k for k, v in path_of.items()}
placement = {}
for it in review.get('items', []):
    pr = it.get('placement_review', {})
    placement[stem_of.get(it.get('boss_class'), '?')] = f"{pr.get('status', '未知')} / 遭遇 {pr.get('encounter_type', '未知')} / 空间 {pr.get('spatial_type', '未知')}"

# 实机结果按"类"归（v4 / v5 / v6 层号不同，按类对得上）
def by_class(records, key):
    out = {}
    for it in records.get('items', []):
        st = it.get(key)
        if not st or st in ('pending', 'not_tested') or str(st).startswith('not_tested'): continue
        for c in it.get('planned_classes', []):
            out.setdefault(stem_of.get(c, c), []).append(f"第 {it['floor']} 层：{st}")
        v = it.get('planned_venue_class')
        if v: out.setdefault('venue:' + stem_of.get(v, v), []).append(f"第 {it['floor']} 层：{st}")
    return out
h32 = by_class(hist32, 'logic_status')
h38 = by_class(live38, 'live_status')

def venue_stem(bs, v): return bs[0] if v == 'native' else CLASSIC[v][0]
def vinfo(stem):
    ve = VEN.get(stem)
    if not ve: return {'world': '无场地数据', 'sky': '未知', 'doors': '未知', 'area_m2': '未知'}
    return {'world': ve['world'], 'sky': '高空独立空间' if ve['sky'] else '普通高度', 'doors': ve['doors'], 'area_m2': round(ve['area'], 1)}

def level(stem):
    ev = h32.get(stem, []) + h38.get(stem, [])
    if not ev: return '静态（未实机）'
    if any('fail' in e for e in ev): return '实机失败（见记录）' + ('，基本战斗 / 判胜已观察' if any('victory' in e or 'passed' in e for e in ev) else '')
    if any('victory' in e or 'passed' in e for e in ev): return '实机已观察（辅助配置下，有待核项）'
    return '实机记录（见原文）'

floors = []
for (f, zone, mode, bs, venue, alts, note) in F:
    vs = venue_stem(bs, venue)
    zc = CLASSIC[ZONE_CLASSIC[zone]][0]
    floors.append({
        'floor': f, 'zone': zone, 'mode': mode,
        'bosses': [{'stem': s, 'name': boss[s]['name'], 'identity': boss[s]['id'], 'native_world': boss[s]['native'] or '无',
                    'deps': DEPS.get(s, ''), 'native_only': NATIVE_ONLY.get(s, ''), 'evidence': level(s),
                    'history_30324ba': h32.get(s, []), 'history_ed47e1f_insert': h38.get(s, [])} for s in bs],
        'venue': {'stem': vs, 'kind': '原生' if venue == 'native' else f'经典 {venue}', **vinfo(vs), 'placement_review': placement.get(vs, '未审'),
                  'landing_bad': LANDING_BAD.get(vs, ''), 'live': h32.get('venue:' + vs, []) + h38.get('venue:' + vs, [])},
        'backups': [{'stem': a, 'name': boss[a]['name'], 'deps': DEPS.get(a, ''), 'landing_bad_native': LANDING_BAD.get(a, '')} for a in alts],
        'zone_fallback': {'stem': zc, 'eligible': zc not in LANDING_BAD, 'why': LANDING_BAD.get(zc, '可作兜底（未判坏）')},
        'allies': ALLIES.get(f, []),
        'hp_share': MODE[mode][0], 'note': note,
    })
venues = []
used = {}
for fl in floors: used.setdefault(fl['venue']['stem'], []).append(fl['floor'])
for stem in sorted(VEN, key=lambda s: (VEN[s]['world'], s)):
    venues.append({'stem': stem, 'name': boss.get(stem, {}).get('name', '?'), **vinfo(stem), 'placement_review': placement.get(stem, '未审'),
                   'landing_bad': LANDING_BAD.get(stem, ''), 'used_by_floors': used.get(stem, []),
                   'classic': [k for k, v in CLASSIC.items() if v[0] == stem], 'live': h32.get('venue:' + stem, []) + h38.get('venue:' + stem, []),
                   'coordinate_chain': '出生点偏移 0 只证明对象位置匹配，不证明地面 / 碰撞 / 战斗依赖可用'})
out = {'schema': 1, 'plan_version': plan['PLAN_VERSION'], 'floors': floors, 'venues': venues,
       'evidence_levels': '静态（未实机）/ 实机已观察（有待核项）/ 实机失败；辅助配置（god / 锁血 / 3 倍）下的样本不代表正常伤害、完整阶段'}
(ROOT / 'notes/tower-audit-v6.json').write_text(json.dumps(out, ensure_ascii=False, indent=1), encoding='utf-8')

L = [f'# 影塔 50 层 / 77 场地审计（方案 v{plan["PLAN_VERSION"]}，由 `mods/tower/tools/audit_plan.py` 生成）', '',
     '未知就写"未知"，证据等级不自动升级。放置类型来自 Codex 的 `venue_placement_review_ed47e1f.json`（77 个里 75 个未审）。', '',
     '## 逐层', '', '| 层 | 层型 | Boss（身份） | 证据 | 场地（类型 / 高度 / 门 / 面积） | 落点 | 依赖 / 原生限定 | 备选 | 本区兜底 |', '|---|---|---|---|---|---|---|---|---|']
for fl in floors:
    b = '、'.join(f"{x['name']}（{x['identity']}）" for x in fl['bosses'])
    ev = '；'.join(x['evidence'] for x in fl['bosses'])
    v = fl['venue']
    dep = '；'.join(filter(None, [x['deps'] and '缺依赖：' + x['deps'] for x in fl['bosses']] + [x['native_only'] and '只在原生场地' for x in fl['bosses']])) or '—'
    land = ('**实机落不了地**' if v['landing_bad'] else ('已观察可生成' if any('victory' in e or 'passed' in e for e in v['live']) else '未知'))
    alts = '、'.join(a['name'] for a in fl['backups']) or '—'
    zf = fl['zone_fallback']
    L.append(f"| {fl['floor']} | {fl['mode']} | {b} | {ev} | {v['kind']} {boss[v['stem']]['name']}（{v['world']} / {v['sky']} / {v['doors']} 门 / {v['area_m2']} m²） | {land} | {dep} | {alts} | {boss[zf['stem']]['name']}{'' if zf['eligible'] else '（不可兜底）'} |")
L += ['', '## 场地（77）', '', '| 场地 | 世界 | 高度 | 门 | 面积 | 落点 | 放置审查 | 用于层 | 经典 |', '|---|---|---|---|---|---|---|---|---|']
for v in venues:
    land = '**实机落不了地**：' + v['landing_bad'] if v['landing_bad'] else ('已观察可生成' if any('victory' in e or 'passed' in e for e in v['live']) else '未知')
    L.append(f"| {v['name']} | {v['world']} | {v['sky']} | {v['doors']} | {v['area_m2']} | {land} | {v['placement_review']} | {'、'.join(map(str, v['used_by_floors'])) or '—'} | {'、'.join(v['classic']) or '—'} |")
(ROOT / 'notes/tower-audit-v6.md').write_text('\n'.join(L) + '\n', encoding='utf-8')
print(f'audit: {len(floors)} floors, {len(venues)} venues -> notes/tower-audit-v6.json / .md')
