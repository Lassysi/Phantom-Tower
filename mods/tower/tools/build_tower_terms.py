"""Prepare bilingual text for tower-owned items and powers, keyed by stable IDs.

Values describe the existing 1.1.0 rules. This is content data, not a patch to
the game or a claim that the in-game English UI is complete.
"""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / "mods/tower/data/localization-tower-en.json"

# English uses the game's "Sha-Chi" spelling. Do not translate these IDs or
# use English labels as game state, save keys, commands, or branch conditions.
ITEMS = {
    "heal": ("Rejuvenation Pill", "Immediately restores 40% HP."),
    "reroll": ("Reroll Token", "Reroll one set of three choices."),
    "freeze": ("Binding Talisman", "Freezes the boss for 4 seconds after combat begins."),
    "slow": ("Slowing Talisman", "Slows the boss by 40% for the first 10 seconds of combat."),
    "sunder": ("Armor Break Talisman", "The boss takes 30% more damage on this floor."),
    "shachi": ("Sha-Chi Recovery Pill", "Increases Sha-Chi recovery by 40% on this floor."),
    "intel": ("Intelligence", "Reveals information about one side in a spectator battle (not yet active)."),
    "borrow": ("Reinforcement Token", "Adds one ally to the next team battle (not yet active)."),
    "revive": ("Revival Talisman", "Automatically revives you once with full HP, full Sha-Chi, and refilled medicine."),
    "guard": ("Iron Armor Talisman", "Reduces damage taken by 20% on this floor."),
    "edge": ("Sharp Edge Talisman", "Increases attack and guard-breaking power by 15% on this floor."),
    "break": ("Stance Break Talisman", "Increases Sha-Chi damage dealt to the boss by 30% on this floor."),
    "rebirth": ("Rebirth Talisman", "If you fall without a Revival Talisman, continue 10 floors earlier instead of restarting. If you fall within the first 10 floors, continue from floor 1."),
}
POWERS = {
    "bati": ("Unshakable", "Prevents stagger on this floor."),
    "kuanxin": ("Open Mind", "Doubles the parry window on this floor."),
    "baidu": ("Immunity", "Grants immunity to status effects on this floor."),
    "yingwu": ("Endless Arsenal", "Grants unlimited Phantom Edges on this floor."),
    "jifeng": ("Swift Wind", "Increases attack speed by 25% on this floor."),
    "ponian": ("Instant Break", "Breaks enemy guard in one hit on this floor."),
    "huajin": ("Deflecting Force", "Automatically blocks normal attacks on this floor."),
    "youshen": ("Elusive Form", "Automatically dodges normal attacks on this floor."),
    "zidong": ("Auto Attack", "Automatically attacks on this floor."),
    "lingxi_blue": ("Instinct: Blue", "Automatically blocks blue-flash attacks on this floor."),
    "lingxi_red": ("Instinct: Red", "Automatically dodges red-flash attacks on this floor."),
}
BLESSINGS = {
    "temper": ("Honed Edge", "Attack and Sha-Chi attack +8%."),
    "iron": ("Iron Bones", "Damage taken -8%."),
    "life": ("Vitality", "Maximum HP +10%."),
    "break": ("Guard Breaker", "Sha-Chi damage dealt +15%."),
    "gather": ("Gathering Force", "Sha-Chi recovery speed +15%."),
    "swift": ("Swift Wind", "Attack speed +5%."),
    "spring": ("Rejuvenation", "Restore 15% HP at the start of each floor (retired; no longer offered)."),
    "last_stand": ("Last Stand", "Attack +20% while HP is below 30%."),
    "riposte": ("Riposte", "Attack +15% for 3 seconds after a perfect block."),
    "feast": ("Blood Feast", "Restore 10% HP after an execution."),
}


def main():
    payload = {"schema": 1, "source": "tower 1.1.0, tower_economy.h and tower_growth.h",
               "status": "draft: gameplay terminology and presentation require review",
               "categories": {kind: {k: {"name_en": v[0], "description_en": v[1]}
                                      for k, v in values.items()}
                              for kind, values in (("items", ITEMS), ("powers", POWERS), ("blessings", BLESSINGS))}}
    OUT.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("TOWER_TERMS_OK", *(f"{k}={len(v)}" for k, v in payload["categories"].items()))


if __name__ == "__main__":
    main()
