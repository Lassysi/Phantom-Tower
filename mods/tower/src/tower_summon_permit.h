#pragma once

namespace tower::summon {
struct Identity {
    void* pawn = nullptr;
    void* asc = nullptr;
    int pawnIndex = -1, ascIndex = -1;
    bool Valid() const { return pawn && asc && pawnIndex >= 0 && ascIndex >= 0; }
    bool operator==(const Identity&) const = default;
};

// Track only the one loose tag added by the tower. Never set the native count
// to a constant: other effects and area triggers may own their own counts.
struct Lease {
    Identity owner{};
    bool held = false;
    template<class Live, class Remove> void Reset(Live live, Remove remove) {
        if (held && live(owner)) remove(owner);
        owner = {}; held = false;
    }
    template<class Live, class Add, class Remove>
    void Update(bool active, Identity current, Live live, Add add, Remove remove) {
        if (held && (!active || !(owner == current) || !live(owner))) Reset(live, remove);
        if (!held && active && current.Valid() && live(current) && add(current)) { owner = current; held = true; }
    }
};
}
