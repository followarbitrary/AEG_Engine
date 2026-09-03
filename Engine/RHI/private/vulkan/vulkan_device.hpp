/**
 * @file vulkan_device.hpp
 * @brief Private declaration of the Vulkan backend factory entry point.
 */
#pragma once

#include <memory>

#include "../../public/rhi.hpp"

namespace aeg::rhi {
    [[nodiscard]] foundation::ResultValue<std::unique_ptr<Device> > create_vulkan_device(
        const DeviceCreateDesc &desc);
} // namespace aeg::rhi
