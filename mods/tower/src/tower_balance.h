// 影塔数值体系 v3（用户 2026-11-12 审定，方案 notes/balance-v3-proposal-20261112.md）。
// 所有可调数值集中在这里；层号 s 一律是显示层（1–100：奇数 Boss 层、偶数小怪段），内部层 F 的 Boss 层 s = 2F − 1。
#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

namespace tower::balance {

// ---------------------------------------------------------------- 魂玉收入
inline double J(int s) { return 30.0 + 3.0 * std::max(1, s); }
// 3.3.3（用户：魂玉获取手段要多一点）：Boss 层 1.2 → 1.5、小怪段 0.8 → 1.0（+25%）
inline constexpr double kBossJade = 1.5, kWaveJade = 1.0, kRewardJade = 1.5, kZoneBossJade = 2.0, kBonusJade = 3.0;
inline constexpr double kBossJadeSpread = 0.15;  // Boss 层随机 ±15%
// 无尽模式（第 51 层起每层一个显示层，51 → 100）：价格 / 收入 / 难度按显示层算
inline int ShownOfBoss(int F) { return F > 50 ? F + 49 : 2 * F - 1; }
inline int ShownOfWave(int F) { return F > 50 ? F + 49 : std::max(1, 2 * F - 2); }

// ---------------------------------------------------------------- 驿站价格：基准 × 两层收入 2J(s) × (1 + 涨幅)^已买次数
enum class Buy { Weapon, Accessory, Medicine, Refine, Temper, Cultivate, Mod, Count };
inline constexpr int kBuyCount = static_cast<int>(Buy::Count);
inline constexpr double kPriceBase[kBuyCount] = {3.6, 2.5, 2.25, 1.75, 1.6, 1.35, 0.9};
// 3.1.0（用户 2026-11-13：涨价太快）：每买一件的涨幅改为原来的 2/3，仍然一直涨
// 3.3.3（用户：涨价太快，玩家一直追不上）：每买一件按原始涨幅的 1/3 线性上涨（不再复利），同类最多涨到 ×2
inline constexpr double kPriceRise[kBuyCount] = {0.15 / 3, 0.12 / 3, 0.12 / 3, 0.10 / 3, 0.08 / 3, 0.08 / 3, 0.20 / 3};
inline constexpr double kPriceRiseCap = 2.0;
inline constexpr double kWeaponPriceCap = 9.5;  // 武器按约 7.2–9.5 个显示层的基础收入定价，再应用海克斯折扣
// 3.0.1 价格曲线（玩家反馈：前期魂玉少、驿站太贵）：在上面的基准价上再乘 PriceCurve(s)（s = 显示层）。
//   显示 1–6 层（前三次驿站）：属性点数（精炼 / 淬炼 / 修为）和 MOD 道具 ×0.50、武器 / 饰品 / 药 ×0.65，基本每次驿站都买得起一样；
//   6–20 层线性回到 ×1.00：20 层起一件属性约 3 个显示层的收入、武器约 7 层；
//   50–100 层 ×1.00 → ×1.50，无尽 100–200 层 ×1.50 → ×2.00，之后不再涨。收入不变，后期想随心买要靠魂玉类海克斯（钱生钱 / 对冲基金 / 积少成多 / 速战速决）
inline constexpr double kPriceEarlyAttr = 0.50, kPriceEarlyGear = 0.65, kPriceLate = 1.50, kPriceEndless = 2.00;
inline double PriceCurve(Buy b, int s) {
    s = std::max(1, s);
    const bool attr = b == Buy::Refine || b == Buy::Temper || b == Buy::Cultivate || b == Buy::Mod;
    const double early = attr ? kPriceEarlyAttr : kPriceEarlyGear;
    if (s <= 6) return early;
    if (s < 20) return early + (1.0 - early) * (s - 6) / 14.0;
    if (s <= 50) return 1.0;
    if (s <= 100) return 1.0 + (kPriceLate - 1.0) * (s - 50) / 50.0;
    return kPriceLate + (kPriceEndless - kPriceLate) * std::min(1.0, (s - 100) / 100.0);
}
inline int Price(Buy b, int s, int bought) {
    const int i = static_cast<int>(b);
    double layers = 2.0 * kPriceBase[i] * std::min(kPriceRiseCap, 1.0 + kPriceRise[i] * std::max(0, bought));
    if (b == Buy::Weapon) layers = std::min(layers, kWeaponPriceCap);
    layers *= PriceCurve(b, s);
    constexpr int maxBasePrice = std::numeric_limits<int>::max() / 27;  // 留出特殊商品的整数倍率空间
    return std::max(1, static_cast<int>(std::lround(std::min(layers * J(s), static_cast<double>(maxBasePrice)))));
}
inline int RefreshPrice(int s, int refreshes) { return std::max(1, static_cast<int>(std::lround(0.12 * 2.0 * J(s) * std::pow(1.5, std::max(0, refreshes))))); }
// 货架：武器 2、饰品（含精炼）2、药 1、MOD 道具 2（每种 1 次）、点数 1 种随机；武库扩建武器 +2；钱生钱每类 +1、MOD 3 个各 3 次、点数两种
// 3.2.1（用户）：驿站默认武器 4（含影之武装）、药 2、饰品 3；武库扩建 / 钱生钱照旧往上加
inline constexpr int kShelfWeapon = 4, kShelfAccessory = 3, kShelfMedicine = 2, kShelfMod = 2, kArsenalWeapons = 2, kInvestorExtra = 1, kInvestorMod = 3;
// 3.1.0（用户 2026-11-13）魂玉结算顺序：基础 ×（1 + 加成合计，如聚宝盆 +50%）× 倍数（钱生钱 ×2、悬赏令 ×2）×（1 + 积少成多）
inline int JadeTotal(int earned, double additive, double multiplier, double snowball) {
    if (earned <= 0) return 0;
    return static_cast<int>(std::lround(earned * (1.0 + std::max(0.0, additive)) * std::max(1.0, multiplier) * (1.0 + std::max(0.0, snowball))));
}
inline int InterestCap(int s) { return static_cast<int>(std::lround(2.0 * J(s))); }  // 对冲基金：单次利息上限 2 × J(s)

// ---------------------------------------------------------------- 攻速 / 移速软上限
// 合计加成超过 knee 的部分只算一半；绝对上限 ×2.5
inline constexpr double kMoveKnee = 0.80, kAttackSpeedKnee = 1.00, kSpeedHardCap = 2.5;
// 用户 2026-11-13：移速 / 攻速也不封顶（原来超过阈值折半、最高 2.5 倍）。knee 参数保留只为调用处不变
inline double SoftSpeed(double bonus, double knee) {
    (void)knee;
    return 1.0 + std::max(-0.9, bonus);
}

// ---------------------------------------------------------------- 减伤：MOD 最多 80%；与原生乘算后实际受伤最少 10%
inline constexpr double kMinModTaken = 0.2, kMinTotalTaken = 0.1;
// 用户 2026-11-12：异常技能一下 / 两下秒杀 → 单帧受伤最多生命上限的 40%（生命下限抬到「当前 − 40% 上限」，见 tower.cpp FightRules）
inline constexpr double kSpikeCap = 0.40;
// 用户 2026-11-12：显示 300 层起，敌人杀气 5 秒没被削减就回复（每秒 15% 上限）
inline constexpr int kShachiRegenFrom = 300;
inline constexpr double kShachiRegenDelay = 5.0, kShachiRegenRate = 0.15;
// 3.6.0（用户 2026-11-14）无尽层（100 层起）敌人回复：距上次扣血或杀气减少满 15 秒开始（实机后 3 → 15）；
// 生命每秒 2% 最大生命，每 100 层 +1%，最多 5%；杀气每秒 10% 上限（300 层起保留原来的 15%）
inline constexpr int kEndlessFrom = 100;  // 无尽模式第一层（显示层）
inline constexpr double kRegenDelay = 15.0;  // 3.6.0 实机后（用户 2026-11-15）：3 → 15 秒（虚化 / 游龙钻地这类角色躲着不挨打也会回）
// 3.6.0 实机（用户 2026-11-15）：有蓝盾 / 虚化机制的 Boss 不回复（没带对应武器破盾、虚化时打不到，会一直回、永远打不死）
inline constexpr std::string_view kRegenExempt[] = {"拙壁", "荆野", "烟千岁", "骨婉约"};
inline bool RegenExempt(std::string_view name) { for (auto k : kRegenExempt) if (name.find(k) != std::string_view::npos) return true; return false; }
inline double EnemyHpRegen(int s) { return s < kEndlessFrom ? 0.0 : std::min(0.05, 0.02 + 0.01 * ((s - kEndlessFrom) / 100)); }
inline double EnemyShachiRegen(int s) { return s < kEndlessFrom ? 0.0 : s >= kShachiRegenFrom ? kShachiRegenRate : 0.10; }

// ---------------------------------------------------------------- 3.0.1 前期减负（玩家反馈：前期太肉；用户：1–60 层尽量轻松，选满 3 个海克斯（1 / 30 / 60 层）后再慢慢加难）
// Boss 与小怪共用，乘在下面的曲线上（显示层）。血量：1–30 层 ×0.55，30–60 层 → ×0.70，60–90 层 → ×1.00；
// 伤害：1–30 层 ×0.75，30–60 层 → ×0.85，60–90 层 → ×1.00。90 层以后、无尽层不变
inline double Ramp3(int s, double a, double b, double c) {
    if (s <= 30) return a;
    if (s <= 60) return a + (b - a) * (s - 30) / 30.0;
    if (s < 90) return b + (c - b) * (s - 60) / 30.0;
    return c;
}
// 3.1.0（用户 2026-11-13：50 层后 Boss 太脆、伤害太高，玩家经常秒 Boss，叠再多血也被一套秒）：
// 血量 30–60 层回升更快（60 层 ×0.90），75 层回到 ×1.00；伤害 1–60 层都保持 ×0.75，60–100 层才回到 ×1.00
// 3.1.0 N6（用户 2026-11-13：前 50 层不动；50 层往后 Boss / 小怪多加血、攻击略降）：
//   血量 50 层后 0.783 → 60 层 1.00 → 80 层 1.50，之后保持 ×1.5；伤害 50 层 0.75 → 60 层 0.70 → 100 层 0.90，之后保持 ×0.9
inline double EarlyHpRelief(int s) {
    if (s <= 30) return 0.55;
    if (s <= 50) return 0.55 + 0.35 * (s - 30) / 30.0;   // 与之前相同（50 层 = 0.7833）
    // 3.1.0（用户 2026-11-13：60–99 层太肉，压低并平滑）：50 层 0.783 → 100 层 1.0 线性，不再 80 层起 ×1.5
    const double at50 = 0.55 + 0.35 * 20 / 30.0;
    if (s < 100) return at50 + (1.0 - at50) * (s - 50) / 50.0;
    return 1.0;
}
// 3.4.3（用户：1–50 层 Boss 血量略升；50 层后整体先降，曲线不变）：Boss 血量再乘整体系数——
// 显示 1–50 层 ×1.10，50–60 层线性过渡，60 层起（含无尽）×0.85。只作用于 Boss，小怪段不变
// 3.5.0（用户：1–20 层没装备太难打；20–50 层加血；50 层后略降、攻击降低）：1–20 ×0.85，20–30 平滑升到 ×1.20，30–50 ×1.20，
// 50–60 平滑降到 ×0.80，60 起（含无尽）×0.80
// 4.0.0（用户 2026-11-15：20 层起 Boss 血量太低，玩家已成长）：显示 20 层起再乘一层，20→30 层由 ×3 渐进到 ×5.5，之后保持（含无尽）
// 4.0.1（用户 2026-11-16：19→20 层一下跳 3 倍不平滑，前 20 层玩家也打得过）：显示 5 层起就开始抬，5→20 层线性，接着 20→30 层，之后保持（含无尽）。
// 同日最后一轮（用户：4.0 的生命提升用力过猛，一层打 10 多分钟）：上抬从 ×3→×5.5 降到 ×2.2→×3.5，Boss 生命 / 杀气基数再 ×0.8（kBossBase）
inline constexpr double kMidHpLiftStart = 5, kMidHpLiftFrom = 2.2, kMidHpLiftTo = 3.5;
inline constexpr double kBossBase = 0.8;
inline double MidHpLift(int s) {
    if (s <= kMidHpLiftStart) return 1.0;
    if (s < 20) return 1.0 + (kMidHpLiftFrom - 1.0) * (s - kMidHpLiftStart) / (20 - kMidHpLiftStart);
    if (s < 30) return kMidHpLiftFrom + (kMidHpLiftTo - kMidHpLiftFrom) * (s - 20) / 10.0;
    return kMidHpLiftTo;
}
// 4.0.1（用户 2026-11-16 实机：Boss 血量上抬后杀气没跟上，3 刀破防）：杀气上限跟着血量一起上抬，取血量上抬倍数的 0.7 次方
// （20 层 ×1.74、30 层起 ×2.40，含无尽），再乘同一个基数 kBossBase
inline constexpr double kShachiLiftExp = 0.7;
inline double MidShachiLift(int s) { return std::pow(MidHpLift(s), kShachiLiftExp); }
inline double BossHpShift(int s) {
    if (s <= 20) return 0.85;
    if (s < 30) return 0.85 + (1.20 - 0.85) * (s - 20) / 10.0;
    if (s <= 50) return 1.20;
    if (s < 60) return 1.20 + (0.80 - 1.20) * (s - 50) / 10.0;
    return 0.80;
}
// 3.5.0：Boss 攻击 50 层前不变，50–60 平滑降到 ×0.85，60 起（含无尽）×0.85；只作用于 Boss
inline double BossDmgShift(int s) {
    if (s <= 50) return 1.0;
    if (s < 60) return 1.0 + (0.85 - 1.0) * (s - 50) / 10.0;
    return 0.85;
}
inline double EarlyDmgRelief(int s) {
    if (s <= 50) return 0.75;
    if (s <= 60) return 0.75 - 0.05 * (s - 50) / 10.0;
    return 0.70;  // 3.4.0（用户：60 层后 Boss 攻击太高，叠满减伤 / 生命也扛不住）：不再从 0.70 回升到 0.90
}

// ---------------------------------------------------------------- Boss / 小怪难度：在现有逐层曲线之上再乘 E(s)
// 用户 2026-11-12：前 30 层整体加难，30 层后立刻上升，60 层起加速。现有曲线中期已上涨很多（30 层 Boss 伤害 ×2.3、60 层血量 ×9.9），
// 所以 E(s) 叠在现有曲线上（不是取代），保证每一层都只会更难
// 用户 2026-11-12：100 层后按 100–150 / 150–200 / 200+ 逐段攀升（参考前 100 层：30–60 每层 +2%、60–100 每层 +3%）
inline constexpr double kEarly = 1.15, kMidRate = 0.02, kLateRate = 0.03, kEndlessRate = 0.02, kEndlessRate2 = 0.025, kEndlessRate3 = 0.03;
inline double E(int s) {
    s = std::max(1, s);
    double e = kEarly;
    if (s > 30) e *= std::pow(1.0 + kMidRate, std::min(s, 60) - 30);
    if (s > 60) e *= std::pow(1.0 + kLateRate, std::min(s, 100) - 60);
    if (s > 100) e *= std::pow(1.0 + kEndlessRate, std::min(s, 150) - 100);
    if (s > 150) e *= std::pow(1.0 + kEndlessRate2, std::min(s, 200) - 150);
    if (s > 200) e *= std::pow(1.0 + kEndlessRate3, s - 200);
    return e;
}
// 各属性随 E 的指数：伤害涨得比血量慢（玩家减伤有上限、攻击无上限）
inline constexpr double kBossHpExp = 1.0, kBossDmgExp = 0.75, kBossShachiExp = 0.8, kBossBreakExp = 0.6;
inline constexpr double kMobHpExp = 0.9, kMobDmgExp = 0.7, kMobShachiExp = 0.7, kMobBreakExp = 0.5;
// 3.0.0（用户 2026-11-12 同意平滑，notes/stress-3.0.0.md 第五节）：分段指数，后期"越来越危险"而不是"越来越肉"。
//   血量：60 层前按 E 的 1.0 次方，60–100 层 0.6 次方，无尽层 0.5 次方（不再乘 LateHp）；
//   伤害：60 层前 0.75 次方，之后 0.9 次方；杀气上限：0.8 / 0.5 / 0.4。小怪同理略低
inline double Grow(int s, double early, double late, double endless) {
    s = std::max(1, s);
    double g = std::pow(E(std::min(s, 60)), early);
    if (s > 60) g *= std::pow(E(std::min(s, 100)) / E(60), late);
    if (s > 100) g *= std::pow(E(s) / E(100), endless);
    return g;
}
// 3.1.0：60 层后血量增长 0.6 → 0.9 次方、无尽 0.5 → 0.7 次方（Boss 更耐打）；伤害 60 层前 0.75 → 0.7、之后 1.0 → 0.8 次方（不再一套秒）
// 3.6.0（用户 2026-11-14：100–200 层几乎不受伤）：进 100 层一次性抬高（血 ×3、攻 ×2.5、破防 ×2、杀气上限 ×1.5），
// 之后换更陡的次方（血 0.7 → 0.85、攻 0.55 → 0.75、破防 0.9 → 1.0、杀气 0.4 → 0.6）。小怪抬高部分取 Boss 的 80%、次方同样加陡。
// 测试包 QA 命令 endless_scale 可在游戏里实时改这四个倍数（正式包里没有该命令，始终用默认值）
// 3.6.0 实机后（用户 2026-11-15）：Boss 攻击偏低再加 50–100%（取 75%：×2.5 → ×4.4）；血量太高（有回复了）基数减半 ×3 → ×1.5、之后血量次方 0.85 → 0.75（小怪 0.6 → 0.5）
struct EndlessStep { double hp = 2.5, atk = 4.4, brk = 2.0, shachi = 1.5; };  // 4.0.0（用户）：无尽血量基数 ×1.5 → ×2.5
inline EndlessStep g_endlessStep{};
inline constexpr double kMobStepShare = 0.8;
inline double StepAt(int s, double m, bool mob) {
    if (s < kEndlessFrom || !std::isfinite(m) || m <= 0) return 1.0;
    return mob ? std::max(0.05, 1.0 + kMobStepShare * (m - 1.0)) : m;
}
inline double BossHpGrow(int s) { return Grow(s, 1.0, 0.7, 0.85) * StepAt(s, g_endlessStep.hp, false); }  // 4.0.0（用户）：血量曲线略升——60–100 层 0.6 → 0.7、无尽 0.75 → 0.85  // 3.1.0：60–100 层 0.9 → 0.6（与上面一起压低 60–99 层血量）
// 用户 2026-11-12：后期 Boss 攻击 / 破防要跟上玩家叠起来的减伤（不只是血量）
// 3.2.0（用户：玩家多次被后期 Boss 秒杀）：后期伤害缓慢成长——60–100 层指数 0.8 → 0.6、无尽 0.8 → 0.55（100 层约 −21%、200 层约 −43%，60 层前不变）
// 3.4.0（用户：60 层后 Boss 攻击太高）：60–100 层指数 0.6 → 0.4；与上面的减负一起，99 层 Boss 伤害约为 3.3.x 的 62%（11.7 → 7.3 倍原生）
inline double BossDmgGrow(int s) { return Grow(s, 0.7, 0.4, 0.55) * StepAt(s, g_endlessStep.atk, false); }  // 4.0.0（用户：无尽 Boss 几下砍死发育好的玩家）：无尽攻击成长 0.75 → 0.55
inline double BossBreakGrow(int s) { return Grow(s, 0.6, 0.9, 1.0) * StepAt(s, g_endlessStep.brk, false); }
inline double MobBreakGrow(int s) { return Grow(s, 0.5, 0.8, 0.9) * StepAt(s, g_endlessStep.brk, true); }
// 减伤穿透：60 层起敌人无视一部分影塔减伤（属性 / 海克斯 / 铁甲符；原生饰品减伤不穿透），100 层约 14%，200 层起 50%。
// 实际受伤 = 影塔受伤倍率 + (1 − 影塔受伤倍率) × 穿透
// 3.4.0（用户：攻击已经在涨，穿透要慢、平滑）：线性 0 → 30%（200 层）改为 25% × (1 − e^(−(s−60)/200))：
// 100 层约 4.5%、150 层约 9%、200 层约 12.6%、300 层约 17%，永远不到 25%
// 3.6.0（用户 2026-11-14）：穿透 = 0.9 ×（层−60）/（层−60+200），60 层前为 0，无限逼近 90%（100 层 15%、200 层 37%、1660 层 80%）。
// 穿透直接从「封顶后的影塔减伤」里扣：有效减伤 = max(0, min(减伤合计, 上限) − 穿透)，见 TakenFactor
inline double DrPenetration(int s) { return s <= 60 ? 0.0 : 0.9 * (s - 60) / (s - 60 + 200.0); }
// 3.6.0（用户 2026-11-14）：影塔所有减伤（奖励属性、海克斯、铁甲符）相加，封顶 cap（80%，艾卡西亚 90%），再减穿透；
// 返回影塔受伤倍率（1 = 不减伤）。减伤合计为负（易伤）时不封顶、不穿透
inline double TakenFactor(double reduction, double cap, double pen) {
    if (!std::isfinite(reduction)) reduction = 0;
    if (reduction <= 0) return 1.0 - reduction;
    return 1.0 - std::max(0.0, std::min(reduction, cap) - std::max(0.0, pen));
}
inline double BossShachiGrow(int s) { return Grow(s, 0.8, 0.5, 0.6) * StepAt(s, g_endlessStep.shachi, false); }
inline double MobHpGrow(int s) { return Grow(s, 0.9, 0.55, 0.5) * StepAt(s, g_endlessStep.hp, true); }
inline double MobDmgGrow(int s) { return Grow(s, 0.65, 0.6, 0.75) * StepAt(s, g_endlessStep.atk, true); }  // 3.2.0 同 Boss 后期放缓；3.6.0 无尽加陡
inline double MobShachiGrow(int s) { return Grow(s, 0.7, 0.45, 0.6) * StepAt(s, g_endlessStep.shachi, true); }
// 现有血量后期增长 1.1^(s−70) 与 E 叠加过陡：改为 1.03^(s−70)，第 100 层血量与现版相近、中期更高
inline constexpr double kLateHpRate = 0.03;
inline double LateHp(int s) { return std::pow(1.0 + kLateHpRate, std::clamp(s - 70, 0, 30)); }

}  // namespace tower::balance
