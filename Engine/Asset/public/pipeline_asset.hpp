#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "pipeline_document.hpp"

namespace aeg::asset {

/** @brief Encodes the executable portion of one authored Pipeline document. */
[[nodiscard]] foundation::ResultValue<std::vector<std::byte>> encode_pipeline_asset(
    const pipeline::PipelineDocument& document);

/** @brief Decodes and bounds-checks an executable Pipeline asset payload. */
[[nodiscard]] foundation::ResultValue<pipeline::PipelineDocument> decode_pipeline_asset(
    std::span<const std::byte> bytes);

}  // namespace aeg::asset
