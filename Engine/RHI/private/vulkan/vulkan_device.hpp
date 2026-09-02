#pragma once

#include <memory>

#include "../../public/rhi.hpp"

namespace aeg::rhi {
    [[nodiscard]] ResultValue<std::unique_ptr<Device> > create_vulkan_device(const DeviceCreateDesc &desc);
} // namespace aeg::rhi
