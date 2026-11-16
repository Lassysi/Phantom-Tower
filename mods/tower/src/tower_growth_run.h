// 影塔 v2 一局里的原生成长（纯逻辑，经 IGame 落到游戏里；离线测试接假游戏端）：
//   进塔套用塔档、每层收获、玩家在影塔菜单里升级 / 回退 / 点能力 / 洗点 / 领药 / 领饰品，
//   以及"玩家在游戏原生界面里直接升级 / 点能力"的对账：读回差异，点数够就记账扣点，不够就退回。
// 原生界面的升级 / 激活是游戏脚本直调（2026-10-31 追踪：不经 ProcessEvent），无法逐次拦截，所以用对账接管。
#pragma once
#include <algorithm>
#include <format>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "tower_growth.h"
#include "tower_hex.h"
#include "tower_hex_rewards.h"
#include "tower_trade.h"
#include "tower_growth_text.h"
#include "tower_rewards.h"
#include "tower_session.h"

namespace tower::growthrun {

using growth::Err;

struct Runner {
    bool on = false;                 // 本局走原生成长
    hex::State hexes;                // 独立随机流与持有状态；旧局没有此字段时关闭
    hex::AttributeBurst hexAttributes;
    hex::ForgeBatch forgeBatch;      // 3.6.1 批量锻造：已付款、待逐组选择的属性三选一
    struct HexJournal { bool active = false; int jade = 0; std::array<int, kItemCount> items{}; } hexJournal;
    uint64_t hexForgePurchases = 0;
    std::array<int, 4> arsenal{-1, -1, -1, -1};  // 武库扩建：玩家在装备页选的第 3、4 把（主武器 2 + 影之武装 2，kWeapons 下标；-1 = 按获得顺序自动补）
    int difficulty = 0;              // 与 Run 同步，不改变成长档格式
    growth::Profile profile;
    session::Session session;
    std::vector<int> med;            // 待选的新药（kMedicines 下标），选一个
    std::vector<int> acc;            // 待选的饰品匣（kAccessories 下标），选一个
    std::vector<rewards::Offer> bonus;  // 1.0.1：额外挑战成功的额外三选一（领完本层普通奖励后选）
    // N9i：回收过的东西（编码）。从下一次驿站起进随机池；再买回来（持有）就不进池，再回收再进（随存档）
    std::vector<int> recycled, returned;  // returned 只为兼容 n9-i 早期档，读进来并入 recycled
    static rewards::Offer CodeOffer(int code) {
        if (code >= trade::kAccCode) return {rewards::Kind::Accessory, code - trade::kAccCode, 1, 1};
        if (code >= trade::kMedCode) return {rewards::Kind::Medicine, code - trade::kMedCode, 1, 1};
        return {rewards::Kind::Weapon, code, 1, 1};
    }
    mutable std::vector<rewards::Offer> returnedOffers;
    std::vector<int> pendingRecycle;  // 本次驿站刚回收的：本次货架不变，下一次驿站才进池
    bool backpackSynced = false;      // 3.3.1：已按原生背包认回残留兵器（每局一次）
    int hexRewardSecond = 0, hexBonusSecond = 0;  // 3.6.0：投掷狂人本页已多选了几次（0 = 第 1 次；Lv2 最多到 2）
    std::vector<std::string> held;   // 本层 Boss 关联尚未核实、没发的兵器 ID
    std::string note;                // 最近一次对账 / 操作的说明（界面显示）
    // 新奖励模型（notes/tower-rewards-model.md）：成长模式的通关三选一、祝福、驿站、准备道具、快捷栏都在这里，随局存档保存
    growth::Blessings bless;
    std::array<rewards::Offer, 3> offers{};
    bool offered = false;                 // offers 是本层待领取的一组
    std::set<std::pair<int, int>> seen;   // 本局展示过的装备
    uint64_t rewardRng = 0;               // 奖励 / 货架的随机状态（读档不重掷）
    rewards::Shop shop;
    rewards::Prep prep;
    rewards::QuickBar quick;
    rewards::Catalog catalog = rewards::Catalog::Default();  // 引擎开局 / 继续时用 Codex 的进度表覆盖
    // v14（用户 2026-11-05）：属性奖励累计；刷新令（第四位，不占普通位）；本组属于哪一层（每层重新抽，不复用上一层缓存）
    rewards::AttrTotals attrs;
    bool refresh = false;
    int offerFloor = -1;
    std::vector<rewards::Offer> lastGroup;  // 上一层最终那一组（相邻两层避免整组相同）
    trade::Visit tradeVisit;                 // 3.1.0 驿站交易：本次驿站已用过的交易（每种限 1 次），存档 gr.trade
    std::string equip;                       // 保存时实际装备（主|影|药|饰品，逗号分隔；用户 2026-11-05：继续上一局要还原）
    int draws = 0;                          // 本轮抽取次数（随机流按它推进，读档不会白得一次刷新）
    // v3（用户 2026-11-12）：驿站各类已买次数（价格复利上涨），MOD 道具按每一种计；随局存档 gr.buys
    std::array<int, balance::kBuyCount> buys{};
    std::array<int, kItemCount> itemBuys{};
    void CountBuy(const rewards::Offer& o) {
        const auto b = rewards::BuyOf(o);
        if (b == balance::Buy::Mod) { if (o.index >= 0 && o.index < kItemCount) ++itemBuys[static_cast<size_t>(o.index)]; }
        else ++buys[static_cast<size_t>(b)];
    }

    rewards::Context Ctx(int floor, int floors, const std::array<int, kItemCount>& items) const {
        rewards::Context c;
        c.floor = floor; c.floors = floors; c.difficulty = difficulty; c.profile = &profile; c.blessings = &bless; c.catalog = &catalog; c.items = items; c.seen = seen;
        attrs.tier = rewards::EndlessTier(floor);
        c.attrs = &attrs; c.lastFloor = lastGroup; c.modPurchaseLimit = hexes.Has(hex::Investor) ? 3 : 1; c.weaponPricePct = hexes.Has(hex::Arsenal) ? hex::kArsenalWeaponPricePct : 100;
        c.shopExtraWeapons = hexes.Has(hex::Arsenal) ? balance::kArsenalWeapons : 0; c.shopExtraAll = hexes.Has(hex::Investor) ? balance::kInvestorExtra : 0;
        c.shopBothPoints = hexes.Has(hex::Investor); c.shopModItems = hexes.Has(hex::Investor) ? balance::kInvestorMod : balance::kShelfMod;
        c.buys = &buys; c.itemBuys = &itemBuys;
        returnedOffers.clear();
        for (int code : recycled) if (std::find(pendingRecycle.begin(), pendingRecycle.end(), code) == pendingRecycle.end()) returnedOffers.push_back(CodeOffer(code));
        c.returned = &returnedOffers;
        return c;
    }
    void GrantHexChoice(hex::Id id) {
        if (id == hex::Forge) { profile.Earn(20, 0); profile.soulTokens += 5; }
#ifdef PBZ_HEX_TEST
        // QA：武器扩建验收要 4 主 + 4 影；开局只有 3 主 2 影，按表序补齐（正式包不送）
        if (id == hex::Arsenal) for (int kind = 0; kind < 2; ++kind) {
            int have = 0;
            for (const auto& w : profile.weapons) if (growth::kWeapons[w.index].kind == kind && !rewards::RetiredWeapon(w.index)) ++have;
            for (int i = 0; i < growth::kWeaponCount && have < 4; ++i)
                if (growth::kWeapons[i].kind == kind && !rewards::RetiredWeapon(i) && profile.Find(i) < 0 && profile.AddWeapon(i)) ++have;
        }
#endif
    }
    // 3.6.1 批量锻造：一次买 want 个（≤ 买得起的、≤ 50），返回实际买了几个；0 = 没买
    int BeginForgeBatch(uint64_t runSeed, int& jade, int want, int floor, int floors, const std::array<int, kItemCount>& items) {
        const int price = hex::ForgePrice(floor, floors);
        if (!hexes.Has(hex::AttributeStack) || hexAttributes.remaining || forgeBatch.Active() || hexJournal.active || price <= 0) return 0;
        const int n = std::min({std::max(0, want), hex::kForgeBatchMax, jade / price});
        if (n <= 0) return 0;
        hex::ForgeBatch batch;
        batch.Begin(runSeed ^ ((hexForgePurchases + 1) * 0x9E3779B97F4A7C15ull), n, hexes.ThrowerMul(), price, Ctx(floor, floors, items));
        if (!batch.Active()) return 0;
        hexForgePurchases += static_cast<uint64_t>(n); forgeBatch = std::move(batch); jade -= n * price;
        hexJournal = {true, jade, items};
        return n;
    }
    bool BeginAttributeForge(uint64_t runSeed, int& jade, int floor, int floors, const std::array<int, kItemCount>& items) {
        if (!hexes.Has(hex::AttributeStack) || hexAttributes.remaining || forgeBatch.Active() || hexJournal.active || jade < hex::ForgePrice(floor, floors)) return false;
        const uint64_t next = hexForgePurchases + 1;
        hex::AttributeBurst nextRewards;
        nextRewards.BeginPurchaseMul(runSeed ^ (next * 0x9E3779B97F4A7C15ull), hexes.ThrowerMul(), Ctx(floor, floors, items));
        if (!nextRewards.remaining) return false;
        hexForgePurchases = next; hexAttributes = std::move(nextRewards); jade -= hex::ForgePrice(floor, floors);
        hexJournal = {true, jade, items};
        return true;
    }
    growth::Xs Rng(uint64_t seed) { return growth::Xs{rewardRng ? rewardRng : (seed ^ 0xD1B54A32D192ED03ull) | 1}; }
    // 通关后抽一组（v14）：三个普通候选 + 约 30% 第四位刷新令。reroll = 选了刷新令：避开刷新前那一组再抽
    void DrawRewards(int floor, int floors, uint64_t seed, const std::array<int, kItemCount>& items, bool reroll = false) {
        auto c = Ctx(floor, floors, items);
        c.soulFloor = rewards::SoulFloor(seed, floor, floors);
        if (reroll && offered) c.avoid.assign(offers.begin(), offers.end());
        growth::Xs r{(seed ^ 0xD1B54A32D192ED03ull ^ (0x9E3779B97F4A7C15ull * static_cast<uint64_t>(floor * 64 + ++draws))) | 1};
        r.Next(); r.Next();
        if (floors >= 50 && floor == 1) {  // 第 1 层后：随机开局武器三选一，没有刷新令
            offers = rewards::DrawStarterWeapons(r, c);
            refresh = false;
        } else {
            const auto g = rewards::Draw14(r, c);
            offers = g.normal;
            refresh = g.refresh || (!reroll && hexes.Has(hex::Fortune));  // 天赐良机：每页至少一次免费刷新
        }
        rewardRng = r.s;
        offered = true;
        offerFloor = floor;
        for (auto& o : offers) if (o.kind == rewards::Kind::Weapon || o.kind == rewards::Kind::Accessory || o.kind == rewards::Kind::Medicine || o.kind == rewards::Kind::Refine)
            seen.insert({static_cast<int>(o.kind), o.index});
    }
    // 本层的候选是否还有效（层号对不上 = 上一层残留，必须重抽）
    bool OffersFor(int floor) const { return offered && offerFloor == floor && std::none_of(offers.begin(), offers.end(), rewards::RetiredOffer); }
    // Preserve owned items and valid saved choices; remove only retired shelf
    // entries. A completely invalidated bonus is redrawn, not silently lost.
    void RetirePendingWeapons(int floor, int floors, uint64_t seed, const std::array<int, kItemCount>& items) {
        const auto before = bonus.size();
        std::erase_if(bonus, rewards::RetiredOffer);
        if (before && bonus.empty()) DrawBonus(floor, floors, seed, items);
        const auto shelfBefore = shop.shelf.size();
        std::erase_if(shop.shelf, [](const rewards::Ware& w) { return rewards::RetiredOffer(w.offer); });
        if (shop.shelf.size() != shelfBefore) ++shop.version;
    }
    // 选刷新令：立即重抽，不消耗本层领奖机会
    bool UseRefresh(int floor, int floors, uint64_t seed, const std::array<int, kItemCount>& items) {
        if (!OffersFor(floor) || !refresh) return false;
        DrawRewards(floor, floors, seed, items, true);
        return true;
    }
    // 发放一个奖励项（三选一与商店共用）。返回 false = 没发出去（调用方不结算 / 不扣钱）
    bool holdSync = false, syncOwed = false;  // 不存档：读档进塔时 Sync(first) 会按塔档补齐
    bool Grant(session::IGame& g, const rewards::Offer& o, std::array<int, kItemCount>& items, int& jade, bool sync = true) {
        using rewards::Kind;
        switch (o.kind) {
            case Kind::Blessing: return bless.Add(o.index, o.amount) > 0;
            case Kind::TemperPack: profile.Earn(o.amount, 0); return true;
            case Kind::CultivatePack: profile.Earn(0, o.amount); return true;
            case Kind::Weapon: if (rewards::RetiredWeapon(o.index) || !profile.AddWeapon(o.index)) return false; break;
            case Kind::Accessory: case Kind::Refine: if (!profile.AddAccessory(o.index)) return false; break;
            case Kind::Medicine: if (profile.HasMedicine(o.index)) return false; profile.AddMedicine(o.index); break;
            case Kind::Item: if (o.index < 0 || o.index >= kItemCount || items[o.index] >= (o.index == static_cast<int>(Item::Foresight) ? (hexes.Has(hex::Investor) ? 3 : 1) : rewards::kItemStackCap)) return false; ++items[o.index]; return true;
            case Kind::Jade: jade += o.amount; return true;
            case Kind::Attr: return o.amount > 0 && attrs.Room(o.index) >= o.amount && attrs.Add(o.index, o.amount) == o.amount;
            case Kind::SoulToken: profile.soulTokens += o.amount; return true;
        }
        // 用户 2026-11-12：菜单开着时不同步到原生（原生发放会弹"获得"界面，和影塔菜单抢焦点）；记下欠账，菜单全关后一次补齐、一起弹
        if (sync) { if (holdSync) syncOwed = true; else session.Sync(g, profile); }  // 先持久化奖励后再同步到原生（塔档里）
        return true;
    }
    // 领取：先复核仍然合法，再发放；失败保留这一组
    Err Claim(session::IGame& g, int slot, int floor, int floors, std::array<int, kItemCount>& items, int& jade, rewards::Offer* got) {
        if (!offered || slot < 0 || slot > 2) return Err::Bad;
        const auto o = offers[slot];
        if (!rewards::StillValid(o, Ctx(floor, floors, items))) return Err::AtCap;
        if (!Grant(g, o, items, jade)) return Err::Bad;
        *got = o;
        lastGroup.assign(offers.begin(), offers.end());
        offers = {};
        offered = false;
        refresh = false;
        offerFloor = -1;
        return Err::Ok;
    }
    void OpenShop(int floor, int floors, uint64_t seed, const std::array<int, kItemCount>& items, const std::vector<rewards::Offer>& supplement = {}) {
        shop = {};
        shop.floor = floor;
        shop.seed = seed | 1;
        pendingRecycle.clear();  // 上次驿站回收的，从这次起进池子
        rewards::Stock14(shop, Ctx(floor, floors, items), supplement);
    }
    // 买完 / 读档后重排货架（已买下的消失，其余原样保留）
    void Restock(int floor, int floors, const std::array<int, kItemCount>& items, const std::vector<rewards::Offer>& supplement = {}) {
        if (!shop.Open()) return;
        const auto version = shop.version;
        rewards::Stock14(shop, Ctx(floor, floors, items), supplement);
        shop.version = std::max(shop.version, version + 1);
    }
    // 1.0.1：额外挑战成功 → 抽额外三选一（按层种子固定，读档不重掷）；返回抽到几项
    int DrawBonus(int floor, int floors, uint64_t seed, const std::array<int, kItemCount>& items) {
        growth::Xs r{(seed ^ 0xB0B05EEDull ^ (0xD6E8FEB86659FD93ull * static_cast<uint64_t>(hexBonusSecond)) ^ (0x9E3779B97F4A7C15ull * static_cast<uint64_t>(floor + 101))) | 1};
        r.Next();
        bonus = rewards::DrawBonus3(r, Ctx(floor, floors, items));
        return static_cast<int>(bonus.size());
    }
    // 3.1.0（玩家反馈：额外奖励选了魂玉没加上）：原来魂玉发进一个局部变量就丢了，改为发进本局魂玉
    Err PickBonus(session::IGame& g, int slot, int floor, int floors, std::array<int, kItemCount>& items, int& jade, rewards::Offer* got) {
        if (slot < 0 || slot >= static_cast<int>(bonus.size())) return Err::Bad;
        const auto o = bonus[slot];
        if (!rewards::StillValid(o, Ctx(floor, floors, items))) return Err::AtCap;
        if (!Grant(g, o, items, jade)) return Err::Bad;
        bonus.clear();
        *got = o;
        return Err::Ok;
    }
    // 额外挑战奖励：发一件进度内非 Boss 关联原生装备 / 药（原生获取界面）
    bool GrantBonusNative(session::IGame& g, int floor, int floors, uint64_t seed, rewards::Offer* got) {
        growth::Xs r{(seed ^ 0xB0B05EEDull ^ (0x9E3779B97F4A7C15ull * static_cast<uint64_t>(floor + 101))) | 1};
        r.Next();
        std::array<int, kItemCount> none{};
        rewards::Offer o;
        if (!rewards::DrawBonusNative(r, Ctx(floor, floors, none), &o)) return false;
        int jade = 0;
        if (!Grant(g, o, none, jade)) return false;
        *got = o;
        return true;
    }

    // 进塔：塔档起点（黑伤 + 选定影之武装 1 级、药葫芦、能力全关），试玩送一些点；备份 → 沙盒 → 成就 → 套用
    bool Start(session::IGame& g, int shaqiChoice, bool trial, std::string* why) {
        profile = growth::Profile::Start(shaqiChoice);
        if (trial) profile.TrialKit();
        med.clear(); acc.clear(); note.clear();
        return Enter(g, why);
    }
    // 独立塔档：塔档已经写进塔内存档并载入，只登记在塔里（不再在原档上改等级）
    void Adopt(const growth::Profile& p) {
        profile = p;
        session.Adopt();
        on = true;
    }
    void Release() { session.Release(); on = false; }
    // 继续上一局：塔档从局存档读回，重新进塔
    bool Resume(session::IGame& g, const growth::Profile& p, std::string* why) {
        profile = p;
        return Enter(g, why);
    }

    // 每层通过：固定收获（点数 / 药瓶等级 / 新药 / 饰品匣 / 铸魂令）+ Boss 兵器 100% 掉落（drops：兵器 ID 与种类）
    // bosses：本层实际 Boss 数（固定收获按实际层型，见 PlanFor）
    std::string FloorCleared(session::IGame& g, int floor, int floors, uint64_t seed, const std::vector<std::pair<std::string, int>>& drops, int bosses = -1) {
        const auto plan = growth::PlanFor(floor, floors, bosses);
        profile.Earn(plan.temper, plan.cultivate);
        if (plan.medLevel > profile.medLevel) profile.medLevel = std::min(plan.medLevel, growth::kMedLevelMax);
        if (plan.soulToken) ++profile.soulTokens;
        growth::Xs r{seed ^ (0x9E3779B97F4A7C15ull * static_cast<uint64_t>(floor + 1))};
        // 固定药匣 / 饰品匣与三选一、商店同一份装备进度表合同（313987b：固定匣发出了待核的绝地求生令等三件）
        if (plan.medTier && plan.medChoices) med = growth::DrawMedicines(r, plan.medTier, plan.medChoices, profile, [&](int m) { return Admit(rewards::Cat::Medicine, m, floor, floors, true); });
        if (plan.accessoryBox) acc = growth::DrawAccessoryBox(r, profile, [&](int a) { return Admit(rewards::Cat::Accessory, a, floor, floors, true); });
        int got = 0;
        held.clear();
        // 2026-11-03 用户选择：冻结原生战后链，按已核 Boss 关联独立发武器。
        // Boss 击杀是获得条件，不再借用三选一/商店的首次入池区；未知关联仍不发。
        for (auto& [id, kind] : drops)
            if (const int w = growth::WeaponIndex(id, kind); w >= 0) {
                if (!tower::EarnedBossWeapon(id, kind)) { held.push_back(id); continue; }
                if (profile.AddWeapon(w)) ++got;
            }
        session.Sync(g, profile);
        return growth::text::Gains(plan, got);
    }
    // 装备进度表准入：reward=true 时还要求合同允许进奖励（固定匣同三选一）
    bool Admit(rewards::Cat c, int index, int floor, int floors, bool reward) const {
        const auto* gate = catalog.Find(c, index);
        return gate && gate->zone >= 0 && gate->zone <= rewards::ZoneOf(floor, floors) && (!reward || gate->reward);
    }
    // 读回的旧局存档里待选的匣（合同生效前抽的）：按合同重新过滤，不发待核的
    void Readmit(int floor, int floors) {
        std::erase_if(med, [&](int m) { return !Admit(rewards::Cat::Medicine, m, floor, floors, true); });
        std::erase_if(acc, [&](int a) { return !Admit(rewards::Cat::Accessory, a, floor, floors, true); });
    }

    // ---- 影塔菜单里的操作：先改塔档，再同步到游戏
    Err Upgrade(session::IGame& g, int w) {
        profile.temperMaster = hexes.Has(hex::TemperMaster);
        const auto before = profile;
        const int cap = hexes.Has(hex::Forge) ? 60 : growth::kWeaponCap;
        const auto result = profile.Upgrade(w, cap);
        if (result != Err::Ok) return result;
        session.Sync(g, profile);
        const int index = profile.Find(w);
        if (index >= 0 && profile.weapons[index].level > growth::kWeaponCap && session.phase() == session::Phase::Inside &&
            g.WeaponLevel(growth::kWeapons[w].id, growth::kWeapons[w].kind) != profile.weapons[index].level) {
            profile = before; session.Sync(g, profile); return Err::Bad;
        }
        return Err::Ok;
    }
    Err Downgrade(session::IGame& g, int w, int to) { profile.temperMaster = hexes.Has(hex::TemperMaster); return Apply(g, profile.Downgrade(w, to)); }
    Err Activate(session::IGame& g, int n, int clearedFloors = 1 << 20) { return Apply(g, profile.Activate(n, clearedFloors)); }
    Err Snap(session::IGame& g, int w) { return Apply(g, profile.Snap(w)); }
    void ResetNodes(session::IGame& g) { profile.ResetNodes(); session.Sync(g, profile); }
    Err PickMedicine(session::IGame& g, int slot) {
        if (slot < 0 || slot >= static_cast<int>(med.size())) return Err::Bad;
        if (const auto* gate = catalog.Find(rewards::Cat::Medicine, med[slot]); !gate || gate->zone < 0 || !gate->reward) return Err::Bad;  // 发放前再核一次
        profile.AddMedicine(med[slot]);
        med.clear();
        session.Sync(g, profile);
        return Err::Ok;
    }
    Err PickAccessory(session::IGame& g, int slot) {
        if (slot < 0 || slot >= static_cast<int>(acc.size())) return Err::Bad;
        if (const auto* gate = catalog.Find(rewards::Cat::Accessory, acc[slot]); !gate || gate->zone < 0 || !gate->reward) return Err::Bad;
        profile.AddAccessory(acc[slot]);
        acc.clear();
        session.Sync(g, profile);
        return Err::Ok;
    }

    // 对账：原生界面里升过的兵器 / 点过的能力 → 点数够就扣点接受，不够（或超出塔内上限 / 前置不满足）就退回。
    // 返回本次说明（没有变化返回空）。只看塔档里的兵器和全部节点；塔档以外的兵器在进塔时已降到 1 级，
    // full=true 时也检查它们（被原生升过的退回 1 级）
    std::string Reconcile(session::IGame& g, bool full = false) {
        if (!on || session.phase() != session::Phase::Inside) return {};
        std::vector<std::string> took, back;
        for (auto& w : profile.weapons) {
            const auto& d = growth::kWeapons[w.index];
            const int native = g.WeaponLevel(d.id, d.kind);
            if (native <= w.level) continue;  // 低了的由 Sync 补回
            const int cap = std::min(hexes.Has(hex::Forge) ? 60 : growth::kWeaponCap, d.maxLevel);
            const int want = std::min(native, cap) - w.level;
            const int pay = std::min(want, profile.temper);
            if (pay > 0) { profile.temper -= pay; w.level += pay; if (w.spent >= 0) w.spent += pay; took.push_back(std::format("{} +{} 级（淬炼点 −{}）", d.name, pay, pay)); }
            if (w.level < native) back.push_back(std::format("{} 退回 {} 级", d.name, w.level));
        }
        if (full)
            for (int kind : {0, 1})
                for (const auto& id : g.OwnedWeapons(kind)) {
                    const int wi = growth::WeaponIndex(id, kind);
                    if (wi >= 0 && profile.Find(wi) >= 0) continue;
                    if (g.WeaponLevel(id, kind) > 1) { g.SetWeaponLevel(id, kind, 1); back.push_back(std::format("{} 不是塔内兵器，退回 1 级", id)); }
                }
        // 节点：按线 / 列顺序逐级接受（前一个接受了，后面的前置才满足），两遍覆盖跨线的榕树前置
        std::array<int, growth::kNodeCount> native{};
        for (int i = 0; i < growth::kNodeCount; ++i) native[i] = g.NodeLevel(growth::kNodes[i].id);
        for (int pass = 0; pass < 2; ++pass)
            for (int i = 0; i < growth::kNodeCount; ++i)
                while (native[i] > profile.nodes[i] && profile.Activate(i) == Err::Ok)
                    took.push_back(std::format("{} → {} 级（修为点 −{}）", growth::kNodes[i].name, profile.nodes[i], growth::NodeCost(i)));
        for (int i = 0; i < growth::kNodeCount; ++i)
            if (native[i] > profile.nodes[i] && native[i] >= 0) back.push_back(std::format("{} 退回 {} 级", growth::kNodes[i].name, profile.nodes[i]));
        if (took.empty() && back.empty()) return {};
        session.Sync(g, profile);  // 退回多出来的、补上低了的
        note.clear();
        if (!took.empty()) note += "原生界面里的升级已记账：" + growth::text::Join(took);
        if (!back.empty()) note += std::string(note.empty() ? "" : "\n") + "点数不足或超出塔内上限，已退回：" + growth::text::Join(back);
        return note;
    }

    // 出塔：撤销 + 把进塔基准放回内存；之后由玩家回标题 → 继续，进完档才关沙盒（Tick）
    bool Exit(session::IGame& g, std::string* why) {
        if (!on) return true;
        if (!session.Exit(g)) { *why = session.error(); return false; }
        return true;
    }
    bool Exiting() const { return session.phase() == session::Phase::ExitReload; }
    // 空闲时轮询：进档完成 → 关沙盒、恢复成就；返回 true 表示刚完成出塔
    bool Tick(session::IGame& g) {
        if (!Exiting()) return false;
        session.Tick(g);
        if (session.phase() != session::Phase::Outside) return false;
        on = false;
        return true;
    }

    // ---- 局存档：塔档行（g.*）+ 待选（gr.*）
    std::string Save() const {
        std::string s = profile.Save();
        auto list = [](const std::vector<int>& v) { std::string o; for (size_t i = 0; i < v.size(); ++i) o += (i ? "," : "") + std::to_string(v[i]); return o; };
        s += "gr.med=" + list(med) + "\ngr.acc=" + list(acc) + "\n";
        std::string bn;
        for (auto& o : bonus) bn += " " + rewards::SaveOffer(o);
        s += std::format("gr.bonus={}{}\n", bonus.size(), bn);
        s += std::format("gr.hexpages={} {}\n", hexRewardSecond, hexBonusSecond);
        s += "gr.hexattrs=" + hexAttributes.Save() + "\n";
        s += std::format("gr.hexforge={}\n", hexForgePurchases);
        if (forgeBatch.Active()) s += "gr.forgebatch=" + forgeBatch.Save() + "\n";
        if (hexes.seenUp || hexes.away) s += (hexes.seenUp.hi || hexes.away.hi) ? std::format("gr.hexseen={} {} {} {}\n", hexes.seenUp.lo, hexes.away.lo, hexes.seenUp.hi, hexes.away.hi) : std::format("gr.hexseen={} {}\n", hexes.seenUp.lo, hexes.away.lo);  // 3.7.0：编号 64 以上时多写两个高位字  // 3.6.2 升级卡见过 / 本页刷掉过
        {  // 4.1.0 海克斯冷却：编号:剩余次数（只写冷却中的；旧档没有这行 = 全部不在冷却）
            std::string c;
            for (int id = 0; id < hex::Count; ++id) if (const int n = hexes.cool[static_cast<size_t>(id)]) c += std::format(" {}:{}", id, n);
            if (!c.empty()) s += "gr.hexcool=" + c.substr(1) + "\n";
        }
        s += std::format("gr.hexcnt={} {}\n", hexes.asceticStacks, hexes.flawlessStacks);  // F 批：苦修 / 无伤大师层数
        s += std::format("gr.arsenal={} {} {} {}\n", arsenal[0], arsenal[1], arsenal[2], arsenal[3]);
        { std::string b; for (int x : buys) b += std::format(" {}", x); b += " |"; for (int x : itemBuys) b += std::format(" {}", x); s += "gr.buys=" + b + "\n"; }
        if (hexJournal.active) {
            s += std::format("gr.hexjournal={}", hexJournal.jade);
            for (int item : hexJournal.items) s += std::format(" {}", item);
            s += "\n";
        }
        // 新奖励模型
        std::string b;
        for (int i = 0; i < growth::kBlessCount; ++i) b += (i ? "," : "") + std::to_string(bless.stacks[i]);
        s += "gr.bless=" + b + "\n";
        std::string of;
        for (auto& o : offers) of += (of.empty() ? "" : " ") + rewards::SaveOffer(o);
        s += std::format("gr.offer={} {}\ngr.rng={}\n", offered ? 1 : 0, of, rewardRng);
        std::string sn;
        for (auto& [k, i] : seen) sn += (sn.empty() ? "" : ",") + std::format("{}:{}", k, i);
        s += "gr.seen=" + sn + "\n";
        s += "gr.shop=" + rewards::Save(shop, prep, quick) + "\n";
        std::string at;
        for (int i = 0; i < rewards::kAttrCount; ++i) at += (i ? "," : "") + std::to_string(attrs.pm[i]);
        s += "gr.attr=" + at + "\n";
        s += std::format("gr.v14={} {} {}\n", offerFloor, refresh ? 1 : 0, draws);
        std::string lg;
        for (auto& o : lastGroup) lg += (lg.empty() ? "" : " ") + rewards::SaveOffer(o);
        s += std::format("gr.last={} {}\n", lastGroup.size(), lg);
        if (!equip.empty()) s += "gr.equip=" + equip + "\n";
        if (tradeVisit.floor >= 0) s += std::format("gr.trade={} {}\n", tradeVisit.floor, tradeVisit.used);
        if (!recycled.empty()) s += "gr.recycled=" + list(recycled) + "\n";
        if (!pendingRecycle.empty()) s += "gr.pendrec=" + list(pendingRecycle) + "\n";
        if (backpackSynced) s += "gr.bpsync=1\n";
        if (hexes.enabled) s += "gr.hex=" + hexes.Save() + "\n";
        return s;
    }
    static bool Load(std::string_view text, Runner* out) {
        std::pair<int, int> hexCountsLoaded{0, 0};
        std::pair<hex::Mask, hex::Mask> hexSeenLoaded{}; bool hasHexSeen = false;  // 3.6.2
        std::array<uint8_t, hex::Count> hexCoolLoaded{};  // 4.1.0
        growth::Profile p;
        if (!growth::Profile::Load(text, &p, 60)) return false;
        Runner r;
        r.profile = p;
        bool hexSeen = false;
        auto ints = [](std::string_view v, int limit) {
            std::vector<int> o;
            size_t a = 0;
            while (a < v.size()) {
                size_t b = v.find(',', a);
                if (b == std::string_view::npos) b = v.size();
                int x = 0;
                bool okNum = b > a;
                for (size_t i = a; i < b; ++i) { if (v[i] < '0' || v[i] > '9') { okNum = false; break; } x = x * 10 + (v[i] - '0'); }
                if (okNum && x < limit) o.push_back(x);
                a = b + 1;
            }
            return o;
        };
        size_t a = 0;
        while (a < text.size()) {
            size_t b = text.find('\n', a);
            if (b == std::string_view::npos) b = text.size();
            auto line = text.substr(a, b - a);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (line.rfind("gr.med=", 0) == 0) r.med = ints(line.substr(7), growth::kMedicineCount);
            if (line.rfind("gr.recycled=", 0) == 0) r.recycled = ints(line.substr(12), trade::kAccCode + growth::kAccessoryCount);
            if (line.rfind("gr.pendrec=", 0) == 0) r.pendingRecycle = ints(line.substr(11), trade::kAccCode + growth::kAccessoryCount);
            if (line == "gr.bpsync=1") r.backpackSynced = true;
            if (line.rfind("gr.equip=", 0) == 0) r.equip = std::string(line.substr(9));
            if (line.rfind("gr.trade=", 0) == 0) { std::istringstream in{std::string(line.substr(9))}; int f = -1; unsigned u = 0; if (in >> f >> u) { r.tradeVisit.floor = f; r.tradeVisit.used = u & 7u; } }
            if (line.rfind("gr.hex=", 0) == 0) {
                if (hexSeen || !hex::State::Load(line.substr(7), &r.hexes) || !r.hexes.enabled) return false;
                hexSeen = true;
            }
            if (line.rfind("gr.hexpages=", 0) == 0) {
                std::istringstream in{std::string(line.substr(12))}; int normal = -1, extra = -1;
                if (!(in >> normal >> extra) || normal < 0 || normal > 2 || extra < 0 || extra > 2) return false;  // 3.6.0 投掷狂人 Lv2 可到 2
                r.hexRewardSecond = std::clamp(normal, 0, 2); r.hexBonusSecond = std::clamp(extra, 0, 2);
            }
            if (line.rfind("gr.hexattrs=", 0) == 0 && !hex::AttributeBurst::Load(line.substr(12), &r.hexAttributes)) return false;
            if (line.rfind("gr.forgebatch=", 0) == 0 && !hex::ForgeBatch::Load(line.substr(14), &r.forgeBatch)) return false;
            if (line.rfind("gr.hexseen=", 0) == 0) { std::istringstream in{std::string(line.substr(11))}; uint64_t a = 0, b = 0, ah = 0, bh = 0; if (in >> a >> b) { in >> ah >> bh; hexSeenLoaded = {hex::Mask{a, ah} & hex::kAllMask, hex::Mask{b, bh} & hex::kAllMask}; hasHexSeen = true; } }
            if (line.rfind("gr.hexcool=", 0) == 0) {  // 4.1.0：读不懂的项跳过（冷却只影响出卡，不值得因此拒档）
                std::istringstream in{std::string(line.substr(11))}; std::string tok;
                while (in >> tok) {
                    const auto colon = tok.find(':');
                    if (colon == std::string::npos) continue;
                    const int id = std::atoi(tok.substr(0, colon).c_str()), n = std::atoi(tok.substr(colon + 1).c_str());
                    if (id >= 0 && id < hex::Count && n > 0) hexCoolLoaded[static_cast<size_t>(id)] = static_cast<uint8_t>(std::min(n, hex::State::kCoolRerolled));
                }
            }
            if (line.rfind("gr.buys=", 0) == 0) {  // v3 已买次数；旧档没有这行 = 全 0
                std::istringstream in{std::string(line.substr(8))}; std::string tok; size_t i = 0, k = 0; bool items = false;
                while (in >> tok) {
                    if (tok == "|") { items = true; continue; }
                    const int v = std::max(0, std::atoi(tok.c_str()));
                    if (!items && i < r.buys.size()) r.buys[i++] = v; else if (items && k < r.itemBuys.size()) r.itemBuys[k++] = v;
                }
            }
            if (line.rfind("gr.arsenal=", 0) == 0) { std::istringstream in{std::string(line.substr(11))}; for (int& x : r.arsenal) { if (!(in >> x) || x < -1 || x >= growth::kWeaponCount) x = -1; } }
            if (line.rfind("gr.hexcnt=", 0) == 0) { std::istringstream in{std::string(line.substr(10))}; int a = 0, b = 0; in >> a >> b; hexCountsLoaded = {std::max(0, a), std::max(0, b)}; }
            if (line.rfind("gr.hexforge=", 0) == 0) {
                std::istringstream in{std::string(line.substr(12))};
                if (!(in >> r.hexForgePurchases)) return false;
                in >> std::ws; if (!in.eof()) return false;
            }
            if (line.rfind("gr.hexjournal=", 0) == 0) {
                if (r.hexJournal.active) return false;
                std::istringstream in{std::string(line.substr(14))};
                if (!(in >> r.hexJournal.jade) || r.hexJournal.jade < 0) return false;
                for (auto& item : r.hexJournal.items) if (!(in >> item) || item < 0) return false;
                in >> std::ws; if (!in.eof()) return false;
                r.hexJournal.active = true;
            }
            if (line.rfind("gr.acc=", 0) == 0) r.acc = ints(line.substr(7), growth::kAccessoryCount);
            if (line.rfind("gr.bonus=", 0) == 0) {
                std::istringstream in{std::string(line.substr(9))};
                size_t n = 0;
                if (in >> n && n <= 3)
                    for (size_t i = 0; i < n; ++i) { rewards::Offer o; if (!rewards::LoadOffer(in, &o)) { r.bonus.clear(); break; } r.bonus.push_back(o); }
            }
            if (line.rfind("gr.bless=", 0) == 0) {
                const auto v = ints(line.substr(9), 100);
                for (int i = 0; i < growth::kBlessCount && i < static_cast<int>(v.size()); ++i) r.bless.stacks[i] = std::min(v[i], growth::kBlessings[i].maxStack);
            }
            if (line.rfind("gr.offer=", 0) == 0) {
                std::istringstream in{std::string(line.substr(9))};
                int on = 0;
                rewards::Offer o[3];
                if (in >> on && on == 1 && rewards::LoadOffer(in, &o[0]) && rewards::LoadOffer(in, &o[1]) && rewards::LoadOffer(in, &o[2])) {
                    bool sane = true;
                    for (auto& x : o) sane &= rewards::Sane(x);
                    if (sane) { r.offers = {o[0], o[1], o[2]}; r.offered = true; }  // 坏的一组丢掉：引擎读档后按新模型重抽
                }
            }
            if (line.rfind("gr.rng=", 0) == 0) { try { r.rewardRng = std::stoull(std::string(line.substr(7))); } catch (...) {} }
            if (line.rfind("gr.seen=", 0) == 0) {
                std::istringstream in{std::string(line.substr(8))};
                std::string tok;
                while (std::getline(in, tok, ',')) { const auto c = tok.find(':'); if (c != std::string::npos) try { r.seen.insert({std::stoi(tok.substr(0, c)), std::stoi(tok.substr(c + 1))}); } catch (...) {} }
            }
            if (line.rfind("gr.shop=", 0) == 0) rewards::Load(std::string(line.substr(8)), &r.shop, &r.prep, &r.quick);  // 读不了就保持空（不开店），不影响其他
            if (line.rfind("gr.attr=", 0) == 0) {
                const auto v = ints(line.substr(8), 1000000);  // 3.0.0+：属性不封顶
                for (int i = 0; i < rewards::kAttrCount && i < static_cast<int>(v.size()); ++i) r.attrs.pm[i] = std::clamp(v[i], 0, rewards::AttrCapAt(i, rewards::kMaxEndlessTier));  // 读档不能突破上限（3.0.0：无尽档抬高的上限照样保留）
            }
            if (line.rfind("gr.v14=", 0) == 0) {
                std::istringstream in{std::string(line.substr(7))};
                int f = -1, rf = 0, d = 0;
                if (in >> f >> rf >> d) { r.offerFloor = f; r.refresh = rf != 0; r.draws = std::max(0, d); }
            }
            if (line.rfind("gr.last=", 0) == 0) {
                std::istringstream in{std::string(line.substr(8))};
                size_t n = 0;
                if (in >> n && n <= 3)
                    for (size_t i = 0; i < n; ++i) { rewards::Offer o; if (!rewards::LoadOffer(in, &o) || !rewards::Sane(o)) { r.lastGroup.clear(); break; } r.lastGroup.push_back(o); }
            }
            a = b + 1;
        }
        // Older pending lifesteal offers used higher rates. Adjust
        // their displayed amount to the new +2% / 6% rule on resume. If an
        // old total has already reached the new cap, redraw the reward group
        // rather than leaving an unclaimable choice on screen.
        if (r.offered) for (auto& o : r.offers) {
            if (o.kind != rewards::Kind::Attr || o.index != static_cast<int>(rewards::AttrKind::LifeSteal)) continue;
            const int room = r.attrs.Room(o.index);
            if (room <= 0 || r.offerFloor < 36) { r.offered = false; break; }
            o.amount = std::min({o.amount, 20, room});
        }
        if (r.hexAttributes.remaining && !r.hexes.Has(hex::AttributeStack) && !r.hexes.Has(hex::Blitz)) return false;  // 速战速决也用这条属性三选一
        if (!r.hexes.Has(hex::Forge)) for (auto& w : r.profile.weapons) w.level = std::min(w.level, growth::kWeaponCap);
        r.hexes.asceticStacks = hexCountsLoaded.first; r.hexes.flawlessStacks = hexCountsLoaded.second;
        if (hasHexSeen) { r.hexes.seenUp = hexSeenLoaded.first; r.hexes.away = hexSeenLoaded.second; }  // 3.6.2
        r.hexes.cool = hexCoolLoaded;  // 4.1.0
        *out = std::move(r);
        return true;
    }

private:
    bool Enter(session::IGame& g, std::string* why) {
        if (!session.Enter(g, profile)) { *why = session.error(); on = false; return false; }
        on = true;
        return true;
    }
    Err Apply(session::IGame& g, Err e) {
        if (e == Err::Ok) session.Sync(g, profile);
        return e;
    }
};

}  // namespace tower::growthrun
