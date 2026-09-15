#pragma once

#include <cstdint>
#include <string>

namespace aeg::application {
// Overview intentionally groups compiled implementation details by execution phase.
inline std::string pipeline_overview_key(uint32_t phase) {
    return "phase:" + std::to_string(phase);
}
}  // namespace aeg::application
