#pragma once

#include "tower_l10n_text.h"
#include "tower_rewards.h"

// Offer wording only; selection, eligibility and all numerical effects still
// come from the original Offer and game rules.
namespace tower::language {
inline std::string OfferItemHint(const tower::rewards::Offer& o) {
    if (Current() != Language::English || o.kind != tower::rewards::Kind::Item || tower::rewards::Empty(o)) return {};
    return tower::rewards::ItemTiming(tower::kItems[o.index].item) == tower::rewards::Timing::Prep
               ? " Prepare in Mod Inventory before the next floor." : "";
}
inline std::string OfferEffect(const tower::rewards::Offer& o) {
    using namespace tower::rewards;
    if (Current() == Language::Chinese) return Effect(o);
    if (Empty(o)) return {};
    const auto tr = [](std::string_view s) { return Display(s, Language::English); };
    switch (o.kind) {
        case Kind::Blessing: {
            const auto& b = tower::growth::kBlessings[o.index];
            return std::format("Lasts for this run: {} Gain {} stack(s), up to {}.", tr(b.text), o.amount, b.maxStack);
        }
        case Kind::TemperPack: return "Spend in Preparation to upgrade weapons, one point per level.";
        case Kind::CultivatePack: return "Spend in Preparation to unlock ability nodes.";
        case Kind::Weapon: return std::format("{}: added to tower inventory at level 1. Equip under Esc → Character; tower only.",
                                              tower::growth::kWeapons[o.index].kind == 1 ? "Phantom Edge" : "Main Weapon");
        case Kind::Accessory: return std::format("Accessory: {} Equip under Esc → Character; lasts for this run.", tr(tower::growth::kAccessories[o.index].text));
        case Kind::Refine: return std::format("Refine to level {}: {}", o.amount, tr(tower::growth::kAccessories[o.index].text));
        case Kind::Medicine: return std::format("New medicine: {} Equip it in a medicine slot to use it in battle.", tr(tower::growth::kMedicines[o.index].text));
        case Kind::Item: return ItemTiming(tower::kItems[o.index].item) == Timing::Prep
            ? std::format("{} Stored in the Mod Inventory; prepare between floors. Activates when the next fight starts.", tr(tower::kItems[o.index].text))
            : tr(tower::kItems[o.index].text);
        case Kind::Jade: return "Spend at rest stops.";
        case Kind::Attr: return std::format("{} +{:g}%", tr(kAttrs[o.index].what), o.amount / 10.0);
        case Kind::SoulToken: return "Use under Preparation → Soul Forge to forge a weapon's soul.";
    }
    return {};
}
}
