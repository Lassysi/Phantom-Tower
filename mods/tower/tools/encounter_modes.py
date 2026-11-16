# 每个 Boss 身份用哪种方案（用户 2026-11-02）：
#   重放 = 原生遭遇重放：原生刷怪点 + 原生遭遇流程，初见 / 转阶段 CG、多阶段连打、场地机关与原生同组全部按原生跑，
#          塔只管数值、胜负和副作用边界（发奖 / 任务 / 存档 / 自动换图照样拦）；
#   生成 = 场地 + 塔生成 Boss（现行方案）：只借场地模型 + 雾门，原生遭遇不启动、CG 冻结；
#   待核 = 先按"生成"跑，取证后再定（有已知问题，或证据不足）。
# 证据：DT_WwiseVoiceDuration 的配音事件名 Vo_<分类>_<地图>_Seq<编号>_<角色>（谁在哪段过场里开口）、Boss 池的阶段类、
# floors_plan.py 的 DEPS / NATIVE_ONLY / LANDING_BAD、1–38 层实机记录。用法：python -X utf8 -I mods/tower/tools/encounter_modes.py
import collections, contextlib, io, json, pathlib, re, runpy

ROOT = pathlib.Path(__file__).resolve().parents[3]
VOICE = pathlib.Path(r'E:\Phantom_Blade_Zero\notes\dump\tables\ProjectPBZ\Content\PBZContent\DataTable\Audio\DT_WwiseVoiceDuration.json')
with contextlib.redirect_stdout(io.StringIO()):
    plan = runpy.run_path(str(ROOT / 'mods/tower/tools/floors_plan.py'), run_name='modes')
boss, F = plan['boss'], plan['F']

# 配音里的角色名 → Boss 身份（人工对照；对不上的不硬套）
SPEAKER = {
    'ZhuiMing': 'HookMan', 'BaiChiLian': 'ChiLian', 'MuXiaoKui': 'MuXiaoKui', 'LiZhe': 'PowerMan', 'JuZhe': 'PowerMan',
    'LeiDang': 'LeiDang', 'ChangHong': 'ChangHong_Spear', 'YuYao': 'Yuu', 'HuangXingZi': 'MBoss', 'MoTian': 'MoTian',
    'QuanChi': 'LuanShi', 'DangHui': 'Danghui', 'JianChi': 'HuaQianXIao', 'HuoLiuXing': 'GuiChai_Red', 'LengTu': 'LengTu',
    'HanBing': 'YuCong', 'WuLi': 'YuCong', 'HunDun': 'YuCong', 'DuYanGongKe': 'DuYanBoss', 'YanQianSui': 'YanQianSui',
    'WanJun': 'WanJun_JinGua', 'FuLaoDa': 'PengMeng_Axe', 'XinNiang': 'AHua_GuiTouDao', 'WuShiZunZhu': 'LionDancer',
    'YuiDing': 'FemaleVessel_YuDing', 'MengHua': 'MengHua', 'ZuoShang': 'ZuoShang', 'YingMei': 'YingMei_RedShadowBoneBlade',
    'SanHao': 'FemaleVessel_YuZan', 'XiaoXue': 'XiaoXue',
}
voiced = collections.defaultdict(set)
try:
    d = json.loads(VOICE.read_text(encoding='utf-8'))
    rows = d[0]['Rows'] if isinstance(d, list) else d['Rows']
    for k in rows:
        parts = k.split('_')  # Vo_<分类>_[<地图>_]Seq<编号>_<角色>[_…]
        for i, t in enumerate(parts):
            if re.match(r'Seqs?[-0-9]', t) and i + 1 < len(parts) and parts[i + 1] in SPEAKER:
                where = parts[i - 1] if i >= 3 else '-'
                voiced[SPEAKER[parts[i + 1]]].add(f'{where}:{t}')
                break
except Exception as e:
    print('voice table unreadable:', e)

# 决定（人工）：方案 + 理由。阶段 / 依赖 / 实机问题写进理由，证据字段由脚本附上
R, G, C = '重放', '生成', '待核'
DECIDE = {
    # ---- 重放：名场面 / 多阶段原生连打 / 场地或同伴依赖
    'HookMan': (R, '追命 → 六绝凶镰两阶段，溪谷主线过场多段（Seq022–024）'),
    'ChiLian': (R, '白赤练 → 献祭之蛇两阶段 + 过场（Seq034 / 035）；1–32 第 10 层长 CG / 原生兵器 / 自动换图全出自这条流程'),
    'YuCong': (R, '三张脸 → 卅四号 → 寒冰 / 钢刃 / 混沌多形态，形态切换有过场（Seqs028–030）'),
    'MuXiaoKui': (R, '沐小葵 → 铁玉女王两阶段 + 过场（Seq045–047）；19 层原生迟发乱入就是这条遭遇'),
    'Yuu': (R, '少主双卫（玉妖 + 铁鬼）原生同组同时战 + 过场（Seq110 / 111）；现在拆成车轮，不是原生'),
    'TieGui': (R, '与玉妖同组（少主双卫），随玉妖一起重放'),
    'GuiChai_Red': (R, '火流星 → 磷火意识两阶段 + 过场（Seq051 / 052）'),
    'ChangHong_Spear': (R, '长鸿 → 雷电枪骑两阶段 + 过场（Seq106–108）；25 层虚空读秒 / 原生雷霆枪界面'),
    'LionDancer': (R, '舞狮烈焰 → 最后一舞柱上对抗（柱子由二阶段自身能力召出，受场地放置约束）；原生场地 26 层落地失败——原生刷怪点不经塔的落点'),
    'HuaShan_Blue': (R, '七星剑阵 → 绝望剑意：丝线 / 操控者 / 剑阵成员（AIScript_NvJianKe_JianOu、Sequencer/JianZhen 33 个资产）'),
    'MBoss': (R, '荒行子 → 撕裂锯轮两阶段 + 过场（Seq094 / Seqs032）；原生场地无门、在高空'),
    'LeiDang': (R, '虎炮 → 独臂虎两阶段 + 过场（庞镇 Seq069–071、戏梦 Seqs018 / 019）'),
    'PowerMan': (R, '力者和锯者双人 → 石破天惊之态，断魂过场多段（Seq102–113）；1–32 旧版第 2 层开打后触发原剧情'),
    'ZuoShang': (R, '冥主左殇 → 真龙之躯，终章过场多段；塔主层'),
    'UltimateBoss': (R, '终极首领与左殇终章同一条流程（待核实是否原生阶段链），随左殇重放'),
    'MoTian': (R, '魔渊：金面具任务线终点 + 过场（XMJX Seq018 / 021），高空独立空间带柱子'),
    'MengHua': (R, '断魂圣母梦华：终章 Boss + 过场'),
    'LengTu': (R, '梦差冷荼 + 过场（Seqs025 / 033），技能里召唤旧 Boss（舞狮二阶段、大师兄等的入场能力）'),
    # ---- 待核：先按"生成"，取证后再定
    'Scavenger': (C, '清道夫不出招（1–32 第 6 层）：先查是不是依赖原生组 MetaAI / 操控者；是就改重放'),
    'GuWanYue': (C, '骨婉约普通受击不命中、反制能伤（23 层）：先确认原生机制（参考烟千岁的破杀气），不是 Bug 就保持生成'),
    'GreatBladeMan': (C, '毕峰有专门的出场过场（Sequencer/BlademanIntro）；场地在 LANDING_BAD——重放走原生刷怪点可绕开落点问题'),
    'JadeMaskMan': (C, '玉面原生场地落不了地（38 层）；重放可能绕开，否则保持经典场地生成'),
    'QiShu_Bing': (C, '灭法之法·寒岁：高空挑战场地落不了地（7 层）；看高空挑战是不是独立遭遇，适合就重放'),
    'QiShu_Du': (C, '灭法之法·蛊鹤：高空挑战场地落不了地（36 层），同寒岁'),
    'QiShu_Huo': (C, '灭法之法·炎茹：高空挑战场地（17 层生成正常、但有入场"斩"），同类一起定'),
    'QiShu_Lei': (C, '灭法之法·雷婷：剧情面具 + Boss 闪到门外（24 层），同类一起定'),
    'GuiChai_Yellow_MoDao': (C, '斩骑场地在高空、放置类型未证（22 层用户质疑选址）'),
    'GuiChai_Yellow_ShuangGou': (C, '勾魂场地同 22 层问题（27 层）'),
    'EMei_Sword': (C, '红衣女剑客：疑为大师兄绝望剑意的操控者（剑阵版），核实后并入大师兄重放；单独出场保持生成'),
    'YingMei_RedShadowBoneBlade': (C, '囚锋与鬼差首领同一段过场（XSSL Seq104），看是否原生同场'),
    # ---- 生成：来到场地没有剧情或剧情很短、单阶段、没有场地依赖（随便召出来都能打）
    'MobMan_Elite_Sword': (G, '残钢：第一个小 Boss，单阶段、无依赖'),
    'PengMeng_Axe': (G, '彭猛：单阶段；有一段短出场过场（XG Seq014），冻结不影响战斗'),
    'gold_mask_guard': (G, '金面具黑衣人：原生在多处出现、单阶段'),
    'AHua_GuiTouDao': (G, '新娘阿花：单阶段；短过场（XG Seq016）'),
    'HuaQianXIao': (G, '花千笑：单阶段；短过场（Seq027）'),
    'LuanShi': (G, '铁狂：单阶段；短过场（Seq030 / 031）'),
    'JingYe': (G, '荆野：单阶段、无过场记录'),
    'Zhuobi': (G, '拙壁：单阶段（13 层隔墙由读秒前准入处理）'),
    'BiYuan': (G, '毕远残躯：单阶段（14 层原生兵器页已由会话隔离处理）'),
    'CanQu_BiHai': (G, '碧海残躯：单阶段（15 层"斩"已由静默顺序修复）'),
    'FemaleVessel_YuZan': (G, '玉瓒：单阶段；短过场（三号 Seq043）'),
    'Danghui': (G, '荡秽：单阶段；短过场（Seq040 / 041），原生场地无门在高空 → 经典场地'),
    'Maddoctor_Elite': (G, '谢秋冬：单阶段'),
    'XiaoXue': (G, '小雪：单阶段'),
    'WanJun_JinGua': (G, '万钧：单阶段；短过场（ZDDY Seq074）'),
    'YuBi': (G, '玉璧：单阶段（29 层原生兵器页已由会话隔离处理）'),
    'HuoDaoDuiZhang': (G, '祝原：单阶段（原生场地在高空 → 经典场地）'),
    'HuaShan_RedTroop_BigBro': (G, '阿瑾：单阶段'),
    'DuYanBoss': (G, '独眼：单阶段；出场过场 Seq082 会经刷怪点刷原生替身（33 层），76653f0 已拦'),
    'YouLong_Harpoon': (G, '游龙：单阶段'),
    'YanQianSui': (G, '烟千岁：单阶段；先破杀气才能伤血是原生机制（34 层用户确认），生成方案下照常'),
    'GuiChai_Cyan_ShuangYue': (G, '断肠：单阶段（35 层通过）'),
    'MobMan_Elite': (G, '铁杀：单阶段'),
    'YanYan_Club': (G, '奎艳艳：单阶段'),
    'IronMaskMan_Boss': (G, '铁面杀手：单阶段'),
    'FengXueDaoKe': (G, '风雪刀客：单阶段'),
    'FemaleVessel_YuDing': (G, '玉鼎：单阶段；短过场（Seq119）'),
    'GuiChai_White_PangZhenBoss': (G, '白衣鬼差（变体）：没有原生场地'),
    'Hun': (G, '魂（镜像）：内置禁用（没有血条、不能锁定），不进方案'),
}

ids = collections.OrderedDict()
for stem, b in boss.items(): ids.setdefault(b['id'], []).append(stem)
floor_of = collections.defaultdict(list)
for (f, zone, mode, bs, venue, alts, note) in F:
    for s in bs: floor_of[boss[s]['id']].append(f)
rows, missing = [], []
for ident, stems in ids.items():
    if ident not in DECIDE: missing.append(ident); continue
    mode, why = DECIDE[ident]
    rows.append({'identity': ident, 'mode': mode, 'why': why, 'classes': stems, 'names': sorted({boss[s]['name'] for s in stems}),
                 'phases': len(stems), 'floors_v6': floor_of.get(ident, []), 'voiced_cutscenes': sorted(voiced.get(ident, [])),
                 'native_world': sorted({boss[s]['native'] or '无' for s in stems}),
                 'gates': {k: v for k, v in (('deps', [s for s in stems if s in plan['DEPS']]), ('native_only', [s for s in stems if s in plan['NATIVE_ONLY']]),
                                              ('landing_bad', [s for s in stems if s in plan['LANDING_BAD']])) if v}})
if missing: raise SystemExit(f'no decision for identities: {missing}')
order = {R: 0, C: 1, G: 2}
rows.sort(key=lambda r: (order[r['mode']], min(r['floors_v6'] or [99])))
count = collections.Counter(r['mode'] for r in rows)
(ROOT / 'notes/tower-encounter-modes.json').write_text(json.dumps({'schema': 1, 'counts': count, 'rows': rows}, ensure_ascii=False, indent=1), encoding='utf-8')
L = ['| 方案 | 身份（阶段类） | v6 层 | 配音过场 | 理由 |', '|---|---|---|---|---|']
for r in rows:
    L.append(f"| {r['mode']} | {'、'.join(r['names'])}（{r['phases']}） | {'、'.join(map(str, r['floors_v6'])) or '—'} | {'、'.join(r['voiced_cutscenes'][:4]) + ('…' if len(r['voiced_cutscenes']) > 4 else '') or '—'} | {r['why']} |")
(ROOT / 'notes/tower-encounter-modes.table.md').write_text('\n'.join(L) + '\n', encoding='utf-8')
print(dict(count), len(rows), 'identities')
