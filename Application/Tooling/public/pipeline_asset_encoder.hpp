#pragma once

#include <cstddef>
#include <vector>

#include "pipeline_document.hpp"
#include "result.hpp"

namespace aeg::tooling {

[[nodiscard]] foundation::ResultValue<std::vector<std::byte>> encode_pipeline_asset(
    const pipeline::PipelineDocument& document);

}  // namespace aeg::tooling
