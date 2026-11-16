# 3.6.0：把 qa_measure 实测结果（notes/venue-measure-20261114.jsonl）生成三份表：
#   src/tower_venue_real.h        真实可站边界（出界拉回 / 硬墙用）
#   src/tower_endless_venues.h    无尽场地名单（GENERATED VENUES）与实测小场地名单（GENERATED SMALL）
#   python -X utf8 -I E:\PBZ-claude\mods\tower\tools\gen_venue_real.py            # 生成
#   python -X utf8 -I E:\PBZ-claude\mods\tower\tools\gen_venue_real.py --report   # 只打印筛选表
# 同一场地量过多次取最后一次成功的。用户 2026-11-15 定：
#   无尽 = 10% 分位半径 ≥ 30 米（不看形状比），不在小场地名单；榕树之心固定在表首（选场时约 30%）
#   小场地 = 10% 分位半径 < 20 米（另加点名的花千笑、囚锋、鬼差类；火流星不算）
# 边界平滑：每个方向取前后各 2 个方向里的最大值（细柱子 / 树干只挡住一两条射线，不当墙），再不低于中位数的一半；量不出地面（最大 < 3 米）的场地不进边界表
import json, pathlib, sys
root = pathlib.Path(__file__).resolve().parents[1]
real_h = root / 'src' / 'tower_venue_real.h'
endless_h = root / 'src' / 'tower_endless_venues.h'
log = root.parents[1] / 'notes' / 'venue-measure-20261114.jsonl'
sweep = json.loads((root / 'data' / 'capacity_sweep_20261112.json').read_text(encoding='utf-8'))
full = {v['venue'].rsplit('.', 1)[-1]: v['venue'] for v in sweep['venues']}
RONG = 'fixed:rongshu_zdy'
full[RONG] = RONG

# 4.0.0（用户 2026-11-15 确认）：无尽层不用火流星场地（实测够大，但玩家看来是小场地）
ENDLESS_EXCLUDE = ('BP_Monster_GuiChai_Red',)

def named_small(path):
    return ('GuiChai' in path and 'GuiChai_Red' not in path) or 'BP_Monster_HuaQianXIao' in path or 'BP_Monster_YingMei_RedShadowBoneBlade' in path

best = {}
for line in log.read_text(encoding='utf-8-sig').splitlines():
    line = line.strip()
    if not line:
        continue
    try:
        e = json.loads(line)
    except ValueError:
        continue
    if e.get('event') != 'qa_measure' or not e.get('ok') or not e.get('rays'):
        continue
    best[full.get(e['venue'], e['venue'])] = e

rows = sorted(((e['area_m2'], k, e) for k, e in best.items()), reverse=True)
endless, small = [], []
print(f'{"场地":<28} {"10%":>5} {"中位":>5} {"最大":>5} {"形状":>5} {"面积":>6}  结论')
for area, key, e in rows:
    is_small = key != RONG and (e['p10_m'] < 20 or named_small(key)) and not ('GuiChai_Red' in key)
    is_endless = key != RONG and not is_small and e['p10_m'] >= 30 and e['max_m'] >= 3 and not any(x in key for x in ENDLESS_EXCLUDE)
    if is_small:
        small.append((key, e))
    if is_endless:
        endless.append((key, e))
    tag = '无尽' if is_endless else '小场地' if is_small else '榕树' if key == RONG else '-'
    print(f'{e["name"][:26]:<28} {e["p10_m"]:5.1f} {e["median_m"]:5.1f} {e["max_m"]:5.1f} {e["ratio_p10_p90"]:5.2f} {area:6.0f}  {tag}')
print(f'无尽 {len(endless)} 个 + 榕树；小场地 {len(small)} 个')
if '--report' in sys.argv:
    sys.exit(0)

def smooth(rays):
    n = len(rays)
    med = sorted(rays)[n // 2]
    return [max(max(rays[(i + d) % n] for d in range(-2, 3)), 0.5 * med) for i in range(n)]

def replace(path, begin, end, body):
    text = path.read_text(encoding='utf-8')
    a = text.index(begin)
    a = text.index('\n', a) + 1
    b = text.index(end, a)
    path.write_text(text[:a] + body + text[b:], encoding='utf-8')

lines = ['inline constexpr RealVenue kReal[] = {']
for area, key, e in rows:
    rays = [r[0] for r in e['rays']]
    if len(rays) < 4 or len(rays) > 72 or e['max_m'] < 3:
        continue
    vals = ', '.join(f'{x:.1f}f' for x in smooth(rays))
    lines.append(f'    {{"{key}", {e["center"][0]:.0f}, {e["center"][1]:.0f}, {len(rays)}, {{{vals}}}}},  // {e["name"]}')
lines.append('};')
replace(real_h, '// BEGIN GENERATED', '// END GENERATED', '\n'.join(lines) + '\n')

rong = best.get(RONG)
v = ['inline constexpr Venue kVenues[] = {', f'    {{kRongshu, {rong["area_m2"] if rong else 2000:.0f}}},  // 人面榕树（正道大营）：固定约 30%']
for key, e in sorted(endless, key=lambda x: -x[1]['area_m2']):
    v.append(f'    {{"{key}", {e["area_m2"]:.0f}}},  // {e["name"]}：10% 分位 {e["p10_m"]:.1f} 米、中位 {e["median_m"]:.1f}')
v.append('};')
replace(endless_h, '// BEGIN GENERATED VENUES', '// END GENERATED VENUES', '\n'.join(v) + '\n')

sm = ['inline constexpr std::string_view kMeasuredSmall[] = {']
for key, e in small:
    sm.append(f'    "{key}",  // {e["name"]}：10% 分位 {e["p10_m"]:.1f} 米')
sm.append('};')
replace(endless_h, '// BEGIN GENERATED SMALL', '// END GENERATED SMALL', '\n'.join(sm) + '\n')
print(f'写入 {real_h}（{len(lines) - 2} 个场地）、{endless_h}（无尽 {len(endless)} + 榕树，小场地 {len(small)}）')
