/**
 * @file rhi_factory.cpp
 * @brief Selects and constructs the compiled RHI backend.
 */
#include "../public/rhi.hpp"

#include "vulkan/vulkan_device.hpp"

namespace aeg::rhi {
    foundation::ResultValue<std::unique_ptr<Device> > create_device(const DeviceCreateDesc &desc) {
        switch (desc.backend) {
            case BackendType::kVulkan:
                return create_vulkan_device(desc);
        }
        return {foundation::Result::failure(foundation::ResultCode::kUnsupported,
                                            "Requested RHI backend is unavailable"), nullptr};
    }
} // namespace aeg::rhi
