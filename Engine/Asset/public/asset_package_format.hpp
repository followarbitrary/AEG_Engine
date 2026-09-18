/**
 * @file asset_package_format.hpp
 * @brief Stable constants shared by runtime package readers and tooling writers.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace aeg::asset::package_format {
inline constexpr uint32_t kVersion = 1;
inline constexpr std::size_t kHeaderBytes = 64;
inline constexpr std::size_t kIndexEntryBytes = 48;
inline constexpr std::size_t kPayloadAlignment = 16;
}  // namespace aeg::asset::package_format
