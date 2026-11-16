namespace pbz {
void RegisterLootFeatures();
void RegisterResFeatures() {
    // Exactly two resource acquisition multipliers. Provenance stays internal;
    // neither this module nor the native bridge declares a Dust attribute bonus.
    RegisterLootFeatures();
    // Progress owns upgrade_free and weapon/cosmetic bulk actions. Do not
    // expose unimplemented crafting, ambiguous death-resource preservation,
    // or an all-skills shortcut with no verified acquisition contract.
}
}  // namespace pbz
