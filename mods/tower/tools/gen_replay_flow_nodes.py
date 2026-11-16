# 重放契约的原生流程节点表：每个契约的出场 / 死亡 Flow 的全部节点 GUID（cooked Flow 导出 flow-audit.json）。
# 塔档复位时，存档 EventPipeline.ExecuteNodeGuid 里这些节点的"已执行"记录一并删除，遭遇回到"从未打过"的状态。
# 6d105f2 第 4 层追命：原档打败过追命，死亡 Flow 转阶段分支 SetExcuteType 2B132618… 记为已执行 → 一阶段死后不播 Seq023。
# 用法：python -X utf8 -I mods/tower/tools/gen_replay_flow_nodes.py [--check]
import hashlib, json, pathlib, sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
AUDIT = pathlib.Path(r'E:\Phantom_Blade_Zero\build\tower-383fa69\batch-repair\flow-audit.json')
DATA = ROOT / 'data' / 'replay_flow_nodes.json'
HEADER = ROOT / 'src' / 'replay_flow_nodes.generated.h'
FLOWS = {  # 契约 id → 只取这场遭遇自己的出场 / 死亡 Flow
    'lion': ['BPF_WuShiZunZhu_ChuChang', 'BPF_WuShiZunZhu_Death'],
    'red': ['BPF_HongYiGuiChai_ChuChang', 'BPF_HongYiGuiChai_Death'],
    'changhong': ['BPF_ChangHongChuChang', 'BPF_ChangHongDeath'],
    'hook': ['BPF_ZhuiMingChuChang', 'BPF_ZhuiMingDeath'],
    'chilian': ['BPF_NongSheChuChang', 'BPF_NongSheDeath'],
    'muxiaokui': ['BPF_MuXiaoKuiChuChang', 'BPF_MuXiaoKuiDeath'],
    'leidang': ['BPF_LeiDangOrYuer_ChuChang', 'BPF_LeiDangOrYuer_Dead'],
    'lizhe': ['BPF_Seq113_LiZheRuZhan'],
    'mboss': ['BPF_HuangXingZiChuchang', 'BPF_HuangXingZiDeath', 'BPF_HuangXingZiDeath_2', 'BPF_HuangXingZiFeidie'],
    'jianzhen': ['BPF_JianZhenChuChang', 'BPF_JianZhenDeath'],  # 入场 / 转阶段 / 战后都在这一张 Narrative/DHY Flow 里
    'zhuobi': ['BPF_ZhuoBiDead'],
    'wanjun': ['BPF_WanJunKe', 'BPF_WanJunKeChainBlocker', 'BPF_WanJunKeDeath', 'BPF_WanJunKeDeathAfterCamera'],
}
# 2026-11-05 补充导出（OfflineProbe，ReadScriptData）：flow-audit.json 不含 Narrative 下的 Flow
EXTRA = pathlib.Path('E:/Phantom_Blade_Zero/build/lion-replay-research/f28-32-34-export')
EXTRA2 = pathlib.Path('E:/Phantom_Blade_Zero/build/boss9-20-26/assets')  # v15：9 / 26 层（tools/extract grep asset:）

def extra_flow(name):
    for f in [*EXTRA.rglob(name + '.json'), *EXTRA2.rglob(name + '.json')]:
        d = json.loads(f.read_text(encoding='utf-8-sig'))
        guid = next(e['Properties']['AssetGuid'] for e in d if e.get('Type') == 'FlowAsset')
        nodes = [{'guid': e['Properties']['NodeGuid']} for e in d if (e.get('Properties') or {}).get('NodeGuid')]
        return {'name': name, 'asset_guid': guid, 'nodes': nodes}
    return None

def build():
    if AUDIT.exists():
        raw = AUDIT.read_bytes()
        audit = json.loads(raw)
        by = {f['name']: f for f in audit['flows']}
        doc = {'schema': 1, 'source': str(AUDIT), 'source_sha256': hashlib.sha256(raw).hexdigest(), 'contracts': {}}
        for cid, names in FLOWS.items():
            flows = []
            for n in names:
                f = by.get(n) or (extra_flow(n) if EXTRA.exists() else None)
                if not f: sys.exit(f'{cid}: flow {n} missing from audit')
                flows.append({'flow': n, 'asset_guid': f['asset_guid'], 'nodes': sorted(x['guid'].replace('-', '').upper() for x in f['nodes'])})
            doc['contracts'][cid] = flows
    else:  # 审计源不在本机时，用仓库里已提交的数据（--check 照样核对头文件）
        doc = json.loads(DATA.read_text(encoding='utf-8'))
    return doc

def header(doc):
    rows = []
    for cid, flows in doc['contracts'].items():
        for f in flows:
            for g in f['nodes']:
                rows.append(f'    {{"{cid}", "{g}"}},  // {f["flow"]}')
    return '\n'.join([
        '// 生成文件，勿手改：python -X utf8 -I mods/tower/tools/gen_replay_flow_nodes.py',
        '// 重放契约的出场 / 死亡 Flow 节点 GUID（32 位大写十六进制，无连字符）；塔档复位时删掉这些节点的"已执行"记录',
        '#pragma once', '#include <string_view>', 'namespace tower::replay {',
        'struct FlowNode { std::string_view contract, guid; };',
        'inline constexpr FlowNode kFlowNodes[] = {', *rows, '};', '}  // namespace tower::replay', ''])

doc = build()
text = json.dumps(doc, ensure_ascii=False, indent=1) + '\n'
h = header(doc)
if '--check' in sys.argv:
    bad = [p.name for p, t in ((DATA, text), (HEADER, h)) if not p.exists() or p.read_text(encoding='utf-8') != t]
    if bad: sys.exit('stale: ' + ', '.join(bad) + ' (run gen_replay_flow_nodes.py)')
    print(f'REPLAY_FLOW_NODES_OK {sum(len(f["nodes"]) for fl in doc["contracts"].values() for f in fl)} nodes')
else:
    DATA.write_text(text, encoding='utf-8'); HEADER.write_text(h, encoding='utf-8')
    print(f'wrote {sum(len(f["nodes"]) for fl in doc["contracts"].values() for f in fl)} nodes for {len(doc["contracts"])} contracts')
