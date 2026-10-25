#pragma once

#include "pipeline.hpp"

namespace aeg::pipeline::detail {

/**
 * @brief Pipeline-owned access to PhaseRegistry execution metadata.
 *
 * Registration and declaration lookup remain on the public authoring
 * contract. Compiler and frame orchestration use this opaque bridge for the
 * lock-consistent phase ordering snapshot and mutation revision.
 */
struct PhaseRegistryExecution final {
    [[nodiscard]] static foundation::ResultValue<PhaseRegistrySnapshot> snapshot(const PhaseRegistry& registry);
    [[nodiscard]] static uint64_t revision(const PhaseRegistry& registry) noexcept;
};

}  // namespace aeg::pipeline::detail
