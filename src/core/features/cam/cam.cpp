#include <windows.h>
#include "../res/runtime_contract.h"
#include "../../host/log.h"
#include "../../services/hooks.h"

namespace pbz {
namespace {
#include "fov_feature.h"
}
void RegisterCamFeatures() { Features().push_back(std::make_unique<FovFeature>()); }
}
