# 影塔 50 层方案逐层表生成器（notes/tower-50-floors-plan.md 第 6 节）。用法：python -X utf8 -I mods/tower/tools/floors_plan.py > 表.md
# 检查：一个身份整局只打一次（金面具黑衣人成组例外，最多 2 次；押注层的两只、团战友军不算）、类都在 Boss 池里；血量按方案第 4 节公式计算。
import re, sys, collections

POOL = r'E:\PBZ-claude\mods\tower\src\boss_pool.generated.h'
src = open(POOL, encoding='utf-8').read()
boss = {}
for m in re.finditer(r'\{"([^"]+)", "([^"]+)", ([\d.]+), \'([A-F])\', "([^"]*)", "([^"]*)", (true|false)\}', src):
    path, name, hp, cls, ident, native, scene = m.groups()
    stem = path.rsplit('.', 1)[1][:-2].replace('BP_Monster_', '')  # 例：YuCong_HunDun
    boss[stem] = dict(path=path, name=name, hp=float(hp), cls=cls, id=ident, native=native, scene=scene == 'true')

WORLD = {'XiGu': '悲泣溪谷 Xigu', 'PangZhen': '庞镇 PangZhen', 'HouJie': '后街 Houjie', 'FuJiZhiLin': '血色森林 Fujizhilin',
         'MeiGuHu': '梅谷湖（Level=PBZ_Pangzhen_WP）', 'MeiGu_YiJi': '梅谷遗迹', 'LuJiaCun': '陆家村（Level=PBZ_Zhengdaodaying_WP）',
         'ZhengDaoDaYing': '正道大营', 'DengDingShanDao': '枷笼山 DengDingShanDao', 'XiMeng': '戏梦山庄 Ximeng',
         'DuanHunYa': '断魂崖 DuanHunYa', 'ZhuLinGuWu': '竹林故屋（教学图，无钟）', '': '无原生场地'}

# 模式：血量份额 / 伤害份额 / 基准限时（秒，标准限时用于"限时内额外奖励"）
MODE = {
    '1v1': (1.00, 1.00, 300), '1v1限时': (0.90, 1.00, 300), '1v2': (0.65, 0.75, 150), '车轮2': (0.60, 0.90, 150),
    '车轮3': (0.45, 0.90, 180), '混战': (0.60, 0.80, 150), '1v3': (0.50, 0.60, 180), '区首领': (1.15, 1.03, 300),
    '区首领车轮': (0.70, 1.02, 300), '塔主': (0.70, 1.04, 300),
    '团战': (1.00, 1.00, 120), '押注': (1.00, 1.00, 120),  # 接入前按 1v1 的数值
    '重放': (0.60, 1.00, 300),  # 原生遭遇重放：每阶段 0.6 份血量（两阶段合计约 1.2）；标准限时含初见 / 转阶段过场
}
# 经典场地（替代）：从 venues.json 里选有雾门、可落地、面积够的，名字按地图暂定
CLASSIC = {  # 经典场地 = 某只 Boss 的原生场地（venues.json：有激活雾门、普通高度、面积够）
    'C1': ('AHua_GuiTouDao', '溪谷·阿花场地（Xigu，2 门，凸包约 1541 m²）'),
    'C2': ('FengXueDaoKe', '梅谷·风雪刀客场地（Pangzhen，2 门，约 3062 m²）'),
    'C3': ('LuanShi', '血色森林·铁狂场地（Fujizhilin，3 门）'),
    'C4': ('WanJun_JinGua', '正道大营·万钧场地（Zhengdaodaying，2 门）'),
    'C5': ('GreatBladeMan', '枷笼山·毕峰场地（DengDingShanDao，约 2902 m²，大体型用）'),
    'C6': ('LeiDang', '戏梦山庄·雷当场地（Ximeng，约 2065 m²）'),
    'C7': ('MengHua', '断魂·梦华场地（Duanhun，2 门，约 3916 m²）'),
    'C8': ('DuYanBoss', '梅谷湖·独眼弓客场地（3 门，凸包 11 点；ed47e1f 第 33 层实测可生成）'),
    'C9': ('GuiChai_Cyan_ShuangYue', '梅谷湖·断肠场地（1 门；ed47e1f 第 35 层实测可生成）'),
}
ZONE_CLASSIC = {1: 'C1', 2: 'C3', 3: 'C4', 4: 'C2', 5: 'C7'}  # v6：四区原 C5 毕峰场地三个独立 run 都落不了地，换 C2（31 层实测可生成）  # 本层场地运行时不可用时退回本区经典场地
ALLIES = {}  # 团战 2v2 的友军候选（创意层以后再接入；v12 暂无团战 / 押注层）
LATER = ('团战', '押注')
# 原生遭遇重放层：层 → 遭遇契约 id（src/tower_replay.h）。层上的 Boss 写第一阶段（场地 = 它的原生场地）；运行时不接重放时按 1v1 打它
REPLAY = {9: 'zhuobi', 20: 'ajin', 4: 'hook', 8: 'chilian', 17: 'red', 38: 'muxiaokui', 19: 'lion', 33: 'changhong', 34: 'lizhe', 35: 'yingmei', 37: 'leidang', 28: 'jianzhen'}
PLAN_VERSION = 14  # v14（用户 2026-11-05）：铁面杀手前移到 14，沐小葵铁玉女王改到 38 走场地 + 生成 + 掉落；固定限时层取消，挑战每轮随机 7 个（tower_floorplay.h）；Boss 血量 / 伤害改为逐层配置表（不再跟玩家实时同步）。v13（用户 2026-11-04）：最后三层冷荼 / 魔渊 / 左殇；铁面杀手前移到 38，风雪刀客挪到 47。v12： v12（用户 2026-11-03）：按游戏流程重排；去金面具；魔渊 49；玉琮车轮；限时 / 附加挑战只在单只生成层。v11： v11（2026-11-03 18bb300 第 50 层飞远）：左殇两段改回原生终章战场（只许原生场地）。v10： v10（用户 2026-11-03）：12 层玉琮最后阶段三形态同时 1v3；50 层左殇真结局两段（义体 → 真龙之躯）。v9：追命 / 白赤练 / 沐小葵 / 雷当重放，19 层原生 1v2
# 缺必要战斗依赖（环境 Actor / 操控者 / 阶段初始化）的类：运行时不可用（换备选、不退经典场地），依赖还原并实测前不解禁
DEPS = {
    # HuaShan_JianOu / HuaShan_Blue 移出（2026-11-05）：28 层改为原生遭遇重放 jianzhen，剑阵成员 / 操偶 / 丝线都由原生遭遇自己提供；
    # 这两类不再作为任何层的直接生成 Boss 或备选
}
# 实机生成前落点全部被拒（Core"无地面"）的场地（按 Boss 场地 stem）：不作为任何层的场地 / 兜底，运行时当本局已判坏。
# 证据见 notes/tower-repair-33-38.md 第 C 节；Core 落点规则修正（地面类型）并实测后再解禁
LANDING_BAD = {
    # QiShu_Bing 移出（2026-11-04）：只有 30324ba 一次旧失败；同类高空独立空间雷婷 47a77104 实测能到、地面审计 12/12，
    # 当时失败疑为落点在雾门凸包外（本批入场先放进界内）。原生场地运行时再落不了地会自动退本区经典场地
    'GreatBladeMan': '30324ba 第 11 层、ed47e1f 第 36 / 37 / 38 层（三个独立 run）：毕峰场地 7 次 × 36 候选全部无地面',
    # JadeMaskMan 移出（用户 2026-11-05：所有 Boss 优先原生场地）：ed47e1f 的失败早于 708039e 落点高度修正、fce400e 两次拒绝后直接放落点、
    # 6aa7cd9 原生刷怪点兜底；原生场地运行时仍落不了地会自动退本区经典场地
    # QiShu_Du 移出（2026-11-05）：同类高空独立空间雷婷 590561b 实测——旧凸包与实际平台对不上，按站立位置改圆后通过；原生场地运行时落不了地仍自动退经典场地
    'LionDancer_P2': '30324ba 第 26 层：舞狮原生场地落地失败（被回退到万钧场地）',
}
# 只能在自己的原生场地打的类：原生场地本局不可用（找不到 / 落地失败 / 剧情冻结失败）就换备选，绝不退经典场地冒充通过
NATIVE_ONLY = {
    'LionDancer_P2': '升柱 / 柱上对抗由 Boss 自身能力召出（GA_LionDancer_P2_PolePassive、技能池 OnPole），受场地放置约束（BPI_LionSummonPlacementGuard）',
    # 18bb300 第 50 层：在梦华场地开打，左殇一招飞到空气墙外、15 秒后越界作废。AbilityPool_ZuoShang_Ending 里的 GA_ZuoShang_CA_03
    # 在 SetupDarkBladeWarpTargets 里 GetActorOfClass(BP_ZuoShang_CA_03_SnapPoint / LandingPoint)，取组件位置作 MotionWarping 目标；
    # 这两个锚点摆在 PBZ_Duanhun_WP 的流式分区 802NKFODLXIOJ3T67PDWNPKPN（原生终章战场）。同图别处开打，Boss 就被扭曲到原生锚点
    'ZuoShang_Ending': '技能 GA_ZuoShang_CA_03 把 Boss 扭曲到原生终章战场的锚点（BP_ZuoShang_CA_03_SnapPoint / LandingPoint，GetActorOfClass），同图别的场地开打会飞出战斗区',
    'ZuoShang_P2': '与义体同一原生战斗域（.218）；CA 系技能（GA_ZuoShang_CA_GEx_01 / GEd_01）同样按类找 SnapPoint / DomainCenter 锚点',
}  # 以后接入的小创意（共不超过 3 层）；接入前引擎按 1v1 打第一只

# 层：(层, 区, 层型, [boss stem…], 场地, 备选 stems, 说明)
F = [
 # v12（用户 2026-11-03）：按游戏流程排，而不只按地图。依据：①每个 Boss 出场 / 死亡 Flow 引用的任务属于哪一章主线还是支线
 # （第一章到陆家村沐小葵为止，火流星 / 舞狮第二章，雷当与终章第三章）；②原生过场编号大体按剧情先后（014 彭猛 → 022 追命 → 034 白赤练
 # → 045 沐小葵 → 051 火流星 → 062 舞狮 → 082 独眼 → 093 荒行子 → 106 长鸿 → 119 玉鼎 → 123 雷当 → 126 风雪 → 150 终章）；
 # ③支线 / 回访（昆仑三杰、灭法之法、三面混沌、湖心秘密、清道夫）放到能打的阶段之后。金面具黑衣人去掉；魔渊第 49 层，左殇第 50 层。
 # 同章内弱的在前、经典的在后（双卫放沐小葵前）。限时层 / 附加挑战只出现在单只 Boss、3/2/1 直接开打的层（见 tower_floorplay.h ChallengeFloor）。
 # ---------------- 一区 · 第一章：溪谷 → 血色森林 → 陆家村（1–10）
 (1, 1, '1v1', ['MobMan_Elite_Sword'], 'native', ['AHua_GuiTouDao'], '溪谷第一个主线 Boss'),
 (2, 1, '1v1', ['PengMeng_Axe'], 'native', ['MobMan_Elite_Sword'], '溪谷主线：斧老大（MainCh1-NiLiuErShang，Seq014）'),
 (3, 1, '1v1', ['AHua_GuiTouDao'], 'native', ['PengMeng_Axe'], '溪谷：新娘阿花（Seq016）'),
 (4, 1, '重放', ['HookMan'], 'native', [], '追命原生两阶段重放（MainCh1-2-GangTieJuTa；Seq022/023 允许，Seq024 战后冻结）'),
 (5, 1, '1v1', ['HuaQianXIao'], 'native', ['LuanShi'], '血色森林主线：剑痴花千笑（Seq027）'),
 (6, 1, '1v1', ['LuanShi'], 'native', ['HuaQianXIao'], '血色森林主线：拳痴铁狂（Seq030/031）'),
 (7, 1, '1v1', ['JingYe'], 'native', ['LuanShi'], '血色森林：亡命徒荆野，原生场地（用户 2026-11-05：不再借铁狂场地；带马，直接生成本体落在原点不可见 → 原生刷怪点刷出，见 tower_floorplay.h SpawnerFirst）'),
 (8, 1, '重放', ['ChiLian'], 'native', [], '白赤练原生两阶段重放（Seq034/035 允许；LD_Post、黑屏、自动去陆家村战后冻结）'),
 (9, 1, '重放', ['Zhuobi'], 'native', [], '守门人拙壁：原生遭遇（正道大营山道石阶，自己的刷怪点刷出；死亡 Flow 冻结，v15 用户 2026-11-05）'),
 (10, 1, '1v1', ['BiYuan'], 'native', ['Zhuobi'], '陆家村主线：天玄派毕远掌门残躯'),
 # ---------------- 二区 · 第一章收尾（陆家村）→ 第二章庞镇（11–20）
 (11, 2, '1v1', ['CanQu_BiHai'], 'native', ['BiYuan'], '陆家村主线：苦叶寺碧海大师残躯'),
 (12, 2, '1v1', ['FemaleVessel_YuZan'], 'native', ['CanQu_BiHai'], '陆家村主线：三号乐师偶玉瓒（Seq043）'),
 (13, 2, '1v2', ['Yuu', 'TieGui'], 'native', [], '陆家村：少主双卫，场地 + 同时生成两只。原生剧情在沐小葵之后，爬塔把弱的两只放前面（用户 2026-11-03）'),
 (14, 2, '1v1', ['IronMaskMan_Boss'], 'native', ['CanQu_BiHai'], '铁面杀手（v14 用户：代表性弱，前移到 14，和沐小葵对调；原生场地 + 生成）'),
 (15, 2, '1v1', ['GuiChai_Yellow_MoDao'], 'native', ['GuiChai_Yellow_MoDao_LengTu'], '第二章庞镇：鬼差高手·斩骑'),
 (16, 2, '1v1', ['GuiChai_Yellow_ShuangGou'], 'native', ['GuiChai_Yellow_MoDao_LengTu'], '第二章庞镇：鬼差高手·勾魂'),
 (17, 2, '重放', ['GuiChai_Red'], 'native', [], '火流星原生两阶段重放（MainCh2-SiZhen；Seq051/052 允许，Seq053 战后冻结）'),
 (18, 2, '1v1', ['Maddoctor_Elite'], 'native', ['GuWanYue'], '第二章后街：神医谢秋冬'),
 (19, 2, '重放', ['LionDancer'], 'native', [], '第二章区首领：舞狮尊主原生遭遇重放（MainCh2-XinZhiZongJi；Seq062 → Seq063 → 最后一舞，Seq064 冻结）'),
 (20, 2, '重放', ['HuaShan_RedTroop_BigBro'], 'native', [], '悲痛欲绝的阿瑾：原生战斗组（阿瑾 + 6 个红衣兵同组刷出，击败阿瑾过关；v15）'),
 # ---------------- 三区 · 第二章梅谷湖 → 第二章后的支线（21–30）
 (21, 3, '1v1', ['DuYanBoss'], 'native', ['YouLong_Harpoon'], '梅谷湖主线：独眼弓客（Seq082）'),
 (22, 3, '1v1', ['YouLong_Harpoon'], 'native', ['DuYanBoss'], '梅谷湖：发狂的水行帮主游龙'),
 (23, 3, '1v1', ['GuiChai_Cyan_ShuangYue'], 'native', ['GuiChai_Cyan_ShuangYue_LengTu'], '梅谷湖主线：鬼差高手·断肠'),
 (24, 3, '1v1', ['HuoDaoDuiZhang'], 'native', ['GuiChai_Cyan_ShuangYue_LengTu'], '庞镇杀气塔：火刀祝原，原生高空场地（同类高空空间雷婷 / 蛊鹤已实测：按站位改圆 + 原生刷怪点兜底）'),
 (25, 3, '1v1', ['YanQianSui'], 'native', ['GuWanYue'], '支线（幻术琵琶 / 西楼废墟，Seq083）：琴女烟千岁，先破杀气才能伤血是原生机制'),
 (26, 3, '1v1', ['WanJun_JinGua'], 'native', [], '巨锤客万钧：原生场地 + 塔生成 + 掉落（1.0.1 用户 2026-11-07：原生遭遇重放玩家卡在场外进不了剧情；实测塔生成的万钧正常出招可击杀）'),
 (27, 3, '1v1', ['YuBi'], 'native', ['YuBi_LengTu'], '白山秘闻（剑偶 Seq076）：廿一号剑偶玉璧'),
 (28, 3, '重放', ['HuaShan_Blue'], 'native', [], '正道大营七星剑阵：大师兄原生两阶段重放（Seq076 入场 / Seq077 转阶段允许；Seq078、结束游戏战后冻结；剑阵弟子 / 操偶保留；用户 2026-11-05）'),
 (29, 3, '1v1', ['GuWanYue'], 'native', ['Maddoctor_Elite'], '支线（昆仑三杰）：女妖骨婉约'),
 (30, 3, '1v1', ['YuCong_FirstTime'], 'native', ['IronMaskMan_Boss'], '三张脸的诡异魔偶（玉琮初见战，溪谷原生场地；用户 2026-11-05：玉面郎君在枷笼山走金面具刷怪点兜底会带出原生同伙、和 31 层同一片场地，换掉）'),
 # ---------------- 四区 · 第三章：枷笼山 → 戏梦山庄（31–40）
 (31, 4, '车轮3', ['MobMan_Elite', 'GreatBladeMan', 'YanYan_Club'], 'C2', [], '枷笼山车轮：铁杀 → 毕峰 → 奎艳艳（三场原生相邻的独立遭遇；毕峰场地落不了地 → C2；ui3 改原生后 ui7 实测毕峰在铁杀场地 36 候选全无地面、车轮中断 → 回 C2（b2e221c 三只实测通过））'),
 (32, 4, '区首领', ['MBossVariants'], 'native', ['MBoss'], '昆仑三杰收尾：荒行子二阶段撕裂锯轮，在它自己的原生二阶段场地（高空独立空间，刷怪点 …_1635134170）直接生成；用户 2026-11-05：原生剧情要先解谜、拖节奏，改场地 + 二阶段 + 血滴子；一阶段挂线处 / 寺庙都不是它的场地'),
 (33, 4, '重放', ['ChangHong_Spear'], 'native', [], '长鸿原生两阶段重放（Seq106/107 允许，Seq108 战后冻结）'),
 (34, 4, '重放', ['PowerMan'], 'native', [], '力者原生两阶段重放（断魂 Seq113 入场、Seq114 转阶段允许；Seq115/116 战后冻结；用户 2026-11-05）'),
 (35, 4, '重放', ['YingMei_RedShadowBoneBlade'], 'native', [], '戏梦：赤影骨刃·囚锋（直接生成会自毁 → 原生刷怪点 ClaimSeqSpawn 刷出，单阶段）'),
 (36, 4, '1v1', ['FemaleVessel_YuDing'], 'native', ['FemaleVessel_YuDing_LengTu'], '戏梦：十二号偃月刀偶玉鼎（Seq119；“戏票怪物”一阶段待取数，暂生成）'),
 (37, 4, '重放', ['LeiDang'], 'native', [], '第三章：雷当原生两阶段重放（MainCh3-PoJu；从 Seq123-3 入场，Seq123-4 允许，Seq123-5 战后冻结）'),
 (38, 4, '重放', ['MuXiaoKui'], 'native', [], '沐小葵原生两阶段重放（v14 用户：从 14 层挪来；Seq045/046new 允许，Seq047 与额外剧情分支冻结；转二阶段循环播放主题曲《心湖》）'),
 (39, 4, '1v1', ['QiShu_Bing'], 'native', ['QiShu_Huo'], '回访支线（灭法之法）：寒岁（原生高空独立空间；落不了地运行时退本区经典场地）'),
 (40, 4, '1v1', ['QiShu_Huo'], 'native', ['QiShu_Lei'], '回访支线（灭法之法）：炎茹'),
 # ---------------- 五区 · 回访支线 → 终章断魂崖（41–50）
 (41, 5, '1v1', ['QiShu_Lei'], 'native', ['QiShu_Huo'], '回访支线（灭法之法）：雷婷'),
 (42, 5, '1v1', ['QiShu_Du'], 'native', ['QiShu_Lei'], '回访支线（灭法之法）：蛊鹤（原生高空独立空间；用户 2026-11-05 所有 Boss 优先原生场地）'),
 (43, 5, '1v1', ['Scavenger'], 'native', ['Scavenger_LengTu'], '回访：活人偶清道夫（拿到后期道具才能回溪谷开启，不算第一章流程）'),
 (44, 5, '车轮3', ['YuCong_Bing', 'YuCong_HunDun', 'YuCong_WuLi'], 'native', [], '回访支线（三面混沌）：玉琮最后阶段三形态逐只车轮（用户实测：三只同时上太难）'),
 (45, 5, '1v1', ['Danghui'], 'native', ['CanQu_BiHai'], '回访支线（湖心秘密）：操偶师荡秽（原生高空场地无门：战斗区按站立位置取圆；用户 2026-11-05 优先原生场地）'),
 (46, 5, '1v1', ['MengHua'], 'native', ['LengTu'], '救苦救难梦华圣母娘娘（Seq148）'),
 (47, 5, '1v1', ['FengXueDaoKe'], 'native', ['IronMaskMan_Boss'], '风雪刀客（Seq126）'),
 (48, 5, '1v1', ['LengTu'], 'native', ['MengHua'], '倒数第三层：梦差冷荼（流程长、召唤多：技能召旧 Boss，原生遭遇契约待开发；用户 2026-11-04）'),
 (49, 5, '1v1', ['MoTian'], 'native', ['IronMaskMan_Boss'], '倒数第二层：暗魔天堡之主魔渊（用户 2026-11-03）'),
 (50, 5, '塔主', ['ZuoShang_Ending', 'ZuoShang_P2'], 'native', [], '塔主：左殇真结局两段——义体 → 真龙之躯，在原生终章战场（断魂崖高空的竹林故屋，战斗域 .218）。技能吸附原生锚点，只许原生场地（NATIVE_ONLY）'),
]

# ---- Boss 难度曲线（v14，用户 2026-11-05：按主角成长预设，不跟玩家实时属性同步；平滑、不叠乘）
# 主角预期成长（正常养成、普通奖励，不按最优搭配）：两把主兵器各约每层 +1 级（淬炼点 2–4 / 层，到 35 层），
# MOD 攻击奖励整轮 ≤ +10%，生命奖励整轮 ≤ +35%，原生饰品 / 药按进度开放。估算主角有效输出第 50 层约为第 1 层的 2 倍、
# 生命约 1.4 倍；Boss 原生招式本身按剧情进度变强。所以：
#   血量 H(f) = 4000 ×（1 + 0.04 (f−1) + 0.0008 (f−1)²）——第 1 层 4000、第 20 层约 8200、第 50 层约 19500（二次项缓增，无指数膨胀）
#   伤害 D(f) = 0.95 + 0.003 (f−1)——第 1 层 ×0.95、第 50 层 ×1.10（原生后期 Boss 本身伤害已高，只做小幅抬升，避免秒杀）
# 5 分钟限时挑战：按预期输出，单只 Boss 层正常发挥约 60–180 秒可完成，留出余量。数值是开发 / 复测用表，不进玩家界面。
# 隐藏第 51 层（用户 2026-11-05）：同一轮完成 2 项禁药 + 2 项额外限时，第 50 层通关后解锁；魔渊场地，魔渊 + 沐小葵（二阶段）+ 左殇：真龙之躯 1v3。
# 不进 1–50 的身份计数 / 层型检查；引擎按 floorplay::Find(51) 读
HIDDEN = [
 (51, 5, '1v3', ['ZuoShang_P2', 'MoTian', 'MuXiaoKui_P2'], 'native', [], '隐藏层：左殇·真龙之躯的原生终章战场（ui15 实机：在魔渊场地左殇没有 AI 控制器、一脚就死），左殇 + 魔渊 + 沐小葵·铁玉女王三只同时 1v3'),
]

def target(f):
    x = f - 1
    # 用户 2026-11-05："Boss 整体血量至少提升 2.5–3 倍" → 原曲线（基数 4000）×3；
    # 用户 2026-11-06 分段：1–15 层 ×3、16–25 层 ×3.5、26–45 层 ×4、46–50 层 ×4.5（第 51 层同 ×4.5）
    # 困难难度（逐层表即困难）。1.0.1 用户 2026-11-07（困难太难）：1–15 ×3，16–25 ×5、26–45 ×6、46–51 ×7
    # 1.4.0 用户 2026-11-08（攻击 / 减伤奖励大幅加强，Boss 要拉高很多）：1–15 ×5，16–25 ×10，26–45 ×13，46–51 ×16
    # 2.0.0 用户 2026-11-08（后期别互秒：伤害压低，血量加高）：26–45 ×16，46–51 ×22
    mult = 5.0 if f <= 15 else 10.0 if f <= 25 else 16.0 if f <= 45 else 22.0
    return 4000 * mult * (1 + 0.04 * x + 0.0008 * x * x)
def dmg_curve(f):
    # 困难难度。1.0.1 用户 2026-11-07（困难太难）：1–15 层 ×1.5，16 层起 ×2.5（原 ×2 / ×3）
    # 1.4.0 用户 2026-11-08：1–15 层 ×2.5，16–35 层 ×4，36 层起 ×5
    return (0.95 + 0.003 * (f - 1)) * (2.5 if f <= 15 else 3.0 if f <= 35 else 3.5)  # 2.0.0 用户 2026-11-08：后期伤害太高，16–35 层 ×4 → ×3，36 层起 ×5 → ×3.5
# 个别 Boss 的修正（原生强度 / 机制不同，不对所有 Boss 套同一倍率）：(血量 ×, 伤害 ×, 依据)
BOSS_ADJ = {
    'YanQianSui': (0.85, 1.00, '先破杀气才能伤血（原生机制），有效血量天然偏厚'),
    'LengTu': (0.85, 0.90, '技能连续召出旧 Boss 替身 / 人偶，场上同时受压'),
    'Danghui': (0.90, 1.00, '操偶召物多、场地无门'),
    'MengHua': (0.90, 1.00, '远程弹幕多，近身窗口少'),
    'QiShu_Lei': (0.90, 1.00, '瞬移出界频繁（41 层实测），追击时间长'),
    'YingMei_RedShadowBoneBlade': (0.90, 0.95, '连段快、霸体长'),
    'MoTian': (1.00, 0.90, '倒数第二层，原生伤害已很高'),
    'MBossVariants': (1.00, 0.95, '撕裂锯轮范围大'),
    'IronMaskMan_Boss': (1.10, 1.00, '原生偏弱（用户评价），前移到 14 层后略加血量'),
    # 1.3.4（用户 2026-11-07：玩家反映个别 Boss 难度太大，单独调低）
    # ① 玩家点名
    'ChiLian': (0.75, 0.75, '原生战有 NPC 助战，塔里一个人打：两阶段血量 / 伤害都单独降'),
    'ChiLian_P2': (0.75, 0.68, '同上；献祭之蛇原生攻击 120（再 ×0.91）'),
    'GuiChai_Red': (0.90, 0.60, '一招能秒人；第 17 层正好在伤害 ×1.5 → ×2.5 的台阶后，降到约等于第 15 层的强度'),
    'GuiChai_Red_P2': (0.90, 0.60, '同上（磷火意识）'),
    # ② 原生基础攻击高于 100 的（DT_CharacterAttributes Attack_Base；塔的层倍率又乘在它上面）：伤害 ×√(100 / 原生攻击)
    'PowerMan': (1.00, 0.78, '原生攻击 150（×0.82）；力者和锯者同场（×0.95）'),
    'PowerMan_P2': (1.00, 0.82, '原生攻击 150'),
    'Scavenger': (1.00, 0.82, '原生攻击 150'),
    'Zhuobi': (1.00, 0.88, '原生攻击 130'),
    'BiYuan': (1.00, 0.88, '原生攻击 130'),
    'LionDancer': (1.00, 0.88, '原生攻击 130（区首领）'),
    'WanJun_JinGua': (1.00, 0.88, '原生攻击 130'),
    'PengMeng_Axe': (1.00, 0.91, '原生攻击 120'),
    'GuiChai_Yellow_MoDao': (1.00, 0.91, '原生攻击 120'),
    'GuWanYue': (1.00, 0.91, '原生攻击 120'),
    'MuXiaoKui': (1.00, 0.91, '原生攻击 120'),
    'MuXiaoKui_P2': (1.00, 0.91, '原生攻击 120'),
    'ZuoShang_Ending': (1.00, 0.91, '原生攻击 120（塔主）'),
    'ZuoShang_P2': (1.00, 0.91, '原生攻击 120（塔主）'),
}
def adj(stem):
    return BOSS_ADJ.get(stem, (1.0, 1.0, ''))

def short(stem):
    b = boss[stem]
    return b['name']

ECO = open('E:/PBZ-claude/mods/tower/src/tower_economy.h', encoding='utf-8').read()
drops = {m.group(1): m.group(2) for m in re.finditer(r'\{"BP_Monster_([^"]+)", "[^"]+", "([^"]+)", (?:true|false), (?:true|false)\}', ECO)}
counts = collections.Counter()
rows = []
errors = []
for (f, zone, mode, bs, venue, alts, note) in F:
    for s in bs + alts + ALLIES.get(f, []) + list(DEPS) + list(NATIVE_ONLY):
        if s not in boss: errors.append(f'{f}: unknown {s}')
    if (mode == '团战') != (f in ALLIES): errors.append(f'{f}: 团战层要有友军候选，别的层不能有')
    if (mode == '重放') != (f in REPLAY): errors.append(f'{f}: 重放层要有遭遇契约，别的层不能有')
    ids = [boss[s]['id'] for s in bs]
    same = [i for i, c in collections.Counter(ids).items() if c > 1]
    # 同场同时出现的层（1v2 / 混战 / 1v3）不许同身份，除非是有意的成组（金面双卫、断肠两版同队）；车轮 / 连战允许同身份的阶段类接力
    simultaneous = mode in ('1v2', '混战', '1v3')
    allowed = {'gold_mask_guard', 'GuiChai_Cyan_ShuangYue'}
    if simultaneous and [i for i in same if i not in allowed]: errors.append(f'{f}: same identity on the field {same}')
    if mode != '押注':
        for i in set(ids): counts[i] += 1
    hpShare, dmgShare, par = MODE[mode]
    tgt = target(f)
    each = tgt * hpShare
    dmg = dmg_curve(f) * dmgShare
    rows.append((f, zone, mode, bs, venue, alts, note, each, dmg, par))
over = {i: c for i, c in counts.items() if c > (2 if i in ('gold_mask_guard', 'YuCong') else 1)}  # 玉琮：30 层初见战与 44 层最后三形态是两场不同遭遇（用户 2026-11-05）
spice = [r[0] for r in F if r[2] in ('1v2', '车轮2', '车轮3', '混战', '1v3', '区首领车轮', '塔主')]
if len(spice) > 10: errors.append(f'creative floors {len(spice)} > 10: {spice}')
for (f, zone, mode, bs, venue, al, note) in F:
    vstem = bs[0] if venue == 'native' else CLASSIC[venue][0]
    if vstem in LANDING_BAD and bs[0] not in NATIVE_ONLY: errors.append(f'{f}: venue {vstem} is landing-bad ({LANDING_BAD[vstem]})')
for z, c in ZONE_CLASSIC.items():
    if CLASSIC[c][0] in LANDING_BAD: errors.append(f'zone {z} classic {c} is landing-bad')
for k in BOSS_ADJ:
    if k not in boss: errors.append(f'unknown BOSS_ADJ stem {k}')
for k in LANDING_BAD:
    if k not in boss: errors.append(f'unknown landing-bad stem {k}')
timed_bad = [r[0] for r in F if r[2] == '1v1限时' and (r[0] in REPLAY or len(r[3]) != 1)]
if timed_bad: errors.append(f'timed floors must be single spawned bosses: {timed_bad}')
small = [r[0] for r in F if r[2] in LATER]
if len(small) > 3: errors.append(f'small creative floors {len(small)} > 3: {small}')
NATIVE_DUO = {13}  # 原生同组同时双人战（少主双卫 1v2，用户 2026-11-03）；前两区只许这一层例外
NATIVE_TRIO = set()
early = [r[0] for r in F if r[1] <= 2 and r[2] in ('1v2', '1v3', '1v1限时', '团战', '押注') and not (r[2] == '1v2' and r[0] in NATIVE_DUO) and not (r[2] == '1v3' and r[0] in NATIVE_TRIO)]
if early: errors.append(f'zones 1-2 must not have 1v2 / 1v3 / timed floors: {early}')
if over: errors.append(f'identity repeated: {over}')
if errors:
    print('\n'.join(errors)); sys.exit(1)

used = set(s for r in F for s in r[3])
alts = set(s for r in F for s in r[5])
unused = [s for s in boss if s not in used and s not in alts]

VEN = {}
for m in re.finditer(r'\{"([^"]+)", "([^"]*)", "([^"]+)", ([-\d.]+), ([-\d.]+), ([-\d.]+), [-\d.]+, \d+, (\d+), \d+, \d+, ([\d.]+), [-\d.]+, [-\d.]+, [\d.]+, (true|false)',
                     open('E:/PBZ-claude/mods/tower/src/venues.generated.h', encoding='utf-8').read()):
    stem = m.group(1).rsplit('.', 1)[1][:-2].replace('BP_Monster_', '')
    VEN[stem] = dict(world=m.group(2), doors=int(m.group(7)), area=float(m.group(8)), sky=m.group(9) == 'true')

def venue_stem(stems, v):
    return stems[0] if v == 'native' else CLASSIC[v][0]

def venue_text(stems, v):
    st = venue_stem(stems, v)
    ve = VEN.get(st)
    tag = '' if not ve else f'（{ve["doors"]} 门{"，高空空间" if ve["sky"] else ""}）'
    if v == 'native':
        b = boss[stems[0]]
        return f'原生：{WORLD.get(b["native"], b["native"])}{tag}' if ve else f'原生：{WORLD.get(b["native"], b["native"])}（无场地数据 → 运行时退回本区经典场地）'
    return f'经典 {v}：{CLASSIC[v][1]}'

MODENAME = {'重放': '原生遭遇重放', '1v1': '1v1', '1v1限时': '1v1 · 限时', '1v2': '1v2 双雄', '车轮2': '车轮 ×2', '车轮3': '车轮 ×3', '混战': '混战（三方）',
            '1v3': '1v3 错峰', '区首领': '区首领', '区首领车轮': '区首领 · 连战 ×2', '塔主': '塔主 · 连战',
            '团战': '团战 2v2（待接入）', '押注': '押注观战 BvB（待接入）'}
ZONES = {1: '一区 · 悲泣溪谷 → 血色森林（1–10 层）', 2: '二区 · 血色森林 → 陆家村（11–20 层）', 3: '三区 · 庞镇 / 后街 → 正道大营（21–30 层）',
         4: '四区 · 梅谷湖 → 枷笼山（31–40 层）', 5: '五区 · 戏梦山庄 → 断魂（41–50 层）'}
out = []
for z in range(1, 6):
    out.append(f'\n### {ZONES[z]}\n')
    out.append('| 层 | 层型 | Boss（出场顺序） | 场地 | 每只目标血量 | 伤害倍率 | 标准限时 | 可掉兵器 | 备选 | 说明 |')
    out.append('|---|---|---|---|---|---|---|---|---|---|')
    for (f, zone, mode, bs, venue, al, note, each, dmg, par) in rows:
        if zone != z: continue
        names = ' → '.join(short(s) for s in bs) if mode.startswith('车轮') or '连战' in MODENAME[mode] or mode == '塔主' or mode == '1v3' else ' vs '.join(short(s) for s in bs) if mode == '押注' else ' + '.join(short(s) for s in bs)
        if f in ALLIES: names += '；友军三选一：' + ' / '.join(short(s) for s in ALLIES[f])
        alt = '、'.join(short(s) for s in al) or '—'
        par_t = f'{par // 60}:{par % 60:02d}'
        dw = '、'.join(dict.fromkeys(drops[s] for s in bs if s in drops)) or '—'
        out.append(f'| {f} | {MODENAME[mode]} | {names} | {venue_text(bs, venue)} | {each:,.0f} | ×{dmg:.2f} | {par_t} | {dw} | {alt} | {note} |')
print('\n'.join(out))
print('\n<!-- unused -->')
for s in unused: print(f'- {boss[s]["name"]}（{s}，{boss[s]["cls"]} 类，{WORLD.get(boss[s]["native"], boss[s]["native"])}）')
print('\n<!-- counts -->', len(used), 'classes used,', len(set(boss[s]['id'] for s in used)), 'identities;', len(alts - used), 'alt-only')

# ---- 引擎用的逐层表：python -X utf8 -I mods/tower/tools/floors_plan.py --header [--check]
MODE_ENUM = {'1v1': 'Solo', '1v1限时': 'SoloTimed', '1v2': 'Duo', '车轮2': 'Relay', '车轮3': 'Relay', '混战': 'Brawl', '1v3': 'Trio',
             '区首领': 'Zone', '区首领车轮': 'ZoneRelay', '塔主': 'Final', '团战': 'Team', '押注': 'Bet', '重放': 'Replay'}
if '--header' in sys.argv:
    import pathlib
    out = pathlib.Path(__file__).resolve().parents[1] / 'src' / 'floors.generated.h'
    L = ['// 生成文件，勿手改：python -X utf8 -I mods/tower/tools/floors_plan.py --header（方案 notes/tower-50-floors-plan.md 第 6 节）',
         '#pragma once', '#include <string_view>', 'namespace tower::floorplan {',
         '// Team（团战 2v2）/ Bet（押注观战）以后接入，接入前引擎按 1v1 打 bosses[0]',
         '// Replay（原生遭遇重放）：replay = 遭遇契约 id（src/tower_replay.h）',
         'enum class Mode { Solo, SoloTimed, Duo, Relay, Brawl, Trio, Zone, ZoneRelay, Final, Team, Bet, Replay };',
         '// bosses：出场顺序（车轮 / 连战按顺序一只一只来；同时开打的层一起刷）；venue：用谁的原生场地（类路径）；alts：主选不可用时的备选',
         '// allies：团战层开层前给玩家选的友军 Boss',
         f'inline constexpr int kPlanVersion = {PLAN_VERSION};',
         '// 缺必要战斗依赖、运行时不可用的类（换备选；依赖还原并实测前不解禁）',
         'struct Dep { std::string_view path, why; };',
         'inline constexpr Dep kNeedsDeps[] = {' + (', '.join(f'{{"{boss[k]["path"]}", "{v}"}}' for k, v in DEPS.items()) or '{"", ""}') + '};  // 空表时留一个空占位（C++ 不允许零长数组；空 path 不匹配任何 Boss）',
         '// 实机生成前落点全部被拒的场地（Boss 类路径）：运行时当本局已判坏，不作为场地 / 兜底',
         'inline constexpr Dep kLandingBad[] = {' + ', '.join(f'{{"{boss[k]["path"]}", "{v}"}}' for k, v in LANDING_BAD.items()) + '};',
         '// 只能在自己的原生场地打的类（原生场地本局不可用就换备选，不退经典场地）',
         'inline constexpr Dep kNativeOnly[] = {' + ', '.join(f'{{"{boss[k]["path"]}", "{v}"}}' for k, v in NATIVE_ONLY.items()) + '};',
         '// hpTarget / dmgMul：本层单只 Boss 的目标血量与伤害倍率（v14 逐层配置表，已含层型份额；个别 Boss 另乘 kBossAdj）。开层后固定，不随玩家属性 / 读档 / 重试变化',
         'struct Floor { int floor, zone; Mode mode; std::string_view bosses[3]; int count; std::string_view venue; std::string_view alts[2]; int altN; double hpShare, dmgShare; int par; std::string_view allies[3]; int allyN; std::string_view replay; double hpTarget, dmgMul; };',
         '// 个别 Boss 的血量 / 伤害修正（floors_plan.py BOSS_ADJ）',
         'struct BossAdj { std::string_view path; double hp, dmg; };',
         'inline constexpr BossAdj kBossAdj[] = {' + ', '.join(f'{{"{boss[k]["path"]}", {v[0]}, {v[1]}}}' for k, v in BOSS_ADJ.items()) + '};',
         'inline constexpr Floor kFloors[] = {']
    for (f, zone, mode, bs, venue, al, note, each, dmg, par) in rows:
        b = [boss[s]['path'] for s in bs] + [''] * (3 - len(bs))
        a = [boss[s]['path'] for s in al[:2]] + [''] * (2 - len(al[:2]))
        hp, dm, _ = MODE[mode]
        v = boss[venue_stem(bs, venue)]['path']
        al3 = ALLIES.get(f, [])
        rp = REPLAY.get(f, "")
        y = [boss[s]['path'] for s in al3] + [''] * (3 - len(al3))
        L.append(f'    {{{f}, {zone}, Mode::{MODE_ENUM[mode]}, {{"{b[0]}", "{b[1]}", "{b[2]}"}}, {len(bs)}, "{v}", {{"{a[0]}", "{a[1]}"}}, {len(al[:2])}, {hp}, {dm}, {par}, {{"{y[0]}", "{y[1]}", "{y[2]}"}}, {len(al3)}, "{rp}", {each:.0f}, {dmg:.3f}}},')
    L.append('};')
    L.append('// 隐藏层（第 51 层起；解锁条件与入口见 tower.cpp Hidden*）')
    L.append('inline constexpr Floor kHiddenFloors[] = {')
    for (f, zone, mode, bs, venue, al, note) in HIDDEN:
        for x in bs:
            if x not in boss: sys.exit(f'hidden {f}: unknown {x}')
        b = [boss[x]['path'] for x in bs] + [''] * (3 - len(bs))
        hp, dm, par = MODE[mode]
        v = boss[venue_stem(bs, venue)]['path']
        L.append(f'    {{{f}, {zone}, Mode::{MODE_ENUM[mode]}, {{"{b[0]}", "{b[1]}", "{b[2]}"}}, {len(bs)}, "{v}", {{"", ""}}, 0, {hp}, {dm}, {par}, {{"", "", ""}}, 0, "", {target(f) * hp:.0f}, {dmg_curve(f) * dm:.3f}}},')
    L.append('};')
    L.append('// 本层场地运行时不可用（对象没加载 / 去不了）时退回的本区经典场地（类路径，按区 1–5）')
    L.append('inline constexpr std::string_view kZoneClassic[5] = {' + ', '.join(f'"{boss[CLASSIC[ZONE_CLASSIC[z]][0]]["path"]}"' for z in range(1, 6)) + '};')
    L.append('}  // namespace tower::floorplan')
    text = '\n'.join(L) + '\n'
    if '--check' in sys.argv:
        if not out.exists() or out.read_text(encoding='utf-8') != text:
            sys.stderr.write(f'{out.name} is stale: run floors_plan.py --header\n'); sys.exit(1)
        sys.stderr.write(f'{out.name}: up to date\n')
    else:
        out.write_text(text, encoding='utf-8', newline='\n')
        sys.stderr.write(f'{out.name}: {len(rows)} floors\n')
