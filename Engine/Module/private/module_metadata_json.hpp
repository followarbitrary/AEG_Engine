#pragma once

#include <string_view>

#include "module_bundle.hpp"

namespace aeg::module::detail {
[[nodiscard]] foundation::ResultValue<std::string> encode_metadata_json(const ModuleMetadata& metadata);
[[nodiscard]] foundation::ResultValue<ModuleMetadata> decode_metadata_json(std::string_view json);
[[nodiscard]] foundation::ResultValue<std::string> metadata_sha256(std::string_view bytes);
}  // namespace aeg::module::detail
