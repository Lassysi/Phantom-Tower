#include "../src/tower_language.h"
#include "../src/tower_l10n_text.h"
#include "../src/tower_logic.h"
#include "../src/tower_l10n_offer.h"
#include <cassert>
#include <cstdio>

// Run against a dedicated LOCALAPPDATA fixture, never the player's save.
int main() {
    using tower::language::Language;
    tower::Run a;
    a.Start(42, 3);
    assert(a.NextFloor(10) > 0);
    a.FloorCleared({});
    const auto baseline = a.Save();
    assert(tower::language::Set(Language::English));
    assert(tower::language::Display("第 1 层通过", Language::English) == "Floor 1 Cleared");
    const int med = tower::growth::MedicineIndex("Hulu");
    assert(med >= 0);
    const tower::rewards::Offer medicine{tower::rewards::Kind::Medicine, med, 1, 1};
    const auto englishEffect = tower::language::OfferEffect(medicine);
    assert(englishEffect.find("40%") != std::string::npos && englishEffect.find("medicine") != std::string::npos);
    const tower::rewards::Offer prep{tower::rewards::Kind::Item, static_cast<int>(tower::Item::Break), 1, 1};
    const tower::rewards::Offer passive{tower::rewards::Kind::Item, static_cast<int>(tower::Item::Revive), 1, 1};
    assert(tower::language::OfferItemHint(prep).find("before the next floor") != std::string::npos);
    assert(tower::language::OfferItemHint(passive).empty());
    assert(tower::language::OfferEffect(passive).find("prepare between floors") == std::string::npos);
    assert(tower::language::OfferEffect(prep).find("prepare between floors") != std::string::npos);
    assert(a.Save() == baseline);
    tower::Run loaded;
    assert(loaded.Load(baseline));
    assert(loaded.Save() == baseline);
    assert(tower::language::Set(Language::Chinese));
    assert(tower::language::OfferItemHint(prep).empty());
    assert(tower::language::OfferEffect(medicine) == tower::rewards::Effect(medicine));
    assert(a.Save() == baseline);
    assert(loaded.Pick(0));
    assert(loaded.phase == tower::Phase::Ready);
    tower::Run resumed;
    assert(resumed.Load(loaded.Save()));
    assert(resumed.phase == tower::Phase::Ready);
    std::puts("LANGUAGE_RUN_SAVE_OK");
}
