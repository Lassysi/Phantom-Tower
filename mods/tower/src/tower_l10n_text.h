#pragma once

#include "tower_language.h"
#include "native_terms.generated.h"
#include "tower_terms.generated.h"
#include "tower_l10n_patterns.h"
#include <array>
#include <string>
#include <string_view>
#include <utility>

// Only translate at the presentation boundary. Game commands, IDs, saved
// data, and existing Chinese comparisons keep their original values.
// Initial vocabulary only: an unmatched string remains unchanged until it is
// explicitly translated and reviewed; this is NOT a complete English UI.
namespace tower::language {
inline constexpr auto kInterface = std::to_array<std::pair<std::string_view, std::string_view>>({
    {"影塔·百战", "Phantom Tower"}, {"影塔·百战 · 提示", "Phantom Tower · Notice"},
    {"返回", "Back"}, {"知道了", "OK"}, {"继续战斗", "Resume Battle"}, {"重试", "Retry"},
    {"恢复原进度", "Restore Original Progress"}, {"已是原进度", "Already Restored"},
    {"上次影塔未正常结束", "The previous tower session did not end normally."},
    {"第 100 层", "Floor 100"}, {"继续上一局", "Continue Run"}, {"开始新局", "Start New Run"},
    {"试玩 3 层", "Three-Floor Trial"}, {"下一层", "Next Floor"}, {"整备", "Preparation"},
    {"保存并退出", "Save and Exit"}, {"信息", "Information"}, {"背包", "Inventory"},
    {"影塔·百战 · 保存并退出", "Phantom Tower · Save and Exit"},
    {"保存本局并返回原进度？战斗中退出会从本层重新开始。", "Save this run and return to your original progress? Leaving during battle restarts this floor."},
    {"确认保存并退出", "Confirm Save and Exit"}, {"取消", "Cancel"},
    {"商品介绍", "Item Details"}, {"确认购买", "Confirm Purchase"}, {"货架已变化，未购买", "Stock changed; no purchase was made."},
    {"交易未确认，请检查货架与魂玉", "Purchase unconfirmed; check the stock and Soul Jade."},
    {"退出影塔", "Leave Tower"}, {"重新开始", "Restart"}, {"再来一局", "Start Another Run"},
    {"退出", "Exit"}, {"上一页", "Previous"}, {"下一页", "Next"},
    // 3.6.1 批量锻造
    {"锻造", "Forge"}, {"最少", "Min"}, {"最多", "Max"}, {"确定", "Confirm"}, {"输入数量", "Amount"}, {"锻造 · 选择成长属性", "Forge · Choose Growth"},
    {"买了不退，选完才能开下一层", "No refunds. Finish choosing before the next floor."},
    {"每组选一项，全部选完才能点「确定」，一起加到属性上。伤害减免、抗性有上限，超出部分作废。",
     "Pick one option in every group; Confirm unlocks when all are chosen and adds them together. Damage reduction and resistances are capped; the excess is discarded."},
    {"已付款，点这里接着选择成长属性。", "Paid. Continue choosing growth attributes here."},
    {"还有组没选，请每组选一项", "Some groups are not chosen; pick one in each group."},
    {"请先在驿站选完锻造的成长属性", "Finish choosing forge attributes at the Rest Stop first."},
    {"现在没有待选的锻造", "No forge is pending."},
    {"锻造保存失败，请重试", "Failed to save the forge; please try again."},
    {"锻造已记录，正在等待保存；请稍后重试", "Forge recorded; waiting to save. Please try again shortly."},
    {"使用刷新令", "Use Reroll Token"}, {"选择奖励", "Choose Reward"},
    {"使用重生符", "Use Rebirth Talisman"}, {"入场准备中", "Preparing the arena"},
    {"返回准备", "Back to Preparation"}, {"取消，回到原进度", "Cancel and Restore Original Progress"},
    {"Boss 有问题", "Report a Boss Issue"}, {"属性 / 离开…", "Stats / Leave…"},
    {"信息 / 跳过…", "Info / Skip…"}, {"跳过本层", "Skip This Floor"}, {"确认跳过本层", "Confirm: Skip This Floor"},
    {"信息 / 保存…", "Info / Save…"}, {"本层问题", "Floor Issue"},
    {"透视眼", "Foresight"}, {"本局永久预告下一场 Boss", "Reveals the next boss for the rest of this run."},
    {"更多选项", "More Options"},
    {"暂无法确定", "not yet known"},
    {"整备 / 保存…", "Prepare / Save…"}, {"整备 / 保存", "Prepare / Save"},
    {"此层暂不能跳过，请重新开战或保存退出", "This floor cannot be skipped. Retry the fight or save and exit."},
    {"确认跳过本层小怪？跳过后不发本层小怪魂玉，可继续下一层。", "Skip this wave? You will not receive its jade, and can continue to the next floor."},
    {"确认跳过本层 Boss？可获得本层固定收获，但不发 Boss 兵器和三选一奖励。", "Skip this boss? You get fixed floor rewards, but no boss weapon or choice reward."},
    {"敌人可能卡住，可在影塔菜单选择本层问题", "An enemy may be stuck. Open the tower menu and choose Floor Issue."},
    {"影塔·百战 · 战斗中", "Phantom Tower · In Battle"}, {"影塔·百战 · 跳过本层", "Phantom Tower · Skip Floor"},
    {"这一层出了问题（Boss 不出现、不出招、卡在地里、剧情不触发……）时，可以跳过本层直接到下一层。",
     "If this floor is broken (the boss does not appear, does not attack, is stuck in the ground, or the scene does not start), you can skip to the next floor."},
    {"跳过后：本层按坏层处理，发本层固定收获（点数等），不发 Boss 兵器和三选一奖励，直接到下一层的准备页。",
     "Skipping counts this floor as broken: you get the floor's fixed rewards (points and so on) but no boss weapon or choice reward, and go straight to the next floor's preparation page."},
    {"Boss 卡住无法恢复，本层按通过结算", "The boss got stuck and could not be recovered; the floor counts as cleared."},
    {"只能在战斗中跳过本层", "You can only skip a floor during battle."},
    {"简单", "Easy"}, {"困难", "Hard"}, {"无", "None"}, {"空", "Empty"},
    {"开始种子局", "Start Seed Run"}, {"普通开局", "Normal Run"},
    {"导入的种子无效或版本不一致，请重新导入。", "The imported seed is invalid or incompatible. Please import it again."},
    {"（装备中）", " (Equipped)"}, {"（读不到角色属性）", "(Player stats unavailable)"},
    {"（读不到装备）", "(Equipment unavailable)"}, {"本轮属性奖励", "Run Attribute Bonuses"},
    {"影塔·百战 · 进入影塔", "Phantom Tower · Entering"},
    {"影塔·百战 · 通关", "Phantom Tower · Victory"},
    {"影塔·百战 · 登临绝顶", "Phantom Tower · Summit Cleared"},
    {"影塔·百战 · 起手", "Phantom Tower · Starting Loadout"},
    {"影塔·百战 · Boss 有问题", "Phantom Tower · Boss Issue"},
    {"影塔·百战 · 离开", "Phantom Tower · Exit"},
    {"影塔·百战 · 额外挑战奖励（三选一）", "Phantom Tower · Bonus Challenge Reward (Choose One)"},
    {"影塔·百战 · 选一种新药", "Phantom Tower · Choose a Medicine"},
    {"影塔·百战 · 饰品匣", "Phantom Tower · Accessory Box"},
    {"武器升级", "Upgrade Weapon"}, {"铸魂", "Soul Forge"},
    {"能力修行", "Cultivate Abilities"}, {"MOD背包", "Mod Inventory"}, {"Mod 背包", "Mod Inventory"},
    {"准备下一层要用的 MOD 道具", "Prepare mod items for the next floor."},
    {"驿站", "Rest Stop"}, {"离开驿站", "Leave Rest Stop"}, {"进入驿站", "Enter Rest Stop"}, {"整备 / 驿站", "Preparation / Rest Stop"}, {"暂无商品", "No items available"},
    {"杀气兵器", "Phantom Edge"}, {"换回自己的兵器", "Restore Original Weapons"},
    {"（待生效）", " (Prepared)"}, {"待生效", "Prepared"}, {"（已售）", " (Sold)"},
    {"刷新 / 离开…", "Refresh / Leave…"}, {"回第一页", "First Page"},
    {"刷新货架", "Refresh Stock"}, {"刷新货架换一批货，或离开驿站", "Refresh the stock or leave the rest stop."},
    {"未换", "Unchanged"},
    {"加成：", "Bonuses:"}, {"本层生效：", "Active this floor:"},
    {"本次三选一：", "Choose one:"}, {"已选：", "Selected:"}, {"塔内兵器：", "Tower weapons:"},
    {"本层生效", "Active this floor"},
    {"本层收获", "Floor rewards"}, {"新药可选", "New medicine choice"},
    {"饰品匣", "Accessory box"}, {"饰品精炼匣", "Accessory refinement box"},
    {"完成", "Completed"}, {"未达成", "Failed"},
    {"使用", "Use"}, {"自动生效", "Activates Automatically"}, {"领奖时使用", "Use When Choosing Rewards"},
    {"淬炼点", "Tempering Points"}, {"修为点", "Cultivation Points"}, {"铸魂令", "Soul Tokens"},
    {"每层准备一件，下一层开战时生效，本层结束清除", "Prepare one item per floor. It activates at the start of the next battle and expires after that floor."},
    {"本局加成", "Run Bonuses"},
    {"已预装，下一层开战时扣除并生效", "Prepared. Consumed and activated at the start of the next battle."},
    {"奖励时用", "Use When Choosing Rewards"}, {"撤销", "Remove"}, {"准备", "Prepare"}, {"更换", "Change"},
    {"已购买", "Purchased"}, {"购买", "Buy"}, {"不足", "Insufficient Jade"}, {"未准备", "None Prepared"},
    {"返回整备", "Back to Preparation"}, {"没有可用的道具", "No items available"},
    {"属性", "Stats"}, {"离开影塔…", "Leave Tower…"},
    {"暂离，本层重来", "Suspend and Retry This Floor"}, {"暂离", "Suspend"},
    {"放弃本局，退出影塔", "Abandon Run and Leave Tower"},
    {"确认：作废本层并禁用这个 Boss", "Confirm: Cancel This Floor and Disable This Boss"},
    {"退后 10 层", "Go back 10 floors"},
    {"旧版爬塔进度不能继续，请开始新局", "Old tower runs cannot continue. Please start a new run."},
    {"【属性】", "[Stats]"},
    {"武器", "Weapons"},
    {"【武器】", "[Weapons]"},
    {"【影之武装】", "[Shadow Arms]"},
    {"饰品", "Accessories"},
    {"【饰品】", "[Accessories]"},
    {"药", "Medicine"},
    {"【药】", "[Medicine]"},
    {"MOD道具", "Mod Items"},
    {"【MOD道具】", "[Mod Items]"},
    {"倒在第 11 层以内，从第 1 层开始", "Fell within the first 11 floors; restart at floor 1"},
    {"限时挑战", "Timed Challenge"}, {"额外挑战：限时", "Bonus Challenge: Time Limit"},
    {"额外挑战：无伤", "Bonus Challenge: No Damage"}, {"额外挑战：禁药", "Bonus Challenge: No Medicine"},
    {"世界还在载入，稍后再开", "The world is still loading. Try again shortly."},
    {"正在还原世界", "Restoring the world"},
    {"这一层出了问题，已跳过，可以直接打下一层", "This floor ran into a problem and was skipped. You can go on to the next floor."},
    {"伤害减免", "Damage Reduction"}, {"生命吸取", "Lifesteal"},
    {"吸血符", "Lifesteal Talisman"}, {"下一层额外获得 10% 生命吸取", "Gain an extra 10% lifesteal for the next floor"},
    {"减免受到的伤害", "Reduces damage taken"}, {"按敌方掉血比例回复生命", "Heals based on enemy health lost"},
    {"火焰抗性", "Fire Resistance"}, {"冰霜抗性", "Frost Resistance"}, {"雷电抗性", "Lightning Resistance"},
    {"毒素抗性", "Toxin Resistance"}, {"流血抗性", "Bleed Resistance"}, {"混沌抗性", "Chaos Resistance"},
    {"减少火焰伤害与点燃积累", "Reduces fire damage and ignite buildup"}, {"减少冰霜伤害与冰冻积累", "Reduces frost damage and freeze buildup"},
    {"减少雷电伤害与感电积累", "Reduces lightning damage and shock buildup"}, {"减少毒素伤害与中毒积累", "Reduces toxin damage and poison buildup"},
    {"减少血伤害与出血、血爆积累", "Reduces blood damage and bleed buildup"}, {"减少混沌伤害", "Reduces chaos damage"},
    {"武器攻击", "Weapon Attack"}, {"攻击力", "Attack"}, {"武器破防", "Weapon Guard Break"},
    {"生命上限", "Maximum HP"}, {"杀气上限", "Maximum Sha-Chi"},
    {"所有武器的攻击力", "Attack of all weapons"}, {"所有武器的破防能力", "Guard-breaking power of all weapons"},
    {"凡", "Common"}, {"灵", "Rare"}, {"玄", "Epic"}, {"（空）", "(Empty)"},
    {"主兵器", "Main Weapon"}, {"影之武装", "Phantom Edge"},
    {"整备里升级兵器用，每级 1 点", "Spend in Preparation to upgrade weapons, one point per level."},
    {"整备里点能力节点用", "Spend in Preparation to unlock ability nodes."},
    {"驿站购物用", "Spend at rest stops."},
    {"整备 → 铸魂里为武器铸魂用", "Use under Preparation → Soul Forge to forge a weapon's soul."},
    {"难度只影响 Boss 血量、伤害和杀气", "Difficulty affects boss HP, damage, and Sha-Chi only."},
    {"退出：本局结束，进入结算", "Exit: end the run and show the results."},
    {"回标题后点“继续”", "Return to the title screen and select Continue."},
    {"正在恢复原进度…", "Restoring original progress…"},
    {"正在恢复原进度，请稍候", "Restoring original progress. Please wait."},
    {"正在载入影塔，请稍候", "Loading the tower. Please wait."},
    {"正在进入塔档：请回到标题画面，再点“继续”", "Loading the tower save: return to the title screen and select Continue."},
    {"正在出塔：请回到标题画面，再点“继续”", "Leaving the tower: return to the title screen and select Continue."},
    {"塔档已准备好：请回到标题画面，再点“继续”进入影塔", "The tower save is ready. Return to the title screen and select Continue to enter."},
    {"出塔：请回到标题画面，再点“继续”进档，完成后存档沙盒自动关闭", "Return to the title screen and select Continue to leave the tower. Save protection closes automatically when restoration completes."},
    {"上次影塔会话没有正常结束：先在影塔菜单里选“恢复原进度”或“已是原进度”", "The previous tower session did not end normally. In the tower menu, choose Restore Original Progress or Already Restored."},
    {"已经在影塔里", "Already inside the tower."},
    {"原生对话框类还没加载", "The game's dialog is not loaded yet."},
    {"原生对话框接口缺失", "The game's dialog interface is unavailable."},
    {"载入影塔", "Loading the tower"}, {"进入影塔", "Enter the tower"},
    {"点击「退出影塔」恢复原进度。", "Select Leave Tower to restore your original progress."},
    {"正在进入塔档，不能再次进塔", "The tower save is loading; you cannot enter again yet."},
    {"已经在影塔会话里（或上次没正常结束）", "A tower session is already active or was not closed normally."},
    {"游戏正在加载或传送，稍后再进塔", "The game is loading or fast travelling. Try entering again later."},
    {"游戏正在加载或传送，稍后再重开", "The game is loading or fast travelling. Try restarting later."},
    {"读不到进塔基准", "Could not read the original-save snapshot."},
    {"进塔基准不是有效的存档 JSON", "The original-save snapshot is not valid save data."},
    {"不在塔档里，不能塔内重开", "You can only restart a tower run while inside the tower save."},
    {"不在影塔会话里", "There is no active tower session."},
    {"正在恢复原进度，不能重复出塔", "Original progress is being restored; do not exit again yet."},
    {"没有失败待重试的切档", "There is no failed save transition to retry."},
    {"自动返回标题没有反应（可手动回标题）", "Automatic return to the title screen did not respond. You can return manually."},
    {"自动继续没有载入存档（可在标题手动点继续）", "Automatic Continue did not load the save. Select Continue manually at the title screen."},
    {"现在不能开下一层", "The next floor cannot be started right now."},
    {"后续层数暂未开放", "Further floors are not available yet."},
    {"请先关闭原生整备面板", "Close the game's preparation panel first."},
    {"正在前往场地", "Travelling to the arena."},
    {"还没进入塔档：请回到标题画面，再点“继续”", "The tower save is not loaded yet. Return to the title screen and select Continue."},
    {"还没进入独立塔档，未开层（原存档未改动）。请退出影塔后重新进入", "The separate tower save is not loaded, so the floor was not started. Your original save was not changed. Leave the tower and enter again."},
    {"找不到玩家角色", "Player character not found."},
    {"没有进行中的局", "There is no run in progress."},
    {"没有可继续的局", "There is no saved run to continue."},
    {"这一局的塔档数据缺失或来自旧版本，无法继续。请开新局（原存档未改动）", "This run's tower save is missing or from an older version and cannot be resumed. Start a new run; your original save was not changed."},
    {"复活符会在倒下时自动生效", "A Revival Talisman activates automatically when you fall."},
    {"没有刷新令，或现在不能刷新", "You have no Reroll Tokens, or cannot reroll right now."},
    {"开层前才能升级 / 换配置", "Upgrade or change your setup before starting a floor."},
    {"读不到背包，稍后再试", "Could not read the inventory. Try again later."},
    {"功能不存在", "Feature unavailable"},
    {"没有这个道具", "This item is unavailable."},
    {"开战前或战斗中才能用神通", "Powers can only be used before or during a battle."},
    {"神通栏里没有这一个", "This power is not in your inventory."},
    {"开层前才能换兵器", "Weapons can only be changed before starting a floor."},
    {"本局没有得到这把兵器", "You have not obtained this weapon in this run."},
    {"塔内换兵器暂时不可用", "Changing weapons in the tower is currently unavailable."},
    {"换兵器没有生效", "The weapon change did not take effect."},
    {"第 100 层尚未开启", "Floor 100 is not unlocked yet."},
    {"请先保存并退出当前一轮", "Save and exit the current run first."},
    {"第 100 层存档读取失败", "Could not load the Floor 100 save."},
    {"没有装备主兵器：先在 Esc → 角色里装上再开层", "No main weapon equipped. Equip one under Esc → Character before starting a floor."},
    {"请先关闭修改器的升级无消耗，再使用影塔点数", "Disable the trainer's free-upgrade option before spending tower points."},
    {"原生费用面板或枚举尚未就绪，未开放交易", "The game's cost panel is not ready; the transaction is unavailable."},
    {"影塔的所得只在塔内有效，不带出塔", "Tower rewards are available only inside the tower and do not carry over."},
    {"没有待领取的兵器", "There are no weapons waiting to be claimed."},
    // These four pool labels have no exact official key. Suffixes describe the
    // tower's own variants; preserve that distinction instead of pretending
    // these labels are verbatim game-localized names.
    {"力者和锯者（基础）", "The Crusher and the Saw (Base)"},
    {"白衣鬼差（变体）", "White-Robed Shadow Warden (Variant)"},
    {"魂（镜像）（暂译）", "Soul (Mirror; provisional name)"},
    {"终极首领（暂译）", "Ultimate Boss (provisional name)"},
    {"货架已变化，请重新打开商店", "Stock changed. Reopen the rest stop."},
    {"已售出", "Sold out"}, {"魂玉不足", "Not enough Soul Jade"},
    {"这件现在不能买", "This item cannot be purchased now."},
    {"发放失败，未扣魂玉", "Could not grant the item. No Soul Jade was spent."},
    {"这个道具不能预装", "This item cannot be prepared."}, {"没有库存", "None in inventory"},
    {"已经预装了", "Already prepared"},
    {"每层只能准备一件，先撤销已准备的", "Only one item can be prepared per floor. Remove the prepared item first."},
    {"新物品", "New item"}, {"下一层 / 下一次三选一", "Next floor / next reward choice"},
    {"奖励已更新，请重新选择", "Rewards have changed. Please choose again."},
    {"这个道具没有预装", "This item is not prepared."},
    {"原生剧情入场没有启动，改为直接开打本层", "The original encounter did not start. Retrying this floor as a direct fight."},
    {"驿站开了", "The rest stop is open."}, {"等待入场倒计时", "Waiting for the entry countdown."},
    {"游戏暂停 / 加载中，就绪后重试", "The game is paused or loading. Retry when it is ready."},
    {"这里没有可落地的位置，换个开阔地方再开这一层", "No safe spawn location was found. Move to an open area and retry."},
    {"现在不能使用重生符", "A Rebirth Talisman cannot be used now."},
});

inline std::string DisplayPart(std::string_view input, unsigned depth) {
    if (input.empty() || depth > 5) return std::string(input);
    if (input.starts_with("　")) return "  " + DisplayPart(input.substr(std::string_view("　").size()), depth + 1);
    for (const auto& [zh, en] : kInterface) if (input == zh) return std::string(en);
    for (const auto& [zh, en] : kNativeTerms) if (input == zh) return std::string(en);
    for (const auto& [zh, en] : kTowerTerms) if (input == zh) return std::string(en);
    if (input.find('\n') != input.npos) return std::string(input);
    for (const auto& [pattern, translated] : kPatterns) {
        std::array<std::string_view, 8> fields{};
        size_t count = 0, p = 0, pos = 0;
        bool matched = true;
        while (p < pattern.size()) {
            const auto slot = pattern.find("{}", p);
            const auto literal = pattern.substr(p, slot == pattern.npos ? pattern.size() - p : slot - p);
            if (!input.substr(pos).starts_with(literal)) { matched = false; break; }
            pos += literal.size();
            if (slot == pattern.npos) break;
            if (count == fields.size()) { matched = false; break; }
            p = slot + 2;
            const auto next = pattern.find("{}", p);
            const auto delimiter = pattern.substr(p, next == pattern.npos ? pattern.size() - p : next - p);
            // Adjacent placeholders are ambiguous and are not allowed.
            if (delimiter.empty() && next != pattern.npos) { matched = false; break; }
            const auto end = delimiter.empty() ? input.size() : input.find(delimiter, pos);
            if (end == input.npos || end == pos) { matched = false; break; }
            fields[count++] = input.substr(pos, end - pos);
            pos = end;
        }
        if (!matched || pos != input.size() || count == 0) continue;
        std::string out;
        for (size_t i = 0; i < translated.size(); ++i) {
            if (translated[i] == '{' && i + 2 < translated.size() && translated[i + 2] == '}' &&
                translated[i + 1] >= '0' && translated[i + 1] <= '7') {
                const size_t field = static_cast<size_t>(translated[i + 1] - '0');
                if (field >= count) { matched = false; break; }
                out += DisplayPart(fields[field], depth + 1);
                i += 2;
            } else out += translated[i];
        }
        if (matched) return out;
    }
    if (const auto sep = input.find("、"); sep != input.npos) {
        std::string result;
        size_t start = 0;
        do {
            const auto end = input.find("、", start);
            if (!result.empty()) result += ", ";
            result += DisplayPart(input.substr(start, end == input.npos ? input.size() - start : end - start), depth + 1);
            if (end == input.npos) break;
            start = end + std::string_view("、").size();
        } while (start < input.size());
        return result;
    }
    return std::string(input);
}

inline std::string Display(std::string_view input, Language language) {
    if (language == Language::Chinese) return std::string(input);
    // First try a whole string; fall back to individual lines so a dynamic
    // message assembled by existing game logic can translate without changing
    // its meaning or the state used by the underlying callbacks.
    auto whole = DisplayPart(input, 0);
    if (whole != input || input.find('\n') == input.npos) return whole;
    std::string out;
    size_t start = 0;
    while (start <= input.size()) {
        const auto end = input.find('\n', start);
        const auto line = input.substr(start, end == input.npos ? input.size() - start : end - start);
        out += DisplayPart(line, 0);
        if (end == input.npos) break;
        out += '\n';
        start = end + 1;
    }
    return out;
}
}  // namespace tower::language
