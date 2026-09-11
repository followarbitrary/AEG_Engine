#include "imgui_texture_registry.hpp"

namespace aeg::editor {
foundation::ResultValue<ImGuiTextureHandle> ImGuiTextureRegistry::register_texture(ImGuiTextureBinding binding) {
    if (!binding.sampled_view.is_valid() || !binding.sampler.is_valid()) {
        return {foundation::invalid_argument("ImGui texture binding requires a sampled view and sampler"), {}};
    }

    std::scoped_lock lock(mutex_);
    for (uint32_t index = 0; index < slots_.size(); ++index) {
        Slot& slot = slots_[index];
        if (!slot.occupied) {
            slot.binding = binding;
            slot.occupied = true;
            return {foundation::Result::success(), ImGuiTextureHandle{index, slot.generation}};
        }
    }

    if (slots_.size() == UINT32_MAX) {
        return {foundation::out_of_memory("ImGui texture registry exhausted"), {}};
    }
    slots_.push_back(Slot{1, binding, true});
    return {foundation::Result::success(),
            ImGuiTextureHandle{static_cast<uint32_t>(slots_.size() - 1), slots_.back().generation}};
}

foundation::Result ImGuiTextureRegistry::update_texture(ImGuiTextureHandle texture, ImGuiTextureBinding binding) {
    if (!binding.sampled_view.is_valid() || !binding.sampler.is_valid()) {
        return foundation::invalid_argument("ImGui texture binding requires a sampled view and sampler");
    }

    std::scoped_lock lock(mutex_);
    if (texture.index >= slots_.size()) {
        return foundation::not_found("ImGui texture identity is not registered");
    }
    Slot& slot = slots_[texture.index];
    if (!slot.occupied || slot.generation != texture.generation) {
        return foundation::not_found("ImGui texture identity is stale");
    }
    slot.binding = binding;
    return foundation::Result::success();
}

foundation::Result ImGuiTextureRegistry::unregister_texture(ImGuiTextureHandle texture) {
    std::scoped_lock lock(mutex_);
    if (texture.index >= slots_.size()) {
        return foundation::not_found("ImGui texture identity is not registered");
    }

    Slot& slot = slots_[texture.index];
    if (!slot.occupied || slot.generation != texture.generation) {
        return foundation::not_found("ImGui texture identity is stale");
    }
    slot.binding = {};
    slot.occupied = false;
    ++slot.generation;
    if (slot.generation == 0) {
        slot.generation = 1;
    }
    return foundation::Result::success();
}

foundation::ResultValue<ImGuiTextureBinding> ImGuiTextureRegistry::resolve(ImGuiTextureHandle texture) const {
    std::scoped_lock lock(mutex_);
    if (texture.index >= slots_.size()) {
        return {foundation::not_found("ImGui texture identity is not registered"), {}};
    }

    const Slot& slot = slots_[texture.index];
    if (!slot.occupied || slot.generation != texture.generation) {
        return {foundation::not_found("ImGui texture identity is stale"), {}};
    }
    return {foundation::Result::success(), slot.binding};
}

void ImGuiTextureRegistry::clear() noexcept {
    std::scoped_lock lock(mutex_);
    for (Slot& slot : slots_) {
        slot.binding = {};
        slot.occupied = false;
        ++slot.generation;
        if (slot.generation == 0) {
            slot.generation = 1;
        }
    }
}
}  // namespace aeg::editor
