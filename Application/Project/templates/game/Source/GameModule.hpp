#pragma once

#include "feature_module.hpp"

// One declaration factory is shared by the DLL and its host contract exporter.
// Construct contracts and callbacks here; initialize runtime/GPU resources only
// when the host executes the registered Feature callbacks.
[[nodiscard]] aeg::module::FeatureModuleDefinition aeg_define_GameModule();
