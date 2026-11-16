// 影塔经济（纯数据 + 抽取规则，不碰游戏）：魂玉、道具池、神通、Boss→兵器。
// 数值与 notes/tower-floors.md 第五节一致；Codex 的 data/*.json 就绪后由引擎读表替换这里的内置表。
#pragma once
#include "tower_balance.h"
#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>

namespace tower {

// ---- 区：50 层每 10 层一区（0..4）；不足 10 层的短局算第一区 ----
inline int ZoneOf(int floor, int floors) { return floors < 10 ? 0 : std::clamp((floor - 1) / 10, 0, 4); }
inline bool IsZoneBoss(int floor, int floors) { return floors >= 10 && floor % 10 == 0; }

// ---- 魂玉：在原曲线基础上增加 20%，±20%；区首领 ×3 ----
// v3（用户 2026-11-12）：按显示层 s 的收入 J(s) = 30 + 3s；Boss 层 1.2J ±15%，区末 Boss（每 20 显示层）×2
inline int JadeFor(int floor, bool zoneBoss, double roll01) {
    namespace b = tower::balance;
    const double base = b::kBossJade * b::J(b::ShownOfBoss(floor)) * (1.0 - b::kBossJadeSpread + 2.0 * b::kBossJadeSpread * roll01);
    return static_cast<int>(std::lround(base * (zoneBoss ? b::kZoneBossJade : 1.0)));
}

// 2.0.0 用户 2026-11-08：波次层打完也给魂玉（点数都买还能攒钱，一两层买得起武器）；不随机、没有区首领倍率。
// 回退重打的波次层保留四分之一魂玉，给玩家留一点购物容错。
inline int StageJade(int floor, bool replayed) {
    const int j = static_cast<int>(std::lround(tower::balance::kWaveJade * tower::balance::J(tower::balance::ShownOfWave(floor))));  // v3：小怪段 0.8 J(s)
    return replayed ? std::max(1, j / 4) : j;
}

// ---- 强化卡稀有度（凡 灵 玄 天，百分比），按区抬升 ----
inline constexpr int kRarityByZone[5][4] = {{70, 25, 5, 0}, {58, 30, 11, 1}, {45, 35, 17, 3}, {35, 38, 22, 5}, {25, 40, 28, 7}};

// ---- 道具（一次性，放背包）。用户 2026-11-01 方案调整：去掉战斗中即时使用的道具（免得为它另做界面和按键），
// 全部改成"开层前预装、下一层正式开战时生效、作用整层（定身 / 缓时只作用开头几秒）"的准备道具；另有自动生效的复活符、
// 三选一时用的刷新令。下标保持不变（旧局存档的库存照读），新道具追加在后面。规则见 notes/tower-items-scheme.md
enum class Item { Heal, Reroll, Freeze, Slow, Sunder, Shachi, Intel, Borrow, Revive, Guard, Edge, Break, Rebirth, Foresight, Leech,
                  JadeBag, ExpPill, Rage, Calm, Bounty };  // 3.1.0 新增（用户 2026-11-13），只在末尾追加
struct ItemDef { Item item; std::string_view id, name, text; int weight; };
inline constexpr ItemDef kItems[] = {
    {Item::Heal, "heal", "回春丹", "立即回复 40% 生命", 30},
    {Item::Reroll, "reroll", "刷新令", "重抽一次三选一", 15},
    {Item::Freeze, "freeze", "定身符", "开战后定住敌人 4 秒", 12},
    {Item::Slow, "slow", "缓时符", "开战后 10 秒内敌人动作放慢 40%", 12},
    {Item::Sunder, "sunder", "破甲符", "本层敌人受到的伤害 +30%", 10},
    {Item::Shachi, "shachi", "聚煞丹", "本层杀气恢复 +40%", 10},
    {Item::Intel, "intel", "情报", "观看局揭示一方底细（M2 起生效）", 6},
    {Item::Borrow, "borrow", "借将令", "下一场团战多带一个队友（M2 起生效）", 3},
    {Item::Revive, "revive", "复活符", "倒下后满状态复活一次", 2},
    {Item::Guard, "guard", "铁甲符", "本层受到的伤害 -20%", 10},
    {Item::Edge, "edge", "锐锋符", "本层攻击力、破防能力 +15%", 10},
    {Item::Break, "break", "破势符", "本层削减敌人杀气 +30%", 10},
    // 1.0.1 用户 2026-11-07：没有复活符时倒下，可用它退后 10 层继续（10 层以内倒下则从第 1 层继续）；有几个用几次
    {Item::Rebirth, "rebirth", "重生符", "倒下后（没有复活符时）退后 10 层继续，不用重开；第 11 层以内倒下从第 1 层继续", 2},
    {Item::Foresight, "foresight", "透视眼", "本局永久预告下一场 Boss", 4},
    {Item::Leech, "leech_charm", "吸血符", "下一层额外获得 10% 生命吸取", 3},
    {Item::JadeBag, "jade_bag", "魂玉袋", "使用后获得魂玉，层数越高越多", 3},  // 3.1.0（用户）：放 MOD 背包，点了才用
    {Item::ExpPill, "exp_pill", "经验丹", "使用后获得 3 个淬炼点和 1 个修为点", 3},
    {Item::Rage, "rage", "狂暴药", "本层攻击力 +150%，受到的伤害 +25%", 8},
    {Item::Calm, "calm", "定魂香", "本层敌人杀气恢复速度 -50%", 8},
    {Item::Bounty, "bounty", "悬赏令", "本层击败 Boss 后魂玉 ×3", 3},  // 3.3.3（用户：×2 不够回本）；3.6.0（用户）×5 → ×3、权重 6 → 3、价格跟层走
};
inline constexpr int kItemCount = static_cast<int>(std::size(kItems));
// 3.6.0（用户 2026-11-14）：悬赏令只乘 Boss 层账单 ×3；售价不超过本层 Boss 最低基础魂玉 × 0.8（用了一定赚：净赚 ≥ 基础 × 1.2）
inline constexpr double kBountyMul = 3.0, kBountyPriceShare = 0.8;
inline int BountyPriceCap(int floor) {
    namespace b = tower::balance;
    return std::max(1, static_cast<int>(std::floor(kBountyPriceShare * b::kBossJade * b::J(b::ShownOfBoss(std::max(1, floor))) * (1.0 - b::kBossJadeSpread))));
}
// Preserve saved bag indexes. Instant healing was rejected by the user; Intel/
// Borrow have no implemented use before their respective modes exist.
inline bool ItemInRewardPool(int i) {
    return i >= 0 && i < kItemCount && kItems[i].weight > 0 &&
        kItems[i].item != Item::Heal && kItems[i].item != Item::Intel && kItems[i].item != Item::Borrow && kItems[i].item != Item::Foresight && kItems[i].item != Item::Leech;
}

// ---- 神通：修改器功能，一次性、极稀有，只作用于使用的那一层（用户 2026-10-30 再次强调） ----
enum class Tier { Normal, Rare, Lingxi };
struct Shentong { std::string_view id, name, text, feature; double value; Tier tier; };  // value：倍率类功能的取值，开关类为 0
inline constexpr Shentong kShentong[] = {
    {"bati", "霸体", "本层不被打出硬直", "player.no_stagger", 0, Tier::Normal},
    {"kuanxin", "宽心", "本层弹反窗口加宽一倍", "player.deflect_window_mul", 2.0, Tier::Normal},
    {"baidu", "百毒不侵", "本层免疫异常状态", "player.status_immune", 0, Tier::Normal},
    {"yingwu", "影武无尽", "本层影之武装无限", "combat.phantom_inf", 0, Tier::Normal},
    {"jifeng", "疾风", "本层攻速 +25%", "combat.atk_speed", 1.25, Tier::Normal},
    {"ponian", "一念破防", "本层一击破防", "combat.instant_break", 0, Tier::Normal},
    {"huajin", "化劲", "本层普攻自动格挡", "player.auto_deflect_normal", 0, Tier::Rare},
    {"youshen", "游身", "本层普攻自动闪避", "player.auto_dodge_normal", 0, Tier::Rare},
    {"zidong", "自动出刀", "本层自动攻击", "player.auto_attack", 0, Tier::Rare},
    {"lingxi_blue", "灵犀·蓝", "本层自动格挡蓝光", "player.auto_deflect_blue", 0, Tier::Lingxi},
    {"lingxi_red", "灵犀·红", "本层自动闪避红光", "player.auto_dodge_red", 0, Tier::Lingxi},
};
inline constexpr int kShentongCount = static_cast<int>(std::size(kShentong));
inline constexpr int kShentongBag = 3;

// 每层通过时、每一档掷一次（不是每个神通各掷一次）：普通 0.5% / 区首领 2%；稀有 0.1% / 0.5%；灵犀只在第 30、40、50 层 1%
// 中了再在该档里随机挑一个。期望：整局普通约 0.33 个、稀有约 0.07 个、灵犀约 3%
inline double ShentongChance(Tier t, int floor, int floors) {
    const bool boss = IsZoneBoss(floor, floors);
    switch (t) {
        case Tier::Normal: return boss ? 0.02 : 0.005;
        case Tier::Rare: return boss ? 0.005 : 0.001;
        case Tier::Lingxi: return boss && floor >= 30 ? 0.01 : 0.0;
    }
    return 0;
}

// ---- Boss → 兵器（notes/tower-floors.md 第四节；Codex 核对后改读 data/rewards.json） ----
// stem：Boss 类名（BP_Monster_xxx，不含 _C），同一身份的阶段二 / 梦差版各列一条
struct WeaponDrop { std::string_view stem, weapon, name; bool shaqi; bool verified; };
inline constexpr WeaponDrop kWeaponDrops[] = {
    {"BP_Monster_PengMeng_Axe", "BrutalAxe", "巨凶斧", true, true},
    {"BP_Monster_Danghui", "CaoOuSiXian", "操偶铃铛", true, true},
    {"BP_Monster_YanQianSui", "CuiXinPiPa", "催心琵琶", true, true},
    {"BP_Monster_DuYanBoss", "DuYanGong", "独眼弓", true, true},
    {"BP_Monster_GuiChai_Red", "HuoLiuXing", "火流星", true, true},
    {"BP_Monster_GuiChai_Red_P2", "HuoLiuXing", "火流星", true, true},
    {"BP_Monster_WanJun_JinGua", "JinGua", "长柄金瓜", true, true},
    {"BP_Monster_LionDancer", "LionHead", "烈焰狮头", true, true},
    {"BP_Monster_LionDancer_P2", "LionHead", "烈焰狮头", true, true},
    {"BP_Monster_HookMan", "ShachiHook", "追命锁镰", true, true},
    {"BP_Monster_HookMan_P2", "ShachiHook", "追命锁镰", true, true},
    {"BP_Monster_ChangHong_Spear", "ThunderSpear", "雷霆枪", true, true},
    {"BP_Monster_ChangHong_P2", "ThunderSpear", "雷霆枪", true, true},
    {"BP_Monster_LeiDang", "TigerCannon", "虎炮", true, true},
    {"BP_Monster_LeiDang_JYX", "TigerCannon", "虎炮", true, true},
    {"BP_Monster_LeiDang_P2", "TigerCannon", "虎炮", true, true},
    {"BP_Monster_LuanShi", "TieGongZhuQuanTao", "铁拳套", true, true},
    {"BP_Monster_YouLong_Harpoon", "YouLongCha", "游龙叉", true, true},
    {"BP_Monster_Zhuobi", "ZhuoBiDun", "拙壁盾", true, true},
    {"BP_Monster_AHua_GuiTouDao", "GuiTouBlade", "红妆鬼斩", false, true},
    {"BP_Monster_Yuu", "YuuBlade", "妖锯", false, true},
    // Reachable AddWeaponItem nodes in cooked Death Flows; evidence indexed in
    // build/tower-383fa69/batch-repair/BOSS_DROP_AUDIT_R6.json. Unconnected old
    // LuanYing nodes in MuXiaoKuiDeath are deliberately not a drop contract.
    {"BP_Monster_MuXiaoKui", "YuuBlade", "妖锯", false, true},
    {"BP_Monster_MuXiaoKui_P2", "YuuBlade", "妖锯", false, true},
    {"BP_Monster_MBoss", "XueDiZi", "血滴子", true, true},
    {"BP_Monster_MBossVariants", "XueDiZi", "血滴子", true, true},
    {"BP_Monster_FengXueDaoKe", "BaiYing", "白影", false, true},
    {"BP_Monster_ChiLian", "SnakeDual", "白蟒赤练", false, true},
    {"BP_Monster_ChiLian_P2", "SnakeDual", "白蟒赤练", false, true},
    {"BP_Monster_JingYe", "ZhanMaBlade", "亡命斩马刀", false, true},
    {"BP_Monster_MobMan_Elite_Sword", "IronBlade", "残钢刃", false, true},
    {"BP_Monster_FemaleVessel_YuDing", "RenOuYanYueDao", "偃月刀", false, true},
    {"BP_Monster_FemaleVessel_YuDing_LengTu", "RenOuYanYueDao", "偃月刀", false, true},
    {"BP_Monster_MengHua", "FangHuaXieShan", "圣母邪扇", false, true},
    {"BP_Monster_HuaQianXIao", "ChiZheCanJian", "痴者残剑", false, true},
    {"BP_Monster_GuiChai_Cyan_ShuangYue", "TianShaShuangYue", "天煞双钺", false, true},
    {"BP_Monster_GuiChai_Cyan_ShuangYue_LengTu", "TianShaShuangYue", "天煞双钺", false, true},
    {"BP_Monster_GuiChai_Yellow_MoDao", "PoJun", "破军", false, true},  // 斩骑：庞镇地面原生组 MobGroupScript_AddWeaponAfterCombat WeaponID=PoJun（用户 2026-11-05 实机截图确认）
    {"BP_Monster_GuiChai_Yellow_ShuangGou", "DualHook", "别离残钩", false, true},
    // 部分关联仍待核实，以各行 verified 为准；保留顺序兼容已保存的兵器下标。
    {"BP_Monster_GreatBladeMan", "HorizontalBlade", "荡寇凶刃", false, false},
    // 2026-11-06 改正：炽刃不是 Boss 掉落，是长形黑伤用炎火之握铸魂后的形态（Weapon_FunctionDes_012_Re）；
    // 火刀队长原生只掉碎片"炽热炎火" TU_150012（boss_drops）。本行只为旧存档下标保留，永不发放、不上架（用户：铸魂得来的不算，塔有自己的铸魂逻辑）
    {"BP_Monster_HuoDaoDuiZhang", "XieHuo", "炽刃", false, false},
    {"BP_Monster_YuCong_HunDun", "CiGuHunDun", "刺骨混沌", false, true},
    // rewards.json weapon_drop_27 already confirms this association; live F34
    // also found AddWeaponItem(Jigsaw) in Narrative/DHY/BPF_Seq113_LiZheRuZhan.
    {"BP_Monster_PowerMan", "Jigsaw", "断肢锯", false, true},
    {"BP_Monster_PowerMan_P2", "Jigsaw", "断肢锯", false, true},
    // 2026-11-05 核实：30324ba 第 14 层（毕远）原生流程实发 RenOuQingTongJian 1 级（step46 背包读回），关联成立
    {"BP_Monster_BiYuan", "RenOuQingTongJian", "天玄青铜剑", false, true},
    // 2026-11-05 改正：原"白山风雷剑"来自白山支线 BPF_JianOuQiangOu_Death，不是大师兄；大师兄死亡 Flow BPF_JianZhenDeath（Narrative/ZDDY）
    // 发 AddWeaponItem(JianKeZhaoJiLing)，无 IsNormalWeapon → 影之武装。该行此前未核从未发放，原地改不影响旧存档下标
    {"BP_Monster_HuaShan_Blue", "JianKeZhaoJiLing", "剑客召集令", true, true},
    {"BP_Monster_EMei_Sword_JianZhen", "LuanYing", "乱莺剑", false, false},
    {"BP_Monster_EMei_Sword_Top", "LuanYing", "乱莺剑", false, false},
    {"BP_Monster_JadeMaskMan", "JadeDualChakram", "玉双环", false, true},
    // 用户 2026-11-06：第 50 层打完左殇发紫冥剑（连同铸魂材料破碎龙体），第 51 层可以换上用
    {"BP_Monster_ZuoShang", "ZiMingJian", "紫冥剑", false, true},
    {"BP_Monster_ZuoShang_P2", "ZiMingJian", "紫冥剑", false, true},
    // 2026-11-04 死亡 Flow 全量审计（drop-audit/flow-grants.json）：BPF_YuZanDeath AddWeaponItem(CuiXinPiPa)，无 IsNormalWeapon → 影之武装。
    // 追加在末尾，保持旧存档下标。烟千岁那行同一兵器的关联来源待复核（其死亡 Flow 只发 TU_07002 / I_08002）
    {"BP_Monster_FemaleVessel_YuZan", "CuiXinPiPa", "催心琵琶", true, true},
    {"BP_Monster_HuaShan_JianOu", "JianKeZhaoJiLing", "剑客召集令", true, true},  // 28 层二阶段绝望剑意（同一死亡 Flow）
};
inline constexpr int kWeaponDropCount = static_cast<int>(std::size(kWeaponDrops));
// 铸魂形态（炽刃 = 长形黑伤铸魂）：不是 Boss 掉落，也不单独上架
inline constexpr bool NotBossWeapon(std::string_view id) { return id == "XieHuo"; }
// 塔里打不到的 Boss 的关联武器：按层逐步上驿站（用户 2026-11-06），未列出的从第 25 层起
inline int ShopBossWeaponFloor(std::string_view id) {
    if (id == "JadeDualChakram") return 16;  // 玉面郎君（v14 撤出方案）
    if (id == "LuanYing") return 31;         // 峨眉剑阵
    return 25;
}

// A verified Boss association is an earned drop contract. Random reward/shop
// progression is a separate policy; freezing the native ending must not turn a
// defeated Boss's approved weapon into an unissued "pending random item".
inline bool EarnedBossWeapon(std::string_view id, int kind) {
    if (kind != 0 && kind != 1) return false;
    for (const auto& d : kWeaponDrops)
        if (d.verified && d.weapon == id && d.shaqi == (kind == 1)) return true;
    return false;
}

// 类路径 "/…/BP_Monster_X.BP_Monster_X_C" → 关联兵器下标，没有返回 -1
inline int WeaponForPath(std::string_view path) {
    const auto slash = path.rfind('/'), dot = path.rfind('.');
    if (slash == std::string_view::npos || dot == std::string_view::npos || dot < slash) return -1;
    const auto stem = path.substr(slash + 1, dot - slash - 1);
    for (int i = 0; i < kWeaponDropCount; ++i)
        if (kWeaponDrops[i].stem == stem) return i;
    return -1;
}

// 左殇之后没有普通驿站；第 50 场实际击败他时直接发紫冥剑，供隐藏层使用。
inline int FinalBossWeapon(int floor, const std::vector<std::string_view>& paths) {
    if (floor != 50) return -1;
    for (auto path : paths) {
        const int w = WeaponForPath(path);
        if (w >= 0 && kWeaponDrops[w].verified && kWeaponDrops[w].weapon == "ZiMingJian") return w;
    }
    return -1;
}

}  // namespace tower
