#pragma once

#include <functional>
#include <optional>
#include <string_view>

#include "feature_graph_io.hpp"

namespace aeg::application::authoring::detail {
[[nodiscard]] foundation::Result validate_source_integrity(std::string_view bytes);
[[nodiscard]] std::string protect_source_integrity(std::string_view bytes);
[[nodiscard]] foundation::Result publish_feature_graph_source(
    const std::filesystem::path& file, std::string_view bytes,
    const std::optional<FeatureGraphFileRevision>& expected,
    const std::function<foundation::Result(const std::filesystem::path&)>& validate,
    bool preserve_previous = true);
} // namespace aeg::application::authoring::detail
