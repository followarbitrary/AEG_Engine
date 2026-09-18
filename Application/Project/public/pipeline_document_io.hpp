#pragma once

#include <filesystem>

#include "pipeline_document.hpp"
#include "result.hpp"

namespace aeg::application::project {

[[nodiscard]] foundation::ResultValue<pipeline::PipelineDocument> load_pipeline_document(
    const std::filesystem::path& file);
[[nodiscard]] foundation::Result save_pipeline_document(const std::filesystem::path& file,
                                                        const pipeline::PipelineDocument& document);

}  // namespace aeg::application::project
