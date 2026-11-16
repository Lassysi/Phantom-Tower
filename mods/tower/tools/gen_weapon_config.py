# 生成 src/tower_weapon_config.generated.h：塔内兵器 ID → 原生武器配置对象名（熔铸改 QualityMultiplier 用）
# 来源：DT_WeaponData / DT_ShaqiWeaponData（行名 = 中文名，WeaponConfig = 配置资产）对照 growth_data.generated.h 的 kWeapons（id, 中文名, 类别）
# 用法：python -X utf8 -I mods/tower/tools/gen_weapon_config.py
import json, re, pathlib
here = pathlib.Path(__file__).resolve().parent
src = here.parent / 'src'
tables = pathlib.Path(r'E:\Phantom_Blade_Zero\notes\dump\tables\ProjectPBZ\Content\PBZContent\DataTable\UI')
def rows(fn):
    d = json.load(open(tables / fn, encoding='utf-8'))
    return d[0]['Rows'] if isinstance(d, list) else d['Rows']
cfg = {}
for fn, kind in (('DT_WeaponData.json', 0), ('DT_ShaqiWeaponData.json', 1)):
    for name, r in rows(fn).items():
        c = r.get('WeaponConfig')
        p = c.get('AssetPathName', '') if isinstance(c, dict) else ''
        ic = r.get('Icon')
        icon = ic.get('AssetPathName', '') if isinstance(ic, dict) else ''
        if p and (name, kind) not in cfg: cfg[(name, kind)] = (p.split('.')[-1], icon)
text = (src / 'growth_data.generated.h').read_text(encoding='utf-8')
block = text[text.index('kWeapons[] = {'):]
block = block[:block.index('};')]
out, miss = [], []
for m in re.finditer(r'\{"([^"]+)", "([^"]+)", (\d+),', block):
    wid, name, kind = m.group(1), m.group(2), int(m.group(3))
    c = cfg.get((name, kind))
    if c: out.append((wid, kind, c[0], c[1]))
    else: miss.append((wid, name, kind))
lines = ['// 生成文件，勿手改：python -X utf8 -I mods/tower/tools/gen_weapon_config.py',
         '// 塔内兵器 → 原生武器配置对象名（PBZ_WeaponConfigBase 实例；熔铸按它改 QualityMultiplier / ShachiQualityMultiplier）',
         '#pragma once', '#include <string_view>', 'namespace tower::weaponcfg {',
         'struct Entry { std::string_view weapon; int kind; std::string_view config, icon; };  // icon：DT 行里的 Icon（Texture2D 资产路径）',
         'inline constexpr Entry kConfigs[] = {']
lines += [f'    {{"{w}", {k}, "{c}", "{i}"}},' for w, k, c, i in out]
lines += ['};', f'// 没对上的（{len(miss)}）：' + '、'.join(f'{w}/{n}' for w, n, k in miss), '}  // namespace tower::weaponcfg', '']
(src / 'tower_weapon_config.generated.h').write_text('\n'.join(lines), encoding='utf-8')
print(len(out), 'mapped;', 'missing:', miss)
