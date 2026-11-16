#pragma once
#include <cstdint>

namespace towerwidgets::modal {
// Build26266538 ECommonInputMode, checked against the local SDK/router.
enum class InputMode : uint8_t { Menu = 0, Game = 1, All = 2, Unknown = 255 };
// Activation-focus support is a capability, not evidence of current input capture.
// Only the native router's current, active, external leaf in a menu-input mode
// can keep the Tower menu waiting. Tower-owned panels are handled separately.
inline bool Blocks(bool routerReady, InputMode mode, bool menuInput,
                   bool leafLive, bool leafActive, bool own) {
    return routerReady && menuInput &&
           (mode == InputMode::Menu || mode == InputMode::All) &&
           leafLive && leafActive && !own;
}
} // namespace towerwidgets::modal
