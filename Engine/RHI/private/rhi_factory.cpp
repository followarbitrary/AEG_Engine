#include "../public/rhi.hpp"

#include "vulkan/vulkan_device.hpp"

namespace aeg::rhi {
    ResultValue<std::unique_ptr<Device> > create_device(const DeviceCreateDesc &desc) {
        switch (desc.backend) {
            case BackendType::kVulkan:
                return create_vulkan_device(desc);
        }
        return {Result::failure(ResultCode::kUnsupported, "Requested RHI backend is unavailable"), nullptr};
    }
} // namespace aeg::rhi
