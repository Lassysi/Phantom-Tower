# 3.2.1：游戏内界面发出的每个影塔命令都必须在发布版白名单里（3.1.0 漏了 trade / checkpoint，发布版点了报「发布版不提供这个命令」）
import pathlib, re, sys
src = pathlib.Path(__file__).resolve().parents[1] / 'src'
engine = (src / 'tower.cpp').read_text(encoding='utf-8')
m = re.search(r'kPlayerCmds\[\]\s*=\s*\{(.*?)\};', engine, re.S)
if not m:
    sys.exit('找不到 kPlayerCmds')
allowed = set(re.findall(r'"([a-z_]+)"', m.group(1)))
sent = set()
for f in list(src.glob('tower_*.cpp')) + list(src.glob('tower_*.h')):
    text = chr(10).join(l.split('//')[0] for l in f.read_text(encoding='utf-8').splitlines())  # 只看代码，不看注释
    sent |= set(re.findall(r'\b(?:Send|Cmd)\("([a-z_]+)"', text))
    sent |= set(re.findall(r'"cmd":"([a-z_]+)"', text))
dev_only = {'grow', 'call', 'grant', 'sandbox', 'diagmode', 'equip'}  # 开发命令不由正式界面发出；grow 在白名单里
missing = sorted(c for c in sent - allowed if not c.startswith('qa_') and c not in dev_only)
if missing:
    sys.exit('界面会发但发布版白名单没有：' + ', '.join(missing))
print(f'CMD_WHITELIST_OK 界面命令 {len(sent)} 个，白名单 {len(allowed)} 个')
