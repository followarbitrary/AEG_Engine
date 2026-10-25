#include "project_module_agreement.hpp"

#include "project_context.hpp"
#include "module_bundle.hpp"

namespace aeg::tooling::detail {
foundation::Result validate_project_module_agreement(
    const application::project::ProjectConfig& project, const module::ModuleMetadata& metadata,
    std::string_view configuration) {
    if (project.module.id.empty() || project.target.empty() || configuration.empty())
        return foundation::invalid_argument("Project module identity, platform and configuration are required");
    if (module::ModuleId(project.module.id) != metadata.module.id)
        return foundation::invalid_argument("Selected module identity does not match the project");
    if (project.target != metadata.target)
        return foundation::invalid_argument("Selected module platform does not match the project");
    if (configuration != metadata.configuration)
        return foundation::invalid_argument("Selected module configuration does not match the request");
    return foundation::Result::success();
}
}
