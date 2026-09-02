#include "vulkan_device.hpp"

namespace aeg::rhi {
    ResultValue<std::unique_ptr<Device>> create_device(const DeviceCreateDesc& desc) {
        return create_vulkan_device(desc);
    }
}  // namespace aeg::rhi
