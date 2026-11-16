// 影塔海克斯：局内纯状态；固定 ID，单独随机流，所有效果从持有状态推导。
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include "tower_balance.h"

namespace tower::hex {
#if defined(PBZ_HEX_TEST) && defined(PBZ_RELEASE)
#error PBZ_HEX_TEST must never be included in a release build
#endif
enum Id { Tank, Swift, Icathia, Leech, Ocean, Sharp, Bell, Battle, BreakGrowth, Desperate,
          Sturdy, Keen, Pursuit, Bulwark,
          // 3.6.0：QA 专用的「疾行疾攻（测试）」已移除（QA 构建编号满 64 位），QA 与正式版编号从此一致
          Escape, Grit, KillingIntent, Thrower, Scientist, Forge, Investor, Burn, Heavy, Scavenger, AttributeStack,
          // E 批（2026-11-11）：只在末尾追加，旧存档的 ID 不变
          GlassCannon, BloodPact, Decapitate, Sweep, Fury, Opening, FirstStrike, Reaper, Interest, Charmsmith,
          Arsenal,  // 武器扩建（2026-11-11 用户提案：背包 4 主 + 4 影，原生 2 格轮转）
          // F 批（2026-11-11）
          Invincible,  // 已退役，保留编号以兼容已有塔档
          SnowBall, Fortune, Prepared, Ascetic, Flawless, Blitz, Echo,
          // G 批（2026-11-11）
          Arcane, BackToBasics, Transmutation,
          ServantMaster,
          DivineAid,  // 如有神助（用户 2026-11-12）：持有回归基本功时恢复药瓶 / 绝技 / 影之武装；只在已有回归基本功后出现
          // 3.1.0 新增（用户 2026-11-13）：只在末尾追加，旧存档 ID 不变
          BackWater, WarFeed, IronWall, KillingBoil, TemperMaster, Finisher, Revenge, TreasureBowl,
          FlyingKick,  // 3.6.0（用户 2026-11-15）：复用原「百宝囊」编号（3.1.0 并入有备而来，只在测试档出现过；正式版编号已满 64）
          // 3.3.3（用户：多一些魂玉经济海克斯）：只在末尾追加
          Wealth, Spoils,
          // 3.3.4（用户：影之武装 / 暗源相关）
          MedSpark, SparkFlow, ShadowEdge,
          // 3.6.0（用户 2026-11-14）
          Thorns,
          // 3.7.0 玩法类（用户 2026-11-15 定稿，全部最高 Lv2；数值在 tower_playstyle.h）：编号 64 起，只在末尾追加
          Riposte, Afterimage, Encore, Frenzy, BreakChain, LastStand, ShadowClone,
          MultiJump,  // 3.7.0 连跳（用户 2026-11-15：娱乐向普通卡，常见）
          Count };
static_assert(Count <= 128, "海克斯位图只有 128 位");
// 3.7.0（用户 2026-11-15 同意）：海克斯编号从 64 扩到 128。位图拆成两个 64 位字；老存档只有低位，高位按 0 读
struct Mask {
    uint64_t lo = 0, hi = 0;
    constexpr Mask() = default;
    constexpr Mask(uint64_t low) : lo(low) {}  // 兼容旧写法（只覆盖 0–63）
    constexpr Mask(uint64_t low, uint64_t high) : lo(low), hi(high) {}
    constexpr explicit operator bool() const { return lo || hi; }
    friend constexpr Mask operator|(Mask a, Mask b) { return {a.lo | b.lo, a.hi | b.hi}; }
    friend constexpr Mask operator&(Mask a, Mask b) { return {a.lo & b.lo, a.hi & b.hi}; }
    friend constexpr Mask operator~(Mask a) { return {~a.lo, ~a.hi}; }
    friend constexpr bool operator==(Mask a, Mask b) { return a.lo == b.lo && a.hi == b.hi; }
    constexpr Mask& operator|=(Mask b) { lo |= b.lo; hi |= b.hi; return *this; }
    constexpr Mask& operator&=(Mask b) { lo &= b.lo; hi &= b.hi; return *this; }
};
inline constexpr Mask Bit(int id) { return id < 0 || id >= 128 ? Mask{} : id < 64 ? Mask{1ull << id, 0} : Mask{0, 1ull << (id - 64)}; }
inline constexpr bool Playable(int id) { return id >= 0 && id < Count && id != Invincible && id != Encore; }  // 3.7.0：绝技连发删除（用户：有秘术冲拳），编号保留不用
// v3（用户 2026-11-12）：海克斯分三档。1 普通、2 稀有、3 传说（notes/balance-v3-proposal-20261112.md 第一部分）
inline constexpr int TierOf(int id) {
    switch (id) {
        case Battle: case KillingIntent: case Thrower: case Scientist: case Investor: case AttributeStack:
        case Arsenal: case Arcane: case BackToBasics: case Transmutation: case ServantMaster: case DivineAid: case TemperMaster:
        case Tank: case Swift: case Icathia: case BreakGrowth: case MedSpark: case ShadowClone: return 3;  // 3.4.0（用户）：药引暗源最稀有，放传说
        case Sharp: case Bell: case Desperate: case GlassCannon:
        case Fury: case Heavy: case Scavenger: case Forge: case Burn: case SnowBall: case Ascetic: case Flawless:
        case Blitz: case Echo: case Ocean: case Interest: case BackWater: case WarFeed: case KillingBoil: case Finisher: case Revenge:
        case Wealth: case Spoils: case SparkFlow: case ShadowEdge: case Thorns:
        case Riposte: case BreakChain: case LastStand: return 2;
        default: return 1;
    }
}
// 每个选项先按海克斯层掷等级（百分比：普通 / 稀有 / 传说）；第 90 层及以后（含无尽）用最后一行
inline constexpr int kTierOdds[4][3] = {{65, 30, 5}, {50, 38, 12}, {40, 42, 18}, {30, 45, 25}};
inline int TierRow(int floor) { return floor >= 90 ? 3 : floor >= 60 ? 2 : floor >= 30 ? 1 : 0; }
// 3.1.0（用户）：百宝囊并入有备而来，编号保留（旧测试档照读、持有按有备而来算），不再进卡池
inline constexpr Mask kAllMask = Count >= 128 ? Mask{~0ull, ~0ull} : Count >= 64 ? Mask{~0ull, Count == 64 ? 0ull : ((1ull << (Count - 64)) - 1)} : Mask{(1ull << Count) - 1, 0};  // 3.7.0：128 位
// 3.3.4（用户：魂玉海克斯效果不好）：财源广进 / 战利品不再进卡池；编号保留，已选到的旧档照读、效果照常
// 3.7.0：玩法类海克斯接入游戏并实机确认接口之前不进卡池（编号先占好，存档兼容）；接好一个就从这里拿掉一个
inline constexpr Mask kNotWiredMask = Bit(Riposte) | Bit(Afterimage) | Bit(Encore) | Bit(Frenzy) | Bit(ShadowClone);  // 2026-11-15 绝境时缓实机确认，放进卡池  // 2026-11-15 破防连锁实机确认，放进卡池
inline constexpr Mask kPlayableMask = kAllMask & ~Bit(Invincible) & ~Bit(Wealth) & ~Bit(Spoils) & ~kNotWiredMask;
inline constexpr int kMaxServants = 3;
#ifdef PBZ_HEX_TEST
// 3.0.0 自动爬塔（用户 2026-11-12）：前 24 层逐个走真实选卡流程——仆从大师 ×3 打头，其后是有联动 / 改流程的传说与稀有；
// 第 25 层起自动爬塔把其余海克斯一次性全部发放（全部同时生效，测兼容）
inline constexpr Id kQaPlan[] = {ServantMaster, ServantMaster, ServantMaster,
                                 Transmutation, Arsenal, Arcane, BackToBasics, DivineAid, AttributeStack, Thrower, Investor, Scientist,
                                 Battle, KillingIntent, Echo, Blitz, Ascetic, Flawless, Interest, SnowBall, Fury, Heavy, GlassCannon, Reaper};
#endif
#ifdef PBZ_HEX_TEST
inline constexpr int kSlots = 26, kSchema = 6, kLegacySchema = 4;
#else
inline constexpr int kSlots = 4, kSchema = 3, kLegacySchema = 1;
#endif
struct Def { std::string_view name, text; int from; };
inline constexpr std::array<Def, Count> defs{{
    {"坦克引擎", "每通关一层，增加生命上限", 1},
    {"缩小引擎", "每通关一层，增加移动速度和攻击速度", 1},
    {"艾卡西亚的陷落", "每通关一层，获得1%伤害减免", 1},
    {"渴血", "生命吸取 +5%", 1},
    {"海洋龙魂", "战斗中每秒恢复 1% 最大生命", 1},
    {"锋芒毕露", "攻击力 +40%，破防 +20%", 1},
    {"金钟罩", "生命上限 +50%，减伤 +15%", 1},
    {"越战越勇", "此后每通关一层，攻击力 +2%", 60},
    {"破阵先锋", "每通关一层，破防 +1%", 60},
    {"破釜沉舟", "攻击力 +80%，破防 +30%，受到伤害 +15%", 1},
    {"磐石之躯", "生命上限 +80%", 1},
    {"快意恩仇", "攻击力 +30%，移动速度 +20%", 1},
    {"锐不可当", "破防 +40%", 1},
    {"铜墙铁壁", "减伤 +25%", 1},
    {"逃跑计划", "本层生命首次降至 20% 或以下时，移动速度 +80%", 1},
    {"狂徒豪气", "本层每次生命下降，减伤 +1%（最多 30%）", 1},
    {"杀意不灭", "倒下时杀气超过上限 80%，立即满血复活并清空杀气；每层一次，优先于复活符", 1},
    {"投掷狂人", "每次普通奖励及额外奖励都可各选两次", 1},
    {"科学狂人", "此后每层随机获得：生命 +100%、杀气上限 +100%、攻击力 +40%、破防 +10%；或移速 +60%、攻速 +60%", 1},
    {"锻造大师", "武器等级上限提升至 60；获得 5 个铸魂令和 20 个淬炼点", 1},
    {"钱生钱", "获得的魂玉翻倍；并且可购买更多道具", 1},
    {"火上浇油", "敌人受伤后灼烧 3 秒，每秒损失 1% 最大生命；之后 5 秒内不再触发", 1},
    {"重量级打手", "获得相当于当前生命上限 15% 的固定武器攻击力", 1},
    {"拾荒者", "此后击败 Boss 掉落武器", 1},
    {"属性叠属性", "立即获得 4 次奖励；驿站开放属性锻造器", 1},
    {"玻璃大炮", "攻击力 +160%，生命上限 -30%", 1},
    {"血之契约", "生命吸取 +5%，生命上限 -10%", 1},
    {"斩首行动", "Boss 受到的伤害 +20%", 1},
    {"横扫千军", "小怪受到的伤害 +50%", 1},
    {"绝境狂怒", "生命低于 30% 时，攻击力 +100%、破防 +30%", 1},
    {"先声夺人", "开战后 8 秒内攻击力 +80%", 1},
    {"定身先机", "开战时敌人定身 2 秒", 1},
    {"收割者", "每击败一个小怪，回复 3% 最大生命", 1},
    {"对冲基金", "每次驿站开张，按持有魂玉的 10% 发放利息", 1},
#ifdef PBZ_HEX_TEST
    {"护命符匠", "每通关 3 层获得 1 个复活符（最多持有 1 个；测试间隔，正式为 15 层）", 1},
#else
    {"护命符匠", "每通关 15 层获得 1 个复活符（最多持有 1 个）", 1},
#endif
    {"武库扩建", "主武器、影之武装各可带 4 把，驿站多上架 2 件武器，武器价格 -30%", 1},
    {"", "", 1},  // 退役编号
    {"积少成多", "每通关一层，获得的魂玉额外 +5%", 1},
    {"天赐良机", "普通奖励页每页至少有 1 次免费刷新", 1},
    {"有备而来", "每次进入驿站，随机获得一个 MOD 道具", 1},
    {"苦修", "不用药通关一层，生命上限永久 +10%", 1},
    {"无伤大师", "无伤通关一层，攻击力永久 +10%", 1},
    {"速战速决", "开战 90 秒内通关本层，额外获得 1 次属性成长", 1},
    {"绝技余韵", "释放绝技后 10 秒内攻击力 +50%；10 秒内再放绝技，重新计时并再叠 +50%", 1},  // 3.0.1：3 秒 → 10 秒（很多绝技本身超过 3 秒）
    {"秘术冲拳", "绝技充能效率更快；武器切换无冷却", 1},
    {"回归基本功", "攻击力 +100%，破防 +80%，伤害减免 +10%，生命吸取 +5%\n无法使用药瓶、绝技与影之武装", 1},
    {"质变", "立即随机获得 2 个海克斯", 1},
    {"仆从大师", "随机召唤 1 名 Boss 助战。", 1},
    {"如有神助", "拥有「回归基本功」时，重新获得药瓶、绝技与影之武装（回归基本功的加成保留）", 1},
    {"背水一战", "生命每低 10%，攻击力 +15%（最多 +145%）", 1},
    {"以战养战", "本层每击杀一个敌人（含小怪和 Boss），攻击力 +15%", 1},
    {"铁壁", "开战后 15 秒内，伤害减免 +40%", 1},
    {"杀意沸腾", "杀气满时，攻击力 +35%、破防 +35%", 1},
    {"淬炼大师", "武器升级效率翻倍：1 个淬炼点升 2 级（不超过等级上限）；回退时按一半返还", 1},
    {"斩草除根", "敌人生命低于 15% 时，受到的伤害翻倍", 1},
    {"以伤换伤", "受到伤害后 3 秒内，下一次命中攻击力 +100%", 1},
    {"聚宝盆", "每层通关时按剩余生命额外获得魂玉（满血 +100%）", 1},
    {"飞身踢", "Boss 生命低于 5% 时，下一次命中直接斩杀", 1},  // 3.6.0（原百宝囊编号）
    {"财源广进", "通关获得的魂玉 +50%", 1},
    {"战利品", "本层每击败一个敌人，通关获得的魂玉 +3%", 1},
    {"药引暗源", "喝药时回复 1 点暗源", 1},
    {"暗源充盈", "击败敌人时有概率回复 1 点暗源", 1},
    {"影刃专精", "影之武装的攻击力 +100%", 1},
    {"反震", "受到伤害时，按减伤前的伤害反震敌人", 1},
    {"以牙还牙", "完美格挡后，下一击伤害 ×1.5 并削减敌人杀气", 1},
    {"残影步", "完美闪避后 3 秒内，攻击额外造成杀气伤害", 1},
    {"", "", 1},  // 3.7.0 绝技连发：删除（编号保留，测试档里可能有，读作退役）
    {"处决狂热", "处决敌人后回复生命与杀气，并短暂提高攻击力", 1},
    {"破防连锁", "敌人被破防时，附近的敌人也会损失杀气", 1},
    {"绝境时缓", "每层生命首次低于 30% 时，敌人放慢 5 秒", 1},
    {"化影随行", "完美闪避时留下一个分身协助作战", 1},
    {"连跳", "空中可以再跳 1 次", 1},
}};
// ---------------------------------------------------------------- 3.6.0 海克斯升级（用户 2026-11-15 定稿：notes/upgrade.txt）
// 已持有、没到最高级的海克斯会再次出现，选中升一级（持有几张就是几级，同仆从大师）；卡面和信息面板都写「名字 LvN」+ 该级效果。
// 最高级：3 级 / 2 级 / 不升级（1）；仆从大师维持最多 3 名、不显示等级
inline constexpr int MaxRank(int id) {
    switch (id) {
        case Tank: case Swift: case Battle: case BreakGrowth: case Ascetic: case Flawless: case Ocean: case Desperate: case Sturdy:  /* 4.0.0 渴血改为不升级 */
        case Keen: case Pursuit: case Bulwark: case GlassCannon: case ShadowEdge: case Heavy: case FlyingKick: case Thorns:
        case Opening: case FirstStrike: case KillingIntent: case SparkFlow: return 3;
        case SnowBall: case Sharp: case Bell: case Decapitate: case Sweep: case Finisher: case Fury: case BackWater: case WarFeed: case IronWall:
        case KillingBoil: case Revenge: case Echo: case Burn: case Reaper: case Grit: case Blitz: case Scientist: case Investor: case Interest:
        case TreasureBowl: case Prepared: case Charmsmith: case MedSpark: case Thrower: return 2;
        case Riposte: case Afterimage: case Frenzy: case BreakChain: case LastStand: case ShadowClone: case MultiJump: return 2;
        case BloodPact: return 2;  // 4.0.0（用户）：血之契约 Lv1 / Lv2  // 3.7.0 玩法类最高 Lv2  // 3.6.0 投掷狂人 Lv2（用户 2026-11-15）
        case ServantMaster: return 3;
        default: return 1;
    }
}
inline constexpr int kLegacyMaxRank = 3;  // 历史上任何海克斯的最高级（老档校验用）
inline constexpr bool Upgradable(int id) { return MaxRank(id) > 1; }
// 3.7.0 连跳：Lv1 空中再跳 1 次（共 2 段），Lv2 空中再跳 3 次（共 4 段；用户 2026-11-15）
inline constexpr int MultiJumpAirJumps(int rank) { return rank <= 0 ? 0 : rank == 1 ? 1 : 3; }  // 3.6.0（用户 2026-11-15）：仆从大师也按 Lv1 / 2 / 3 显示
struct LevelText { int id; std::string_view text[3]; };
inline constexpr LevelText kLevelText[] = {
    {Tank, {"每通关一层，生命上限 +8%", "每通关一层，生命上限 +12%", "每通关一层，生命上限 +16%"}},
    {Swift, {"每通关一层，移动速度、攻击速度 +1%", "每通关一层，移动速度、攻击速度 +1.5%", "每通关一层，移动速度、攻击速度 +2%"}},
    {Battle, {"此后每通关一层，攻击力 +2%", "此后每通关一层，攻击力 +4%", "此后每通关一层，攻击力 +8%"}},
    {BreakGrowth, {"每通关一层，破防 +1%", "每通关一层，破防 +2%", "每通关一层，破防 +3%"}},
    {Ascetic, {"不用药通关一层，生命上限永久 +10%", "不用药通关一层，生命上限永久 +15%", "不用药通关一层，生命上限永久 +20%"}},
    {Flawless, {"无伤通关一层，攻击力永久 +10%", "无伤通关一层，攻击力永久 +20%", "无伤通关一层，攻击力永久 +30%"}},
    {SnowBall, {"每通关一层，获得的魂玉额外 +5%", "每通关一层，获得的魂玉额外 +10%", ""}},
    {Ocean, {"战斗中每秒恢复 1% 最大生命", "战斗中每秒恢复 1.5% 最大生命", "战斗中每秒恢复 2% 最大生命"}},
    {Sharp, {"攻击力 +40%，破防 +20%", "攻击力 +80%，破防 +40%", ""}},
    {Bell, {"生命上限 +50%，减伤 +15%", "生命上限 +100%，减伤 +25%", ""}},
    {Desperate, {"攻击力 +80%，破防 +30%，受到伤害 +15%", "攻击力 +110%，破防 +40%，受到伤害 +15%", "攻击力 +140%，破防 +50%，受到伤害 +15%"}},
    {Sturdy, {"生命上限 +80%", "生命上限 +120%", "生命上限 +160%"}},
    {Keen, {"攻击力 +30%，移动速度 +20%", "攻击力 +45%，移动速度 +30%", "攻击力 +60%，移动速度 +40%"}},
    {Pursuit, {"破防 +40%", "破防 +60%", "破防 +80%"}},
    {Bulwark, {"减伤 +25%", "减伤 +35%", "减伤 +45%"}},
    {GlassCannon, {"攻击力 +160%，生命上限 -30%", "攻击力 +220%，生命上限 -30%", "攻击力 +280%，生命上限 -30%"}},
    {BloodPact, {"生命吸取 +5%，生命上限 -10%", "生命吸取 +10%，生命上限 -10%", ""}},
    {ShadowEdge, {"影之武装的攻击力 +100%", "影之武装的攻击力 +150%", "影之武装的攻击力 +200%"}},
    {Heavy, {"获得相当于当前生命上限 15% 的固定武器攻击力", "获得相当于当前生命上限 22% 的固定武器攻击力", "获得相当于当前生命上限 30% 的固定武器攻击力"}},
    {FlyingKick, {"Boss 生命低于 5% 时，下一次命中直接斩杀", "Boss 生命低于 10% 时，下一次命中直接斩杀", "Boss 生命低于 15% 时，下一次命中直接斩杀"}},
    {Thorns, {"受到伤害时，按减伤前的伤害反震敌人", "受到伤害时，按减伤前的伤害反震敌人（加强）", "受到伤害时，按减伤前的伤害反震敌人（大幅加强）"}},
    {Riposte, {"完美格挡后，下一击伤害 ×1.5 并削减敌人杀气", "完美格挡后，下一击伤害 ×2 并削减更多敌人杀气", ""}},
    {Afterimage, {"完美闪避后 3 秒内，攻击额外造成杀气伤害", "完美闪避后 5 秒内，攻击额外造成杀气伤害", ""}},
    {Frenzy, {"处决敌人后回复生命与杀气，并短暂提高攻击力", "处决敌人后回复更多生命与杀气，并短暂提高攻击力", ""}},
    {BreakChain, {"敌人被破防时，附近的敌人也会损失杀气", "敌人被破防时，更大范围的敌人损失更多杀气", ""}},
    {LastStand, {"每层生命首次低于 30% 时，敌人放慢 5 秒", "每层生命首次低于 30% 时，敌人放慢 10 秒", ""}},
    {ShadowClone, {"完美闪避时留下一个分身协助作战", "完美闪避时留下一个更强、更持久的分身协助作战", ""}},
    {MultiJump, {"空中可以再跳 1 次", "空中可以再跳 3 次", ""}},
    {Decapitate, {"Boss 受到的伤害 +20%", "Boss 受到的伤害 +50%", ""}},
    {Sweep, {"小怪受到的伤害 +50%", "小怪受到的伤害 +100%", ""}},
    {Finisher, {"敌人生命低于 15% 时，受到的伤害翻倍", "敌人生命低于 25% 时，受到的伤害翻倍", ""}},
    {Fury, {"生命低于 30% 时，攻击力 +100%、破防 +30%", "生命低于 30% 时，攻击力 +180%、破防 +50%", ""}},
    {BackWater, {"生命每低 10%，攻击力 +15%（最多 +145%）", "生命每低 10%，攻击力 +25%（最多 +245%）", ""}},
    {WarFeed, {"本层每击杀一个敌人（含小怪和 Boss），攻击力 +15%", "本层每击杀一个敌人（含小怪和 Boss），攻击力 +20%", ""}},
    {Opening, {"开战后 8 秒内攻击力 +80%", "开战后 12 秒内攻击力 +80%", "开战后 16 秒内攻击力 +80%"}},
    {FirstStrike, {"开战时敌人定身 2 秒", "开战时敌人定身 3 秒", "开战时敌人定身 4 秒"}},
    {IronWall, {"开战后 15 秒内，伤害减免 +40%", "开战后 20 秒内，伤害减免 +40%", ""}},
    {KillingBoil, {"杀气满时，攻击力 +35%、破防 +35%", "杀气满时，攻击力 +65%、破防 +65%", ""}},
    {Revenge, {"受到伤害后 3 秒内，下一次命中攻击力 +100%", "受到伤害后 3 秒内，下一次命中攻击力 +150%", ""}},
    {Echo, {"释放绝技后 10 秒内攻击力 +50%；10 秒内再放绝技，重新计时并再叠 +50%", "释放绝技后 10 秒内攻击力 +75%；10 秒内再放绝技，重新计时并再叠 +75%", ""}},
    {Burn, {"敌人受伤后灼烧 3 秒，每秒损失 1% 最大生命；之后 5 秒内不再触发", "敌人受伤后灼烧 3 秒，每秒损失 1.5% 最大生命；之后 5 秒内不再触发", ""}},
    {Reaper, {"每击败一个小怪，回复 3% 最大生命", "每击败一个小怪，回复 6% 最大生命", ""}},
    {Grit, {"本层每次生命下降，减伤 +1%（最多 30%）", "本层每次生命下降，减伤 +1%（最多 40%）", ""}},
    {KillingIntent, {"倒下时杀气超过上限 80%，立即满血复活并清空杀气；每层一次，优先于复活符", "倒下时杀气超过上限 70%，立即满血复活并清空杀气；每层一次，优先于复活符",
                     "倒下时杀气超过上限 60%，立即满血复活并清空杀气；每层一次，优先于复活符"}},
    {Blitz, {"开战 90 秒内通关本层，额外获得 1 次属性成长", "开战 120 秒内通关本层，额外获得 2 次属性成长", ""}},
    {Thrower, {"每次普通奖励及额外奖励都可各选两次", "每次普通奖励及额外奖励都可各选三次", ""}},
    {ServantMaster, {"随机召唤 1 名 Boss 助战", "随机召唤 2 名 Boss 助战", "随机召唤 3 名 Boss 助战"}},
    {Scientist, {"此后每层随机获得：生命 +100%、杀气上限 +100%、攻击力 +40%、破防 +10%；或移速 +60%、攻速 +60%",
                 "此后每层随机获得：生命 +125%、杀气上限 +125%、攻击力 +50%、破防 +12.5%；或移速 +75%、攻速 +75%", ""}},
    {Investor, {"获得的魂玉翻倍；并且可购买更多道具", "获得的魂玉 ×2.5；并且可购买更多道具", ""}},
    {Interest, {"每次驿站开张，按持有魂玉的 10% 发放利息", "每次驿站开张，按持有魂玉的 15% 发放利息", ""}},
    {TreasureBowl, {"每层通关时按剩余生命额外获得魂玉（满血 +100%）", "每层通关时按剩余生命额外获得魂玉（满血 +200%）", ""}},
    {Prepared, {"每次进入驿站，随机获得一个 MOD 道具", "每次进入驿站，随机获得两个 MOD 道具", ""}},
#ifndef PBZ_HEX_TEST
    {Charmsmith, {"每通关 15 层获得 1 个复活符（最多持有 1 个）", "每通关 10 层获得 1 个复活符（最多持有 1 个）", ""}},
#endif
    {MedSpark, {"喝药时回复 1 点暗源", "喝药时回复 2 点暗源", ""}},
    {SparkFlow, {"击败敌人时有 25% 概率回复 1 点暗源", "击败敌人时有 50% 概率回复 1 点暗源", "击败敌人时有 75% 概率回复 1 点暗源"}},
};
// 第 lv 级（1 起）的效果说明；没有分级说明的用 defs 原文
inline std::string_view TextAt(int id, int lv) {
    if (id < 0 || id >= Count) return {};
    if (Upgradable(id)) for (const auto& t : kLevelText) if (t.id == id) {
        const int i = std::clamp(lv, 1, MaxRank(id)) - 1;
        if (!t.text[i].empty()) return t.text[i];
    }
    return defs[id].text;
}
// 可升级的海克斯显示「名字 LvN」；其余照旧
inline std::string NameAt(int id, int lv) {
    if (id < 0 || id >= Count) return {};
    if (!Upgradable(id)) return std::string(defs[id].name);
    return std::string(defs[id].name) + " Lv" + std::to_string(std::clamp(lv, 1, MaxRank(id)));
}
// 3.4.5（用户）：坦克引擎每层生命 +3% → +8%；缩小引擎每层移速 +1%、攻速 +1%（缩小引擎自身带来的各最多 +100%，其它来源照常叠加，总量不封顶）
inline constexpr double kTankLife = .08, kSwiftStep = .01, kSwiftCap = 1.0;
inline constexpr int kForgePrice = 1500;  // 3.3.3（用户）：锻造 2000 → 1500 魂玉；3.4.5 起改按层数分段（ForgePrice），本常量只留给旧测试对照
// 3.4.5（用户）：锻造价格按显示层分段——100 层前 300，100–149 层 600，150–199 层 900，之后每 50 层再 +300
// 4.0.1（用户 2026-11-16）：500 / 1000 / 1500 / 2000 / 2500（100 层前 / 100–149 / 150–199 / 200–249 / 250–299），之后每 50 层再 +500
inline constexpr int kForgeStep = 500;
inline int ForgePrice(int floor, int floors) {
    const int shown = floors >= 50 ? (floor > 50 ? floor + 49 : 2 * std::max(1, floor) - 1) : std::max(1, floor);
    return shown < 100 ? kForgeStep : kForgeStep + kForgeStep * ((shown - 100) / 50 + 1);
}
inline constexpr double kWealthJade = .50, kSpoilsJade = .03;  // 3.3.3 财源广进 / 战利品
inline constexpr int kSparkFlowPct = 25;                        // 3.3.4 暗源充盈：每击败一个敌人 25% 回复 1 暗源
// 3.6.0 反震（用户同意默认）：原始伤害（掉血 ÷ 影塔受伤倍率）占玩家生命上限的比例 × 3% × 目标最大生命；不设单次上限（用户 2026-11-15）；
// 目标 = 锁定的敌人，没锁定打 15 米内最近的敌人；不穿锁血线
inline constexpr double kThornsShare = .18, kThornsCap = 1.0, kThornsRange = 1500.0;  // 3.6.0 实机后（用户 2026-11-15）：3% → 18%（一管血反弹约 15–20% 敌方生命），取消单次上限（仍不穿锁血线）
inline double ThornsDamage(double rawTaken, double playerMax, double targetMax, double share = kThornsShare) {
    if (!(rawTaken > 0) || !(playerMax > 1) || !(targetMax > 0) || !std::isfinite(rawTaken) || !std::isfinite(playerMax) || !std::isfinite(targetMax)) return 0;
    return std::min(share * rawTaken / playerMax, kThornsCap) * targetMax;
}
// 3.6.0 飞身踢（用户 2026-11-15，普通档）：Boss 当前生命 < 自身最大生命 5% 时，玩家下一次命中直接打到 0（遇原生锁血线只打到锁血线）。
// 升级（Lv2 10% / Lv3 15%）待升级方案定稿（notes/tower-3.6.0-hex-upgrade-plan.md）
inline constexpr double kFlyingKickLine = .05;
inline bool KickLine(double bossHp, double bossMax, double line = kFlyingKickLine) { return bossMax > 1 && bossHp > 0.5 && bossHp < line * bossMax; }
// 3.6.2：踢完留多少血——没有锁血线留 1 点，有锁血线（转阶段 / 处决）留在锁血线上方 1 点；绝不写 0（写 0 绕过原生伤害结算，Boss 永远不死）
inline float KickLeave(float healthMin) { return (healthMin > 1.0f ? healthMin : 0.0f) + 1.0f; }
inline constexpr double kShadowEdgeAttack = 1.0;                // 3.3.4 影刃专精：影之武装额外攻击力加成 +100%（不进面板、不加主武器）
inline constexpr double kBasicsAttack = 1.0, kBasicsBreak = .80;
inline constexpr int kBasicsDefense = 100, kBasicsLeech = 50, kTransmuteCount = 2;
// E 批数值（集中在这里，方便调）
inline constexpr double kGlassAttack = 1.60, kGlassHealth = -.30, kPactHealth = -.10, kFuryLine = .30, kFuryAttack = 1.00, kFuryBreak = .30,
                        kOpeningSeconds = 8, kOpeningAttack = .80, kFirstStrikeSeconds = 2, kReaperHeal = .03, kDecapitate = 1.20, kSweep = 1.50,
                        kInterestRate = .10;  // 4.0.0（用户：太强）：Lv1 10% / Lv2 15%，不设上限
inline constexpr int kPactLeech = 50, kInterestCap = 1000;
inline constexpr int kArsenalWeaponPricePct = 70;  // 武库扩建：驿站兵器七折
// F 批数值
inline constexpr double kSnowBallStep = .05 /* 用户 2026-11-13：每层 +5%、不封顶 */, kSnowBallCap = 5.0,  /* 3.1.0：上限 +100% → +500%（用户） */ kAsceticStep = .10 /* 3.6.0（用户）：.06 → .10 */, kFlawlessStep = .10, kEchoAttack = .50, kEchoSeconds = 10, kBlitzSeconds = 90;  // 3.1.0：速战速决 120 → 60 → 90 秒（用户 2026-11-13）
inline constexpr float kArcaneChargeMul = 6.0f;  // 秘术冲拳：原生绝技充能效率 (1+v)×6，即 +500%（用户 2026-11-11：3 倍偏慢）
inline constexpr int kBlitzAttrPicks = 1;
// 3.1.0 新海克斯数值
inline constexpr double kBackWaterStep = .15, kBackWaterCap = 1.45, kWarFeedStep = .15,  /* 3.1.0（用户）：以战养战只算本层、不设上限；3.2.1（用户）：每个 +3% → +15% */ kIronWallSeconds = 15, kBoilAttack = .35, kBoilBreak = .35,
                        kFinisherLine = .15, kFinisherTaken = 2.0, kRevengeSeconds = 3, kRevengeAttack = 1.0, kTreasureBowl = 1.0;  /* 3.5.0（用户）：聚宝盆满血 +50% → +100%，按剩余生命比例 */
// 背水一战：已损失生命每满 10% 一级，每级攻击 +15%，最多 +145%（hpFrac = 当前 / 上限；读不到时传负数 = 0）
inline double BackWaterOn(double hpFrac, double step = kBackWaterStep, double cap = kBackWaterCap) {
    if (!(hpFrac >= 0)) return 0;
    const int steps = std::clamp(static_cast<int>((1.0 - std::min(1.0, hpFrac)) * 10.0 + 1e-9), 0, 10);
    return std::min(cap, step * steps);
}
inline constexpr int kIronWallDefense = 400;  // 3.1.0（用户：太强）：不再魂玉 ×2；只给 1 次属性成长，档位按当前层
#ifdef PBZ_HEX_TEST
inline constexpr int kCharmEvery = 3;
#else
inline constexpr int kCharmEvery = 15;
#endif
// 对冲基金：持有魂玉的 10%，向下取整，单次最多 1000
// v3：单次上限随层涨（2 × J(s)）；cap ≤ 0 时用旧上限 1000
// 3.1.0（用户 2026-11-13：1 万多魂玉只显示 +450）：对冲基金 = 持有魂玉的 10%，不再封顶（cap 参数保留只为调用处不变）
inline int InterestOn(int jade, int cap = 0, double rate = kInterestRate) { (void)cap; return jade <= 0 ? 0 : static_cast<int>(jade * rate); }
inline int Milestone(int floor, [[maybe_unused]] bool acceptLegacyQa = false) {
#ifdef PBZ_HEX_TEST
    // 用户 2026-11-11：QA 只测 E 批，一层一个（第 1–10 层），之后不再弹海克斯，避免旧海克斯干扰其他效果。
    // 栏位编号仍按「层 - 1」，存档格式（26 格）不变
    const int count = static_cast<int>(std::size(kQaPlan));
    if (floor >= 1 && floor <= count) return floor - 1;
    return -1;
#else
    switch (floor) { case 1: return 0; case 30: return 1; case 60: return 2; case 90: return 3; default: return -1; }
#endif
}
// 无尽模式（用户 2026-11-12）：前 100 层仍是第 1 / 30 / 60 / 90 层（质变再给 1 个，最多 5 个）；之后显示第 120、150、180 … 层各选一个，
// 选到池子空为止（剩 2 / 1 个时只给 2 / 1 张，不刷新）。这些层的选择存在 extra（不占固定栏位）
// 3.0.1 玩家测试包（正式版 + /DPBZ_PLAYTEST，只给用户自测，不进发布包）：第 1 层第一张固定火上浇油；
// 第 2 层多给一次选择（走无尽层的额外栏位），三张固定为待复测的海克斯
// 3.0.1（用户 2026-11-12：40 多个海克斯 30 层一个要打上千层）：显示第 1 层，之后到 150 层每 15 层一个（15 / 30 / 45 … 150），
// 150 层后每 20 层一个（170 / 190 …），选到池子空为止。第 1 / 30 / 60 / 90 层仍走固定栏位（旧档兼容），其余层走额外栏位 extra
inline bool ExtraHexFloor(int floor) {
#ifdef PBZ_HEX_TEST
    return floor >= 120 && (floor - 120) % 30 == 0;  // QA 构建前 24 层逐个测，之后沿用旧无尽层规则（自动爬塔脚本按这个走）
#endif
    if (floor <= 0 || Milestone(floor) >= 0) return false;
    return floor % 5 == 0;  // 3.6.0（用户）：每 5 层一个（1 / 5 / 10 / 15 …）；1 / 30 / 60 / 90 仍走固定栏位
}
inline bool HexFloor(int floor) { return floor == 1 || (floor > 0 && floor % 5 == 0); }
// 3.4.0 旧档兼容：待选 / 已选的额外栏位层可以是旧规则的层（15 / 45 / 170 …），只要不是固定栏位层就按额外栏位处理（存档不因规则变化读不进来）
inline bool ExtraSlotFloor(int floor) { return floor > 0 && Milestone(floor) < 0; }
#ifdef PBZ_PLAYTEST
// 用户 2026-11-12 复核：第 1 层第一张绝技余韵，第 2 层第一张狂徒豪气（逐一复测）
inline constexpr Id kPlaytestFirst = Echo;
inline constexpr Id kPlaytestSecond[] = {Grit, Burn, KillingIntent};
inline bool EndlessMilestone(int floor) { return floor == 2 || ExtraHexFloor(floor); }
#else
inline bool EndlessMilestone(int floor) { return ExtraHexFloor(floor); }  // 名字沿用（原来只有无尽层用额外栏位）
#endif
inline int SlotFloor(int slot) {
#ifdef PBZ_HEX_TEST
    return slot < 24 ? slot + 1 : slot + 7;
#else
    return std::array{1, 30, 60, 90}[slot];
#endif
}
inline bool Unlocked(int id, int floor) {
    if (!Playable(id)) return false;
#ifdef PBZ_HEX_TEST
    return floor >= 1;
#else
    return floor >= defs[id].from;
#endif
}
// A weapon unlocked before Scavenger stays in the shop. A later first unlock
// is routed to the player; unencountered fallback shop items are unchanged.
inline bool ScavengerShopAllowed(int selectedAt, int bossFloor, bool unlockedEarlier) {
    return selectedAt <= 0 || bossFloor < selectedAt || unlockedEarlier;
}
// 普通奖励与海克斯是不同来源：各来源内部先累计，再相乘。
inline double Scale(double normalMultiplier, double hexBonus) { return normalMultiplier * (1.0 + hexBonus); }
// 用户 2026-11-11：生命上限加成按加法算（奖励属性 % + 海克斯 %），玻璃大炮的 -30% 之后可以靠属性 / 海克斯加回来；最低 10%
inline double LifeScale(double normalMultiplier, double hexBonus) { return std::max(0.1, normalMultiplier + hexBonus); }
// 用户 2026-11-11：影塔所有 MOD 减伤合计最多 80%（乘法叠加，作用在原生减伤之后的受伤倍率上）
inline constexpr double kMinTakenFactor = 0.2;
// 用户 2026-11-13：有艾卡西亚的陷落时影塔减伤合计上限 90%（受伤最少 10%），否则 80%（不写进说明）
inline constexpr double kMinTakenIcathia = 0.1;
// 3.6.0（用户 2026-11-14）：减伤改为相加（奖励属性 + 海克斯 + 铁甲符），封顶 1 − floor；穿透另算（tower_balance.h TakenFactor）
inline double Reduction(int normalPermille, int hexPermille, bool guard = false) { return normalPermille / 1000.0 + hexPermille / 1000.0 + (guard ? 0.2 : 0.0); }
inline double DamageFactor(int normalPermille, int hexPermille, double floor = kMinTakenFactor) {
    return balance::TakenFactor(Reduction(normalPermille, hexPermille), 1.0 - floor, 0.0);
}
inline bool ReductionCapped(int normalPermille, int hexPermille, double floor = kMinTakenFactor) { return Reduction(normalPermille, hexPermille) >= 1.0 - floor - 1e-9; }
struct State {
    bool enabled = false;
    uint64_t rng = 0, clearedLow = 0, clearedHigh = 0;
    Mask seen{};
    // 3.6.2（用户：选了 Lv1 后老刷 Lv2；刷掉的又刷回来）：seenUp = 这一级的升级卡已经出现过（升级后清掉，下一级重新算新卡）；
    // away = 当前这一页被刷新掉的卡（这一页里不再出现，除非池子真的空了）。由成长档 gr.hexseen 保存
    Mask seenUp{}, away{};
    // 4.1.0（用户 2026-11-16：没选的卡每 5 层又出现，挤占别的卡）：海克斯冷却，按「选择次数」算（主线每 5 层一次）。
    // 一次 3 选 1 里出现过、没选的卡（含刷新出来的）后面 3 次选择不出现（第 10 层出现没选，最早第 30 层才可能再出现，用户 2026-11-16 由 2 次改 3 次）；
    // 被刷新掉的卡后面 4 次不出现；冷却结束按原权重回到卡池（不是必出）。选中的那类不冷却（下一级照常以较低权重进池）。
    // 池子不够 3 张时才放冷却中的卡。由成长档 gr.hexcool 保存
    static constexpr int kCoolShown = 3, kCoolRerolled = 4;
    std::array<uint8_t, Count> cool{};
    bool Cooling(int id) const { return id >= 0 && id < Count && cool[static_cast<size_t>(id)] > 0; }
    void SettleCooldown(int chosen) {
        for (auto& c : cool) if (c) --c;
        for (int id = 0; id < Count; ++id) {
            auto& c = cool[static_cast<size_t>(id)];
            if (id == chosen) { c = 0; continue; }
            if (away & Bit(id)) c = kCoolRerolled;
            else if (std::find(offers.begin(), offers.end(), id) != offers.end()) c = static_cast<uint8_t>(std::max<int>(c, kCoolShown));
        }
    }
    bool Fresh(int id) const { return id == ServantMaster || (CanUpgrade(static_cast<Id>(id)) ? !(seenUp & (Bit(id))) : !(seen & (Bit(id)))); }
    void MarkShown(int id) { if (id < 0 || id >= Count) return; if (Has(static_cast<Id>(id))) seenUp |= Bit(id); else seen |= Bit(id); }
    unsigned completed = 0;
    int pending = 0;
    unsigned rerolled = 0; // One bit per card; each card can refresh once.
    bool escapeActive = false; // Per-floor transient state, never persisted.
    bool furyActive = false, openingActive = false;  // E 批：每帧按当前生命 / 开战时间判定，不存档
    bool echoActive = false;                        // 绝技余韵：每帧判定，不存档
    int echoStacks = 0; double echoLeft = 0;        // 绝技余韵层数 / 剩余秒数（不存档）
    int asceticStacks = 0, flawlessStacks = 0;      // 苦修 / 无伤大师层数：由成长档 gr.hexcnt 保存（不进本结构的存档格式）
    int gritStacks = 0;
    double liveAttack = -1, liveAtkMul = 1, liveTakenMul = 1, liveTakenNative = -1;
    // 3.1.0 新海克斯的每帧状态（引擎写，不存档）
    double backWaterBonus = 0; int warStacks = 0; bool ironActive = false, boilActive = false, revengeActive = false;  // 3.0.1：引擎每帧写入的原生读回（详情页显示，不存档）
    double gritLastHealth = -1, gritLastDrop = -1;
    std::array<int, 3> offers{-1, -1, -1};
    std::array<int, kSlots> picks{}, pickedAt{};
    std::vector<int> extra, extraAt, extraBase;  // 无尽层选的海克斯 / 所在显示层 / 选时已通过的无尽层数（叠层类按此起算）
    int endlessCleared = 0;                      // 首次通过的显示第 101 层以后的层数（叠层类海克斯在无尽层继续叠）
    State() { picks.fill(-1); }
    bool EndlessDone(int floor) const { return std::find(extraAt.begin(), extraAt.end(), floor) != extraAt.end(); }
    // 3.1.0（用户：3.0.0 存档继续）：海克斯层改成每 15 / 20 层后，旧局已经走过的新海克斯层（15、45、75 …）没选过。
    // 返回下一层之前最早一个没选的额外海克斯层（0 = 没有），开下一层前先补发它的选择页
    // 3.4.0（用户：改每 10 层一个，旧档按层数和已选数量补发）：按「数量」补——下一层之前应得的海克斯次数（第 1 层 + 每 10 层）
    // 减去已经选过的次数（固定栏位已完成的 + 额外栏位按层选的；质变 / 测试赠送的 extraAt < 0 不算），差几个补几个；
    // 每次补发记在最早一个还没用过的新海克斯层上（旧版的 15 / 45 层选择照常算数，不重复补）
    int DueHexes(int nextShown) const { int n = 0; for (int f = 1; f < nextShown; ++f) n += HexFloor(f); return n; }
    int HadHexes() const {
        int n = 0;
        for (int m = 0; m < kSlots; ++m) n += (completed >> m) & 1u;
        for (int at : extraAt) n += at > 0;
        return n;
    }
    // 3.6.0（用户 2026-11-15）：每 5 层一个后，前 100 层的局按当前层补齐；已在无尽层（下一层显示 ≥ 100）的存档只多补 5 个——
    // 应得 = 旧规则（第 1 层 + 每 10 层）的次数 + 5，再不超过新规则的次数。按新规则打上来的无尽局已选数早就超过它，不会再补
    static constexpr int kEndlessBackfill = 5;
    int OwedHexes(int nextShown) const {
        const int due = DueHexes(nextShown);
        if (nextShown < 100) return due;  // 显示第 100 层起就是无尽层（存档点 / 刚进无尽的档也按无尽补）
        int old = 0; for (int f = 1; f < nextShown; ++f) old += f == 1 || f % 10 == 0;
        return std::min(due, old + kEndlessBackfill);
    }
    int MissedExtra(int nextShown) const {
        if (!enabled || HadHexes() >= OwedHexes(nextShown)) return 0;
        for (int f = 1; f < nextShown; ++f) if (EndlessMilestone(f) && !EndlessDone(f)) return f;
        return 0;
    }

    void Init(uint64_t seed) { *this = {}; enabled = true; rng = (seed ^ 0xF1BBCDCBFA53E0A9ull) | 1ull; }
    int PickCount(Id id) const { return Playable(id) ? static_cast<int>(std::count(picks.begin(), picks.end(), id) + std::count(extra.begin(), extra.end(), id)) : 0; }
    bool Has(Id id) const { return Playable(id) && (std::find(picks.begin(), picks.end(), static_cast<int>(id)) != picks.end() || std::find(extra.begin(), extra.end(), static_cast<int>(id)) != extra.end()); }
    // 3.6.0 海克斯升级：持有几张就是几级（封顶 MaxRank）；没持有 = 0
    int Rank(Id id) const { return Has(id) ? std::clamp(PickCount(id), 1, MaxRank(id)) : 0; }
    // 卡面上这张卡选中后会是几级（没持有 = Lv1，已持有 = 当前级 + 1）
    int OfferLevel(int id) const { return Playable(id) ? std::clamp(PickCount(static_cast<Id>(id)) + 1, 1, MaxRank(id)) : 1; }
    bool CanUpgrade(Id id) const { return Upgradable(id) && Has(id) && PickCount(id) < MaxRank(id); }
    // 按当前等级取值：Lv1 / Lv2 / Lv3（没给 Lv3 的按 Lv2）；没持有时返回 Lv1 的值（调用处自己判 Has）
    double At(Id id, double l1, double l2, double l3 = -1) const { const int r = Rank(id); return r >= 3 && l3 >= 0 ? l3 : r >= 2 ? l2 : l1; }
    // ---- 各海克斯按等级的数值（notes/upgrade.txt）
    double TankStep() const { return At(Tank, kTankLife, .12, .16); }
    double SwiftStep() const { return At(Swift, kSwiftStep, .015, .02); }
    double BattleStep() const { return At(Battle, .02, .04, .08); }
    double BreakStep() const { return At(BreakGrowth, .01, .02, .03); }
    double AsceticStep() const { return At(Ascetic, kAsceticStep, .15, .20); }
    double FlawlessStep() const { return At(Flawless, kFlawlessStep, .20, .30); }
    double SnowStep() const { return At(SnowBall, kSnowBallStep, .10); }
    double OceanRate() const { return At(Ocean, .01, .015, .02); }
    double FinisherLine() const { return At(Finisher, kFinisherLine, .25); }
    double KickRate() const { return At(FlyingKick, kFlyingKickLine, .10, .15); }
    double ThornsShare() const { return At(Thorns, kThornsShare, .22, .24); }
    double ShadowEdgeBonus() const { return At(ShadowEdge, kShadowEdgeAttack, 1.5, 2.0); }
    double HeavyShare() const { return At(Heavy, .15, .22, .30); }
    double IronSeconds() const { return At(IronWall, kIronWallSeconds, 20); }
    double OpeningSeconds() const { return At(Opening, kOpeningSeconds, 12, 16); }
    double FirstStrikeSeconds() const { return At(FirstStrike, kFirstStrikeSeconds, 3, 4); }
    double InvestorMul() const { return Has(Investor) ? At(Investor, 2.0, 2.5) : 1.0; }
    double BowlRate() const { return At(TreasureBowl, kTreasureBowl, 2.0); }
    double BlitzSeconds() const { return At(Blitz, kBlitzSeconds, 120); }
    int BlitzPicks() const { return Rank(Blitz) >= 2 ? 2 : kBlitzAttrPicks; }
    int ThrowerMul() const { return 1 + Rank(Thrower); }        // 投掷狂人：没有 1、Lv1 每页选 2 次、Lv2 选 3 次
    int ThrowerExtraPages() const { return Rank(Thrower); }     // 每页之后还能再选几次
    double ReaperHeal() const { return At(Reaper, kReaperHeal, .06); }
    double InterestRate() const { return At(Interest, kInterestRate, .15); }
    int PreparedItems() const { return Rank(Prepared) >= 2 ? 2 : 1; }
    int CharmEvery() const {
#ifdef PBZ_HEX_TEST
        return kCharmEvery;
#else
        return Rank(Charmsmith) >= 2 ? 10 : kCharmEvery;
#endif
    }
    int MedSparkPoints() const { return Rank(MedSpark) >= 2 ? 2 : 1; }
    int SparkFlowPct() const { return static_cast<int>(At(SparkFlow, kSparkFlowPct, 50, 75)); }
    double BurnRate() const { return At(Burn, .01, .015); }
    double WarFeedStep() const { return At(WarFeed, kWarFeedStep, .20); }
    double EchoStep() const { return At(Echo, kEchoAttack, .75); }
    double RevengeAttack() const { return At(Revenge, kRevengeAttack, 1.5); }
    double BoilBonus() const { return At(KillingBoil, kBoilAttack, .65); }
    int GritCap() const { return Rank(Grit) >= 2 ? 40 : kGritCap; }
    double ReviveLine() const { return At(KillingIntent, .80, .70, .60); }
    double ScientistScale() const { return At(Scientist, 1.0, 1.25); }
    double BackWaterStep() const { return At(BackWater, kBackWaterStep, .25); }
    double BackWaterCap() const { return At(BackWater, kBackWaterCap, 2.45); }
    double DecapitateMul() const { return At(Decapitate, kDecapitate, 1.50); }
    double SweepMul() const { return At(Sweep, kSweep, 2.00); }
    int PickedFloor(Id id) const {  // 3.6.0：升级后同一海克斯有多张，取最早拿到的那层
        int best = 0;
        for (size_t i = 0; i < picks.size(); ++i) if (picks[i] == id && pickedAt[i] > 0 && (!best || pickedAt[i] < best)) best = pickedAt[i];
        for (size_t i = 0; i < extra.size(); ++i) if (extra[i] == id) { const int at = std::abs(extraAt[i]); if (at > 0 && (!best || at < best)) best = at; }  // 负数 = 质变给的（记在质变那层）
        return best;
    }
    bool CanPick(Id id, int /*floor*/) const {
        if (!Playable(id)) return false;
        if (id == DivineAid) return Has(BackToBasics) && !Has(id);  // 必须先有回归基本功
        if (id == ServantMaster) return PickCount(id) < kMaxServants;
        return !Has(id) || CanUpgrade(id);  // 3.6.0：已持有、没到最高级的可以再选（升级）
    }
    uint64_t Next() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
    bool WasCleared(int floor) const {
        if (floor < 1 || floor > 100) return false;
        return floor <= 64 ? (clearedLow & (1ull << (floor - 1))) != 0 : (clearedHigh & (1ull << (floor - 65))) != 0;
    }
    void ClearFloor(int floor) {
        if (enabled && floor > 100) { ++endlessCleared; return; }
        if (!enabled || floor < 1 || floor > 100) return;
        if (floor <= 64) clearedLow |= 1ull << (floor - 1);
        else clearedHigh |= 1ull << (floor - 65);
    }
    // 叠层数：从第一次拿到起算（3.6.0 升级后有多张，取最多的那张 = 最早拿到的）；数值按当前等级的步长 × 层数重新算
    int Levels(Id id) const {
        int best = 0;
        for (size_t i = 0; i < picks.size(); ++i) if (picks[i] == id) {
            int n = 0; for (int f = pickedAt[i]; f <= 100; ++f) if (WasCleared(f)) ++n;
            best = std::max(best, n + endlessCleared);
        }
        // 额外栏位：从拿到那层起，前 100 层里已通过的层 + 之后无尽层通过数（扣掉拿到时已有的）
        for (size_t i = 0; i < extra.size(); ++i) if (extra[i] == id) {
            int n = 0; for (int f = std::abs(extraAt[i]); f <= 100; ++f) if (WasCleared(f)) ++n;
            best = std::max(best, n + std::max(0, endlessCleared - extraBase[i]));
        }
        return best;
    }
    bool Draw(int floor, bool refresh, Mask available = kPlayableMask) {
        const uint64_t oldRng = rng;
        std::array<int, 3> next{-1, -1, -1};
        if (refresh) { for (int id : offers) if (id >= 0 && id < Count) away |= Bit(id); }  // 4.1.0：整页刷新 = 上一页全部算刷掉（冷却 4 次）
        else away = {};  // 新的一次选择
        for (int slot = 0; slot < 3; ++slot) {
            std::array<int, Count> cand{}; int n = 0;
            // 4.1.0（用户 2026-11-16）：卡池每类一张——没拿到的是 Lv1，拿到的换成下一级（权重稍低，见 WeightOf），全部按权重抽；
            // 不再按「见过」排先后（原规则让刚升过级的下一级算新卡优先，后期页面被升级卡占满）。
            // 冷却中的卡（见 cool）和本次刷掉的卡先不出；凑不够才依次放宽（先放冷却中的，最后才放刷掉的）
            for (int pass = 0; pass < 3 && n == 0; ++pass) for (int id = 0; id < Count; ++id) {
                if (!(available & (Bit(id))) || !Unlocked(id, floor) || !CanPick(static_cast<Id>(id), floor) || std::find(next.begin(), next.end(), id) != next.end()) continue;
                const bool previous = (refresh && std::find(offers.begin(), offers.end(), id) != offers.end()) || (away & Bit(id));
                if (pass == 0 && Cooling(id)) continue;
                if (pass < 2 && previous) continue;
                cand[n++] = id;
            }
#ifdef PBZ_HEX_TEST
            // The final test floors have fewer than three unowned augments.
            // Fill remaining slots with already-owned choices; Pick rejects them.
            if (!n) for (int id = 0; id < Count; ++id)
                if ((available & (Bit(id))) && id != ServantMaster && Has(static_cast<Id>(id)) && std::find(next.begin(), next.end(), id) == next.end()) cand[n++] = id;
#endif
            if (!n) {
                if (slot > 0) break;  // 池子只剩 1 / 2 个：只给这几张
                rng = oldRng; return false;
            }
            next[slot] = PickTiered(cand, n, floor);
        }
        offers = next;
        for (int id : offers) MarkShown(id);
        return true;
    }
    // 4.1.0（用户 2026-11-16：卡池动态流入流出，权重重构）：每张卡的固定份额 = 稀有度概率 × 本卡权重 ÷ 该稀有度满池权重和，
    // 在当前候选里按份额抽一张。满池时各稀有度出现率与原来相同；原来是先掷稀有度再在剩下的卡里抽——冷却 / 拿满让某一档只剩一两张时，
    // 这几张的出现率会翻几倍（变成"必出"）。现在拿走的卡不把份额让给同档剩下的卡，冷却结束的卡按自己的份额流回
    // （权重：普通 20、升 Lv2 14、升 Lv3 10、仆从大师 20 / 10 / 5、连跳 30、对冲基金 12 / 8，见 WeightOf）
    // 满池 = 这一层已解锁的全部可玩卡（按未持有时的权重），与冷却、已拿满无关
    static std::array<double, 4> TierPools(int floor) {
        std::array<double, 4> f{}; const State fresh{};
        for (int id = 0; id < Count; ++id)
            if ((kPlayableMask & Bit(id)) && Unlocked(id, floor)) f[static_cast<size_t>(std::clamp(TierOf(id), 1, 3))] += fresh.WeightOf(id);
        return f;
    }
    double ShareOf(int id, int floor, const std::array<double, 4>& pools) const {
        const int t = std::clamp(TierOf(id), 1, 3);
        const double pool = pools[static_cast<size_t>(t)];
        return pool > 0 ? kTierOdds[TierRow(floor)][t - 1] * WeightOf(id) / pool : WeightOf(id);
    }
    template <size_t N>
    int PickTiered(const std::array<int, N>& cand, int n, int floor) {
        const auto pools = TierPools(floor);
        double total = 0;
        for (int i = 0; i < n; ++i) total += ShareOf(cand[i], floor, pools);
        if (total <= 0) return cand[Next() % static_cast<uint64_t>(n)];
        uint64_t z = Next();  // xorshift 刚初始化时相邻种子的高位相关，取浮点前先混合
        z ^= z >> 33; z *= 0xFF51AFD7ED558CCDull; z ^= z >> 33; z *= 0xC4CEB9FE1A85EC53ull; z ^= z >> 33;
        double x = static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0) * total;
        for (int i = 0; i < n; ++i) if ((x -= ShareOf(cand[i], floor, pools)) < 0) return cand[i];
        return cand[n - 1];
    }
    // 3.6.0（用户 2026-11-15）：基数 20；升级卡按原稀有度出，升 Lv2 ×0.7（14）、升 Lv3 ×0.5（10）；速战速决 Lv2 极难出（2）；
    // 坦克 / 缩小引擎互斥 5；仆从大师 20 / 10 / 5（与旧 4 / 2 / 1 同比例）
    static constexpr int kWeightMultiJump = 30, kWeightInterest = 12, kWeightInterestLv2 = 8;
    static constexpr int kWeightBase = 20, kWeightLv2 = 14, kWeightLv3 = 10, kWeightBlitzLv2 = 2, kWeightRival = 5;
    int WeightOf(int id) const {
        if (id == ServantMaster) { const int picked = PickCount(ServantMaster); return picked == 0 ? 20 : picked == 1 ? 10 : 5; }
        if (Playable(id) && Has(static_cast<Id>(id))) {
            if (id == Blitz || id == Thrower) return kWeightBlitzLv2;  // 速战速决 / 投掷狂人 Lv2 极难出
            if (id == Interest) return kWeightInterestLv2;
            return OfferLevel(id) >= 3 ? kWeightLv3 : kWeightLv2;
        }
        if ((id == Tank && Has(Swift)) || (id == Swift && Has(Tank))) return kWeightRival;
        if (id == MultiJump) return kWeightMultiJump;  // 3.7.0 连跳：娱乐卡，比普通卡更常见
        if (id == Interest) return kWeightInterest;    // 4.0.0（用户）：对冲基金太强，少出一点
        return kWeightBase;
    }
    bool Begin(int floor, Mask available = kPlayableMask) {
        if (EndlessMilestone(floor)) {
            if (!enabled || EndlessDone(floor)) return false;
            if (pending) return pending == floor;
            if (!Draw(floor, false, available)) return false;
#ifdef PBZ_PLAYTEST
            if (floor == 2) {
                int n = 0;
                for (const Id id : kPlaytestSecond) if ((available & (Bit(id))) && CanPick(id, floor) && n < 3) { offers[n++] = id; seen |= Bit(id); }
            }
#endif
            pending = floor; rerolled = 0; return true;
        }
        const int m = Milestone(floor);
        if (!enabled || m < 0 || (completed & (1u << m))) return false;
        if (pending) return pending == floor;
        if (!Draw(floor, false, available)) return false;
#ifdef PBZ_PLAYTEST
        if (floor == 1 && (available & (Bit(kPlaytestFirst))) && CanPick(kPlaytestFirst, floor)) {
            if (const auto at = std::find(offers.begin(), offers.end(), static_cast<int>(kPlaytestFirst)); at != offers.end()) std::swap(*at, offers[0]);
            else offers[0] = kPlaytestFirst;
            seen |= Bit(kPlaytestFirst);
        }
#endif
#ifdef PBZ_HEX_TEST
        // E 批验收顺序：第一张固定是下表里还没拿到的第一个，另外两张照常随机。
        // 用户 2026-11-11：先声夺人 / 定身先机 / 收割者 / 利滚利 / 护命符匠由用户实机验收，排在前面；纯数值的放后面
        for (const Id planned : kQaPlan) {
            if (!(available & (Bit(planned))) || !CanPick(planned, floor) || (planned != ServantMaster && Has(planned))) continue;  // 3.6.0：已拿到的不当计划卡（可升级也跳过）
            {
                if (const auto at = std::find(offers.begin(), offers.end(), static_cast<int>(planned)); at != offers.end()) std::swap(*at, offers[0]);
                else offers[0] = planned;
                seen |= Bit(planned);
            }
            break;
        }
#endif
        pending = floor; rerolled = 0; return true;
    }
    bool ReplaceOffer(int slot, Mask available) {
        if (!pending || slot < 0 || slot >= 3) return false;
        std::array<int, Count> candidates{}; int n = 0;
        if (offers[slot] >= 0 && offers[slot] < Count) away |= Bit(offers[slot]);  // 3.6.2：刷掉的这张，这一页里不再出现
        // 3.6.2：本页刷掉过的卡不再出现，只有池子真的空了才放回。4.1.0：同 Draw，不再按「见过」排先后，全部按权重抽；冷却中的卡先不出
        for (int pass = 0; pass < 3 && !n; ++pass) for (int id = 0; id < Count; ++id) {
            if (!(available & (Bit(id))) || !Unlocked(id, pending) || !CanPick(static_cast<Id>(id), pending) ||
                std::find(offers.begin(), offers.end(), id) != offers.end() || (pass < 2 && (away & (Bit(id)))) || (pass == 0 && Cooling(id))) continue;
            candidates[n++] = id;
        }
        if (!n) return false;
        offers[slot] = PickTiered(candidates, n, pending);
        MarkShown(offers[slot]);
        return true;
    }
    bool Refresh(int slot, Mask available = kPlayableMask) {
        if (!pending || slot < 0 || slot >= 3 || (rerolled & (1u << slot)) || !ReplaceOffer(slot, available)) return false;
        rerolled |= 1u << slot;
        return true;
    }
    bool ReplaceRetiredOffer(Mask available) {
        const auto retired = std::find(offers.begin(), offers.end(), Invincible);
        return pending && retired != offers.end() && ReplaceOffer(static_cast<int>(retired - offers.begin()), available);
    }
    bool Pick(int slot) {
        if (!pending || slot < 0 || slot >= 3) return false;
        if (ExtraSlotFloor(pending)) {
            const int id = offers[slot];
            if (id < 0 || id >= Count || !CanPick(static_cast<Id>(id), pending) || !Unlocked(id, pending) || EndlessDone(pending)) return false;
            extra.push_back(id); extraAt.push_back(pending); extraBase.push_back(endlessCleared);
            SettleCooldown(id);  // 4.1.0：本次出现过 / 刷掉的卡进冷却
            seenUp &= ~(Bit(id)); away = {};  // 3.6.2：升了一级，下一级的升级卡重新算新卡
            pending = 0; rerolled = 0; offers = {-1, -1, -1}; return true;
        }
        const int m = Milestone(pending, true), id = offers[slot];
        if (m < 0 || id < 0 || id >= Count || !CanPick(static_cast<Id>(id), pending) || picks[m] >= 0 || !Unlocked(id, pending)) return false;
        picks[m] = id; pickedAt[m] = pending; completed |= 1u << m;
        SettleCooldown(id);  // 4.1.0：本次出现过 / 刷掉的卡进冷却
        seenUp &= ~(Bit(id)); away = {};  // 3.6.2
        pending = 0; rerolled = 0; offers = {-1, -1, -1}; return true;
    }
    // 3.6.0：所有数值按当前等级取（换数值，不叠加）；叠层类 = 当前等级步长 × 层数
    double Attack() const { return (Has(Sharp) ? At(Sharp, .40, .80) : 0) + (Has(Desperate) ? At(Desperate, .80, 1.10, 1.40) : 0) + (Has(Keen) ? At(Keen, .30, .45, .60) : 0)
                                   + BattleStep() * Levels(Battle)
                                   + (Has(GlassCannon) ? At(GlassCannon, kGlassAttack, 2.20, 2.80) : 0) + (furyActive ? At(Fury, kFuryAttack, 1.80) : 0) + (openingActive ? kOpeningAttack : 0)
                                   + (echoActive ? EchoStep() * echoStacks : 0) + (Has(Flawless) ? FlawlessStep() * flawlessStacks : 0)
                                   + (Has(BackToBasics) ? kBasicsAttack : 0)
                                   + backWaterBonus + WarFeedStep() * warStacks + (boilActive ? BoilBonus() : 0) + (revengeActive ? RevengeAttack() : 0); }
    double Break() const { return (Has(Sharp) ? At(Sharp, .20, .40) : 0) + (Has(Desperate) ? At(Desperate, .30, .40, .50) : 0) + (Has(Pursuit) ? At(Pursuit, .40, .60, .80) : 0)
                                  + BreakStep() * Levels(BreakGrowth) + (furyActive ? At(Fury, kFuryBreak, .50) : 0)
                                  + (Has(BackToBasics) ? kBasicsBreak : 0) + (boilActive ? BoilBonus() : 0); }
    double Health() const { return std::max(-.80, (Has(Bell) ? At(Bell, .50, 1.00) : 0) + (Has(Sturdy) ? At(Sturdy, .80, 1.20, 1.60) : 0) + TankStep() * Levels(Tank)
                                                  + (Has(GlassCannon) ? kGlassHealth : 0) + (Has(BloodPact) ? kPactHealth : 0)
                                                  + (Has(Ascetic) ? AsceticStep() * asceticStacks : 0)); }
    // 积少成多：选中后每通关一段 +2%，最多 +100%（按通关位计，重打的段不重复算）
    // 3.2.0 存档过渡（用户）：3.2.0 之前已通过的层按旧规则每层 +2%（数值不变），之后每层 +5%；不封顶
    int snowLegacy = 0;  // 按旧 +2% 计的层数（新档 0；旧档读入时 = 当时已叠层数）
    static constexpr double kSnowBallLegacyStep = .02;
    double SnowBallBonus() const {
        if (!Has(SnowBall)) return 0.0;
        const int L = Levels(SnowBall), old = std::clamp(snowLegacy, 0, L);
        return kSnowBallLegacyStep * old + SnowStep() * (L - old);
    }  // 3.1.0（用户 2026-11-13）：不封顶
    // 绝境狂怒：当前生命低于 30% 时生效，回到 30% 以上立即失效（不锁存，和逃跑计划不同）
    void UpdateFury(bool fighting, double current, double maximum) {
        furyActive = fighting && Has(Fury) && maximum > 0 && current >= 0 && current < maximum * kFuryLine;
    }
    // 先声夺人：开战（读秒结束）后 8 秒
    void UpdateOpening(bool fighting, double sinceFight) {
        openingActive = fighting && Has(Opening) && sinceFight >= 0 && sinceFight < OpeningSeconds();
    }
    // 斩首行动 / 横扫千军：敌人受到伤害的倍率
    double EnemyTaken(bool mob) const { return mob ? (Has(Sweep) ? SweepMul() : 1.0) : (Has(Decapitate) ? DecapitateMul() : 1.0); }
    // 护命符匠：首次通关第 15（Lv2 10；QA 3）的倍数层时给复活符；已有 1 个不给
    bool CharmDue(int shownFloor, int reviveHeld) const { return Has(Charmsmith) && shownFloor > 0 && shownFloor % CharmEvery() == 0 && reviveHeld < 1 && !WasCleared(shownFloor); }
    void UpdateEscape(bool fighting, double current, double maximum) {
        if (!fighting || !Has(Escape)) { escapeActive = false; return; }
        if (maximum > 0 && current >= 0 && current <= maximum * .20) escapeActive = true;
    }
    // 一次"生命下降"= 掉血前至少 kGritQuiet 秒没有掉过血。持续掉血（灼烧、流血、
    // 斩马刀持续耗血）在停下之前只算一次，不能每帧叠层；间隔内的连击合并为一次。
    static constexpr double kGritQuiet = 0.5;
    static constexpr int kGritCap = 30;  // 用户 2026-11-11 定稿：每次 +1%，最多 30 层 = 30%
    void UpdateGrit(bool fighting, double current, double now) {
        if (!fighting || !Has(Grit)) { gritStacks = 0; gritLastHealth = -1; gritLastDrop = -1; return; }
        if (!(current >= 0) || !std::isfinite(now)) { gritLastHealth = -1; return; }
        if (gritLastHealth >= 0 && current < gritLastHealth - .01) {
            if (gritLastDrop < 0 || now - gritLastDrop >= kGritQuiet) gritStacks = std::min(GritCap(), gritStacks + 1);
            gritLastDrop = now;
        }
        gritLastHealth = current;
    }
    bool CanRevive(bool used, double current, double maximum) const {
        return Has(KillingIntent) && !used && maximum > 0 && current > maximum * ReviveLine();
    }
    int ScientistForm(int floor, uint64_t seed) const {
        bool chosen = false;
        if (const int at = PickedFloor(Scientist); at > 0 && at <= floor) chosen = true;  // 3.0.1：额外栏位里的也算
        if (!chosen || floor < 1) return 0;
        uint64_t n = seed ^ (0x9E3779B97F4A7C15ull * static_cast<uint64_t>(floor)) ^ 0xAC0FFEE87219AB53ull;
        n ^= n >> 30; n *= 0xBF58476D1CE4E5B9ull; n ^= n >> 27; n *= 0x94D049BB133111EBull; n ^= n >> 31;
        return 1 + static_cast<int>(n & 1ull);
    }
    struct ScienceBoost { double health, attack, armorBreak, move, attackSpeed, shachiCap; };
    ScienceBoost ScientistBoost(int floor, uint64_t seed) const {
        const double k = ScientistScale();  // 3.6.0 Lv2 ×1.25
        switch (ScientistForm(floor, seed)) {
            case 1: return {1.0 * k, .40 * k, .10 * k, 0, 0, 1.0 * k};
            case 2: return {0, 0, 0, .60 * k, .60 * k, 0};
            default: return {0, 0, 0, 0, 0, 0};
        }
    }
    double SwiftBonus() const { return std::min(kSwiftCap, SwiftStep() * Levels(Swift)); }
    double AttackSpeed() const { return SwiftBonus(); }  // 海克斯带来的攻速（目前只有缩小引擎）
    double Speed() const { return (Has(Keen) ? At(Keen, .20, .30, .40) : 0) + SwiftBonus() + (escapeActive ? .80 : 0)  // v3：快意恩仇 +20%、缩小引擎每层 +1%
        ; }
    int Defense() const { return (Has(Icathia) ? 10 * Levels(Icathia) : 0) + (Has(Bell) ? static_cast<int>(At(Bell, 150, 250)) : 0) +
                                 (Has(Bulwark) ? static_cast<int>(At(Bulwark, 250, 350, 450)) : 0) + (Has(BackToBasics) ? kBasicsDefense : 0) + 10 * gritStacks + (ironActive ? kIronWallDefense : 0); }
    int LifeSteal() const { return (Has(Leech) ? 50 : 0) + (Has(BloodPact) ? static_cast<int>(At(BloodPact, kPactLeech, 100)) : 0) + (Has(BackToBasics) ? kBasicsLeech : 0); }
    // 质变：从没拥有的海克斯里随机拿 n 个，放进还空着的栏位（从最后一格往前，层号按该栏位）；返回实际拿到的
    std::vector<int> GrantRandom(int n, Mask available) {
        std::vector<int> got;
        for (int k = 0; k < n; ++k) {
            std::array<int, Count> cand{}; int c = 0;
            for (int id = 0; id < Count; ++id)
                if (Playable(id) && (available & (Bit(id))) && id != Transmutation && !Has(static_cast<Id>(id)) && CanPick(static_cast<Id>(id), 1)) cand[c++] = id;
            if (!c) break;
            // 3.0.1：海克斯层变多后，质变给的放额外栏位（记作负的质变层号），不再占后面第 30 / 60 / 90 层的固定栏位
            const int at = std::max(1, PickedFloor(Transmutation));
            const int id = PickTiered(cand, c, at);  // v3：质变给的卡按当前海克斯层的等级概率
            extra.push_back(id); extraAt.push_back(-at); extraBase.push_back(endlessCleared); seen |= Bit(id);
            got.push_back(id);
        }
        return got;
    }
    double MinTaken() const { return Has(Icathia) ? kMinTakenIcathia : kMinTakenFactor; }
    double DamageTaken() const { return Has(Desperate) ? 1.15 : 1.0; }
    std::string Save() const {
        std::ostringstream s; s << kSchema << ' ' << (enabled ? 1 : 0) << ' ' << rng << ' ' << seen.lo << ' ' << completed << ' ' << pending << ' ' << rerolled;
        for (int x : offers) s << ' ' << x;
        for (int x : picks) s << ' ' << x;
        for (int x : pickedAt) s << ' ' << x;
        s << ' ' << clearedLow << ' ' << clearedHigh;
        if (!extra.empty() || endlessCleared) {  // 无尽层：可选尾段（旧档没有，照旧读）
            s << " x " << endlessCleared << ' ' << extra.size();
            for (size_t i = 0; i < extra.size(); ++i) s << ' ' << extra[i] << ' ' << extraAt[i] << ' ' << extraBase[i];
        }
        s << " s " << snowLegacy;  // 3.2.0 可选尾段：积少成多旧规则层数（没有 = 3.2.0 之前的档）
        if (seen.hi) s << " h " << seen.hi;  // 3.7.0 可选尾段：编号 64–127 的见过位
        return s.str();
    }
    static bool Load(std::string_view input, State* out) {
        State t; int version = 0, en = 0, rr = 0;
        uint64_t seenHi = 0;
        if (const auto h = input.rfind(" h "); h != std::string_view::npos) {  // 3.7.0 尾段：编号 64–127 的见过位
            std::istringstream hs{std::string(input.substr(h + 3))}; std::string rest;
            if (!(hs >> seenHi) || (hs >> rest)) return false;
            input = input.substr(0, h);
        }
        std::istringstream s{std::string(input)};
        if (!(s >> version >> en >> t.rng >> t.seen.lo >> t.completed >> t.pending >> rr)) return false;
#ifdef PBZ_HEX_TEST
        const bool oldBoolRefresh = version == 2;
        if (version != kSchema && version != 5 && version != kLegacySchema && version != 2) return false;
        const int savedSlots = version == kSchema ? kSlots : version == 5 ? 24 : 16;
#else
        const bool oldBoolRefresh = version == kLegacySchema;
        if (version != kSchema && version != kLegacySchema) return false;
        const int savedSlots = kSlots;
#endif
        if ((en != 0 && en != 1) || rr < 0 || rr > (oldBoolRefresh ? 1 : 7)) return false;
        for (int& x : t.offers) if (!(s >> x)) return false;
        for (int i = 0; i < savedSlots; ++i) if (!(s >> t.picks[i])) return false;
        for (int i = 0; i < savedSlots; ++i) if (!(s >> t.pickedAt[i])) return false;
        if (!(s >> t.clearedLow >> t.clearedHigh)) return false;
        s >> std::ws;
        bool snowSaved = false;
        if (!s.eof() && s.peek() == 's') {  // 没有无尽尾段、只有积少成多尾段
            std::string tag; int legacy = 0;
            if (!(s >> tag >> legacy) || tag != "s" || legacy < 0 || legacy > 100000) return false;
            t.snowLegacy = legacy; snowSaved = true; s >> std::ws;
        }
        if (!s.eof()) {
            std::string tag; size_t n = 0;
            if (!(s >> tag >> t.endlessCleared >> n) || tag != "x" || t.endlessCleared < 0 || n > static_cast<size_t>(Count) * 3) return false;  // 3.6.0：升级卡也记在这里，最多每个 3 张
            for (size_t i = 0; i < n; ++i) {
                int id = -1, at = 0, base = 0;
                if (!(s >> id >> at >> base) || !Playable(id) || base < 0 || base > t.endlessCleared) return false;
                if (at < 0) { if (-at < 1) return false; }  // 质变给的：层号随质变
                // 3.2.0（用户实机：测试包第 2 层送的海克斯让整档读不进）：拿到层号不再要求是现行规则的海克斯层——
                // 规则改过（3.0.0 的 120 / 150 / 180…、测试包第 2 层）的旧档照读、海克斯照留；只拒绝重复层号（真损坏）
                else if (at > 100000 || std::find(t.extraAt.begin(), t.extraAt.end(), at) != t.extraAt.end()) return false;
                t.extra.push_back(id); t.extraAt.push_back(at); t.extraBase.push_back(base);
            }
            s >> std::ws;
            if (!s.eof()) {
                std::string tag; int legacy = 0;
                if (!(s >> tag >> legacy) || tag != "s" || legacy < 0 || legacy > 100000) return false;
                t.snowLegacy = legacy; snowSaved = true;
            }
            s >> std::ws; if (!s.eof()) return false;
        }
        t.seen.hi = seenHi;
        t.enabled = en != 0; t.rerolled = oldBoolRefresh && rr ? 7u : static_cast<unsigned>(rr);
        if (t.completed >= (1u << kSlots) || (t.seen & ~kAllMask) || (t.clearedHigh >> 36) || (t.enabled && !t.rng)) return false;
        for (int i = 0; i < kSlots; ++i) {
            const int id = t.picks[i]; const bool done = (t.completed & (1u << i)) != 0;
            if (done != (id >= 0) || id >= Count || id < -1 ||
                (done && (t.pickedAt[i] != SlotFloor(i) || (id != Invincible && !Unlocked(id, t.pickedAt[i])))) ||
                (!done && t.pickedAt[i] != 0)) return false;
            if (id >= 0 && id != ServantMaster && id != Leech && !Upgradable(id)) for (int j = 0; j < i; ++j) if (t.picks[j] == id) {  // 3.6.0：可升级的允许重复（张数 = 等级）；4.0.0 渴血以前可升级，老档里的重复照读
#ifdef PBZ_HEX_TEST
                if (id == Scientist && (SlotFloor(i) == 17 || SlotFloor(i) == 20)) continue;
#endif
                return false;
            }
        }
        if (t.PickCount(ServantMaster) > kMaxServants) return false;
        for (int id = 0; id < Count; ++id) if (id != ServantMaster && t.PickCount(static_cast<Id>(id)) > kLegacyMaxRank) return false;  // 4.0.0：最高级降低（渴血 / 血之契约）的老档照读，多出的张数按新上限算
        if (t.pending) {
            if (ExtraSlotFloor(t.pending)) { if (t.EndlessDone(t.pending)) return false; }
            else { const int m = Milestone(t.pending, true); if (m < 0 || (t.completed & (1u << m))) return false; }
            for (int i = 0; i < 3; ++i) {
                const int id = t.offers[i];
                if (id < 0 && i > 0 && ExtraSlotFloor(t.pending)) continue;  // 无尽层池子见底：允许空位
                if ((id != Invincible && !Unlocked(id, t.pending)) || !(t.seen & (Bit(id)))) return false;
                if (id == ServantMaster && !t.CanPick(ServantMaster, t.pending)) return false;
#ifndef PBZ_HEX_TEST
                if (id != Invincible && !t.CanPick(static_cast<Id>(id), t.pending)) return false;
#endif
                for (int j = 0; j < i; ++j) if (t.offers[j] == id) return false;
            }
        } else if (t.rerolled || t.offers != std::array{-1,-1,-1}) return false;
        if (!snowSaved) t.snowLegacy = t.Has(SnowBall) ? t.Levels(SnowBall) : 0;  // 3.2.0 之前的档：已有层数按旧 +2% 保留
        *out = t; return true;
    }
};
}
