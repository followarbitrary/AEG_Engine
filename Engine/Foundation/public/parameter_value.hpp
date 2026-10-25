/**
 * @file parameter_value.hpp
 * @brief Neutral scalar/vector/asset parameter values shared by domains.
 *
 * This type is intentionally below Pipeline and Asset. Pipeline authoring,
 * material payloads, and shader/runtime adapters may use the same value model
 * without introducing a module dependency between those domains.
 */
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace aeg::foundation {

struct AssetReference final {
    std::string uuid{};
    friend bool operator==(const AssetReference&, const AssetReference&) = default;
};

using ParameterValue =
    std::variant<bool, int64_t, uint64_t, double, std::string, std::vector<double>, AssetReference>;
using ParameterBlock = std::map<uint64_t, ParameterValue>;

}  // namespace aeg::foundation
