#pragma once

#include <string_view>
#include "result.hpp"

namespace aeg::application::project { struct ProjectConfig; }
namespace aeg::module { struct ModuleMetadata; }

namespace aeg::tooling::detail {
/** The metadata must already have passed the formal bundle validator. */
[[nodiscard]] foundation::Result validate_project_module_agreement(
    const application::project::ProjectConfig& project, const module::ModuleMetadata& metadata,
    std::string_view configuration);
}
