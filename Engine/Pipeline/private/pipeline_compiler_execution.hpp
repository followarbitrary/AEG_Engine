#pragma once

#include "pipeline.hpp"

namespace aeg::pipeline::detail {

/**
 * @brief Engine-owned access to Pipeline definition compilation.
 *
 * PipelineCompileInput describes an Engine-private technical projection;
 * the compiler implementation and registry/builder access stay behind this bridge.
 */
struct PipelineCompilerExecution final {
    [[nodiscard]] static foundation::ResultValue<PipelinePlan> compile(const PipelineCompileInput& input);
};

}  // namespace aeg::pipeline::detail
