# 影塔 v14 逐层烟测（静态，不碰游戏）：python -X utf8 -I mods/tower/tools/audit_v14.py > notes/tower-v14-floor-audit.md
# 每层核：Boss 在池里、来源（原生遭遇重放 / 场地 + 生成）、场地数据（原生刷怪点、高空空间、剧情数据层）、生成兜底（原生刷怪点）、
# 武器 / 道具掉落（已核 → 发放；未核 → 驿站补充渠道）、挑战资格、本层血量 / 伤害配置，以及已知的最近实测结论。
# 同类缺漏（Boss 不出现 / 原生遭遇不进 CG / 掉落缺失）按规则逐层列出风险，不靠记忆。
import re, sys, pathlib
SRC = pathlib.Path(__file__).resolve().parents[1] / 'src'
read = lambda n: (SRC / n).read_text(encoding='utf-8')

pool = {}
for m in re.finditer(r'\{"([^"]+)", "([^"]+)", ([\d.]+), \'([A-F])\', "([^"]*)", "([^"]*)", (true|false)\}', read('boss_pool.generated.h')):
    pool[m.group(1)] = dict(name=m.group(2), hp=float(m.group(3)), cls=m.group(4), ident=m.group(5), native=m.group(6), scene=m.group(7) == 'true')
stem = lambda path: path.rsplit('.', 1)[1][:-2]

floors = []
for m in re.finditer(r'\{(\d+), (\d+), Mode::(\w+), \{"([^"]*)", "([^"]*)", "([^"]*)"\}, (\d), "([^"]*)", \{"([^"]*)", "([^"]*)"\}, (\d), ([\d.]+), ([\d.]+), (\d+), \{[^}]*\}, \d, "([^"]*)", (\d+), ([\d.]+)\}', read('floors.generated.h').split('kHiddenFloors')[0]):
    g = m.groups()
    floors.append(dict(floor=int(g[0]), zone=int(g[1]), mode=g[2], bosses=[b for b in g[3:6] if b][:int(g[6])], venue=g[7], alts=[a for a in g[8:10] if a],
                       replay=g[14], hp=int(g[15]), dmg=float(g[16])))
assert len(floors) == 50, len(floors)

spots = set(re.findall(r'\{"(/[^"]+)", "PBZ_', read('native_spots.generated.h')))
venues = {}
for m in re.finditer(r'\{"(/[^"]+)", "([^"]*)", "([^"]*)", ([-\d.]+), ([-\d.]+), ([-\d.]+), [-\d.]+, \d+, (\d+), \d+, \d+, ([\d.]+), [-\d.]+, [-\d.]+, [\d.]+, (true|false)', read('venues.generated.h')):
    venues[m.group(1)] = dict(world=m.group(2), spawner=m.group(3), doors=int(m.group(7)), sky=m.group(9) == 'true')
weapons = [dict(stem=m.group(1), weapon=m.group(2), name=m.group(3), verified=m.group(5) == 'true')
           for m in re.finditer(r'\{"(BP_Monster_[^"]+)", "([^"]+)", "([^"]+)", (true|false), (true|false)\}', read('tower_economy.h'))]
items = {}
for m in re.finditer(r'\{"(BP_Monster_[^"]+)", "([^"]+)", (\d+), "([^"]+)"\}', read('boss_drops.generated.h')):
    items.setdefault(m.group(1), []).append(m.group(2))
contracts = set(re.findall(r'id = "(\w+)"|\{"(\w+)", "[0-9A-F]{32}"\}', read('replay_flow_nodes.generated.h')))
contract_ids = {a or b for a, b in contracts}
replay_ids = set(re.findall(r'"(\w+)",\s*//\s*契约|\bid\s*=\s*"(\w+)"', read('tower_replay.h')))
layer_paths = set(re.findall(r'\{"(/[^"]+)",\s*\n?\s*"DataLayer_', read('tower_travel.h')))
plan = read('floors.generated.h')
landing_bad = set(re.findall(r'\{"(/[^"]+)", "[^"]*"\}', plan.split('kLandingBad[] = {')[1].split('};')[0]))
native_only = set(re.findall(r'\{"(/[^"]+)", "[^"]*"\}', plan.split('kNativeOnly[] = {')[1].split('};')[0]))
adj = {m.group(1): (float(m.group(2)), float(m.group(3))) for m in re.finditer(r'\{"(/[^"]+)", ([\d.]+), ([\d.]+)\}', plan.split('kBossAdj[] = {')[1].split('};')[0])}

SPAWNER_KICK = {'yingmei'}  # tower_replay.h spawnerKick：没有入场 Flow，由契约刷怪点 ClaimSeqSpawn 刷出（赤影骨刃）
# 本会话 / 交接记录里的最近实测（有证据的才写；其余写"早期测试"）
LIVE = {
    28: 'b2e221c 通过（重放两阶段，02:00）；之后重放框架又改过，需复测', 30: 'b2e221c 通过', 31: 'b2e221c 通过（车轮）', 32: 'd7b6aac 通过；之后重放框架又改过，需复测',
    33: 'd7b6aac 通过', 34: '3371822 通过；174792c 改了直接放置，需复测', 35: '174792c 用户无异常', 36: '3371822 日志结算，画面未确认',
    37: '708039e 用户无异常（两阶段 + 虎炮）', 38: '旧：铁面杀手 3371822 日志通过；v14 换成沐小葵两阶段重放（第 14 层曾用此契约），未在 38 层测',
    39: '71ad9ff 用户无异常（数据层激活）', 40: '71ad9ff 用户无异常', 41: '71ad9ff 用户无异常', 42: '6aa7cd9 用户正常（刷怪点兜底）',
    43: '71ad9ff 用户无异常', 44: '71ad9ff 用户无异常（残留血条已收）', 45: '71ad9ff 用户无异常（原生高空）', 46: '1412be4 通过', 47: '1412be4 通过',
    48: '1412be4 通过（召唤保留）', 49: '1412be4 通过', 50: 'e703aba 用户正常（两段）', 14: '旧：沐小葵重放；v14 换成铁面杀手，未测',
}
MODE = {'Solo': '1v1', 'Duo': '1v2', 'Relay': '车轮', 'Zone': '区首领', 'Final': '塔主连战', 'Replay': '原生遭遇重放', 'SoloTimed': '1v1'}

def challenge_ok(f):
    return len(f['bosses']) == 1 and f['floor'] >= 3 and not f['replay'] and f['mode'] in ('Solo', 'SoloTimed', 'Zone')

out = ['# 影塔 v14 逐层烟测（静态）', '', '生成：`python -X utf8 -I mods/tower/tools/audit_v14.py`。只读源码表，不碰游戏；“风险”一列按已修过的同类 bug 规则逐层核。', '',
       '| 层 | 层型 | Boss | 来源 | 场地 | 血量 / 伤害 | 武器 | 原生道具 | 挑战 | 最近实测 | 风险 |', '|---|---|---|---|---|---|---|---|---|---|---|']
risks_total = 0
for f in floors:
    names, wcol, icol, risks = [], [], [], []
    for b in f['bosses']:
        if b not in pool: risks.append(f'Boss 不在池里：{b}')
        names.append(pool.get(b, {}).get('name', b))
        st = stem(b)
        for w in weapons:
            if w['stem'] == st:
                wcol.append(f"{w['name']}（{'掉落' if w['verified'] else '驿站补充'}）")
        icol += items.get(st, [])
    wcol = list(dict.fromkeys(wcol)); icol = list(dict.fromkeys(icol))
    lead = f['bosses'][0]
    v = venues.get(f['venue'])
    if f['replay']:
        src = f'重放 `{f["replay"]}`'
        kick = f['replay'] in SPAWNER_KICK
        if f['replay'] not in contract_ids and not kick: risks.append('重放契约缺 Flow 节点（不进 CG 风险）')
        if kick: src += '（原生刷怪点直刷，无入场 Flow）'
    else:
        src = '场地 + 生成'
    if not v: risks.append('场地无数据（运行时退经典场地）')
    venue_txt = ('原生' if f['venue'] == lead else '借用 ' + pool.get(f['venue'], {}).get('name', '?'))
    if v:
        venue_txt += f"（{v['doors']} 门{'，高空' if v['sky'] else ''}）"
        if v['sky'] and f['venue'] not in spots: risks.append('高空场地没有原生落点（不可达风险）')
        if not v['spawner']: risks.append('场地无原生刷怪点（运行库拒绝落点时没有兜底）')
    if f['venue'] in landing_bad: risks.append('场地在 LANDING_BAD')
    if f['venue'] != lead and not f['replay']: risks.append('借用场地：运行库落点被拒时没有本 Boss 的原生刷怪点兜底（已按实测可生成选场地）')
    if lead in layer_paths: venue_txt += '，剧情数据层自动激活'
    if not wcol and not icol: risks.append('无原生掉落（只有普通奖励）')
    if any('驿站补充' in w for w in wcol): risks.append('武器关联未核：改由驿站补充渠道，不在击杀时发')
    a = adj.get(lead)
    hp = f"{f['hp']:,}{f' ×{a[0]}' if a and a[0] != 1 else ''} / ×{f['dmg']:.2f}{f' ×{a[1]}' if a and a[1] != 1 else ''}"
    risks_total += len(risks)
    out.append(f"| {f['floor']} | {MODE.get(f['mode'], f['mode'])} | {' → '.join(names)} | {src} | {venue_txt} | {hp} | {'、'.join(wcol) or '—'} | {'、'.join(icol) or '—'} | "
               f"{'可' if challenge_ok(f) else '—'} | {LIVE.get(f['floor'], '早期测试，本轮复测')} | {'；'.join(risks) or '—'} |")
out += ['', f'挑战可放层 {sum(challenge_ok(f) for f in floors)} 个（每轮随机 7 个：限时 2、额外限时 2、无伤 1、禁药 2）。', f'风险条目合计 {risks_total}。']
print('\n'.join(out))
