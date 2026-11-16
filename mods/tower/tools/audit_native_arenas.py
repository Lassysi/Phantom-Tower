"""Offline Boss -> MobGroup -> fog door audit. Never connects to the game.

Consumes complete CUE4Parse package JSON exports. ObjectPath export indices are
authoritative; repeated component names and geometric proximity are not joins.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]
DATA = ROOT / 'mods/tower/data/native_arena_links.json'
REPORT = ROOT / 'notes/codex-tower-native-arenas.md'


def exports(path):
    """Stream one top-level export at a time; map dumps can exceed 250 MB."""
    decoder = json.JSONDecoder()
    with path.open(encoding='utf-8-sig') as stream:
        buf, pos, eof, first = '', 0, False, True
        while True:
            if pos > 4 * 1024 * 1024:
                buf, pos = buf[pos:], 0
            if pos >= len(buf) and not eof:
                chunk = stream.read(4 * 1024 * 1024)
                eof = not chunk
                buf += chunk
            while pos < len(buf) and buf[pos].isspace():
                pos += 1
            if pos >= len(buf):
                if eof:
                    raise ValueError('Incomplete export array')
                continue
            if first:
                if buf[pos] != '[':
                    raise ValueError('Expected export array')
                first = False
                pos += 1
                continue
            if buf[pos] == ']':
                return
            if buf[pos] == ',':
                pos += 1
                continue
            try:
                obj, end = decoder.raw_decode(buf, pos)
            except json.JSONDecodeError:
                if eof:
                    raise
                buf, pos = buf[pos:], 0
                chunk = stream.read(4 * 1024 * 1024)
                eof = not chunk
                buf += chunk
                continue
            pos = end
            yield obj


def package(path):
    s = path.as_posix()
    m = re.search(r'ProjectPBZ/Content/(.+)\.json$', s, re.I)
    return '/Game/' + m[1] if m else None


def level_of(pkg):
    m = re.search(r'/([^/]+)/_Generated_/', pkg)
    return m[1] if m else pkg.rsplit('/', 1)[-1]


def xyz(v):
    return [v[k] for k in ('X', 'Y', 'Z')] if isinstance(v, dict) and all(k in v for k in ('X', 'Y', 'Z')) else None


def audit_file(path, bosses):
    pkg = package(path)
    if not pkg:
        return [], 0
    selected = {}
    total = 0
    keep = {'SceneComponent', 'SplineComponent', 'DynamicMeshComponent', 'BrushComponent', 'MobGroup'}
    for i, e in enumerate(exports(path)):
        total += 1
        t = e.get('Type', '')
        if t in keep or any(s in t for s in ('Spawner', 'MobDomain', 'FogDoor', 'MobGroupScript', 'TriggerScripts')):
            selected[i] = e
    by_name = {}
    for i, e in selected.items():
        by_name.setdefault(e.get('Name'), []).append(i)

    def resolve(ref):
        if not isinstance(ref, dict):
            return None, None
        obj = ref.get('ObjectPath', '')
        m = re.fullmatch(r'(.+)\.(\d+)', obj)
        if m and m[1].casefold() == pkg.casefold():
            i = int(m[2])
            return selected.get(i), f'{pkg}.{i}'
        # Soft actor refs contain a unique UAID. Never use generic Scene names.
        name = ref.get('SubPathString', '').rsplit('.', 1)[-1]
        if '_UAID_' in name and len(by_name.get(name, [])) == 1:
            i = by_name[name][0]
            return selected[i], f'{pkg}.{i}'
        return None, obj or ref.get('SubPathString') or None

    def pose(actor):
        props = actor.get('Properties', {})
        root, source = resolve(props.get('RootComponent'))
        if not root:
            return None
        p = root.get('Properties', {})
        loc = xyz(p.get('RelativeLocation'))
        return {'position': loc, 'rotation': p.get('RelativeRotation', {'Pitch': 0, 'Yaw': 0, 'Roll': 0}),
                'scale': xyz(p.get('RelativeScale3D')) or [1, 1, 1],
                'attached': bool(p.get('AttachParent')), 'source_ref': source,
                'space': 'component_relative' if p.get('AttachParent') else 'unattached_root',
                'level_instance_transform_verified': False} if loc else None

    def geometry(domain):
        p = domain.get('Properties', {})
        verts = [xyz(v) for h in p.get('ConvexHullCache', []) for v in h.get('Vertices', [])]
        verts = [v for v in verts if v]
        spline, sr = resolve(p.get('SplineComponent') or p.get('Spline'))
        sp = (spline or {}).get('Properties', {})
        points = sp.get('SplineCurves', {}).get('position', {}).get('Points', [])
        bounds = {'min': [min(v[i] for v in verts) for i in range(3)],
                  'max': [max(v[i] for v in verts) for i in range(3)]} if verts else None
        return {'convex_hull_count': len(p.get('ConvexHullCache', [])), 'bounds_local': bounds,
                'spline_point_count': len(points), 'spline_closed': sp.get('bClosedLoop'), 'spline_ref': sr,
                'radius': None, 'walkable_verified': False}

    rows = []
    for i, e in selected.items():
        p = e.get('Properties', {})
        cls = p.get('Class', {})
        cls = cls.get('AssetPathName') if isinstance(cls, dict) else None
        if 'Spawner' not in e.get('Type', '') or cls not in bosses:
            continue
        spawn = p.get('SpawnInfo', {})
        group, gr = resolve(p.get('MobGroup'))
        domain, dr = resolve(spawn.get('MobDomain'))
        if not domain and group:
            domain, dr = resolve(group.get('Properties', {}).get('MobDomainActor') or group.get('Properties', {}).get('MobDomain'))
        if not group and domain:
            for kv in domain.get('Properties', {}).get('MobDomainConfig', {}).get('MobGroups', []):
                if kv.get('Key') == spawn.get('MobGroupName'):
                    group, gr = resolve(kv.get('Value'))
                    break
        scripts = []
        fogs = {}
        unresolved = []
        for ref in (group or {}).get('Properties', {}).get('Scripts', []):
            script, script_ref = resolve(ref)
            if not script:
                unresolved.append(script_ref)
                continue
            t = script.get('Type', '')
            scripts.append(t)
            if 'LinkFogDoor' not in t:
                continue
            for event in ['FogDoorsToActivate', 'FogDoorsToDeactivateOnClear', 'FogDoorsToDeactivateOnOffCombat']:
                for door_ref in script.get('Properties', {}).get(event, []):
                    door, door_src = resolve(door_ref)
                    key = door_src or json.dumps(door_ref, sort_keys=True)
                    fog = fogs.setdefault(key, {'source_ref': door_src, 'resolved': bool(door), 'events': []})
                    fog['events'].append(event)
                    if door:
                        dp = door.get('Properties', {})
                        fog.update({'actor': door.get('Name'), 'transform': pose(door),
                                    'initially_passthrough': dp.get('bInitiallyPassthrough'),
                                    'spline_length': dp.get('SplineLength'), 'bounds_local': dp.get('Bound'),
                                    'require_player_inside': script.get('Properties', {}).get('bRequirePlayerInside')})
        boss = bosses[cls]
        row = {'boss_id': boss['id'], 'identity': boss['identity'], 'name': boss['name'], 'class_path': cls,
               'level': level_of(pkg), 'spawner': p.get('SpawnerID'), 'spawner_actor': e.get('Name'),
               'boss_transform': pose(e), 'should_spawn': p.get('ShouldSpawn'),
               'group_name': spawn.get('MobGroupName'), 'group_ref': gr, 'group_resolved': bool(group),
               'domain_ref': dr, 'domain_resolved': bool(domain), 'group_scripts': scripts,
               'domain_transform': pose(domain) if domain else None, 'domain_geometry': geometry(domain) if domain else None,
               'fog_doors': list(fogs.values()), 'fog_link': 'explicit_group_script' if fogs else 'not_found_in_export',
               'unresolved_scripts': unresolved, 'source_ref': f'{pkg}.{i}', 'evidence': 'static',
               'fog_runtime_verified': False, 'spawn_verified': False, 'player_landing': None}
        row['challenge_candidate'] = row['boss_transform'] is not None and not bool(re.search(r'PSO|Test|Preview|Debug', pkg, re.I))
        row['selection_hints'] = []
        if not row['challenge_candidate']:
            row['selection_hints'].append('not_a_resolved_gameplay_placement')
        if not row['spawner']:
            row['selection_hints'].append('unnamed_spawner')
        elif re.search(r'Seq\d|Cinematic|Preview', row['spawner'], re.I):
            row['selection_hints'].append('cinematic_spawner_name_requires_review')
        if row['group_name'] and row['group_name'].casefold() == 'boss':
            row['selection_hints'].append('explicit_boss_group')
        if row['boss_transform'] and abs(row['boss_transform']['position'][2]) > 300000:
            row['selection_hints'].append('outside_current_core_ground_scan_height')
        row['id'] = 'native-link-' + hashlib.sha256((row['level']+'|'+row['spawner_actor']+'|'+cls).encode()).hexdigest()[:16]
        rows.append(row)
    return rows, total


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--dump', type=Path, action='append', required=True)
    args = ap.parse_args()
    catalog = json.loads((ROOT/'mods/tower/data/bosses.json').read_text(encoding='utf-8'))['items']
    bosses = {b['path']: b for b in catalog}
    files = sorted({p.resolve() for root in args.dump for p in root.rglob('*.json') if package(p)})
    unique, errors, total, inputs = {}, [], 0, []
    for n, f in enumerate(files, 1):
        try:
            rows, count = audit_file(f, bosses)
            with f.open('rb') as source:
                digest = hashlib.file_digest(source, 'sha256').hexdigest()
            inputs.append({'package': package(f), 'sha256': digest, 'bytes': f.stat().st_size})
            for row in rows:
                row['source_dump_sha256'] = digest
            total += count
            for row in rows:
                score = (sum(d['resolved'] for d in row['fog_doors']), row['group_resolved'], row['boss_transform'] is not None)
                old = unique.get(row['id'])
                if old is None or score > (sum(d['resolved'] for d in old['fog_doors']), old['group_resolved'], old['boss_transform'] is not None):
                    unique[row['id']] = row
            print(f'{n}/{len(files)} {f.name}: {len(rows)} boss placements', flush=True)
        except (ValueError, OSError, KeyError, TypeError) as e:
            errors.append({'file': str(f), 'reason': str(e)[:250]})
            print(f'{n}/{len(files)} incomplete/unsupported: {f.name}: {type(e).__name__}', flush=True)
    rows = sorted(unique.values(), key=lambda x: (x['boss_id'], x['level'], x['spawner_actor']))
    covered = {r['boss_id'] for r in rows if r['challenge_candidate']}
    fogged = {r['boss_id'] for r in rows if r['challenge_candidate'] and any(d['resolved'] for d in r['fog_doors'])}
    stats = {'input_files': len(files), 'packages': len({package(f).casefold() for f in files}), 'exports_scanned': total, 'placements': len(rows), 'pool_classes': len(catalog),
             'classes_with_placement': len(covered), 'classes_with_explicit_resolved_fog': len(fogged), 'input_errors': len(errors)}
    DATA.write_text(json.dumps({'schema': 1, 'source': 'CUE4Parse cooked map exports; explicit ObjectPath joins',
                               'evidence': 'static', 'stats': stats, 'inputs': inputs, 'errors': errors, 'items': rows}, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    report = ['# Boss 原生场地与雾门关联核对', '', '2026-10-31 · T2/T3 · 证据等级：静态。不开游戏、不连接管道、不改引擎。', '',
              f"共查 {len(files)} 份导出文件（{stats['packages']} 个唯一地图包）、处理 {total} 个导出；重复源按候选ID去重。{len(covered)}/{len(catalog)} 个池内类有出生点，{len(fogged)} 个类找到明确的组脚本雾门关联。输入未覆盖全部关卡时，这不是全游戏覆盖率。", '',
              '数据：`mods/tower/data/native_arena_links.json`；工具：`mods/tower/tools/audit_native_arenas.py`。只按精确 ObjectPath / 唯一 UAID 关联，不按距离或重名 Scene 组件猜配。', '',
              'PSO/测试/预览包及没有显式根位置的记录不计入挑战场地覆盖率；保留原始审计记录供查漏。', '',
              '**出生点不是玩家落点。** 输出保留原生组/域/雾门引用和区域边界摘要；player_landing、radius 与实机通过字段不伪造。未找到关联不等于原生没有雾门。附着组件/关卡实例变换仍须进一步核对。', '',
              '**传送到场地不会自动把塔 Boss 加入原生战斗组。** 雾门初始可能可通行，需确认组脚本或受控开关；不能直接触发剧情组清空/死亡回调来充当塔结算。', '',
              '所有战斗由塔独立生成敌人。候选的 selection_hints 标出匿名/过场命名/超出当前扫描高度等风险，不应按第一个出生点自动当作正式挑战；原生敌群需在进入前隔离。输入包SHA-256已随数据保留。', '',
              '| Boss / ID | 出生点候选 | 明确雾门候选 | 关卡 |', '|---|---:|---:|---|']
    for b in catalog:
        own = [r for r in rows if r['boss_id'] == b['id'] and r['challenge_candidate']]
        num = sum(any(d['resolved'] for d in r['fog_doors']) for r in own)
        report.append(f"| {b['name']} / {b['id']} | {len(own)} | {num} | {', '.join(sorted({r['level'] for r in own})) or '待查'} |")
    report += ['', '当前结果为静态候选；最终需在沙盒中核对落地、雾门、过场、原生 Boss 是否额外刷出、离场还原。', '']
    REPORT.write_text('\n'.join(report), encoding='utf-8')
    print(json.dumps(stats), flush=True)


if __name__ == '__main__':
    import sys
    sys.stdout.reconfigure(encoding='utf-8')
    main()
