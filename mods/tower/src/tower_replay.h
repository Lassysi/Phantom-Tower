// 原生遭遇重放（用户 2026-11-02：经典 / 场地依赖强的 Boss 重放原生遭遇；普通 Boss 继续"场地 + 生成"）。
// 每个重放 Boss 一条遭遇契约（数据），设计见 notes/tower-encounter-modes.md、舞狮证据见 notes/tower-replay-lion-p0.md 与
// Codex build/lion-replay-research/FINDINGS.md。纯逻辑 + 数据，离线测试。
//
// 舞狮的原生链（静态已证）：
//   点睛交互（BPF_WuShiZunZhu_ChuChang，宿主 BP_FlowInteractActor_C_…_1377775561）→ 原生序列节点播 Seq062 → 经正式刷怪点 LionDanceBattle_Seq062 刷出一阶段
//   → 一阶段原生死亡（刷怪点 AIScript_AddTagOnDead 写 pz.击杀.舞狮尊主烈焰P1）→ Death Flow 播 Seq063（刚播放时清掉一阶段刷怪点）→ 刷出二阶段
//   → 二阶段死亡（写 pz.击杀.舞狮尊主烈焰）→ Death Flow 要播 Seq064 战后 CG、写结算 / 任务标签、发 I_06004、自动保存——**全部冻结**。
//
// 硬规则（用户 2026-11-02）：Boss 死后的过场一律冻结。契约只放行 allowSeq（初见 / 转阶段）；战后过场写进 freezeSeq（显式列出，
// 其余没列的过场按会话隔离默认规则照样冻结）；最后一阶段确认死亡后塔直接结算，不等任何原生收尾。
#pragma once
#include <string_view>
#include <array>
#include <cstdint>

namespace tower::replay {

struct Contract {
    std::string_view id, name;
    std::string_view phases[4];      // 阶段类名（BP_X_C），按出场顺序；最后一个死了才算打完
    int phaseN;
    std::string_view allowSeq[4];    // 放行的过场（序列资产名）：初见 / 转阶段
    int allowN;
    std::string_view freezeSeq[4];   // 显式冻结的战后过场
    int freezeN;
    std::string_view spawners[4];    // 原生刷怪点对象名：不静默、过场生成不拦
    int spawnerN;
    std::string_view interact[2];    // 开遭遇要用的原生交互物件对象名：交互过滤放行
    int interactN;
    std::string_view resetTags[12];  // 塔档里删掉的完成标记（打过原生这场的存档：遭遇重新可开）
    int resetN;
    std::string_view supportClasses[4]; // 初见 / 转阶段必需的非战斗角色；仅本契约内清场豁免，不计阶段或胜负
    int supportN;
    std::string_view hint;           // 到场后给玩家的提示
    double phaseGap;                 // 上一阶段死后，下一阶段最多等多久（秒；转阶段过场 + 余量）
    double hpShare;                  // 每个阶段的血量份额（相对本层目标血量）
    std::array<uint32_t,4> entryAsset{},entryNode{}; // optional exact native intro node (0 = player interaction)
    std::string_view clearTags[4]{}; // exact per-phase native ClearGroup milestone; empty = no delegate exception
    bool spawnerKick = false;        // 没有入场 Flow：到场后调契约第一个刷怪点的 ClaimSeqSpawn，让原生刷怪点自己刷出 Boss
    bool phaseTravel = false;        // 下一阶段在别处（独立空间）：上一阶段死后等转阶段过场，仍没出现就由塔传送到下一阶段刷怪点并请求原生刷出
    bool nativeGroups = false;       // 原生阶段组整组按原生运行（不静音、不吞脚本），前提是组里脚本全在已核验的无奖励清单里
    bool kickGroup = false;          // spawnerKick 时同组的刷怪点一起 ClaimSeqSpawn（20 层阿瑾：本体 + 6 个红衣兵同组刷出）
};

inline bool UsesCountdown(const Contract& c) {
    return c.spawnerKick && c.interactN==0 && c.allowN==0 && c.entryAsset==std::array<uint32_t,4>{};
}

inline constexpr Contract kContracts[] = {
    {"lion", "舞狮尊主（烈焰 → 最后一舞）",
     {"BP_Monster_LionDancer_C", "BP_Monster_LionDancer_P2_C"}, 2,
     {"Seq062", "Seq063"}, 2,
     {"Seq064"}, 1,
     {"BP_AISpawner_C_UAID_E89C25A7512642AF02_1937586477", "BP_AISpawner_C_UAID_E89C25A7512642AF02_1937587478"}, 2,
     {"BP_FlowInteractActor_C_UAID_E89C25A751264AAF02_1377775561"}, 1,
     {"pz.舞狮.点睛", "pz.击杀.舞狮尊主烈焰P1", "pz.击杀.舞狮尊主烈焰", "pz.舞狮.结算完成", "pz.舞狮.完成任务", "achievment.焰尽狮魂传.完成"}, 6,
     {"BP_NPC_LionDancer_C"}, 1,
     "走到舞狮台前，靠近后按画面提示的交互键开始点睛（初见过场照原生播放）",
     90, 0.6},
    {"red", "火流星（鬼差队长 → 磷火意识）",
     {"BP_Monster_GuiChai_Red_C","BP_Monster_GuiChai_Red_P2_C"},2,
     {"Seq051","Seq052"},2,{"Seq053"},1,
     {"BP_AISpawner_C_UAID_BCFCE7058A98ED7702_1150320007","BP_AISpawner_C_UAID_E89C25A75434917902_1523163972"},2,
     {},0,{"pz.击杀.红衣鬼差P1","pz.击杀.红衣鬼差P2","pz.火流星.出场"},3,{},0,
     "原生初见演出后开战；完整两阶段，最终击杀直接塔结算",90,0.6,
     {0x90CDA5E6u,0x4F5D241Bu,0xE89B7F90u,0xDAAEBA02u},{0xDDB50F27u,0x41E58B31u,0x36EA54B2u,0xEA82AEBFu}},
    {"changhong", "长鸿（雷枪 → 雷电枪骑）",
     {"BP_Monster_ChangHong_Spear_C","BP_Monster_ChangHong_P2_C"},2,
     {"Seq106","Seq107"},2,{"Seq108"},1,
     {"BP_AISpawner_C_UAID_60CF84809909ACCF02_1428350425","BP_AISpawner_C_UAID_60CF84809909ACCF02_1428352426"},2,
     {},0,{"ljc.击杀.长鸿P1","ljc.击杀.长鸿P2"},2,{},0,
     "原生初见演出后开战；完整两阶段，最终击杀直接塔结算",90,0.6,
     {0x21564D97u,0x486C8ACEu,0x38E0E0B4u,0x78C8E7FCu},{0xBC70118Eu,0x42CA0D17u,0x1ABDAFB1u,0x458BD4E0u},
     {"ljc.击杀.长鸿P1","ljc.击杀.长鸿P2"}},
    // v9（用户 2026-11-03：有转阶段 / 场景依赖的才重放）。下面四条按 Codex 导出的 cooked Flow（batch-repair/flow-audit.json）
    // 与地图刷怪点（native_arena_links.json）逐节点对上：入场 = 出场 Flow 里的初见序列节点；转阶段 = Death Flow 里观察
    // 一阶段击杀标签的分支；战后 = 观察最终击杀标签的分支（terminal_flows 已冻结）。静态证据，均未实机。
    // 追命：Seq022 经 HookManP1_Seq022 刷一阶段 → xg.击杀.追命 → Seq023（载入 SecondStage_GouZhe_Runtime 数据层）经
    // HookManP2_Seq023 刷六绝凶镰 → xg.击杀.追命二阶段 → Seq024 三个变体 + 羽弓 / 锁镰发放 + 存档：冻结。
    {"hook", "追命（猎杀者 → 六绝凶镰）",
     {"BP_Monster_HookMan_C","BP_Monster_HookMan_P2_C"},2,
     {"Seq022","Seq023"},2,{"Seq024"},1,
     {"BP_AISpawner_C_UAID_E89C25A75434C14402_1164908023","BP_AISpawner_C_UAID_0A6230820DE8866302_2033638867"},2,
     {},0,{"xg.击杀.追命","xg.击杀.追命二阶段"},2,{},0,
     "原生初见演出后开战；完整两阶段，最终击杀直接塔结算",90,0.6,
     {0x87778D9Du,0x439B26ADu,0xD6A64985u,0xCE21EAE0u},{0xD672E40Eu,0x47B6C517u,0x7FCF359Au,0x873F9E3Au},
     {"xg.击杀.追命","xg.击杀.追命二阶段"}},
    // 白赤练：Seq034 经 ChiLian_Seq034 刷一阶段 → fjzl.击杀.弄蛇P2（一阶段死亡写的是 P2 标签）→ Seq035，刚播放清一阶段刷怪点，
    // 播完载入 BaiChiLianP2_Post 数据层、SpawnMonsterEx(ChiLianP2_Seq035) 刷献祭之蛇（召弦者是它的被动召唤，共享依赖规则保留）
    // → fjzl.击杀.弄蛇 → LD_Post、黑屏、成就、VideoTeleport 自动去陆家村：全部冻结。
    {"chilian", "白赤练（双蛇夫人 → 献祭之蛇）",
     {"BP_Monster_ChiLian_C","BP_Monster_ChiLian_P2_C"},2,
     {"Seq034","Seq035"},2,{"LD_Post"},1,
     {"BP_AISpawner_C_UAID_E89C25A75434C94402_1491894196","BP_AISpawner_C_UAID_60CF848217CA598502_1953991377"},2,
     {},0,{"fjzl.击杀.弄蛇","fjzl.击杀.弄蛇P2"},2,{},0,
     "原生初见演出后开战；完整两阶段，最终击杀直接塔结算",90,0.6,
     {0x9CD07A20u,0x4A6F694Du,0x05D2CEBBu,0x13DBDE4Eu},{0xC34FC228u,0x464CED61u,0x47F5FA8Bu,0x01F7F37Cu},
     {"fjzl.击杀.弄蛇P2","fjzl.击杀.弄蛇"}},
    // 沐小葵：Seq045 经 MuXiaoKui_Seq045 刷一阶段 → hp.击杀.沐小葵 → Seq046new 经 MuXiaoKuiP2_Seq046new 刷铁玉女王
    // → hp.击杀.沐小葵二阶段 → Seq047 + 乱莺 / 玉妖刀发放 + 存档：冻结。
    // 原生还有一条"满足额外剧情条件"分支：二阶段 25% 血 → Seq046-1 撤掉二阶段、刷玉妖 + 铁鬼 → QTE Seq046-2。塔不接这条：
    // 复位时删掉该条件标签，走简单两阶段；万一战中又被写上，Seq046-1 不在放行表，在执行前就被冻结，二阶段不会被撤掉。
    // 出场 Flow 末尾另有 SpawnMonsterEx("MuXiaoKui")，地图导出里没有这个 ID 的刷怪点：不在契约里，若存在按 Boss 刷怪点静默。
    {"muxiaokui", "沐小葵（组织少主 → 铁玉女王）",
     {"BP_Monster_MuXiaoKui_C","BP_Monster_MuXiaoKui_P2_C"},2,
     {"Seq045","Seq046new"},2,{"Seq047","Seq046-1","Seq046-2-QTE-Goal"},3,
     {"BP_AISpawner_C_UAID_E89C25A75434004502_1084590648","BP_AISpawner_C_UAID_E89C25A75434197502_1369350133"},2,
     {},0,
     {"hp.击杀.沐小葵","hp.击杀.沐小葵二阶段","ljc.沐小葵.战斗结束","ljc.沐小葵.最终形态击杀","ljc.沐小葵.血量降低到25",
      "ljc.沐小葵.满足额外剧情条件","ljc.沐小葵.玉妖铁鬼死亡","ljc.沐小葵.电梯垮塌可被激活","ljc.沐小葵.沐小葵出场镜头开始"},9,
     {"BP_NPC_MuXiaoKui_C"},1,
     "原生初见演出后开战；完整两阶段，最终击杀直接塔结算",90,0.6,
     {0x79104BC0u,0x4AEFB02Au,0x5308C29Au,0xEC863F42u},{0xE9212590u,0x48B6D9F1u,0x66AA22BDu,0x4868F975u}},
    // 雷当：原生出场是 Seqs018 → 分身对话 → Seqs019 → 雷当对话 → Seq123-3。塔从 Seq123-3 节点入场（跳过前面的序列和对话）：
    // 播完写 qsd.圣躯心池.开场 → 分身对话，观察它的分支 SpawnMonsterEx(LeiDang_Seq123-3) 刷虎炮 → qsd.击杀.雷当 → Seq123-4，
    // 刚播放清一阶段刷怪点，播完 SpawnMonsterEx(LeiDangVariant_Seq123-4) 刷独臂虎 → qsd.击杀.雷当二阶段 → Boss战.结束、
    // 丝线解锁、Seq123-5、虎炮发放、存档：冻结。站桩 BP_NPC_LeiDang 在 Seq123-3 里出现后被隐藏，作演出依赖保留。
    {"leidang", "雷当（虎炮 → 独臂虎）",
     {"BP_Monster_LeiDang_C","BP_Monster_LeiDang_P2_C"},2,
     {"Seq123-3","Seq123-4"},2,{"Seq123-5"},1,
     {"BP_AISpawner_C_UAID_60CF848A91CA9B9E02_1410879021","BP_AISpawner_C_UAID_60CF848A91CA9B9E02_1410658862"},2,
     {},0,
     {"qsd.击杀.雷当","qsd.击杀.雷当二阶段","qsd.Boss战.结束","qsd.圣躯心池.开场","qsd.圣躯心池.分身对话",
      "XMSZ.圣躯心池.Seq123-3开始隐藏雷当站桩"},6,
     {"BP_NPC_LeiDang_C"},1,
     "原生初见演出后开战；完整两阶段，最终击杀直接塔结算",90,0.6,
     {0xD5F936E6u,0x4417530Du,0x38847DBAu,0xAADB028Eu},{0x1C7D65EDu,0x4457B05Eu,0x84CBD3A3u,0xA0EE5ED4u}},
    // 力者（用户 2026-11-05：力者走遭遇回放，两阶段都打，免场地问题）。证据：OfflineProbe 导出
    // lion-replay-research/f28-32-34-export Narrative/DHY/BPF_Seq113_LiZheRuZhan（AssetGuid F538F36C…）+ Codex F34 实机刷怪点：
    // 入场节点 = Seq113 过场节点 345B22C3…（塔按原生入场方式直接按它的“播放”输入；SetExcuteType 入场区不是入场节点），经 PowerSawMan_Seq113（…_1144974083）刷力者和锯者
    // → dhy.力者战斗.1阶段击杀（且无 xsd.陆解元.可干力者）→ Seq114 转阶段，PowerManP2_Seq114（…_1349435823）刷石破天惊之态
    // → dhy.力者战斗.2阶段击杀 → Seq116 处决 / Seq115 陆解元分支 + 断肢锯 + 任务 + 存档：冻结（断肢锯由塔按 Boss 掉落发）。
    // 复位删陆解元两个标签，走不带陆解元的转阶段分支 B6FF2A3E…。静态 + 实机刷怪点，未实机打过。
    {"lizhe", "力者（力者和锯者 → 石破天惊之态）",
     {"BP_Monster_PowerMan_C","BP_Monster_PowerMan_P2_C"},2,
     {"Seq113","Seq114"},2,{"Seq115","Seq116"},2,
     {"BP_AISpawner_C_UAID_74563C6BC2BDFF5C02_1144974083","BP_AISpawner_C_UAID_74563C6BC2BD3AD802_1349435823"},2,
     {},0,
     {"dhy.Seq113.力者入战","dhy.力者战斗.1阶段击杀","dhy.力者战斗.2阶段击杀","dhy.Seq114力者转阶段.播放完成",
      "dhy.Seq116处决力者.播放结束","xsd.陆解元.可干力者","pz2.陆解元.帮忙"},7,
     {},0,
     "原生初见演出后开战；完整两阶段，最终击杀直接塔结算",90,0.6,
     {0xF538F36Cu,0x4EF2616Du,0x78154A84u,0xBC569D74u},{0x345B22C3u,0x411A81A1u,0xE3B84CBDu,0x25ABE72Fu},
     {"dhy.力者战斗.1阶段击杀","dhy.力者战斗.2阶段击杀"}},
    // 赤影骨刃（35 层）：塔直接生成时 BeginPlay 里自毁（1412be4 实机采样：生成后对象标志 0x40000008 含 Garbage，下一轮已回收），
    // 只能由它自己的原生刷怪点 BP_AISpawner_C_UAID_60CF848A91CA9B9E02_1410919045 刷。没有出场 Flow：到场后对该刷怪点调
    // ClaimSeqSpawn(不隐藏, 唤醒 AI, 强制入战)，按类名接管为本层唯一阶段；战后无剧情。
    {"yingmei", "赤影骨刃·囚锋（原生刷怪点）",
     {"BP_Monster_YingMei_RedShadowBoneBlade_C"},1,
     {},0,{},0,
     {"BP_AISpawner_C_UAID_60CF848A91CA9B9E02_1410919045"},1,
     {},0,{},0,{},0,
     "到场后由原生刷怪点刷出 Boss，直接开战",30,1.0,
     {},{},{},true},
    // 荒行子（32 层，用户 2026-11-05 走遭遇回放）。证据：OfflineProbe 导出 QuestFlowGraph/DDSD/BPF_HuangXingZi*（f28-32-34-export）
    // + venues 刷怪点：出场 Flow B7950B05… 入场节点 = MBoss_IntroAll 过场节点 9AB6ED59…，昆仑叛教者一阶段
    // （刷怪点 …_1635091137）→ ddsd.击杀.荒行子1 → BPF_HuangXingZiDeath：MBoss_TransSplit、撤一阶段刷怪点、进破钟状态 →
    // 二阶段撕裂锯轮在高空独立空间（刷怪点 …_1635134170，z≈10037551），原生靠传送门过去，塔的隔离会拦 → phaseTravel：
    // 过场后仍没出现二阶段，由塔传送到二阶段刷怪点并 ClaimSeqSpawn → ddsd.击杀.荒行子2 → Seq095、对话、血滴子、任务、存档：冻结。
    // 静态证据，转阶段落地未实机。
    {"mboss", "荒行子（昆仑叛教者 → 撕裂锯轮）",
     {"BP_Monster_MBoss_C","BP_Monster_MBossVariants_C"},2,
     {"MBoss_IntroAll","MBoss_TransSplit"},2,{"Seq095"},1,
     {"BP_AISpawner_C_BP_Monster_MBoss_C_UAID_74563CC57EE2514C02_1635091137","BP_AISpawner_C_UAID_74563CC57EE2514C02_1635134170"},2,
     {},0,
     {"ddsd.击杀.荒行子1","ddsd.击杀.荒行子2","ddsd.荒行子.出场","ddsd.荒行子.二阶段","ddsd.荒行子.返回宗祠"},5,
     {},0,
     "原生初见演出后开战；一阶段死后转阶段过场，二阶段在高空独立空间（塔会带你过去），最终击杀直接塔结算",150,0.6,
     {0xB7950B05u,0x40B99699u,0x6B19C5BBu,0x91B6BCE7u},{0x9AB6ED59u,0x4FC5B084u,0x9196629Eu,0xD4CEC611u},
     {"ddsd.击杀.荒行子1","ddsd.击杀.荒行子2"},false,true},
    // 大师兄（28 层，七星剑阵）。证据：OfflineProbe 导出 Narrative/ZDDY BPF_JianZhenChuChang（AssetGuid 118A0BF2…）/ BPF_JianZhenDeath
    // + Codex F28 实机刷怪点 / 组：入场节点 = PlayLevelSequenceEx Seq076 过场节点 BE68125F…（入场区 E909E790 只写出场标签），
    // 一阶段组 MobGroup_0 七人（大师兄 Jianou_Seq076 …_1979997677 + 剑阵弟子 HuaShan_Normal_Blue）全灭 → zddy.击杀.剑阵1 →
    // Seq077、载入 DaYingJianZhen_P2 数据层，二阶段组 MobGroup_1（绝望剑意 Jianou_Seq077 …_1979998678 + 操偶）→ zddy.击杀.剑阵2
    // → Seq078、剑客召集令、任务、存档、FinishGame：冻结。剑阵弟子 / 操偶是原生组成员（全灭才转阶段），作为支援角色保留不清、不计胜负。
    // 剑阵阶段脚本 MobGroupScript_BossCombat_JianZhen_Phase1/2 自己数组员死亡（OnMemberDead、延时、SetShouldSpawn），不是通用 ClearGroup，
    // 无法用字节合同；两组脚本只有 CombatNotify / LinkFogDoor / PlayerCombatTags / GroupScript_Level / 剑阵阶段脚本，没有发奖脚本 → nativeGroups。
    // 静态 + 实机刷怪点 / 组，未实机打过。
    {"jianzhen", "大师兄（七星剑阵 → 绝望剑意）",
     {"BP_Monster_HuaShan_Blue_C","BP_Monster_HuaShan_JianOu_C"},2,
     {"Seq076","Seq077"},2,{"Seq078"},1,
     {"BP_AISpawner_C_UAID_0A6230820DE85F4502_1979997677","BP_AISpawner_C_UAID_0A6230820DE85F4502_1979998678"},2,
     {},0,
     {"zddy.剑阵.出场","zddy.击杀.剑阵1","zddy.击杀.剑阵2","zddy.剑阵.二阶段","zddy.剑阵.完成任务","zddy.剑阵.显示记忆之花"},6,
     {"BP_Monster_HuaShan_White_C","BP_Monster_HuaShan_Normal_Blue_C","BP_Monster_HuaShan_Normal_Blue_Summon_C","BP_Monster_HuaShan_CaoOu_C"},4,
     "原生初见演出后开战；先破七星剑阵（剑阵弟子全灭才转阶段），再打绝望剑意，最终击杀直接塔结算",180,0.6,
     {0x118A0BF2u,0x4B9B37DDu,0x0AEC75B1u,0x4414630Cu},{0xBE68125Fu,0x437DA013u,0x8CD3DF83u,0x96475270u},
     {"zddy.击杀.剑阵1","zddy.击杀.剑阵2"},false,false,true},
    // v15（用户 2026-11-05：9 / 20 / 26 走原生遭遇）。证据：tools/extract 全量扫描（build/boss9-20-26），三只全游戏各一处真实摆放。静态，未实机。
    // 拙壁（9 层）：没有出场 Flow / 过场，正道大营 4DYOM 单元的刷怪点 + MobDomain 战斗组 + 雾门（山道石阶，守门人）。
    // 到场后由它自己的刷怪点 ClaimSeqSpawn 刷出；死亡 Flow BPF_ZhuoBiDead（发拙壁盾 / 教程 / 存档）终端冻结，拙壁盾由塔按 Boss 掉落发
    {"zhuobi", "守门人拙壁（原生刷怪点）",
     {"BP_Monster_Zhuobi_C"},1,
     {},0,{},0,
     {"BP_AISpawner_C_UAID_E89C25A75434106602_2138290599"},1,
     {},0,{"ljc.拙壁.击杀拙壁","smrds.击杀.拙壁"},2,{},0,
     "到场后由原生刷怪点刷出 Boss，直接开战",30,1.0,
     {},{},{},true},
    // 阿瑾（20 层）：没有 Flow / 过场，庞镇刷怪点 XiongZhang 与 6 个 RedTroop_Soldier 同在 MobGroup_0。同组刷怪点一起刷出，
    // 红衣兵作原生同伴保留、不计胜负；阿瑾死即结算。组脚本 AddItemAfterCombat TU_11006 由会话隔离吞掉
    {"ajin", "悲痛欲绝的阿瑾（原生战斗组）",
     {"BP_Monster_HuaShan_RedTroop_BigBro_C"},1,
     {},0,{},0,
     {"BP_AISpawner_C_UAID_E89C25A750C7A0CB02_2042446446"},1,
     {},0,{},0,{"BP_Monster_HuaShan_RedTroop_Soldier_C"},1,
     "到场后阿瑾与红衣兵由原生刷怪点刷出，直接开战（击败阿瑾即过关）",30,1.0,
     {},{},{},true,false,false,true},
    // 万钧（26 层）：交互 BP_ChainBlocker（8AQU30… 单元，距刷怪点约 26 米）→ BPF_WanJunKeChainBlocker 播 Seq074（入场节点 3EE072C5…）
    // → Seq074 经 WanJun_Seq074 刷怪点刷出 → ljc.击杀.万钧客 → BPF_WanJunKeDeath：Seq075 + 长柄金瓜 + 任务 + 存档（终端已冻结）
    {"wanjun", "巨锤客：万钧",
     {"BP_Monster_WanJun_JinGua_C"},1,
     {"Seq074"},1,{"Seq075"},1,
     {"BP_AISpawner_C_UAID_E89C25A75434014502_1638726190"},1,
     {},0,{"ljc.万钧客.出场","ljc.击杀.万钧客","zddy.运镜.获得金瓜后"},3,{},0,
     "原生初见演出后开战；最终击杀直接塔结算",60,1.0,
     {0xEB920F3Fu,0x43B77265u,0x3A7093A7u,0xB5FE5ACFu},{0x3EE072C5u,0x4823CACEu,0xBF9015A1u,0x21F3B18Du}},
};

inline const Contract* Find(std::string_view id) {
    for (auto& c : kContracts) if (c.id == id) return &c;
    return nullptr;
}
inline bool In(const std::string_view* list, int n, std::string_view s) {
    for (int i = 0; i < n; ++i) if (list[i] == s) return true;
    return false;
}
// 类名 → 第几阶段（-1 = 不是本契约的阶段类）
inline int PhaseOf(const Contract& c, std::string_view cls) {
    for (int i = 0; i < c.phaseN; ++i) if (c.phases[i] == cls) return i;
    return -1;
}
// NPC 只是演出依赖，不能被 ReplayTick 当成战斗阶段，也不能在普通生成层获得全局豁免。
inline bool PreserveActor(const Contract* c, std::string_view cls) {
    return c && (PhaseOf(*c, cls) >= 0 || In(c->supportClasses, c->supportN, cls));
}
// 过场放不放：放行表里的放；显式冻结的、没列的都不放（返回 false = 按冻结处理）
inline bool AllowSequence(const Contract* c, std::string_view seq) {
    return c && !In(c->freezeSeq, c->freezeN, seq) && In(c->allowSeq, c->allowN, seq);
}

// 阶段进度：塔只在最后一阶段确认死亡时结算；中间阶段死后等下一阶段出现（超时 = 本层作废）
struct Progress {
    int phases = 0, seen = -1;
    bool finalDead = false;
    double betweenSince = -1;   // 中间阶段死后开始等的时刻
    double startedAt = -1;      // 第一阶段到场的时刻
    void Begin(int n) { *this = {}; phases = n; }
    bool CanAdopt(int k) const { return !finalDead && k>=0 && k<phases && k==seen+1; }
    // 同一阶段的第二个原生角色（34 层力者和锯者：一阶段原生刷两个同类角色）：当前阶段还没死完时并入
    bool CanJoin(int k) const { return !finalDead && k>=0 && k==seen && betweenSince<0; }
    void Seen(int k, double now) {
        if(!CanAdopt(k))return; // a respawned old phase cannot reset the missing-next-phase timeout
        seen = k;
        if (startedAt < 0) startedAt = now;
        betweenSince = -1;
    }
    void Dead(int k, double now) {
        if (k == phases - 1) { finalDead = true; betweenSince = -1; }
        else if (k >= seen && betweenSince < 0) betweenSince = now;
    }
    bool Started() const { return seen >= 0; }
    bool Settle() const { return finalDead; }
    bool Between() const { return betweenSince >= 0; }
    bool TimedOut(double now, double gap) const { return betweenSince >= 0 && now - betweenSince > gap; }
};

}  // namespace tower::replay
