# Boss 原生道具掉落表：mods/tower/data/boss_item_drops.json（死亡 Flow 静态可达的 GetItems / GiveItems / LootItems）→ src/boss_drops.generated.h
# 用法：python -X utf8 -I mods/tower/tools/gen_boss_drops.py [--check]
import json, sys
from pathlib import Path

here = Path(__file__).resolve().parent
src = here.parent / 'data' / 'boss_item_drops.json'
out = here.parent / 'src' / 'boss_drops.generated.h'
d = json.loads(src.read_text(encoding='utf-8'))
rows = sorted({(r['stem'], r['item'], int(r['count']), r['flow']) for r in d['rows']})
for stem, item, count, flow in rows:
    assert stem.startswith('BP_Monster_') and item and 0 < count <= 9, (stem, item, count)
    assert not (item.startswith('F_') or item.startswith('Item_Piece') or item in ('Item_001', 'Item_002')), item
lines = [
    '// 生成文件：mods/tower/tools/gen_boss_drops.py ← mods/tower/data/boss_item_drops.json，勿手改',
    '// Boss 死亡 Flow 里静态可达的原生道具发放（记忆之花 / 碎片等永久属性道具不发：塔自己管成长）',
    '#pragma once',
    '#include <string_view>',
    'namespace tower {',
    'struct BossItemDrop { std::string_view stem, item; int count; std::string_view flow; };',
    'inline constexpr BossItemDrop kBossItemDrops[] = {',
]
lines += [f'    {{"{s}", "{i}", {c}, "{f}"}},' for s, i, c, f in rows]
lines += ['};', '}  // namespace tower', '']
text = '\n'.join(lines)
if '--check' in sys.argv:
    if not out.exists() or out.read_text(encoding='utf-8') != text:
        print('boss_drops.generated.h 过期：运行 python -X utf8 -I mods/tower/tools/gen_boss_drops.py'); sys.exit(1)
    print(f'BOSS_DROPS_OK {len(rows)} rows'); sys.exit(0)
out.write_text(text, encoding='utf-8', newline='\n')
print(f'wrote {out} ({len(rows)} rows)')
