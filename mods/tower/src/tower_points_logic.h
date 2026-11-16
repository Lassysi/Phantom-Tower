#pragma once
#include <cstdint>
namespace tower::points {
// A native hold-to-upgrade may re-enter several times before the next-frame readback.
// An exit/restart invalidates old callbacks rather than refunding another session.
struct Flight {
    uint64_t sequence = 0, active = 0;
    bool pending = false;
    uint64_t Begin() { if (pending) return 0; pending = true; return active = ++sequence; }
    bool Finish(uint64_t token) { if (!pending || !token || token != active) return false; pending = false; return true; }
    void Reset() { pending = false; active = ++sequence; }
};
} // namespace tower::points
