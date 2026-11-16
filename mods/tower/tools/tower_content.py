"""Reviewed design inputs for prepare_data.py; all prices/new mechanics need playtests."""
from __future__ import annotations

PLAN = "notes/tower-floors.md"
LOGIC = "mods/tower/src/tower_logic.h"
ECONOMY = "mods/tower/src/tower_economy.h"
SELF = "mods/tower/tools/tower_content.py"
ITEM_SCHEME = "notes/tower-items-scheme.md"


def meta(*source, evidence="static", **data):
    return {**data, "evidence": evidence, "source": list(source)}


def cards():
    # id, zh, en, school, rarity, supported stat, additive value, zh/en text.
    definitions = [
        ("blade_1", "锋芒", "Keen Edge", "刀势", 0, "DmgDealt", .15, "伤害 +15%", "Damage +15%"),
        ("blade_2", "锋芒·灵", "Keen Edge II", "刀势", 1, "DmgDealt", .25, "伤害 +25%", "Damage +25%"),
        ("break_1", "破军", "Breaker", "刀势", 0, "PostureDealt", .40, "削杀气 +40%", "Posture damage +40%"),
        ("guard_1", "铁骨", "Iron Guard", "金钟", 0, "DmgTaken", .15, "受到伤害 -15%", "Damage taken -15%"),
        ("guard_2", "铁骨·灵", "Iron Guard II", "金钟", 1, "DmgTaken", .25, "受到伤害 -25%", "Damage taken -25%"),
        ("life_1", "厚生", "Vitality", "生命", 0, "MaxHp", .20, "生命上限 +20%", "Max health +20%"),
        ("life_2", "厚生·玄", "Vitality III", "生命", 2, "MaxHp", .40, "生命上限 +40%", "Max health +40%"),
        ("heal_1", "回春", "Renewal", "金钟", 0, "HealOnFloor", .30, "每层开始回复 30% 生命", "Heal 30% at floor start"),
        ("shachi_1", "聚煞", "Gathering Force", "杀气", 0, "ShachiGain", .40, "杀气恢复速度 +40%", "Sha-Chi recovery speed +40%"),
        ("shachi_2", "聚煞·玄", "Gathering Force III", "杀气", 2, "ShachiGain", .80, "杀气恢复速度 +80%", "Sha-Chi recovery speed +80%"),
        ("speed_1", "疾风", "Quickstep", "时机", 1, "AtkSpeed", .10, "攻击速度 +10%", "Attack speed +10%"),
        ("blade_3", "锋芒·天", "Keen Edge IV", "刀势", 3, "DmgDealt", .50, "伤害 +50%", "Damage +50%"),
        ("break_2", "破军·灵", "Breaker II", "刀势", 1, "PostureDealt", .60, "削杀气 +60%", "Posture damage +60%"),
        ("break_3", "破军·玄", "Breaker III", "刀势", 2, "PostureDealt", .80, "削杀气 +80%", "Posture damage +80%"),
        ("guard_3", "铁骨·玄", "Iron Guard III", "金钟", 2, "DmgTaken", .30, "受到伤害 -30%", "Damage taken -30%"),
        ("guard_4", "铁骨·天", "Iron Guard IV", "金钟", 3, "DmgTaken", .40, "受到伤害 -40%", "Damage taken -40%"),
        ("heal_2", "回春·灵", "Renewal II", "金钟", 1, "HealOnFloor", .40, "每层开始回复 40% 生命", "Heal 40% at floor start"),
        ("shachi_3", "聚煞·灵", "Gathering Force II", "杀气", 1, "ShachiGain", .60, "杀气恢复速度 +60%", "Sha-Chi recovery speed +60%"),
        ("shachi_4", "聚煞·天", "Gathering Force IV", "杀气", 3, "ShachiGain", 1.0, "杀气恢复速度 +100%", "Sha-Chi recovery speed +100%"),
        ("speed_2", "疾风·玄", "Quickstep III", "时机", 2, "AtkSpeed", .15, "攻击速度 +15%", "Attack speed +15%"),
        ("speed_3", "疾风·天", "Quickstep IV", "时机", 3, "AtkSpeed", .20, "攻击速度 +20%", "Attack speed +20%"),
        ("speed_4", "轻刃", "Light Blade", "时机", 0, "AtkSpeed", .05, "攻击速度 +5%", "Attack speed +5%"),
        ("life_3", "厚生·灵", "Vitality II", "生命", 1, "MaxHp", .30, "生命上限 +30%", "Max health +30%"),
        ("life_recover", "养息", "Restoration", "生命", 0, "HealOnFloor", .15, "每层开始回复 15% 生命", "Heal 15% at floor start"),
        ("curse_frail", "薄命", "Frailty", "诅咒", 0, "MaxHp", -.20, "生命上限 -20%", "Max health -20%"),
        ("curse_exposed", "破衣", "Exposure", "诅咒", 0, "DmgTaken", -.15, "受到伤害 +15%", "Damage taken +15%"),
        ("curse_dry", "枯煞", "Withered Force", "诅咒", 1, "ShachiGain", -.20, "杀气恢复速度 -20%", "Sha-Chi recovery speed -20%"),
        ("curse_heavy", "沉腕", "Heavy Hands", "诅咒", 1, "AtkSpeed", -.05, "攻击速度 -5%", "Attack speed -5%"),
        ("curse_dull", "钝刃", "Dull Edge", "诅咒", 2, "DmgDealt", -.10, "伤害 -10%", "Damage -10%"),
        ("curse_weak", "虚势", "Hollow Stance", "诅咒", 2, "PostureDealt", -.20, "削杀气 -20%", "Posture damage -20%"),
    ]
    result = []
    for index, (cid, zh, en, school, rarity, stat, value, text, text_en) in enumerate(definitions):
        result.append(meta(LOGIC, "notes/bossrush-plan.md", SELF, id=cid, name=zh, name_en=en, school=school, rarity=rarity, stat=stat, value=value, text=text, text_en=text_en, weight=[10, 5, 2, 1][rarity], enabled=True, implemented=True, random_offer=school != "诅咒", duration="run", balance_evidence="unverified", origin="builtin" if index < 12 else "proposed"))
    triggers = [
        ("shachi_hit", "煞涌", "Flow of Force", "杀气", 1, "hit", "player", "gain_shachi", .02, "命中回复 2% 杀气（冷却 1 秒）", "Hits restore 2% Sha-Chi (1s cooldown)", 1.0),
        ("shachi_break", "破势聚煞", "Break and Gather", "杀气", 2, "posture_break", "player", "gain_shachi", .15, "破防回复 15% 杀气", "Breaking posture restores 15% Sha-Chi", 0),
        ("timing_parry", "回锋", "Counter Edge", "时机", 1, "perfect_parry", "player", "next_hit_damage", .30, "完美格挡后下次伤害 +30%", "After a perfect parry, next hit +30% damage", 0),
        ("timing_dodge", "掠影", "Passing Shadow", "时机", 2, "perfect_dodge", "player", "next_hit_posture", .50, "完美闪避后下次削杀气 +50%", "After a perfect dodge, next hit +50% posture damage", 0),
        ("life_kill", "饮露", "Dew Drinker", "生命", 1, "kill", "player", "heal", .08, "击杀本塔敌人回复 8% 生命", "Tower kills restore 8% health", 0),
        ("life_low", "余烬", "Last Ember", "生命", 2, "health_threshold", "player", "damage_multiplier", .25, "生命低于 30% 时伤害 +25%", "Below 30% health, damage +25%", 0),
        ("shadow_edge", "同锋", "Shared Edge", "影卫", 0, "floor_start", "ally", "damage_multiplier", .20, "魂契伤害 +20%", "Soul allies deal +20% damage", 0),
        ("shadow_guard", "同甲", "Shared Guard", "影卫", 0, "floor_start", "ally", "damage_reduction", .20, "魂契受到伤害 -20%", "Soul allies take -20% damage", 0),
        ("shadow_life", "同生", "Shared Life", "影卫", 1, "floor_start", "ally", "max_health_multiplier", .30, "魂契生命上限 +30%", "Soul allies gain +30% max health", 0),
        ("shadow_mend", "共济", "Mutual Aid", "影卫", 1, "floor_start", "ally", "heal", .25, "每层开始治疗魂契 25%", "Heal soul allies 25% at floor start", 0),
        ("shadow_rally", "振军", "Rally", "影卫", 2, "kill", "ally", "heal", .10, "本塔敌人死亡时治疗魂契 10%", "Tower kills heal soul allies 10%", 0),
        ("shadow_return", "归魂", "Soul Return", "影卫", 3, "ally_death", "ally", "revive_once", .50, "本层首次阵亡魂契以半血复活", "Revive the first fallen ally at half health this floor", 0),
    ]
    for cid, zh, en, school, rarity, event, target, effect, value, text, text_en, cooldown in triggers:
        trigger = {"event": event, "target": target, "effect": effect, "cooldown_seconds": cooldown, "tower_actors_only": True}
        if event == "health_threshold":
            trigger["below_fraction"] = .30
        if effect == "revive_once":
            trigger["limit_per_floor"] = 1
        result.append(meta(SELF, "notes/bossrush-plan.md", evidence="unverified", id=cid, name=zh, name_en=en, school=school, rarity=rarity, trigger=trigger, value=value, text=text, text_en=text_en, weight=[10, 5, 2, 1][rarity], enabled=False, implemented=False, random_offer=True, duration="run", balance_evidence="unverified", origin="proposed", verification="CARD-TRIGGERS in notes/codex-tower-cards.md"))
    # Retain archival/event references, exclude obsolete floor-start healing
    # from random rewards (full recovery is a tower rule, not a blessing).
    for card in result:
        if card.get("stat") == "HealOnFloor":
            card["random_offer"] = False
    return result


ITEMS = [
    ("heal", "回春丹", "Healing Pill", 30, 45, "已退役，不进奖励池或商店", "Retired; excluded from rewards and shops", False),
    ("reroll", "刷新令", "Reroll Token", 15, 60, "重抽三选一", "Reroll the reward choices", True),
    ("freeze", "定身符", "Binding Charm", 12, 70, "开层前预装；开战后 Boss 定住 4 秒", "Prepare before the floor; freeze bosses for 4 seconds at combat start", True),
    ("slow", "缓时符", "Slowtime Charm", 12, 70, "开层前预装；开战后 10 秒内仅 Boss 动作放慢 40%", "Prepare before the floor; only bosses move 40% slower for the first 10 seconds", True),
    ("sunder", "破甲符", "Sundering Charm", 10, 75, "本层 Boss 受到伤害 +30%", "Bosses take +30% damage this floor", True),
    ("shachi", "聚煞丹", "Sha-Chi Pill", 10, 50, "开层前预装；本层杀气恢复速度 +40%", "Prepare before the floor; Sha-Chi recovery speed +40% this floor", True),
    ("intel", "情报", "Intelligence", 6, 65, "揭示观看局情报", "Reveal spectator match information", False),
    ("borrow", "借将令", "Ally Token", 3, 120, "下一场团战增援一人", "Add one ally to the next team fight", False),
    ("revive", "复活符", "Revival Charm", 2, 200, "倒下时以 50% 生命复活一次（自动）", "Automatically revive once at 50% health when defeated", True),
    ("guard", "铁甲符", "Iron Armor Charm", 10, 40, "开层前预装；本层受到伤害 -20%", "Prepare before the floor; damage taken -20% this floor", True),
    ("edge", "锐锋符", "Keen Edge Charm", 10, 40, "开层前预装；本层攻击 / 杀气攻击 +15%", "Prepare before the floor; attack and Sha-Chi attack +15% this floor", True),
    ("break", "破势符", "Posture Break Charm", 10, 40, "开层前预装；本层削杀气 +30%", "Prepare before the floor; posture damage +30% this floor", True),
]
PREP_ITEMS = {"freeze", "slow", "sunder", "shachi", "guard", "edge", "break"}
SHENTONG = [
    ("bati", "霸体", "Unshaken", "player.no_stagger", 0, "normal"),
    ("kuanxin", "宽心", "Wider Window", "player.deflect_window_mul", 2.0, "normal"),
    ("baidu", "百毒不侵", "Immunity", "player.status_immune", 0, "normal"),
    ("yingwu", "影武无尽", "Endless Phantoms", "combat.phantom_inf", 0, "normal"),
    ("jifeng", "疾风", "Gale", "combat.atk_speed", 1.25, "normal"),
    ("ponian", "一念破防", "Instant Break", "combat.instant_break", 0, "normal"),
    ("huajin", "化劲", "Flowing Guard", "player.auto_deflect_normal", 0, "rare"),
    ("youshen", "游身", "Flowing Dodge", "player.auto_dodge_normal", 0, "rare"),
    ("zidong", "自动出刀", "Automatic Strikes", "player.auto_attack", 0, "rare"),
    ("lingxi_blue", "灵犀·蓝", "Blue Insight", "player.auto_deflect_blue", 0, "lingxi"),
    ("lingxi_red", "灵犀·红", "Red Insight", "player.auto_dodge_red", 0, "lingxi"),
]


def shop():
    result = []
    for sid, name, name_en, stat, value, price in [
        ("growth_damage", "磨刃", "Sharpen", "DmgDealt", .05, 60),
        ("growth_health", "养元", "Fortify", "MaxHp", .10, 55),
        ("growth_posture", "练势", "Hone Stance", "PostureDealt", .10, 50),
        ("growth_shachi", "调息", "Gather Breath", "ShachiGain", .10, 50),
        ("growth_heal", "生息", "Recuperate", "HealOnFloor", .05, 65),
    ]:
        result.append(meta(LOGIC, SELF, evidence="unverified", id=sid, kind="growth", name=name, name_en=name_en, stat=stat, value=value, duration="run", base_price=price, purchase_multiplier=1.35, max_purchases=5, enabled=False, verification="SHOP-BALANCE in notes/codex-tower-cards.md"))
    for iid, name, en, weight, price, text, text_en, implemented in ITEMS:
        timing = "prep" if iid in PREP_ITEMS else "automatic" if iid == "revive" else "settlement" if iid == "reroll" else "retired"
        result.append(meta(ECONOMY, ITEM_SCHEME, SELF, id="item_" + iid, kind="item", item_id=iid, name=name, name_en=en, text=text, text_en=text_en, weight=weight, base_price=price, consumable=True, duration="one_floor" if timing == "prep" else "one_use", use_timing=timing, implemented=implemented, enabled=implemented, balance_evidence="unverified",
            **({"prep_rules": {"max_distinct": 3, "stack_same_kind": False, "consume_at": "countdown_release", "consume_on_cancel": False, "effects_after_blessing_caps": True}} if timing == "prep" else {})))
    for sid, name, en, feature, value, tier in SHENTONG:
        result.append(meta(ECONOMY, PLAN + "#5.4 神通", id="st_" + sid, kind="shentong", shentong_id=sid, name=name, name_en=en, feature=feature, value=value, tier=tier, consumable=True, duration="one_floor", max_stock_per_refresh=1, sold=tier == "normal", base_price=800 if tier == "normal" else None, enabled=tier == "normal", balance_evidence="unverified"))
    result.append(meta(PLAN + "#5.4 神通", SELF, id="shop_policy", kind="policy", currency="jade", zone_multipliers=[1.0, 1.2, 1.45, 1.75, 2.1], price_rounding="ceil", price_formula="ceil(base_price * zone_multiplier * purchase_multiplier ** previous_purchases)", refresh={"base_price": 20, "purchase_multiplier": 1.5, "reset_count": "on_new_shop_visit"}, shentong={"chance_per_refresh": .03, "roll_unit": "once_for_normal_pool_per_refresh", "tiers_sold": ["normal"], "max_per_refresh": 1, "duration": "one_floor", "consumable": True}, engine_shop_implemented=False, balance_evidence="unverified"))
    return result


def events():
    # Effects are data primitives over existing jade/item/card/MaxHp state; a
    # transactional event handler is still needed, not arbitrary UFunction calls.
    defs = [
        ("lost_purse", "遗落钱袋", "Lost Purse", "路边有一只钱袋。", "A purse lies beside the path.", [("取走", "Take it", [{"kind": "jade", "amount": 60}]), ("留下", "Leave it", [])]),
        ("peddler", "行脚货郎", "Wandering Peddler", "货郎拿出一枚铁甲符：开层前预装，本层受到伤害 -20%。", "A peddler offers an Iron Armor Charm: prepare it before combat for 20% less damage taken this floor.", [("买下 · 40 魂玉", "Buy · 40 jade", [{"kind": "jade", "amount": -40}, {"kind": "item", "id": "guard", "amount": 1}]), ("离开", "Leave", [])]),
        ("old_forge", "旧炉", "Old Forge", "炉中仍有余火。", "Embers still glow in the forge.", [("磨刃 · 50 魂玉", "Sharpen · 50 jade", [{"kind": "jade", "amount": -50}, {"kind": "card", "id": "blade_1"}]), ("收下炉灰", "Collect the ashes", [{"kind": "jade", "amount": 25}])]),
        ("quiet_shrine", "静庙", "Quiet Shrine", "供台上有一碗清水。", "A bowl of clear water rests on the altar.", [("饮水", "Drink", [{"kind": "max_hp", "value": .10}]), ("供奉 · 30 魂玉", "Offer · 30 jade", [{"kind": "jade", "amount": -30}, {"kind": "card", "id": "heal_1"}])]),
        ("blood_oath", "血契", "Blood Pact", "石碑许诺力量，也索取代价。", "The stone promises power at a price.", [("立契", "Make the pact", [{"kind": "curse_card", "id": "curse_frail"}, {"kind": "jade", "amount": 140}]), ("拒绝", "Refuse", [])]),
        ("broken_armor", "残甲", "Broken Armor", "残甲中藏着一块魂玉。", "Jade gleams inside a broken breastplate.", [("取玉", "Take the jade", [{"kind": "curse_card", "id": "curse_exposed"}, {"kind": "jade", "amount": 120}]), ("修补甲片", "Mend the armor", [{"kind": "jade", "amount": -45}, {"kind": "card", "id": "guard_1"}])]),
        ("silent_monk", "无言客", "Silent Stranger", "来客以刀势换一袋魂玉。", "A stranger offers jade for a measure of strength.", [("交换", "Trade", [{"kind": "curse_card", "id": "curse_dull"}, {"kind": "jade", "amount": 100}]), ("离开", "Leave", [])]),
        ("sealed_chest", "封箱", "Sealed Chest", "箱上贴着枯煞符。", "A withering seal binds the chest.", [("开箱", "Open", [{"kind": "curse_card", "id": "curse_dry"}, {"kind": "item", "id": "revive", "amount": 1}]), ("绕行", "Walk away", [])]),
        ("training_post", "旧木桩", "Training Post", "木桩上尽是旧刀痕，旁边留着一枚锐锋符，须在开层前预装。", "Old blade marks cover the post; a Keen Edge Charm nearby must be prepared before combat.", [("练刀 · 35 魂玉", "Practice · 35 jade", [{"kind": "jade", "amount": -35}, {"kind": "card", "id": "break_1"}]), ("取锐锋符", "Take the Keen Edge Charm", [{"kind": "item", "id": "edge", "amount": 1}])]),
        ("charm_cache", "符匣", "Charm Cache", "匣中两枚准备符咒只能取其一：定身符开战定住 Boss 4 秒；缓时符开战后 10 秒仅慢 Boss 40%。", "Choose a preparation charm: bind bosses for 4 seconds, or slow only bosses by 40% for the first 10 seconds.", [("定身符", "Binding Charm", [{"kind": "item", "id": "freeze", "amount": 1}]), ("缓时符", "Slowtime Charm", [{"kind": "item", "id": "slow", "amount": 1}])]),
        ("heavy_bargain", "沉腕交易", "Heavy Bargain", "商人愿用魂玉买下你的轻捷。", "A merchant offers jade for your swiftness.", [("成交", "Accept", [{"kind": "curse_card", "id": "curse_heavy"}, {"kind": "jade", "amount": 130}]), ("拒绝", "Refuse", [])]),
        ("last_lantern", "末灯", "Last Lantern", "灯下留着一枚聚煞丹：开层前预装，本层杀气恢复速度 +40%。", "A Sha-Chi Pill waits beneath the lantern: prepare it before combat for 40% faster Sha-Chi recovery this floor.", [("取丹", "Take the pill", [{"kind": "item", "id": "shachi", "amount": 1}]), ("以势换玉", "Trade strength for jade", [{"kind": "curse_card", "id": "curse_weak"}, {"kind": "jade", "amount": 110}])]),
    ]
    result = []
    for eid, name, en, text, text_en, options in defs:
        choices = []
        for index, (zh, eng, effects) in enumerate(options, 1):
            cost = sum(-e["amount"] for e in effects if e["kind"] == "jade" and e["amount"] < 0)
            choices.append(meta(SELF, id=f"{eid}_{index}", text=zh, text_en=eng, requires={"jade_at_least": cost}, effects=effects))
        result.append(meta(SELF, "notes/bossrush-plan.md", ITEM_SCHEME, evidence="unverified", id=eid, name=name, name_en=en, text=text, text_en=text_en, weight=10, choices=choices, once_per_run=True, enabled=False, verification="EVENT-TRANSACTION in notes/codex-tower-cards.md"))
    return result


# Explicit encounters preserve door pairs, teams and sequential fights. Entries
# are short stems, expanded against the shared catalog by Sources.path.
SINGLES = {
    1: ["QiShu_Bing", "MobMan_Elite_Sword", "GuiChai_Yellow_ShuangGou"],
    2: ["AHua_GuiTouDao", "GuiChai_Yellow_MoDao", "HuoDaoDuiZhang"],
    4: ["PengMeng_Axe", "Scavenger"], 6: ["HookMan", "YuCong_FirstTime"],
    8: ["GuiChai_Red"], 10: ["MoTian", "GuiChai_Red"],
    11: ["LuanShi", "FengXueDaoKe", "YanQianSui"], 12: ["DuYanBoss", "YouLong_Harpoon"],
    14: ["QiShu_Du", "HuaShan_RedTroop_BigBro"], 16: ["JingYe", "GuiChai_Cyan_ShuangYue"],
    18: ["ChiLian"], 20: ["HuaQianXIao", "ChiLian"],
    21: ["Danghui", "Zhuobi", "QiShu_Huo"], 22: ["BiYuan", "FemaleVessel_YuZan"],
    24: ["WanJun_JinGua", "XiaoXue"], 26: ["EMei_Sword_JianZhen", "YuBi"],
    28: ["MuXiaoKui"], 30: ["HuaShan_Blue", "MuXiaoKui"],
    31: ["GreatBladeMan", "YanYan_Club", "MobMan_Elite"], 32: ["JadeMaskMan", "YingMei_RedShadowBoneBlade"],
    34: ["FemaleVessel_YuDing", "YuBi_LengTu"], 36: ["GuiChai_Yellow_MoDao_LengTu", "GuiChai_Cyan_ShuangYue_LengTu"],
    38: ["LeiDang"], 40: ["MBoss", "LeiDang"],
    41: ["MengHua", "LengTu"], 42: ["PowerMan"], 44: ["ChangHong_Spear"],
    46: ["LionDancer", "YuCong"], 48: ["HookMan", "GuWanYue"],
}
PAIRS = {
    3: [("GuiChai_Yellow_ShuangGou", "GuiChai_Yellow_MoDao"), ("QiShu_Bing", "MobMan_Elite_Sword")],
    13: [("DuYanBoss", "YouLong_Harpoon"), ("LuanShi", "FengXueDaoKe")],
    23: [("Yuu", "TieGui")],
    33: [("YanYan_Club", "MobMan_Elite"), ("JadeMaskMan", "YingMei_RedShadowBoneBlade")],
    43: [("FengXueDaoKe", "YingMei_RedShadowBoneBlade"), ("IronMaskMan_Boss", "Hun")],
}
TEAM_SHAPES = {5: [2, 2], 9: [2, 1, 1], 15: [2, 2], 19: [2, 2], 25: [3, 3], 29: [2, 1], 35: [1, 1, 1, 1, 1], 39: [2, 2], 45: [3, 3], 49: [3, 3]}


def floors(s, bosses):
    by_path = {b["path"]: b for b in bosses}
    result = []
    for floor in range(1, 51):
        zone = (floor - 1) // 10 + 1
        encounters, rules = [], {"doors": 2, "max_summoners_per_encounter": 1, "identity_max_per_run": 2, "repeat_identity_requires_variant_or_affix": True, "no_same_identity_within_zone": True, "spawn_requires_audit": True}
        rules.update({"ordinary_mobs_allowed": False, "non_boss_enemy_kind": "elite", "non_boss_named_only": True,
                      "non_boss_default_count": 4, "non_boss_max_simultaneous": 5, "native_ambient_mobs": "isolate_before_entry",
                      "spawn_owner": "tower", "native_boss_autospawn": False, "load_scene_before_spawn": True,
                      "scene_dependencies": "validate_and_bind_per_boss", "campaign_callbacks": "isolate",
                      "content_policy_source": "notes/tower-venue-test.md#content-policy"})

        def add(stems, mode="simultaneous", teams=None, **extra):
            paths = [s.path(x) for x in stems]
            reasons = []
            if teams is not None or floor in PAIRS:
                if any(by_path[p]["class"] != "A" for p in paths):
                    reasons.append("nonnative_multi_requires_class_A")
            if floor % 10 != 0 and any("B" in by_path[p]["classes"] for p in paths):
                reasons.append("scene_dependent_boss_outside_landmark_floor")
            if mode != "group" and len({by_path[p]["identity"] for p in paths}) != len(paths):
                reasons.append("duplicate_identity")
            if any("phase2_only" in by_path[p]["tags"] for p in paths):
                reasons.append("standalone_phase2_forbidden")
            if sum("summoner" in by_path[p]["tags"] for p in paths) > 1:
                reasons.append("multiple_summoners")
            encounters.append(meta(PLAN + "#三、逐层表", evidence="unverified", id=f"floor_{floor:02}_door_{len(encounters)+1}", bosses=paths, mode=mode, teams=teams, enabled=not reasons, blocked_reasons=reasons, **extra))

        if floor in PAIRS:
            floor_type = "1v2"
            rules.update({"allowed_class": ["A"], "boss_count": 2, "hp_per_boss": .65, "damage_per_boss": .75, "minimum_card_rarity": 1})
            for pair in PAIRS[floor]:
                add(pair)
        elif floor in TEAM_SHAPES:
            floor_type = "team"
            rules.update({"allowed_class": ["A"], "teams": TEAM_SHAPES[floor], "minimum_card_rarity": 1, "soul_capture_opportunity": True, "requires_engine": ["teams", "soul_contract"]})
            candidates = [b for b in bosses if b["tier"] == zone and b["class"] == "A" and b["role"] == "primary"]
            if floor == 5:
                candidates = [b for b in candidates if "GuiChai" in b["id"]] or candidates
            for b in candidates:
                add([b["id"].removesuffix("_C")], teams=TEAM_SHAPES[floor], selection="sample_distinct_identities_at_runtime")
            if floor == 29:
                rules.update({"challenger_count": 5, "sequential_challengers": True})
            if floor == 19:
                rules["betrayal"] = True
            if floor == 49:
                rules["enemy_affix"] = True
        elif floor in (17, 37):
            floor_type = "spectate"
            rules.update({"allowed_class": ["A"], "weapon_drops": False, "hidden_power_jitter": .15, "max_bet_fraction": .5, "advance_on_either_result": True, "requires_engine": ["spectator_safety", "betting"]})
            if floor == 17:
                add(["ChiLian", "LuanShi"], teams=[1, 1])
                add(["YouLong_Harpoon", "DuYanBoss"], teams=[1, 1])
            else:
                add(["GreatBladeMan", "YanYan_Club", "MobMan_Elite", "JadeMaskMan"], teams=[2, 2])
        elif floor == 7:
            floor_type = "seasoning"
            add(["MoTian_LongZhu_01", "MoTian_LongZhu_02"], mode="group", exception="approved_gold_mask_pair")
            rules.update({"group_size": 2, "group_identity_exception": "gold_mask_guard", "requires_engine": ["group"]})
        elif floor == 27:
            floor_type = "seasoning"
            add(["EMei_Sword_JianZhen", "XiaoXue", "YuBi"], mode="sequential")
            rules.update({"heal_between_stages": False, "max_simultaneous_bosses": 1, "requires_engine": ["sequential"]})
        elif floor == 47:
            floor_type = "seasoning"
            # The plan specifies modifiers, not named enemies. Use A-class zone 5
            # candidates; if none exist, integration must block rather than guess.
            for b in bosses:
                if b["tier"] == zone and b["class"] == "A" and b["role"] == "primary":
                    add([b["id"].removesuffix("_C")])
            rules.update({"variants": ["two_hit_duel", "bullet_time", "execution_only"], "requires_engine": ["rule_floor"]})
        elif floor == 50:
            floor_type = "tower_lord"
            add(["UltimateBoss", "ZuoShang_Ending"], mode="sequential", final_phase=s.path("ZuoShang_P2"))
            rules.update({"stage_count": 3, "heal_between_stages": False, "max_simultaneous_bosses": 1, "requires_engine": ["sequential", "native_arena", "phase2"]})
        else:
            floor_type = "zone_boss" if floor % 10 == 0 else "1v1"
            for stem in SINGLES[floor]:
                add([stem])
            if floor_type == "zone_boss":
                rules.update({"native_arena_for_class_B": True, "minimum_card_rarity": 2, "capture_guaranteed": True, "exclude_identity_if_already_seen_this_zone": True})
        candidates = list(dict.fromkeys(p for enc in encounters for p in enc["bosses"]))
        result.append(meta(PLAN + "#三、逐层表", evidence="unverified", id=f"floor_{floor:02}", floor=floor, zone=zone, type=floor_type, candidates=candidates, encounters=encounters, rules=rules, enabled=False, verification="FLOOR-INTEGRATION in notes/codex-tower-zones.md"))
    return result
