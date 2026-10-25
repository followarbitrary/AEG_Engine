#pragma once

#include <memory>
#include <vector>

#include "rhi.hpp"

namespace aeg::render_graph {
/** Shared native ownership used by graph allocations and Feature histories. Device outlives leases. */
struct TextureAllocationOwner {
    explicit TextureAllocationOwner(rhi::Device& device) : device(device) {}
    ~TextureAllocationOwner() { release_unreferenced(); }
    void release_unreferenced() {
        std::erase_if(views, [this](rhi::TextureViewHandle view) {
            const auto result = device.destroy_texture_view(view);
            return result || result.code == foundation::ResultCode::kNotFound;
        });
        if (views.empty() && texture.is_valid()) {
            const auto result = device.destroy_texture(texture);
            if (result || result.code == foundation::ResultCode::kNotFound) texture = {};
        }
    }
    rhi::Device& device;
    rhi::TextureHandle texture{};
    std::vector<rhi::TextureViewHandle> views{};
    /** Imported views keep their separately owned image alive until views retire. */
    std::shared_ptr<const void> imported_texture_owner{};
};
}
