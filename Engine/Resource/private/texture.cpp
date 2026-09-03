#include "../public/texture.hpp"

#include <limits>

namespace aeg::resource {
    foundation::Result validate_texture_data(const TextureData& texture) noexcept {
        const rhi::TextureDesc& desc = texture.desc;
        if (desc.format == rhi::Format::kUnknown || desc.extent.width == 0 || desc.extent.height == 0 ||
            desc.extent.depth == 0 || desc.mip_levels == 0 || desc.array_layers == 0) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                "Texture description dimensions and format must be valid");
        }
        if (desc.capabilities == rhi::TextureCapability::kNone) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                "Texture capabilities must not be empty");
        }
        if (desc.samples != rhi::SampleCount::kX1) {
            return foundation::Result::failure(foundation::ResultCode::kUnsupported,
                                                "Texture uploads require single-sample textures");
        }
        if (desc.memory == rhi::MemoryClass::kUpload || desc.memory == rhi::MemoryClass::kReadback) {
            return foundation::Result::failure(foundation::ResultCode::kUnsupported,
                                                "Texture uploads require device-local or automatic memory");
        }

        switch (desc.dimension) {
            case rhi::TextureDimension::kTex1D:
                if (desc.extent.height != 1 || desc.extent.depth != 1) {
                    return foundation::Result::failure(
                            foundation::ResultCode::kInvalidArgument,
                            "1D textures must have height and depth equal to one");
                }
                break;
            case rhi::TextureDimension::kTex2D:
                if (desc.extent.depth != 1) {
                    return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                        "2D textures must have depth equal to one");
                }
                break;
            case rhi::TextureDimension::kTex3D:
                if (desc.array_layers != 1) {
                    return foundation::Result::failure(
                            foundation::ResultCode::kInvalidArgument,
                            "3D textures must use exactly one array layer");
                }
                break;
        }

        const uint64_t expected_count = static_cast<uint64_t>(desc.mip_levels) * desc.array_layers;
        if (expected_count != texture.subresources.size()) {
            return foundation::Result::failure(
                    foundation::ResultCode::kInvalidArgument,
                    "Texture subresource count must equal mip levels multiplied by array layers");
        }
        for (const TextureSubresourceData& subresource: texture.subresources) {
            if (subresource.data.empty()) {
                return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                    "Texture subresource data must not be empty");
            }
        }
        if (texture.subresources.size() > std::numeric_limits<uint32_t>::max()) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                "Texture contains too many subresources");
        }
        return foundation::Result::success();
    }
} // namespace aeg::resource
