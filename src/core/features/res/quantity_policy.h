#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace pbz::res {
// Source scopes are synchronous only. An asynchronous/unknown grant is unchanged.
enum class Source { Unknown, CombatLoot, ActorPickup, Shop, Upgrade };
struct Context {
    Source source = Source::Unknown;
    unsigned itemDepth = 0, dustDepth = 0;
};
struct Scope {
    Context& context;
    Context previous;
    Scope(Context& c, Source s) : context(c), previous(c) { context.source = s; }
    ~Scope() { context = previous; }
};
struct Depth {
    unsigned& depth;
    explicit Depth(unsigned& d) : depth(d) { ++depth; }
    ~Depth() { --depth; }
};
// Floor rewards and ceil costs: a fractional reward cannot create an item,
// and a nonzero price cannot become free through integer truncation. Zero
// cost is explicit. Refuse overflow rather than grant/charge INT_MAX.
inline bool ScaleCount(int32_t input, double multiplier, bool cost, int32_t& output) {
    if (input < 0 || !std::isfinite(multiplier) || multiplier < 0 || multiplier > 10) return false;
    const double product = static_cast<double>(input) * multiplier;
    const double rounded = cost ? std::ceil(product) : std::floor(product);
    if (rounded > std::numeric_limits<int32_t>::max()) return false;
    output = static_cast<int32_t>(rounded);
    return true;
}
inline double Bounded(double value, double minimum, double maximum) {
    return std::isfinite(value) ? std::clamp(value, minimum, maximum) : 1.0;
}
}  // namespace pbz::res
