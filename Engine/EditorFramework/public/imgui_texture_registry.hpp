/**
 * @file imgui_texture_registry.hpp
 * @brief Controlled mapping from editor texture identities to RHI views.
 */
#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

#include "editor_imgui_types.hpp"
#include "result.hpp"
#include "rhi_struct.hpp"

namespace aeg::editor {
struct ImGuiTextureBinding final {
    rhi::TextureViewHandle sampled_view{};
    rhi::SamplerHandle sampler{};
};

class ImGuiTextureRegistry final {
public:
    ImGuiTextureRegistry() = default;
    ~ImGuiTextureRegistry() = default;

    ImGuiTextureRegistry(const ImGuiTextureRegistry&) = delete;
    ImGuiTextureRegistry& operator=(const ImGuiTextureRegistry&) = delete;
    ImGuiTextureRegistry(ImGuiTextureRegistry&&) = delete;
    ImGuiTextureRegistry& operator=(ImGuiTextureRegistry&&) = delete;

    /** @brief Registers already-created RHI objects without taking their ownership. */
    [[nodiscard]] foundation::ResultValue<ImGuiTextureHandle> register_texture(ImGuiTextureBinding binding);
    /** @brief Replaces the RHI handles associated with an existing identity. */
    [[nodiscard]] foundation::Result update_texture(ImGuiTextureHandle texture, ImGuiTextureBinding binding);
    /** @brief Removes a texture identity after all submitted UI work is complete. */
    [[nodiscard]] foundation::Result unregister_texture(ImGuiTextureHandle texture);
    /** @brief Resolves an editor identity to backend-neutral RHI handles. */
    [[nodiscard]] foundation::ResultValue<ImGuiTextureBinding> resolve(ImGuiTextureHandle texture) const;
    /** @brief Removes all identities while leaving RHI object ownership untouched. */
    void clear() noexcept;

private:
    struct Slot final {
        uint32_t generation{1};
        ImGuiTextureBinding binding{};
        bool occupied{false};
    };

    mutable std::mutex mutex_{};
    std::vector<Slot> slots_{};
};
}  // namespace aeg::editor
