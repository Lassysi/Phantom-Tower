#include "../res/runtime_contract.h"

namespace pbz {
void RegisterWorldFeatures() {
    using namespace fx;
    Features().push_back(std::make_unique<runtime::BoundedEffect>("world.move_speed",
        std::vector<Effect>{A(Who::Player, "WalkSpeed", Kind::Scale), A(Who::Player, "JogSpeed", Kind::Scale),
                            A(Who::Player, "SprintSpeed", Kind::Scale)},
        std::vector<std::string>{"WalkSpeed", "JogSpeed", "SprintSpeed"}));
}
}  // namespace pbz
