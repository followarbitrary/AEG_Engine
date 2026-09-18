#include "editor_preference_controller.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <new>

#include "editor_imgui.hpp"

namespace aeg::application {
namespace {
foundation::Result copy_catalog(const editor::EditorFontCatalog& source, editor::EditorFontCatalog& destination) {
    editor::EditorFontCatalog copy;
    for (const auto& asset : source.assets()) {
        if (const auto result = copy.register_font(asset.descriptor, asset.font_data, asset.face_index); !result)
            return result;
    }
    destination = std::move(copy);
    return foundation::Result::success();
}
}  // namespace

foundation::Result EditorPreferenceController::initialize(const editor::EditorFontCatalog& bundled_catalog,
                                                          std::string_view default_font_key, float default_font_size) {
    if (initialized_)
        return foundation::invalid_state("Editor preference controller is already initialized");
    if (desc_.repository == nullptr || desc_.store == nullptr || desc_.imgui_host == nullptr)
        return foundation::invalid_argument("Editor preference controller dependencies are incomplete");
    if (default_font_key.empty() || !std::isfinite(default_font_size) || default_font_size <= 0.0f)
        return foundation::invalid_argument("Editor preference defaults are invalid");

    try {
        if (const auto result = copy_catalog(bundled_catalog, bundled_catalog_); !result)
            return result;
        default_font_key_ = default_font_key;
        default_font_size_ = default_font_size;
        if (const auto result = rebuild_working_catalog(); !result)
            return result;
        if (working_catalog_.find(default_font_key_) == nullptr)
            return foundation::not_found("Editor preference default font is not registered");

        auto records = desc_.repository->list_records();
        if (!records)
            return records.result;
        user_records_ = std::move(records.value);
        applied_user_records_ = user_records_;
        std::string active_key = default_font_key_;
        float font_size = default_font_size_;
        if (const auto stored = desc_.store->load(); stored) {
            if (working_catalog_.find(stored.value.active_font_key) != nullptr) {
                active_key = stored.value.active_font_key;
                font_size = stored.value.font_size_pixels;
            }
        }
        editor::EditorPreferenceState state{};
        state.fonts = descriptors(working_catalog_);
        state.active_font_key = active_key;
        state.font_size_pixels = font_size;
        if (const auto result = copy_catalog(working_catalog_, applied_catalog_); !result)
            return result;
        model_ = std::make_unique<editor::EditorPreferenceModel>(std::move(state));
        initialized_ = true;
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor preference controller initialization allocation failed");
    }
}

foundation::Result EditorPreferenceController::rebuild_working_catalog() {
    editor::EditorFontCatalog merged;
    for (const auto& asset : bundled_catalog_.assets()) {
        if (const auto result = merged.register_font(asset.descriptor, asset.font_data, asset.face_index); !result)
            return result;
    }
    const auto users = desc_.repository->scan();
    if (!users)
        return users.result;
    for (const auto& asset : users.value.assets()) {
        if (const auto result = merged.register_font(asset.descriptor, asset.font_data, asset.face_index); !result)
            return result;
    }
    working_catalog_ = std::move(merged);
    return foundation::Result::success();
}

std::vector<editor::EditorFontDescriptor> EditorPreferenceController::descriptors(
    const editor::EditorFontCatalog& catalog) const {
    std::vector<editor::EditorFontDescriptor> result;
    result.reserve(catalog.font_count());
    for (const auto& asset : catalog.assets())
        result.push_back(asset.descriptor);
    return result;
}

foundation::Result EditorPreferenceController::refresh_font_texture(std::string_view key, float size) const {
    if (!desc_.refresh_font_texture)
        return foundation::Result::success();
    return desc_.refresh_font_texture(key, size);
}

foundation::Result EditorPreferenceController::preview(const editor::EditorPreferenceState& state) {
    if (!initialized_ || model_ == nullptr)
        return foundation::invalid_state("Editor preference controller is not initialized");
    if (working_catalog_.find(state.active_font_key) == nullptr)
        return foundation::not_found("Preview font is not registered");
    if (const auto result = desc_.imgui_host->replace_font_catalog(working_catalog_, state.active_font_key); !result)
        return result;
    if (const auto result = desc_.imgui_host->set_preview_font(state.active_font_key, state.font_size_pixels); !result)
        return result;
    return refresh_font_texture(state.active_font_key, state.font_size_pixels);
}

foundation::Result EditorPreferenceController::apply(const editor::EditorPreferenceState& state) {
    if (const auto result = preview(state); !result)
        return result;
    EditorPreferenceFileData file{};
    file.active_font_key = state.active_font_key;
    file.font_size_pixels = state.font_size_pixels;
    file.user_fonts = user_records_;
    if (const auto result = desc_.store->write_atomic(file); !result)
        return result;
    if (const auto result = desc_.repository->commit(); !result)
        return result;
    if (const auto result = copy_catalog(working_catalog_, applied_catalog_); !result)
        return result;
    applied_user_records_ = user_records_;
    model_->accept();
    return foundation::Result::success();
}

foundation::Result EditorPreferenceController::save(const editor::EditorPreferenceState& state) {
    return apply(state);
}

foundation::Result EditorPreferenceController::restore_applied_state() {
    if (const auto result = desc_.imgui_host->replace_font_catalog(applied_catalog_); !result)
        return result;
    if (const auto result = desc_.imgui_host->clear_preview_font(); !result)
        return result;
    const auto& state = model_->applied_state();
    if (const auto result = desc_.imgui_host->set_font(state.active_font_key, state.font_size_pixels); !result)
        return result;
    return refresh_font_texture(state.active_font_key, state.font_size_pixels);
}

foundation::Result EditorPreferenceController::cancel(const editor::EditorPreferenceState&) {
    if (!initialized_ || model_ == nullptr)
        return foundation::invalid_state("Editor preference controller is not initialized");
    desc_.repository->rollback();
    // Publish the applied catalog before restoring the host so Application
    // callbacks observe the same catalog that Cancel is about to display.
    working_catalog_ = applied_catalog_;
    user_records_ = applied_user_records_;
    if (const auto result = restore_applied_state(); !result)
        return result;
    model_->discard();
    return foundation::Result::success();
}

foundation::Result EditorPreferenceController::request_import_font() {
    if (!initialized_ || !desc_.open_font_dialog)
        return foundation::invalid_state("Editor font dialog is unavailable");
    const auto selected = desc_.open_font_dialog();
    if (!selected)
        return selected.result;
    if (!selected.value)
        return foundation::Result::success();
    const auto imported = desc_.repository->stage_import(*selected.value);
    if (!imported)
        return imported.result;
    if (const auto result = working_catalog_.register_font(imported.value.descriptor, imported.value.font_data,
                                                           imported.value.face_index);
        !result)
        return result;
    std::string extension = selected.value->extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    user_records_.push_back({imported.value.descriptor.key, imported.value.descriptor.key + extension});
    const auto fonts = descriptors(working_catalog_);
    return model_->set_draft_fonts(fonts);
}

foundation::Result EditorPreferenceController::request_remove_font(std::string_view font_key) {
    if (!initialized_ || model_ == nullptr)
        return foundation::invalid_state("Editor preference controller is not initialized");
    const auto* asset = working_catalog_.find(font_key);
    if (asset == nullptr)
        return foundation::not_found("Editor font key is not registered");
    if (!asset->descriptor.is_user_font || asset->descriptor.is_protected ||
        font_key == model_->state().active_font_key)
        return foundation::invalid_state("The selected editor font cannot be removed");
    if (const auto result = desc_.repository->stage_remove(font_key); !result)
        return result;
    static_cast<void>(working_catalog_.unregister_font(font_key));
    user_records_.erase(std::remove_if(user_records_.begin(), user_records_.end(),
                                       [font_key](const auto& record) { return record.key == font_key; }),
                        user_records_.end());
    return model_->set_draft_fonts(descriptors(working_catalog_));
}

foundation::Result EditorPreferenceController::reset_defaults() {
    if (!initialized_ || model_ == nullptr)
        return foundation::invalid_state("Editor preference controller is not initialized");
    return model_->reset_defaults();
}

}  // namespace aeg::application
