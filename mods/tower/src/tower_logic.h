// 影塔纯逻辑（不碰游戏）：随机数、强化卡、加成叠算、选 Boss、战力目标、一局的状态机。
// 游戏相关的部分（生成、写属性、判死活）在 tower.cpp；这里的每个函数都能在离线测试里跑。
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "tower_economy.h"
#include "tower_special.h"

namespace tower {
inline constexpr int kPlanVersion = 14;  // 和 floors.generated.h 的 floorplan::kPlanVersion 一致（plan_test 核对）

// ---- 随机数：种子固定则整局可复现（分享码 / 每日之塔用） ----
struct Rng {
    uint64_t s;
    Rng(uint64_t seed = 0x9E3779B97F4A7C15ull) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t Next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }   // xorshift64
    int Below(int n) { return n > 0 ? static_cast<int>(Next() % static_cast<uint64_t>(n)) : 0; }
};

// ---- 强化卡 ----
// 用户 2026-10-30：成长要符合游戏逻辑——原游戏的攻击力主要来自升级兵器，所以"淬锋"类按兵器淬炼表现：
// 直接抬高塔内钉住的攻击 / 杀气攻击（和游戏里升级兵器加的是同一个属性），不再给 Boss 加易伤
enum class Stat { DmgDealt, DmgTaken, MaxHp, ShachiGain, AtkSpeed, PostureDealt, HealOnFloor };
enum Rarity { Fan = 0, Ling = 1, Xuan = 2, Tian = 3 };  // 凡 灵 玄 天
struct Card {
    std::string_view id, name, school, text;
    Rarity rarity;
    Stat stat;
    double value;   // 加成量：倍率类为 +v（受伤为 -v），回复类为最大生命的比例
    int weight;     // 抽卡权重
};
inline constexpr Card kCards[] = {
    {"blade_1", "淬锋", "兵器", "兵器淬炼：攻击 +15%", Fan, Stat::DmgDealt, 0.15, 10},
    {"blade_2", "淬锋·灵", "兵器", "兵器淬炼：攻击 +25%", Ling, Stat::DmgDealt, 0.25, 5},
    {"break_1", "破军", "兵器", "兵器开刃：削杀气 +40%", Fan, Stat::PostureDealt, 0.40, 8},
    {"guard_1", "铁骨", "金钟", "受到伤害 -15%", Fan, Stat::DmgTaken, 0.15, 10},
    {"guard_2", "铁骨·灵", "金钟", "受到伤害 -25%", Ling, Stat::DmgTaken, 0.25, 5},
    {"life_1", "厚生", "生命", "生命上限 +20%", Fan, Stat::MaxHp, 0.20, 8},
    {"life_2", "厚生·玄", "生命", "生命上限 +40%", Xuan, Stat::MaxHp, 0.40, 2},
    {"heal_1", "回春", "金钟", "每层开始回复 30% 生命", Fan, Stat::HealOnFloor, 0.30, 8},
    {"shachi_1", "聚煞", "杀气", "杀气恢复速度 +40%", Fan, Stat::ShachiGain, 0.40, 8},
    {"shachi_2", "聚煞·玄", "杀气", "杀气恢复速度 +80%", Xuan, Stat::ShachiGain, 0.80, 2},
    {"speed_1", "疾风", "时机", "攻击速度 +10%", Ling, Stat::AtkSpeed, 0.10, 5},
    {"blade_3", "淬锋·天", "兵器", "兵器淬炼：攻击 +50%", Tian, Stat::DmgDealt, 0.50, 1},
};
inline constexpr int kCardCount = static_cast<int>(std::size(kCards));
// Keep legacy table indexes stable for existing run saves. Floor-start recovery
// is a tower rule, so healing at floor start must never occupy a reward slot.
inline bool CardInRewardPool(int i) {
    return i >= 0 && i < kCardCount && kCards[i].weight > 0 && kCards[i].stat != Stat::HealOnFloor;
}

// 本局加成：同类相加，最后换算成倍率；带上下限，防止叠出离谱数值（修改器的倍率乘在外面，不受限）
struct Mods {
    double dealt = 0, taken = 0, maxHp = 0, shachi = 0, speed = 0, posture = 0, heal = 0;
    void Add(const Card& c) {
        switch (c.stat) {
            case Stat::DmgDealt: dealt += c.value; break;
            case Stat::DmgTaken: taken += c.value; break;
            case Stat::MaxHp: maxHp += c.value; break;
            case Stat::ShachiGain: shachi += c.value; break;
            case Stat::AtkSpeed: speed += c.value; break;
            case Stat::PostureDealt: posture += c.value; break;
            case Stat::HealOnFloor: heal += c.value; break;
        }
    }
    double DealtMul() const { return std::min(5.0, 1 + dealt); }
    double TakenMul() const { return std::max(0.2, 1 - taken); }
    double MaxHpMul() const { return std::min(5.0, 1 + maxHp); }
    double ShachiMul() const { return std::min(5.0, 1 + shachi); }
    double SpeedMul() const { return std::min(1.6, 1 + speed); }
    double PostureMul() const { return std::min(5.0, 1 + posture); }
    double HealFrac() const { return std::min(1.0, heal); }
};

// ---- 平衡（用户 2026-10-30：选了加成之后下一层仍然是艰难获胜，不要越打越碾压） ----
// 1. 加成补偿：Boss 血量跟着玩家的伤害 / 攻速加成涨，Boss 伤害跟着玩家的生命 / 减伤加成涨，各自只抵消约八成 / 七成，
//    所以加成仍然有感觉，但不会一路碾压。
inline double BuffHpComp(const Mods& m) { return std::pow(m.DealtMul() * m.SpeedMul(), 0.8); }
inline double BuffDmgComp(const Mods& m) { return std::pow(m.MaxHpMul() / m.TakenMul(), 0.7); }
// 2. 自适应：按上一层的表现调下一层（目标：打 45–150 秒、最低掉到三成到七成五的血）。打得太轻松 → 系数 ×1.15，
//    险胜或拖太久 → ×0.92；系数限制在 0.6–2.0。血量乘系数，伤害乘系数的平方根。
struct Pace { double seconds, hpLost; };  // hpLost：本层最低时掉了多少血（0..1）
inline double NextAdapt(double adapt, Pace p) {
    double a = adapt;
    if (p.seconds < 45 || p.hpLost < 0.25) a *= 1.15;
    else if (p.seconds > 150 || p.hpLost > 0.75) a *= 0.92;
    return std::clamp(a, 0.6, 2.0);
}

// 3 选 1：按权重不放回抽 3 张不同的卡
inline std::array<int, 3> Draw(Rng& rng) {
    std::array<int, 3> out{-1, -1, -1};
    std::vector<int> left;
    for (int i = 0; i < kCardCount; ++i) if (CardInRewardPool(i)) left.push_back(i);
    for (int k = 0; k < 3 && !left.empty(); ++k) {
        int total = 0;
        for (int i : left) total += kCards[i].weight;
        int r = rng.Below(total);
        for (size_t j = 0; j < left.size(); ++j) {
            r -= kCards[left[j]].weight;
            if (r < 0) { out[k] = left[j]; left.erase(left.begin() + j); break; }
        }
    }
    return out;
}

inline double Unit(Rng& r) { return static_cast<double>(r.Next() % 1000000) / 1000000.0; }

// 按区稀有度抽一张卡（minRarity：多 Boss 层 / 团战层至少灵级）
inline int DrawCard(Rng& rng, int zone, int minRarity) {
    const auto& w = kRarityByZone[std::clamp(zone, 0, 4)];
    int total = 0;
    for (int r = minRarity; r < 4; ++r) total += w[r];
    int rarity = minRarity;
    if (total > 0) {
        int x = rng.Below(total);
        for (int r = minRarity; r < 4; ++r)
            if ((x -= w[r]) < 0) { rarity = r; break; }
    }
    // 该稀有度没有卡：先往下找最近的，再往上找
    for (int step = 0; step < 4; ++step)
        for (int r : {rarity - step, rarity + step}) {
            if (r < 0 || r > 3) continue;
            int sum = 0;
            for (int i = 0; i < kCardCount; ++i)
                if (CardInRewardPool(i) && kCards[i].rarity == r) sum += kCards[i].weight;
            if (!sum) continue;
            int x = rng.Below(sum);
            for (int i = 0; i < kCardCount; ++i)
                if (CardInRewardPool(i) && kCards[i].rarity == r && (x -= kCards[i].weight) < 0) return i;
        }
    return 0;
}
inline int DrawItem(Rng& rng) {
    int total = 0;
    for (int i = 0; i < kItemCount; ++i) if (ItemInRewardPool(i)) total += kItems[i].weight;
    int x = rng.Below(total);
    for (int i = 0; i < kItemCount; ++i)
        if (ItemInRewardPool(i) && (x -= kItems[i].weight) < 0) return i;
    return 0;
}

// ---- 战力目标：第 floor 层（从 1 起）Boss 的目标血量和伤害倍率 ----
// ---- 统一起点（用户 2026-10-30 定）：塔内玩家强度属性的当前值钉到这组标准值，与存档进度、装备、强化无关 ----
// 实测：BaseValue 永远是 1 级值（攻击 100、生命 850、杀气上限 400），装备 / 强化 / 饰品都加在 CurrentValue 上
struct Standard {
    double attack = 140, shachiAttack = 140, maxHp = 900, shachiMax = 440, infliction = 1050;
};

struct Tuning {
    // 验收期默认（方案 notes/tower-50-floors-plan.md 第 4 节，用户 2026-11-01：数值最后调）：第 1 层 5000，每层 +5%（50 层约 5.5 万）；
    // 伤害每层 +3%。原 20000 / +12% 到 50 层约 129 万，不现实
    double hp0 = 5000;
    double hpGrowth = 0.05;
    double dmg0 = 1.0;
    double dmgGrowth = 0.03;
};
inline double TargetHp(const Tuning& t, int floor) { return t.hp0 * std::pow(1 + t.hpGrowth, std::max(0, floor - 1)); }
inline double BossDmgMul(const Tuning& t, int floor) { return t.dmg0 * (1 + t.dmgGrowth * std::max(0, floor - 1)); }
// 实机读到的最大生命 → 缩放倍率（读不到时不缩放）
inline double HpScale(double nativeMax, double target) { return nativeMax > 1 ? std::clamp(target / nativeMax, 0.05, 100.0) : 1.0; }

// ---- 层型：大部分 1v1，少数 1v2，高层两次 1v3 ----
// 50 层：每区第 3 层是 1v2；不足 10 层的短局第 2 层 1v2，方便试玩。
// 1v3（用户 2026-10-31：原游戏后期就有 1v3，高层可以出，最多两次、要削弱）：第 43 层（第五区的 1v2 换成 1v3）和第 49 层。
// 之前去掉 1v3 的原因（三面同时出手时闪避有空档）靠两点缓解：每只血量 / 伤害压得比 1v2 还低，三只错峰登场。
inline constexpr int kTrioFloors[] = {43, 49};
inline int BossCount(int floor, int floors) {
    if (floors < 10) return floors >= 3 && floor == 2 ? 2 : 1;
    if (floors >= 50 && std::find(std::begin(kTrioFloors), std::end(kTrioFloors), floor) != std::end(kTrioFloors)) return 3;
    return floor % 10 == 3 ? 2 : 1;
}
// 多 Boss 层每只的血量 / 伤害折扣：1v2 总血量约 1.3 倍；1v3 总血量约 1.26 倍（比 1v2 还少）、每只伤害 ×0.55；从池子偏弱的一段挑
inline double ShareHp(int count) { return count >= 3 ? 0.42 : count == 2 ? 0.65 : 1.0; }
inline double ShareDmg(int count) { return count >= 3 ? 0.55 : count == 2 ? 0.75 : 1.0; }
inline int EasierFloor(int floor, int count) { return count >= 3 ? std::max(1, floor / 2) : count > 1 ? std::max(1, floor * 6 / 10) : floor; }
// 错峰登场：1v3 第二只 7 秒后、第三只 14 秒后才生成（引擎按这个延迟提交生成请求）
inline double EntryDelay(int count, int index) { return count >= 3 ? 7.0 * index : 0.0; }

// ---- 选 Boss：池子按基础血量升序；层数越高取越靠后的一段，同一局不重复 ----
// ok：额外条件（多 Boss 层的陪打只能是不吃场景的 A 类、不同身份）；满足条件的一个都没有时不加条件
// admit：准入（正式池只放实机验证过的 Boss）。准入是硬条件：候选用完时只在准入内重复，绝不退回到未准入的内容；
// 一个都没有返回 -1（2026-10-31 实机：旧的兜底会退回全池，抽到了无血条 / 不能锁定的镜像）
inline int PickBoss(Rng& rng, int floor, int floors, int poolSize, const std::vector<int>& used, const std::function<bool(int)>& ok, const std::function<bool(int)>& admit) {
    if (poolSize <= 0) return -1;
    auto in = [&](int i) { return !admit || admit(i); };
    auto pick = [&](auto&& f) {
        std::vector<int> c;
        for (int i = 0; i < poolSize; ++i) if (in(i) && f(i)) c.push_back(i);
        return c.empty() ? -1 : c[rng.Below(static_cast<int>(c.size()))];
    };
    const double t = floors > 1 ? double(floor - 1) / (floors - 1) : 0.0;
    const int window = std::max(1, poolSize / 3);
    const int start = std::clamp(static_cast<int>(t * (poolSize - window)), 0, poolSize - window);
    auto unused = [&](int i) { return std::find(used.begin(), used.end(), i) == used.end(); };
    auto okay = [&](int i) { return !ok || ok(i); };
    std::vector<int> cand;
    for (int i = start; i < start + window; ++i) if (in(i) && okay(i) && unused(i)) cand.push_back(i);
    if (!cand.empty()) return cand[rng.Below(static_cast<int>(cand.size()))];
    if (int b = pick([&](int i) { return okay(i) && unused(i); }); b >= 0) return b;  // 全池里没用过的
    if (int b = pick(okay); b >= 0) return b;                                       // 准入内允许重复
    return -1;
}
inline int PickBoss(Rng& rng, int floor, int floors, int poolSize, const std::vector<int>& used, const std::function<bool(int)>& ok = {}) {
    if (poolSize <= 0) return -1;
    const double t = floors > 1 ? double(floor - 1) / (floors - 1) : 0.0;
    const int window = std::max(1, poolSize / 3);
    const int start = std::clamp(static_cast<int>(t * (poolSize - window)), 0, poolSize - window);
    auto fits = [&](int i) { return (!ok || ok(i)) && std::find(used.begin(), used.end(), i) == used.end(); };
    std::vector<int> cand;
    for (int i = start; i < start + window; ++i)
        if (fits(i)) cand.push_back(i);
    if (cand.empty())  // 这一段用完了 / 没有合条件的：全池里找
        for (int i = 0; i < poolSize; ++i)
            if (fits(i)) cand.push_back(i);
    if (cand.empty() && ok) return PickBoss(rng, floor, floors, poolSize, used);
    if (cand.empty()) return rng.Below(poolSize);
    return cand[rng.Below(static_cast<int>(cand.size()))];
}

// ---- 一局的状态机 ----
enum class Phase { Idle, Ready, Fighting, Reward, Won, Lost };
inline std::string_view PhaseName(Phase p) {
    switch (p) {
        case Phase::Idle: return "idle";
        case Phase::Ready: return "ready";        // 等玩家开下一层
        case Phase::Fighting: return "fighting";
        case Phase::Reward: return "reward";      // 3 选 1
        case Phase::Won: return "won";
        case Phase::Lost: return "lost";
    }
    return "?";
}

enum class OfferKind { Card, Item, Jade };
struct Offer { OfferKind kind = OfferKind::Card; int index = -1; int amount = 0; };

// 本层结算的收获（广播给界面显示）
struct Gains { int jade = 0; std::vector<int> weapons, shentong; };

struct Run {
    Phase phase = Phase::Idle;
    int floor = 0, floors = 3;
    uint64_t seed = 0;
    std::string id;                   // 局号（开局时生成，存档里保留）
    Rng rng;
    Tuning tuning;
    Standard standard;
    Mods mods;
    std::vector<int> picked;          // 已选的卡
    std::vector<int> usedBosses;
    bool legacy = false;              // 读档时发现是 2.0.0 之前的局（不写入存档）
    int shuffleBosses = 0;            // 洗牌方案（floorplay::kShuffleTens / kShuffleWide）；旧存档缺省固定顺序，"1" 仍是每十场一组
    std::vector<int> shopDrops;       // 实际击败 Boss 解锁的武器，使用兼容的掉落表下标
    std::array<std::vector<std::string>, 51> clearedRoute; // 每场实际击败的类路径；备选也要在回退时保持
    std::vector<int> times;           // 每层用时（0.1 秒为单位，按层序；作废的层不记）——方案第 5.2 节
    bool audit = false;               // 开局用的是开发审计池（pool:"audit"）；续局沿用，见 tower.cpp Resume。正式池的验证白名单 / 禁用不因此改变
    int plan = 0;                     // 开局 / 最后一次存档时的 50 层方案版本（0 = v5 之前的旧局：续局按当前方案开下一层，见 tower.cpp 续局日志）
    int chalOffered = 0, chalDone = 0;  // 附加挑战：出现过几个 / 完成几个
    int doneTimed = 0, donePotion = 0;  // v14：本轮完成的额外·限时 / 额外·禁药（各满 2 且打完第 50 层 → 解锁第 51 层）
    bool hidden = false;                // 这一局是从第 51 层入口进的（存档走隐藏层文件，不碰正常局存档）
    bool endless = false;               // 用户 2026-11-12：第 50 层（显示第 99 层左殇）通关后开启无尽模式，之后一直往上打（取代第 51 隐藏层）
    bool endlessSave = false;           // 3.5.0：这一局已是「无尽存档」（99 层确认进入，或导入无尽档开局）；开局菜单的「无尽模式」继续它
    std::string source;                 // 3.5.0：导入无尽档开的局 = 「作者|起始显示层」；只在安装器战绩里显示
    std::array<Offer, 3> offer{};
    std::vector<int> bosses;          // 本层 Boss（池下标），1v1 只有一个
    // 局内资源
    int jade = 0;
    std::array<int, kItemCount> items{};
    std::vector<int> shentong;        // 神通背包（kShentong 下标），最多 3 个
    bool lingxiGot = false;           // 灵犀每局最多 1 个
    std::vector<int> weapons;         // 本局掉落的兵器（kWeaponDrops 下标，按兵器去重）
    int wearMain = -1, wearShaqi = -1;  // 塔内换上的兵器（kWeaponDrops 下标）；不进局存档，暂离 / 出塔即换回
    double adapt = 1.0;               // 自适应难度系数（见 NextAdapt）
    // 统一起点校正（用户 2026-11-05：有玩家进塔起点生命 3000 多）：全新开局实测的原生生命 / 攻击 / 杀气上限高于塔档标准起点时，
    // 整局按 标准 / 实测 缩放（只压不抬）；0 = 还没测。写进局存档，继续上一局沿用同一组系数
    double norm[3] = {0, 0, 0};
    double normPool[2] = {0, 0};
    int specialFrom = 1;            // 4.0.0 创意层起点（小兵段 F）：新局 1 = 完整计划；4.0 以前的老局读档时按进度算（tower::special::FromProgress）
    int mobFloor = 0, mobWave = 0;  // 1.4.0 小怪波次进度：第 mobFloor 层之前已清 mobWave 轮（随局存档，读档接着打、回退不重发）
    int mobVenueFloor = 0;          // 使用场地只属于该段波次，避免续局时换图
    std::string mobVenuePath;       // 稳定的场地拥有者类路径，不保存池下标或内存地址
    bool inMobWave = false;         // 正在打小怪（不存档）：UndoFloor 不退层
    bool specialStage = false;      // 3.7.0 主线创意层（替换小兵段，不 ++floor；不存档）：UndoFloor 同样不退层
    int shopFloor = 0;  // 2.0.0：已经为第几层 Boss 开过驿站（每层 Boss 前一次，回退不重开）
    int peak = 0;  // 1.4.0：本局通过过的最高层；回退（重生符）后 ≤ peak 的层重打不给任何奖励
    int difficulty = 0;  // 1.0.1：0 困难（缺省，旧局都是）/ 1 简单；开局选，整局不变  // 1.0.1：生命 / 杀气上限校正系数（0 = 旧局，不校正）
    Gains last;
    // 3.6.0（用户 2026-11-14）魂玉账单：Boss 层基础魂玉 + 奖励页所选魂玉，奖励页选完后一次性乘倍率（积少成多 / 聚宝盆 / 钱生钱 / 悬赏令）。
    // 基础和所选魂玉入账时先按原数到账，结算时只补差额；倍率在通关那一刻定下（千分比），随局存档（bill=），读档后照样结算
    struct JadeBill { bool open = false; int floor = 0, base = 0, picked = 0, addPm = 0, mulPm = 1000, snowPm = 0; };
    JadeBill bill;

    // 本层 Boss 的血量 / 伤害总倍率：层数曲线 × 多 Boss 折扣 × 自适应 × 加成补偿
    double BossHpTarget(int count) const { return TargetHp(tuning, floor) * ShareHp(count) * adapt * BuffHpComp(mods); }
    double BossDmg(int count) const { return BossDmgMul(tuning, floor) * ShareDmg(count) * std::sqrt(adapt) * BuffDmgComp(mods); }
    void Paced(Pace p) { adapt = NextAdapt(adapt, p); }

    void Start(uint64_t s, int totalFloors) {
        *this = Run{};
        seed = s; rng = Rng(s); floors = std::clamp(totalFloors, 1, 60);
        shuffleBosses = floors >= 50 ? 4 : 0;  // 3.4.3 方案 4（祝原 / 游龙往后）  // 用户 2026-11-12：新局用 20 / 20 / 10 分组；3.0.1 起方案 3（阿瑾 / 大师兄 / 火流星推到 61 层后）
        phase = Phase::Ready;
        plan = kPlanVersion;
    }
    int Zone() const { return ZoneOf(std::max(1, floor), floors); }
    // 开下一层：返回本层 Boss 数，0 = 当前不能开
    // companion(primary, candidate)：多 Boss 层第 2、3 只的条件（引擎给：A 类、身份不同）
    // lead(candidate)：主 Boss 的条件（引擎给：吃场景的必须标定过原生场地，否则不进池）
    // admit：硬准入（见 PickBoss）；给了 admit 时选不出就返回 0、本层不开
    int NextFloor(int poolSize, const std::function<bool(int, int)>& companion = {}, const std::function<bool(int)>& lead = {}, const std::function<bool(int)>& admit = {}, int maxCount = 3) {
        if (phase != Phase::Ready || poolSize <= 0) return 0;
        ++floor;
        const int count = std::min(BossCount(floor, floors), std::max(1, maxCount));
        bosses.clear();
        for (int i = 0; i < count; ++i) {
            std::function<bool(int)> ok = lead;  // 陪打同样要满足（吃场景又没场地的不出）
            if (i > 0 && companion) ok = [&, primary = bosses[0]](int c) {
                if (lead && !lead(c)) return false;
                if (!companion(primary, c)) return false;
                for (int b : bosses) if (!companion(b, c)) return false;
                return true;
            };
            const int b = admit ? PickBoss(rng, EasierFloor(floor, count), floors, poolSize, usedBosses, ok, admit)
                                : PickBoss(rng, EasierFloor(floor, count), floors, poolSize, usedBosses, ok);
            if (b < 0) {  // 准入内选不出（例如 1v2 只剩同一身份）：撤回本层
                for (size_t k = 0; k < bosses.size(); ++k) usedBosses.pop_back();
                bosses.clear(); --floor;
                return 0;
            }
            usedBosses.push_back(b);
            bosses.push_back(b);
        }
        phase = Phase::Fighting;
        return count;
    }
    // 开发审计计划：本层就是这几只（已在引擎里校验准入 / 身份），不经抽取
    int ForcedFloor(const std::vector<int>& list) {
        if (phase != Phase::Ready || list.empty()) return 0;
        ++floor;
        bosses = list;
        for (int b : list) usedBosses.push_back(b);
        phase = Phase::Fighting;
        return static_cast<int>(list.size());
    }
    // 生成失败：退回开层前
    void UndoFloor() {
        if (phase != Phase::Fighting) return;
        if (inMobWave || specialStage) { inMobWave = false; specialStage = false; phase = Phase::Ready; return; }  // 小怪波次 / 主线创意层没有 ++floor，层号不动
        for (size_t i = 0; i < bosses.size() && !usedBosses.empty(); ++i) usedBosses.pop_back();
        bosses.clear(); --floor; phase = Phase::Ready;
    }
    // 三选一：一张卡 / 一件道具 / 一袋魂玉（本层魂玉 ×1.5）
    void DrawOffer() {
        const int minRarity = bosses.size() > 1 ? Ling : Fan;
        offer[0] = {OfferKind::Card, DrawCard(rng, Zone(), minRarity), 0};
        offer[1] = {OfferKind::Item, DrawItem(rng), 1};
        offer[2] = {OfferKind::Jade, -1, std::max(1, last.jade * 3 / 2)};
    }
    // 本层通过：魂玉、Boss 兵器（100%）、神通掉落；最后一层直接通关
    // bossPaths：本层 Boss 的类路径
    void FloorCleared(const std::vector<std::string_view>& bossPaths, bool replayed = false, bool noWeaponDrops = false) {
        if (phase != Phase::Fighting) return;
        last = {};
        // 回退重打只保留四分之一魂玉，不重复发兵器、神通或奖励选项。
        if (replayed) { last.jade = std::max(1, JadeFor(floor, IsZoneBoss(floor, floors), Unit(rng)) / 4); jade += last.jade; phase = Phase::Ready; return; }
        peak = std::max(peak, floor);
        last.jade = JadeFor(floor, IsZoneBoss(floor, floors), Unit(rng));
        jade += last.jade;
        if (!noWeaponDrops) for (auto path : bossPaths) {  // 2.0.0：成长模式 Boss 不再掉兵器，打败后上架驿站
            const int w = WeaponForPath(path);
            if (w < 0) continue;
            const bool dup = std::any_of(weapons.begin(), weapons.end(), [&](int x) { return kWeaponDrops[x].weapon == kWeaponDrops[w].weapon; });
            if (!dup) { weapons.push_back(w); last.weapons.push_back(w); }
        }
        for (Tier tier : {Tier::Normal, Tier::Rare, Tier::Lingxi}) {
            if (static_cast<int>(shentong.size()) >= kShentongBag) break;
            if (tier == Tier::Lingxi && lingxiGot) continue;
            if (!(Unit(rng) < ShentongChance(tier, floor, floors))) continue;
            std::vector<int> pool;
            for (int i = 0; i < kShentongCount; ++i) if (kShentong[i].tier == tier) pool.push_back(i);
            const int pick = pool[rng.Below(static_cast<int>(pool.size()))];
            shentong.push_back(pick); last.shentong.push_back(pick);
            if (tier == Tier::Lingxi) lingxiGot = true;
        }
        // v14：50 层正式局的最后一层不自动结束本轮（后续层预留），照常进奖励 → 层间；短局（试玩）打完仍通关
        if (floor >= floors && floors < 50) { phase = Phase::Won; return; }
        // 用户 2026-11-05：第 51 层（隐藏层）打完直接登顶结算，不再有奖励层
        if (!endless && (hidden || floor > 50) && floor >= floors) { phase = Phase::Won; return; }  // 正常局从第 50 层结算页点下一层进来的第 51 层同样算
        DrawOffer();
        phase = Phase::Reward;
    }
    bool Pick(int slot) {
        if (phase != Phase::Reward || slot < 0 || slot > 2) return false;
        const Offer o = offer[slot];
        switch (o.kind) {
            case OfferKind::Card: if (o.index < 0) return false; picked.push_back(o.index); mods.Add(kCards[o.index]); break;
            case OfferKind::Item: if (o.index < 0) return false; items[o.index] += o.amount; break;
            case OfferKind::Jade: jade += o.amount; break;
        }
        offer = {};
        phase = Phase::Ready;
        return true;
    }
    // 成长模式的三选一由新奖励模型（growthrun::Runner）发放：这里只结束奖励阶段，不碰旧的卡 / 道具 / 魂玉
    bool FinishReward() {
        if (phase != Phase::Reward) return false;
        offer = {};
        phase = Phase::Ready;
        return true;
    }
    // 道具：有就扣一个
    bool Consume(Item it) {
        const int i = static_cast<int>(it);
        if (items[i] <= 0) return false;
        --items[i];
        return true;
    }
    // 刷新令：三选一重抽
    bool Reroll() {
        if (phase != Phase::Reward || !Consume(Item::Reroll)) return false;
        DrawOffer();
        return true;
    }
    // 取出背包里第 slot 个神通（使用后消失），返回 kShentong 下标
    int TakeShentong(int slot) {
        if (slot < 0 || slot >= static_cast<int>(shentong.size())) return -1;
        const int s = shentong[slot];
        shentong.erase(shentong.begin() + slot);
        return s;
    }
    void Died() { if (phase == Phase::Fighting) phase = Phase::Lost; }
    void RecordShopDrops(const std::vector<std::string_view>& paths) {
        for (auto path : paths) {
            const int w = WeaponForPath(path);
            if (w >= 0 && std::find(shopDrops.begin(), shopDrops.end(), w) == shopDrops.end()) shopDrops.push_back(w);
        }
    }
    void RecordClearedRoute(const std::vector<std::string_view>& paths) {
        if (!shuffleBosses || floor < 1 || floor > 50 || paths.empty() || !clearedRoute[floor].empty()) return;
        for (auto path : paths) if (!path.empty()) clearedRoute[floor].emplace_back(path);
    }
    // Rewind progress without replacing the run seed, unlocked drops, or peak.
    void RewindForRebirth(int back, int mobWaves) {
        floor = std::max(0, back);
        if (mobWaves >= 0) { mobFloor = floor + 1; mobWave = mobWaves; }
        mobVenueFloor = 0; mobVenuePath.clear();
        bosses.clear(); phase = Phase::Ready;
        if (static_cast<int>(times.size()) > floor) times.resize(floor);
    }
    bool CanOpenStageShop(int nextBoss) const { return nextBoss > peak && nextBoss > shopFloor; }
    // 复活符：倒下时自动用掉，返回是否复活
    bool TryRevive() { return phase == Phase::Fighting && Consume(Item::Revive); }

    // ---- 局存档：纯文本 key=value，只在两层之间（ready / reward）保存；战斗中退出则回到开层前 ----
    std::string Save() const {
        std::string s = "v=1\n";
        auto line = [&](std::string_view k, const std::string& v) { s += std::string(k) + "=" + v + "\n"; };
        auto list = [](const auto& v) { std::string o; for (auto x : v) o += (o.empty() ? "" : ",") + std::to_string(x); return o; };
        line("id", id); line("seed", std::to_string(seed)); line("rng", std::to_string(rng.s)); line("floors", std::to_string(floors));
        line("floor", std::to_string(floor)); line("phase", phase == Phase::Reward ? "reward" : "ready");
        line("jade", std::to_string(jade)); line("picked", list(picked)); line("used", list(usedBosses));
        line("shuffle", std::to_string(shuffleBosses)); line("shopdrops", list(shopDrops));
        for (int f = 1; f <= 50; ++f) if (!clearedRoute[f].empty()) {
            std::string paths; for (const auto& path : clearedRoute[f]) paths += (paths.empty() ? "" : "|") + path;
            line("route" + std::to_string(f), paths);
        }
        line("items", list(items)); line("shentong", list(shentong)); line("lingxi", lingxiGot ? "1" : "0");
        line("weapons", list(weapons)); line("hp0", std::to_string(tuning.hp0)); line("adapt", std::to_string(adapt));
        line("times", list(times)); line("chal", std::to_string(chalOffered) + "," + std::to_string(chalDone));
        line("chal2", std::to_string(doneTimed) + "," + std::to_string(donePotion)); line("hidden", hidden ? "1" : "0"); line("endless", endless ? "1" : "0"); line("eslot", endlessSave ? "1" : "0"); if (!source.empty()) line("src", source);
        line("plan", std::to_string(plan));
        line("norm", std::format("{:.6f},{:.6f},{:.6f}", norm[0], norm[1], norm[2]));
        line("diff", std::to_string(difficulty));
        line("peak", std::to_string(peak));
        line("shopf", std::to_string(shopFloor));
        line("mob", std::to_string(mobFloor) + "," + std::to_string(mobWave));
        line("spfrom", std::to_string(specialFrom));
        if (mobVenueFloor > 0 && !mobVenuePath.empty()) line("mobvenue", std::to_string(mobVenueFloor) + "|" + mobVenuePath);
        line("normpool", std::format("{:.6f},{:.6f}", normPool[0], normPool[1]));
        if (bill.open) line("bill", std::format("{},{},{},{},{},{}", bill.floor, bill.base, bill.picked, bill.addPm, bill.mulPm, bill.snowPm));
        line("pool", audit ? "audit" : "verified");
        std::string of;
        for (auto& o : offer) of += (of.empty() ? "" : ",") + std::to_string(static_cast<int>(o.kind)) + ":" + std::to_string(o.index) + ":" + std::to_string(o.amount);
        line("offer", of);
        return s;
    }
    bool Load(const std::string& text) {
        Run r;
        r.specialFrom = -1;  // 4.0.0：没有 spfrom 字段 = 4.0 以前开的局，读完按进度算
        auto ints = [](const std::string& v) {
            std::vector<long long> o;
            size_t p = 0;
            while (p < v.size()) {
                size_t e = v.find(',', p);
                if (e == std::string::npos) e = v.size();
                if (e > p) o.push_back(std::stoll(v.substr(p, e - p)));
                p = e + 1;
            }
            return o;
        };
        bool ok = false, hasMobProgress = false;
        try {
            size_t p = 0;
            while (p < text.size()) {
                size_t e = text.find('\n', p);
                if (e == std::string::npos) e = text.size();
                std::string ln = text.substr(p, e - p);
                if (!ln.empty() && ln.back() == '\r') ln.pop_back();  // 手改 / 别的工具写成 CRLF 也能读（2026-11-05：版本行读成 "1\r" 被当坏档）
                p = e + 1;
                const auto eq = ln.find('=');
                if (eq == std::string::npos) continue;
                const std::string k = ln.substr(0, eq), v = ln.substr(eq + 1);
                if (k == "v") ok = v == "1";
                else if (k == "id") r.id = v;
                else if (k == "seed") r.seed = std::stoull(v);
                else if (k == "rng") r.rng.s = std::stoull(v);
                else if (k == "floors") r.floors = std::clamp(std::stoi(v), 1, 60);
                else if (k == "floor") r.floor = std::max(0, std::stoi(v));
                else if (k == "phase") r.phase = v == "reward" ? Phase::Reward : Phase::Ready;
                else if (k == "jade") r.jade = std::max(0, std::stoi(v));
                else if (k == "picked") { for (auto x : ints(v)) if (x >= 0 && x < kCardCount) { r.picked.push_back(int(x)); r.mods.Add(kCards[x]); } }
                else if (k == "used") { for (auto x : ints(v)) r.usedBosses.push_back(int(x)); }
                else if (k == "shuffle") r.shuffleBosses = v == "4" ? 4 : v == "3" ? 3 : v == "2" ? 2 : v == "1" ? 1 : 0;
                else if (k == "shopdrops") { for (auto x : ints(v)) if (x >= 0 && x < kWeaponDropCount && std::find(r.shopDrops.begin(), r.shopDrops.end(), int(x)) == r.shopDrops.end()) r.shopDrops.push_back(int(x)); }
                else if (k.starts_with("route")) {
                    const int f = std::stoi(k.substr(5));
                    if (f >= 1 && f <= 50 && v.size() <= 2048) {
                        size_t start = 0;
                        while (start < v.size() && r.clearedRoute[f].size() < 3) {
                            const size_t end = v.find('|', start); const auto path = v.substr(start, end == std::string::npos ? end : end - start);
                            if (!path.empty() && path.front() == '/') r.clearedRoute[f].push_back(path);
                            if (end == std::string::npos) break; start = end + 1;
                        }
                    }
                }
                else if (k == "items") { auto xs = ints(v); for (int i = 0; i < kItemCount && i < int(xs.size()); ++i) r.items[i] = int(std::max(0LL, xs[i])); }
                else if (k == "shentong") { for (auto x : ints(v)) if (x >= 0 && x < kShentongCount && r.shentong.size() < size_t(kShentongBag)) r.shentong.push_back(int(x)); }
                else if (k == "lingxi") r.lingxiGot = v == "1";
                else if (k == "weapons") { for (auto x : ints(v)) if (x >= 0 && x < kWeaponDropCount) r.weapons.push_back(int(x)); }
                else if (k == "hp0") r.tuning.hp0 = std::stod(v);
                else if (k == "norm") {
                    size_t a = 0;
                    for (int i = 0; i < 3 && a <= v.size(); ++i) {
                        const size_t b = v.find(',', a);
                        r.norm[i] = std::clamp(std::stod(v.substr(a, b == std::string::npos ? std::string::npos : b - a)), 0.0, 3.0);  // 1.0.1：也可能是抬高
                        if (b == std::string::npos) break;
                        a = b + 1;
                    }
                }
                else if (k == "diff") r.difficulty = std::clamp(std::stoi(v), 0, 1);
                else if (k == "bill") {
                    auto xs = ints(v);
                    if (xs.size() == 6 && xs[0] > 0 && xs[1] >= 0 && xs[2] >= 0 && xs[3] >= 0 && xs[4] >= 1000 && xs[5] >= 0 && xs[1] < (1LL << 30) && xs[2] < (1LL << 30) && xs[3] < 1000000 && xs[4] < 1000000 && xs[5] < 1000000)
                        r.bill = {true, int(xs[0]), int(xs[1]), int(xs[2]), int(xs[3]), int(xs[4]), int(xs[5])};
                }
                else if (k == "peak") r.peak = std::max(0, std::stoi(v));
                else if (k == "shopf") r.shopFloor = std::max(0, std::stoi(v));
                else if (k == "spfrom") r.specialFrom = std::clamp(std::stoi(v), 1, 51);
                else if (k == "mob") { auto xs = ints(v); if (xs.size() == 2) { hasMobProgress = true; r.mobFloor = int(std::max(0LL, xs[0])); r.mobWave = int(std::max(0LL, xs[1])); } }
                else if (k == "mobvenue") {
                    const auto sep = v.find('|');
                    if (sep != std::string::npos && sep > 0 && v.size() <= 1024) {
                        const int f = std::stoi(v.substr(0, sep));
                        const auto path = v.substr(sep + 1);
                        if (f >= 2 && f <= 50 && !path.empty() && path.front() == '/' && path.find('|') == std::string::npos) {
                            r.mobVenueFloor = f; r.mobVenuePath = path;
                        }
                    }
                }
                else if (k == "normpool") {
                    const auto c = v.find(',');
                    if (c != std::string::npos) { r.normPool[0] = std::clamp(std::stod(v.substr(0, c)), 0.0, 3.0); r.normPool[1] = std::clamp(std::stod(v.substr(c + 1)), 0.0, 3.0); }
                }
                else if (k == "times") { for (auto x : ints(v)) if (x >= 0 && r.times.size() < 60) r.times.push_back(int(x)); }
                else if (k == "chal") { auto xs = ints(v); if (xs.size() == 2) { r.chalOffered = int(std::max(0LL, xs[0])); r.chalDone = int(std::max(0LL, xs[1])); } }
                else if (k == "chal2") { auto xs = ints(v); if (xs.size() == 2) { r.doneTimed = int(std::max(0LL, xs[0])); r.donePotion = int(std::max(0LL, xs[1])); } }
                else if (k == "hidden") r.hidden = v == "1";
                else if (k == "endless") r.endless = v == "1";
                else if (k == "eslot") r.endlessSave = v == "1";
                else if (k == "src") r.source = v.substr(0, 120);
                else if (k == "adapt") r.adapt = std::clamp(std::stod(v), 0.6, 2.0);
                else if (k == "plan") r.plan = std::max(0, std::stoi(v));
                else if (k == "pool") r.audit = v == "audit";
                else if (k == "offer") {
                    size_t q = 0;
                    int slot = 0;
                    while (q < v.size() && slot < 3) {
                        size_t e2 = v.find(',', q);
                        if (e2 == std::string::npos) e2 = v.size();
                        const std::string t = v.substr(q, e2 - q);
                        q = e2 + 1;
                        const auto c1 = t.find(':'), c2 = t.find(':', c1 + 1);
                        if (c1 == std::string::npos || c2 == std::string::npos) continue;
                        r.offer[slot++] = {static_cast<OfferKind>(std::clamp(std::stoi(t.substr(0, c1)), 0, 2)), std::stoi(t.substr(c1 + 1, c2 - c1 - 1)), std::stoi(t.substr(c2 + 1))};
                    }
                }
            }
        } catch (...) { return false; }
        if (!ok || r.seed == 0 || (r.floor > r.floors && !r.endless)) return false;  // 无尽模式：层号会超过 floors（50）
        // 2.0.0 之前的局没有波次字段：用户 2026-11-08 决定不继承（改动太大），由 tower.cpp 归档后开新局
        r.legacy = !hasMobProgress;
        if (r.specialFrom < 0) r.specialFrom = tower::special::FromProgress(r.floor, r.mobFloor, r.mobWave);  // 4.0.0 老局：创意层按剩余比例放在还没打的小兵段
        if (r.endless && r.floor >= 51) r.endlessSave = true;  // 3.5.0 之前已在无尽层的局：直接就是无尽存档
        // A pending reward saved by an older build can contain retired content.
        // Replace only that slot; preserve other offers and saved bag indexes.
        if (r.phase == Phase::Reward) for (auto& o : r.offer) {
            if (o.kind == OfferKind::Card && !CardInRewardPool(o.index)) o.index = DrawCard(r.rng, r.Zone(), Fan);
            if (o.kind == OfferKind::Item && !ItemInRewardPool(o.index)) o.index = DrawItem(r.rng);
        }
        *this = r;
        return true;
    }
};

// 3.6.0 魂玉账单（纯函数，离线测试 rewards_test 覆盖）
inline Run::JadeBill BillFor(int shown, int base, double additive, double mul, double snowball) {
    Run::JadeBill b;
    if (base <= 0) return b;
    b.open = true; b.floor = std::max(1, shown); b.base = base;
    b.addPm = static_cast<int>(std::lround(std::clamp(additive, 0.0, 900.0) * 1000));
    b.mulPm = static_cast<int>(std::lround(std::clamp(mul, 1.0, 900.0) * 1000));
    b.snowPm = static_cast<int>(std::lround(std::clamp(snowball, 0.0, 900.0) * 1000));
    return b;
}
inline int BillTotal(const Run::JadeBill& b) {
    if (!b.open) return 0;
    const int sum = b.base + b.picked;
    return std::max(sum, balance::JadeTotal(sum, b.addPm / 1000.0, b.mulPm / 1000.0, b.snowPm / 1000.0));
}
inline std::string BillText(const Run::JadeBill& b) {
    const int total = BillTotal(b);
    return std::format("本层魂玉 {}，额外获得 {}，共 {}", b.base, total - b.base, total);
}

}  // namespace tower
