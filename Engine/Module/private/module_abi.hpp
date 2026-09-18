#pragma once
#include "../abi/module_api.h"
#include "result.hpp"
namespace aeg::module {
[[nodiscard]] foundation::Result validate_module_api(const AegAbiModuleApi&, uint64_t supported_capabilities) noexcept;
}
namespace aeg::module {
[[nodiscard]] foundation::Result validate_abi_value(const AegAbiValue&) noexcept;
}
