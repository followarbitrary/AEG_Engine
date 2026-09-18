/**
 * @file editor_imgui_host.cpp
 * @brief Implements the engine-owned Dear ImGui context and panel host.
 */
#include <imgui.h>
#include <imgui_freetype.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "editor_error.hpp"
#include "editor_imgui.hpp"
#include "imgui_platform_bridge.hpp"

namespace aeg::editor {
namespace {
foundation::Result capture_draw_data(const ImDrawData* draw_data, platform::WindowId window, uint64_t frame_number,
                                     ImGuiDrawPacket& packet) {
    if (draw_data == nullptr || !draw_data->Valid || !window.is_valid()) {
        return foundation::invalid_state("Dear ImGui viewport has no valid draw data or window identity");
    }

    try {
        ImGuiDrawPacket captured;
        captured.window = window;
        captured.display_pos_x = draw_data->DisplayPos.x;
        captured.display_pos_y = draw_data->DisplayPos.y;
        captured.display_width = draw_data->DisplaySize.x;
        captured.display_height = draw_data->DisplaySize.y;
        captured.framebuffer_scale_x = draw_data->FramebufferScale.x;
        captured.framebuffer_scale_y = draw_data->FramebufferScale.y;
        captured.frame_number = frame_number;
        if (draw_data->TotalVtxCount > 0) {
            captured.vertices.reserve(static_cast<std::size_t>(draw_data->TotalVtxCount));
        }
        if (draw_data->TotalIdxCount > 0) {
            captured.indices.reserve(static_cast<std::size_t>(draw_data->TotalIdxCount));
        }

        for (int list_index = 0; list_index < draw_data->CmdLists.Size; ++list_index) {
            const ImDrawList* list = draw_data->CmdLists[list_index];
            if (list == nullptr) {
                continue;
            }
            const uint32_t vertex_base = static_cast<uint32_t>(captured.vertices.size());
            const uint32_t index_base = static_cast<uint32_t>(captured.indices.size());
            for (const ImDrawVert& vertex : list->VtxBuffer) {
                captured.vertices.push_back(ImGuiDrawVertex{
                    vertex.pos.x,
                    vertex.pos.y,
                    vertex.uv.x,
                    vertex.uv.y,
                    vertex.col,
                });
            }
            for (const ImDrawIdx index : list->IdxBuffer) {
                captured.indices.push_back(static_cast<uint32_t>(index));
            }
            for (const ImDrawCmd& command : list->CmdBuffer) {
                if (command.UserCallback != nullptr) {
                    return foundation::unsupported("ImGui user draw callbacks are not supported by ImGuiDrawPacket");
                }
                const uint64_t texture_id = static_cast<uint64_t>(command.GetTexID());
                const ImGuiTextureHandle texture = decode_imgui_texture_id(texture_id);
                if (!texture.is_valid()) {
                    return foundation::invalid_argument("ImGui draw command contains an invalid texture ID");
                }
                captured.commands.push_back(ImGuiDrawCommand{
                    texture,
                    command.ClipRect.x,
                    command.ClipRect.y,
                    command.ClipRect.z,
                    command.ClipRect.w,
                    command.ElemCount,
                    index_base + command.IdxOffset,
                    static_cast<int32_t>(vertex_base) + static_cast<int32_t>(command.VtxOffset),
                });
            }
        }

        packet = std::move(captured);
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("ImGui draw packet allocation failed");
    } catch (const std::exception& exception) {
        return foundation::unknown_exception(exception);
    } catch (...) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                           "ImGui draw packet capture threw an unknown exception");
    }
}

[[nodiscard]] platform::WindowId viewport_window_id(const detail::ImGuiPlatformBridgeContext& context,
                                                    const ImGuiViewport* viewport) noexcept {
    if (viewport == nullptr) {
        return {};
    }
    if (viewport == ImGui::GetMainViewport()) {
        return context.window;
    }
    const auto* data = static_cast<const detail::ImGuiPlatformViewportData*>(viewport->PlatformUserData);
    return data != nullptr ? data->window : platform::WindowId{};
}
}  // namespace

struct EditorImGuiHost::Impl final {
    struct FontAtlasDeleter final {
        void operator()(ImFontAtlas* atlas) const noexcept { IM_DELETE(atlas); }
    };
    using FontAtlasPtr = std::unique_ptr<ImFontAtlas, FontAtlasDeleter>;

    struct PanelSlot final {
        uint32_t generation{1};
        std::unique_ptr<EditorPanel> panel{};
        bool visible{true};
    };

    /** @brief Dear ImGui context owned by this host. */
    ImGuiContext* context{nullptr};
    /** @brief Immutable initialization settings retained for the host lifetime. */
    EditorImGuiDesc desc{};
    /** @brief Engine-owned bridge for platform services and viewport callbacks. */
    detail::ImGuiPlatformBridgeContext platform_bridge{};
    /** @brief Protects events submitted by the platform pump before the frame boundary. */
    std::mutex event_mutex{};
    std::vector<platform::WindowEvent> pending_events{};
    std::vector<platform::WindowEvent> frame_events{};
    std::vector<PanelSlot> panels{};
    /** @brief Font assets copied from the application catalog at initialization. */
    std::vector<EditorFontAsset> fonts{};
    /** @brief CPU snapshot of the current atlas awaiting an application upload. */
    ImGuiFontAtlasData font_atlas{};
    /** @brief Detached atlases referenced by draw data until the next frame boundary. */
    std::vector<FontAtlasPtr> retired_font_atlases{};
    /** @brief Font bytes retained with retired atlases until the next frame boundary. */
    std::vector<std::vector<EditorFontAsset>> retired_font_catalogs{};
    /** @brief Engine-managed texture identity currently bound to the atlas. */
    ImGuiTextureHandle font_texture{};
    /** @brief Stable preference key of the active font. */
    std::string active_font_key{};
    /** @brief Base pixel size used to build the active font atlas. */
    float active_font_size_pixels{16.0f};
    /** @brief Stable preference key of the isolated preview font. */
    std::string preview_font_key{};
    /** @brief Base pixel size used to build the isolated preview font. */
    float preview_font_size_pixels{0.0f};
    /** @brief Font object used only by the Preferences preview sample. */
    ImFont* preview_font{nullptr};
    /** @brief Native-size display font used for crisp preference headings. */
    ImFont* display_font{nullptr};
    /** @brief Rasterized size of the native display face. */
    float display_font_size_pixels{0.0f};
    std::array<ImFont*, 4> preference_fonts{};
    ImFont* preference_icons{nullptr};
    ImVector<ImWchar> chinese_ranges{};
    /** @brief Monotonic frame identity copied into every captured viewport packet. */
    uint64_t frame_number{0};
    /** @brief True after initialization has installed the ImGui context and bridge. */
    bool initialized{false};
    /** @brief True between begin_frame() and end_frame(). */
    bool frame_active{false};
    /** @brief Prevents callbacks from invalidating the panel traversal. */
    bool building_panels{false};

    [[nodiscard]] const EditorFontAsset* find_font(std::string_view font_key) const noexcept {
        const auto iterator = std::find_if(fonts.begin(), fonts.end(), [font_key](const EditorFontAsset& asset) {
            return asset.descriptor.key == font_key;
        });
        return iterator == fonts.end() ? nullptr : &*iterator;
    }

    [[nodiscard]] const EditorFontAsset* find_chinese_fallback() const noexcept {
        const auto iterator = std::find_if(fonts.begin(), fonts.end(), [](const EditorFontAsset& asset) {
            return asset.descriptor.kind == EditorFontKind::kInterface && asset.descriptor.supports_chinese;
        });
        return iterator == fonts.end() ? nullptr : &*iterator;
    }

    [[nodiscard]] foundation::Result add_font(ImFontAtlas& atlas, std::string_view font_key, float size_pixels,
                                              ImFont*& output_font, bool include_chinese = true) {
        if (font_key.empty()) {
            ImFontConfig config{};
            config.SizePixels = size_pixels;
            output_font = atlas.AddFontDefault(&config);
            return output_font != nullptr ? foundation::Result::success()
                                          : foundation::invalid_state("Editor primary font could not be created");
        }

        const EditorFontAsset* asset = find_font(font_key);
        if (asset == nullptr) {
            return foundation::not_found("Editor font key is not registered");
        }
        if (const auto result = validate_editor_font_data(asset->font_data, asset->face_index); !result) {
            return result;
        }
        ImFontConfig config{};
        config.FontDataOwnedByAtlas = false;
        config.FontNo = static_cast<int>(asset->face_index);
        config.SizePixels = size_pixels;
        config.GlyphRanges = include_chinese && asset->descriptor.supports_chinese ? chinese_ranges.Data : nullptr;
        output_font = atlas.AddFontFromMemoryTTF(const_cast<std::byte*>(asset->font_data.data()),
                                                 static_cast<int>(asset->font_data.size()), size_pixels, &config);
        if (output_font == nullptr) {
            return foundation::invalid_argument("Editor font TTF data could not be loaded");
        }

        if (include_chinese && !asset->descriptor.supports_chinese) {
            const EditorFontAsset* fallback = find_chinese_fallback();
            if (fallback != nullptr && fallback != asset) {
                if (const auto result = validate_editor_font_data(fallback->font_data, fallback->face_index); !result) {
                    return result;
                }
                ImFontConfig fallback_config{};
                fallback_config.FontDataOwnedByAtlas = false;
                fallback_config.MergeMode = true;
                fallback_config.FontNo = static_cast<int>(fallback->face_index);
                fallback_config.SizePixels = size_pixels;
                if (atlas.AddFontFromMemoryTTF(const_cast<std::byte*>(fallback->font_data.data()),
                                               static_cast<int>(fallback->font_data.size()), size_pixels,
                                               &fallback_config, chinese_ranges.Data) == nullptr) {
                    return foundation::invalid_argument("Editor Chinese fallback font could not be loaded");
                }
            }
        }
        return foundation::Result::success();
    }

    [[nodiscard]] foundation::Result rebuild_font_atlas(std::string_view font_key, float size_pixels,
                                                        std::string_view next_preview_font_key = {},
                                                        float next_preview_font_size_pixels = 0.0f) {
        try {
            ImGuiIO& io = ImGui::GetIO();
            // Build in isolation: a rejected source, fallback or allocation must
            // leave every live font pointer and the uploaded texture intact.
            FontAtlasPtr candidate{IM_NEW(ImFontAtlas)};
            if (candidate == nullptr) {
                return foundation::out_of_memory("Dear ImGui font atlas allocation failed");
            }
            // Keep rasterization on the CPU while accepting both TrueType and
            // CFF OpenType outlines. GPU upload remains owned by Application.
            candidate->SetFontLoader(ImGuiFreeType::GetFontLoader());
            // The static-atlas renderer cannot request missing CJK glyphs while drawing.
            // Include every character in the multilingual preview, not just the common subset.
            // Source configurations retain this storage across atlas rebuilds.
            if (chinese_ranges.empty()) {
                ImFontGlyphRangesBuilder ranges;
                ranges.AddRanges(candidate->GetGlyphRangesChineseSimplifiedCommon());
                ranges.AddText("中文字符预览：场景 组件 属性 资源 多语言文本在编辑器界面中应清晰易读。");
                ranges.BuildRanges(&chinese_ranges);
            }
            std::array<ImFont*, 4> next_preference_fonts{};
            ImFont* next_preference_icons = nullptr;

            ImFont* primary_font = nullptr;
            if (const foundation::Result result = add_font(*candidate, font_key, size_pixels, primary_font); !result) {
                return result;
            }

            ImFont* next_preview_font = nullptr;
            if (!next_preview_font_key.empty()) {
                // Preview uses the draft font at actual size; Settings chrome
                // remains on the active font until Apply or Save.
                const float preview_raster_size = next_preview_font_size_pixels;
                if (next_preview_font_key == font_key && preview_raster_size == size_pixels) {
                    next_preview_font = primary_font;
                } else if (const foundation::Result result =
                               add_font(*candidate, next_preview_font_key, preview_raster_size, next_preview_font);
                           !result) {
                    return result;
                }
            }

            constexpr float kDisplayFontSizePixels = 32.0f;
            const std::string_view display_font_key = font_key;
            ImFont* next_display_font = nullptr;
            if (const foundation::Result result =
                    add_font(*candidate, display_font_key, kDisplayFontSizePixels, next_display_font, false);
                !result) {
                return result;
            }
            if (desc.enable_preference_fonts) {
                const auto settings_key = font_key;
                const float settings_size = size_pixels;
                const std::array<float, 4> sizes{settings_size * 0.875f, settings_size, settings_size * 1.25f,
                                                 settings_size * 1.5f};
                for (std::size_t index = 0; index < sizes.size(); ++index) {
                    if (settings_key == font_key && sizes[index] == kDisplayFontSizePixels) {
                        next_preference_fonts[index] = next_display_font;
                    } else if (const auto result =
                                   add_font(*candidate, settings_key, sizes[index], next_preference_fonts[index], true);
                               !result) {
                        return result;
                    }
                }
                const auto icon_asset = std::find_if(fonts.begin(), fonts.end(), [](const EditorFontAsset& asset) {
                    return asset.descriptor.display_name == "Segoe MDL2 Assets";
                });
                if (icon_asset != fonts.end()) {
                    if (const auto result = validate_editor_font_data(icon_asset->font_data, icon_asset->face_index);
                        !result) {
                        return result;
                    }
                    // Microsoft Segoe MDL2 Assets: Settings, KeyboardClassic, Font.
                    static constexpr ImWchar icon_ranges[]{0xE713, 0xE713, 0xE765, 0xE765, 0xE8D2, 0xE8D2, 0};
                    ImFontConfig config{};
                    config.FontDataOwnedByAtlas = false;
                    config.FontNo = static_cast<int>(icon_asset->face_index);
                    next_preference_icons = candidate->AddFontFromMemoryTTF(
                        const_cast<std::byte*>(icon_asset->font_data.data()),
                        static_cast<int>(icon_asset->font_data.size()), 20.0f, &config, icon_ranges);
                    if (next_preference_icons == nullptr) {
                        return foundation::invalid_argument("Editor preference icon font could not be loaded");
                    }
                }
            }

            if (primary_font == nullptr || next_display_font == nullptr) {
                return foundation::invalid_state("Editor primary font could not be created");
            }
            if (!candidate->Build()) {
                return foundation::invalid_state("Dear ImGui font atlas construction failed");
            }

            unsigned char* font_pixels = nullptr;
            int font_width = 0;
            int font_height = 0;
            candidate->GetTexDataAsRGBA32(&font_pixels, &font_width, &font_height);
            if (font_pixels == nullptr || font_width <= 0 || font_height <= 0) {
                return foundation::invalid_state("Dear ImGui font atlas construction failed");
            }
            const uint64_t row_pitch = static_cast<uint64_t>(font_width) * 4u;
            const uint64_t pixel_size = row_pitch * static_cast<uint64_t>(font_height);
            if (static_cast<uint64_t>(font_width) > std::numeric_limits<uint32_t>::max() ||
                static_cast<uint64_t>(font_height) > std::numeric_limits<uint32_t>::max() ||
                row_pitch > std::numeric_limits<uint32_t>::max() ||
                pixel_size > std::numeric_limits<std::size_t>::max()) {
                return foundation::invalid_state("Dear ImGui font atlas size exceeds engine limits");
            }

            ImGuiFontAtlasData captured{};
            captured.width = static_cast<uint32_t>(font_width);
            captured.height = static_cast<uint32_t>(font_height);
            captured.row_pitch = static_cast<uint32_t>(row_pitch);
            const auto* begin = reinterpret_cast<const std::byte*>(font_pixels);
            captured.rgba8_pixels.assign(begin, begin + static_cast<std::size_t>(pixel_size));
            // Own potentially aliased keys and finish allocations before committing.
            std::string committed_font_key{font_key};
            std::string committed_preview_key{next_preview_font_key};
            retired_font_atlases.reserve(retired_font_atlases.size() + 1);

            ImFontAtlas* previous_atlas = io.Fonts;
            ImGui::UnregisterFontAtlas(previous_atlas);
            io.Fonts = candidate.release();
            io.Fonts->OwnerContext = context;
            ImGui::RegisterFontAtlas(io.Fonts);
            io.FontDefault = primary_font;
            ImGui::SetCurrentFont(primary_font, size_pixels, 0.0f);
            // Finalized draw commands retain ImTextureData pointers. Keep their
            // atlas alive until begin_frame invalidates those commands.
            retired_font_atlases.emplace_back(previous_atlas);

            font_atlas = std::move(captured);
            active_font_key = std::move(committed_font_key);
            active_font_size_pixels = size_pixels;
            preview_font_key = std::move(committed_preview_key);
            preview_font_size_pixels = next_preview_font == nullptr ? 0.0f : next_preview_font_size_pixels;
            preview_font = next_preview_font;
            display_font = next_display_font;
            display_font_size_pixels = kDisplayFontSizePixels;
            preference_fonts = next_preference_fonts;
            preference_icons = next_preference_icons;
            font_texture = {};
            io.Fonts->SetTexID(ImTextureRef(static_cast<ImTextureID>(0)));
            return foundation::Result::success();
        } catch (const std::bad_alloc&) {
            return foundation::out_of_memory("Dear ImGui font atlas snapshot allocation failed");
        }
    }
};

EditorImGuiHost::EditorImGuiHost() : impl_(std::make_unique<Impl>()) {}

EditorImGuiHost::~EditorImGuiHost() {
    if (impl_ != nullptr && impl_->initialized) {
        static_cast<void>(abort_frame());
        static_cast<void>(guard_editor_operation([this] { return shutdown(); }));
    }
}

foundation::Result EditorImGuiHost::initialize(const EditorImGuiDesc& desc) {
    if (impl_->initialized) {
        return foundation::invalid_state("EditorImGuiHost is already initialized");
    }
    if (!std::isfinite(desc.font_global_scale) || desc.font_global_scale <= 0.0f) {
        return foundation::invalid_argument("EditorImGui font_global_scale must be finite and positive");
    }

    if (!std::isfinite(desc.font_size_pixels) || desc.font_size_pixels <= 0.0f) {
        return foundation::invalid_argument("Editor ImGui font_size_pixels must be finite and positive");
    }

    try {
        impl_->fonts.clear();
        if (desc.font_catalog != nullptr) {
            const std::span<const EditorFontAsset> assets = desc.font_catalog->assets();
            impl_->fonts.assign(assets.begin(), assets.end());
        }
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor font catalog copy allocation failed");
    }

    impl_->desc = desc;
    impl_->desc.font_catalog = nullptr;
    impl_->font_atlas.clear();
    impl_->font_texture = {};
    impl_->active_font_key.clear();
    impl_->active_font_size_pixels = desc.font_size_pixels;
    impl_->preview_font_key.clear();
    impl_->preview_font_size_pixels = 0.0f;
    impl_->preview_font = nullptr;
    impl_->display_font = nullptr;
    impl_->preference_fonts.fill(nullptr);
    impl_->preference_icons = nullptr;
    impl_->display_font_size_pixels = 0.0f;
    impl_->context = ImGui::CreateContext();
    if (impl_->context == nullptr) {
        return foundation::out_of_memory("Dear ImGui context creation failed");
    }

    ImGui::SetCurrentContext(impl_->context);
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags = desc.enable_keyboard_navigation ? ImGuiConfigFlags_NavEnableKeyboard : ImGuiConfigFlags_None;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    if (desc.enable_gamepad_navigation) {
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    }
    if (desc.enable_viewports && desc.window_system != nullptr) {
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    }
    io.FontGlobalScale = desc.font_global_scale;
    // Treat user-provided font data errors as Result failures instead of process-level assertions.
    io.ConfigErrorRecoveryEnableAssert = false;
    // The editor workspace owns its dock layout and rebuilds it from the registered
    // panel model. Avoid stale ImGui settings overriding the designed layout.
    io.IniFilename = nullptr;
    io.BackendPlatformName = "AEG.Editor.ImGuiPlatformBridge";
    io.BackendRendererName = nullptr;
    io.BackendFlags = ImGuiBackendFlags_None;
    detail::ImGuiPlatformBridge::install(io, impl_->platform_bridge, desc.platform_services, desc.window_system,
                                         desc.enable_viewports);
    if (const foundation::Result result = impl_->rebuild_font_atlas(desc.default_font_key, desc.font_size_pixels);
        !result) {
        ImGui::DestroyContext(impl_->context);
        impl_->context = nullptr;
        impl_->fonts.clear();
        impl_->font_atlas.clear();
        return result;
    }
    impl_->desc.default_font_key = {};
    impl_->initialized = true;
    return foundation::Result::success();
}

foundation::Result EditorImGuiHost::shutdown() {
    if (!impl_->initialized) {
        return foundation::Result::success();
    }
    if (impl_->frame_active) {
        return foundation::invalid_state("EditorImGuiHost cannot shut down during an active frame");
    }

    ImGui::SetCurrentContext(impl_->context);
    if (impl_->platform_bridge.viewports_enabled) {
        ImGui::DestroyPlatformWindows();
    }
    detail::ImGuiPlatformBridge::shutdown(ImGui::GetIO(), impl_->platform_bridge);
    ImGui::DestroyContext(impl_->context);
    impl_->context = nullptr;
    impl_->retired_font_atlases.clear();
    impl_->retired_font_catalogs.clear();
    impl_->pending_events.clear();
    impl_->frame_events.clear();
    impl_->panels.clear();
    impl_->fonts.clear();
    impl_->font_atlas.clear();
    impl_->font_texture = {};
    impl_->active_font_key.clear();
    impl_->active_font_size_pixels = 16.0f;
    impl_->preview_font_key.clear();
    impl_->preview_font_size_pixels = 0.0f;
    impl_->preview_font = nullptr;
    impl_->display_font = nullptr;
    impl_->preference_fonts.fill(nullptr);
    impl_->preference_icons = nullptr;
    impl_->display_font_size_pixels = 0.0f;
    impl_->desc = {};
    impl_->initialized = false;
    return foundation::Result::success();
}

foundation::Result EditorImGuiHost::submit_event(const platform::WindowEvent& event) {
    if (!impl_->initialized) {
        return foundation::invalid_state("EditorImGuiHost is not initialized");
    }
    if (!event.window.is_valid()) {
        return foundation::invalid_argument("Editor ImGui event has an invalid window identity");
    }

    std::scoped_lock lock(impl_->event_mutex);
    impl_->pending_events.push_back(event);
    return foundation::Result::success();
}

foundation::Result EditorImGuiHost::set_font_texture(ImGuiTextureHandle texture) {
    if (!impl_->initialized) {
        return foundation::invalid_state("EditorImGuiHost is not initialized");
    }
    if (!texture.is_valid()) {
        return foundation::invalid_argument("ImGui font texture identity is invalid");
    }
    if (impl_->frame_active) {
        return foundation::invalid_state("ImGui font texture cannot change during an active frame");
    }

    ImGui::SetCurrentContext(impl_->context);
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->SetTexID(ImTextureRef(static_cast<ImTextureID>(make_imgui_texture_id(texture))));
    impl_->font_texture = texture;
    return foundation::Result::success();
}

foundation::Result EditorImGuiHost::capture_font_atlas(ImGuiFontAtlasData& atlas) const {
    if (!impl_->initialized) {
        return foundation::invalid_state("EditorImGuiHost is not initialized");
    }
    try {
        ImGuiFontAtlasData captured = impl_->font_atlas;
        atlas = std::move(captured);
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Dear ImGui font atlas copy allocation failed");
    }
}

foundation::Result EditorImGuiHost::replace_font_catalog(const EditorFontCatalog& catalog,
                                                         std::string_view fallback_active_font_key) {
    if (!impl_->initialized) {
        return foundation::invalid_state("EditorImGuiHost is not initialized");
    }
    if (impl_->frame_active) {
        return foundation::invalid_state("Editor font catalog cannot change during an active frame");
    }

    try {
        // Reserve before changing the live catalog. Once the new atlas is
        // committed, this vector keeps old byte storage alive for finalized
        // draw data that still references its atlas.
        impl_->retired_font_catalogs.reserve(impl_->retired_font_catalogs.size() + 1);
        const auto assets = catalog.assets();
        std::vector<EditorFontAsset> replacement(assets.begin(), assets.end());
        std::vector<EditorFontAsset> previous = std::move(impl_->fonts);
        impl_->fonts = std::move(replacement);

        std::string target_active_key = impl_->active_font_key;
        if (!target_active_key.empty() && impl_->find_font(target_active_key) == nullptr) {
            if (fallback_active_font_key.empty() || impl_->find_font(fallback_active_font_key) == nullptr) {
                impl_->fonts = std::move(previous);
                return foundation::not_found("Active editor font is not present in the replacement catalog");
            }
            target_active_key = std::string(fallback_active_font_key);
        }
        if (target_active_key.empty() && !fallback_active_font_key.empty() &&
            impl_->find_font(fallback_active_font_key) != nullptr) {
            target_active_key = std::string(fallback_active_font_key);
        }

        std::string next_preview_key = impl_->preview_font_key;
        if (!next_preview_key.empty() && impl_->find_font(next_preview_key) == nullptr) {
            next_preview_key.clear();
        }
        ImGui::SetCurrentContext(impl_->context);
        const foundation::Result rebuilt = [&] {
            if (impl_->desc.enable_preference_fonts) {
                return impl_->rebuild_font_atlas(target_active_key, impl_->active_font_size_pixels, next_preview_key,
                                                 impl_->preview_font_size_pixels);
            }
            return impl_->rebuild_font_atlas(target_active_key, impl_->active_font_size_pixels);
        }();
        if (!rebuilt) {
            impl_->fonts = std::move(previous);
            return rebuilt;
        }
        impl_->retired_font_catalogs.emplace_back(std::move(previous));
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor font catalog replacement allocation failed");
    }
}

foundation::Result EditorImGuiHost::set_font(std::string_view font_key, float size_pixels) {
    if (!impl_->initialized) {
        return foundation::invalid_state("EditorImGuiHost is not initialized");
    }
    if (impl_->frame_active) {
        return foundation::invalid_state("Editor font cannot change during an active frame");
    }
    if (!std::isfinite(size_pixels) || size_pixels <= 0.0f) {
        return foundation::invalid_argument("Editor font size must be finite and positive");
    }
    if (!font_key.empty() && impl_->find_font(font_key) == nullptr) {
        return foundation::not_found("Editor font key is not registered");
    }

    ImGui::SetCurrentContext(impl_->context);
    return impl_->desc.enable_preference_fonts
               ? impl_->rebuild_font_atlas(font_key, size_pixels, impl_->preview_font_key,
                                           impl_->preview_font_size_pixels)
               : impl_->rebuild_font_atlas(font_key, size_pixels);
}

foundation::Result EditorImGuiHost::set_preview_font(std::string_view font_key, float size_pixels) {
    if (!impl_->initialized) {
        return foundation::invalid_state("EditorImGuiHost is not initialized");
    }
    if (impl_->frame_active) {
        return foundation::invalid_state("Editor preview font cannot change during an active frame");
    }
    if (font_key.empty()) {
        return foundation::invalid_argument("Editor preview font key is empty");
    }
    if (!std::isfinite(size_pixels) || size_pixels <= 0.0f) {
        return foundation::invalid_argument("Editor preview font size must be finite and positive");
    }
    if (impl_->find_font(font_key) == nullptr) {
        return foundation::not_found("Editor preview font key is not registered");
    }

    ImGui::SetCurrentContext(impl_->context);
    return impl_->rebuild_font_atlas(impl_->active_font_key, impl_->active_font_size_pixels, font_key, size_pixels);
}

foundation::Result EditorImGuiHost::clear_preview_font() {
    if (!impl_->initialized) {
        return foundation::invalid_state("EditorImGuiHost is not initialized");
    }
    if (impl_->frame_active) {
        return foundation::invalid_state("Editor preview font cannot change during an active frame");
    }
    ImGui::SetCurrentContext(impl_->context);
    return impl_->rebuild_font_atlas(impl_->active_font_key, impl_->active_font_size_pixels);
}

std::string_view EditorImGuiHost::active_font_key() const noexcept {
    if (impl_ == nullptr) {
        return {};
    }
    return impl_->active_font_key;
}

float EditorImGuiHost::active_font_size_pixels() const noexcept {
    return impl_ != nullptr ? impl_->active_font_size_pixels : 0.0f;
}

std::string_view EditorImGuiHost::preview_font_key() const noexcept {
    if (impl_ == nullptr) {
        return {};
    }
    return impl_->preview_font_key;
}

float EditorImGuiHost::preview_font_size_pixels() const noexcept {
    return impl_ != nullptr ? impl_->preview_font_size_pixels : 0.0f;
}

const void* EditorImGuiHost::preview_font() const noexcept {
    return impl_ != nullptr ? impl_->preview_font : nullptr;
}

const void* EditorImGuiHost::display_font() const noexcept {
    return impl_ != nullptr ? impl_->display_font : nullptr;
}

float EditorImGuiHost::display_font_size_pixels() const noexcept {
    return impl_ != nullptr ? impl_->display_font_size_pixels : 0.0f;
}

const void* EditorImGuiHost::preference_font(float size_pixels) const noexcept {
    constexpr std::array<float, 4> sizes{16.0f, 20.0f, 24.0f, 32.0f};
    if (impl_ != nullptr) {
        for (std::size_t index = 0; index < sizes.size(); ++index) {
            if (size_pixels == sizes[index])
                return impl_->preference_fonts[index];
        }
    }
    return nullptr;
}

const void* EditorImGuiHost::preference_icon_font() const noexcept {
    return impl_ != nullptr ? impl_->preference_icons : nullptr;
}

foundation::Result EditorImGuiHost::begin_frame(const EditorImGuiFrameDesc& desc) {
    return guard_editor_operation([&]() -> foundation::Result {
        if (!impl_->initialized) {
            return foundation::invalid_state("EditorImGuiHost is not initialized");
        }
        if (impl_->frame_active) {
            return foundation::invalid_state("EditorImGuiHost frame is already active");
        }

        ImGui::SetCurrentContext(impl_->context);
        detail::ImGuiPlatformBridge::set_window(impl_->platform_bridge, desc.window);
        platform::WindowExtent display_extent = desc.display_extent;
        float framebuffer_scale_x = desc.framebuffer_scale_x;
        float framebuffer_scale_y = desc.framebuffer_scale_y;
        if (impl_->platform_bridge.services != nullptr) {
            const foundation::ResultValue<platform::WindowMetrics> metrics_result =
                impl_->platform_bridge.services->query_window_metrics(desc.window);
            if (!metrics_result) {
                return metrics_result.result;
            }
            display_extent = metrics_result.value.display_extent;
            framebuffer_scale_x = metrics_result.value.framebuffer_scale_x;
            framebuffer_scale_y = metrics_result.value.framebuffer_scale_y;
        }
        if (!display_extent.is_valid() || !std::isfinite(desc.delta_seconds) || desc.delta_seconds <= 0.0f ||
            !std::isfinite(framebuffer_scale_x) || !std::isfinite(framebuffer_scale_y) || framebuffer_scale_x <= 0.0f ||
            framebuffer_scale_y <= 0.0f) {
            return foundation::invalid_argument("Editor ImGui frame description is invalid");
        }
        ImGuiIO& io = ImGui::GetIO();
        if (io.Fonts == nullptr || !io.Fonts->IsBuilt() || io.FontDefault == nullptr) {
            return foundation::invalid_state("Editor font atlas is not ready for rendering");
        }
        io.DisplaySize = ImVec2(static_cast<float>(display_extent.width), static_cast<float>(display_extent.height));
        io.DisplayFramebufferScale = ImVec2(framebuffer_scale_x, framebuffer_scale_y);
        io.DeltaTime = desc.delta_seconds;

        {
            std::scoped_lock lock(impl_->event_mutex);
            impl_->frame_events.clear();
            impl_->frame_events.swap(impl_->pending_events);
        }
        for (const platform::WindowEvent& event : impl_->frame_events) {
            if (!impl_->platform_bridge.viewports_enabled && event.window != desc.window) {
                continue;
            }
            detail::ImGuiPlatformBridge::submit_event(io, event, impl_->platform_bridge);
        }
        ImGui::NewFrame();
        impl_->retired_font_atlases.clear();
        impl_->retired_font_catalogs.clear();
        ++impl_->frame_number;
        impl_->frame_active = true;
        return foundation::Result::success();
    });
}

foundation::Result EditorImGuiHost::build_panels() {
    if (!impl_->initialized || !impl_->frame_active) {
        return foundation::invalid_state("Editor ImGui panels require an active frame");
    }
    if (impl_->building_panels) {
        return foundation::invalid_state("Editor panel callbacks cannot recursively build panels");
    }

    ImGui::SetCurrentContext(impl_->context);
    struct TraversalScope {
        bool& active;
        explicit TraversalScope(bool& value) : active(value) { active = true; }
        ~TraversalScope() { active = false; }
    } traversal(impl_->building_panels);
    for (const Impl::PanelSlot& slot : impl_->panels) {
        if (slot.panel != nullptr && slot.visible) {
            ImGuiErrorRecoveryState checkpoint{};
            ImGui::ErrorRecoveryStoreState(&checkpoint);
            const auto result = guard_editor_operation([&] {
                slot.panel->build_ui();
                return foundation::Result::success();
            });
            const auto recovery = guard_editor_operation([&] {
                ImGui::SetCurrentContext(impl_->context);
                ImGui::ErrorRecoveryTryToRecoverState(&checkpoint);
                return foundation::Result::success();
            });
            if (!result)
                return result;
            if (!recovery)
                return recovery;
        }
    }
    return foundation::Result::success();
}

foundation::Result EditorImGuiHost::end_frame() {
    if (!impl_->initialized || !impl_->frame_active) {
        return foundation::invalid_state("Editor ImGui frame is not active");
    }
    if (impl_->building_panels) {
        return foundation::invalid_state("Editor panel callbacks cannot end the host frame");
    }

    return guard_editor_operation([&] {
        ImGui::SetCurrentContext(impl_->context);
        ImGui::Render();
        if (impl_->platform_bridge.viewports_enabled) {
            ImGui::UpdatePlatformWindows();
        }
        detail::ImGuiPlatformBridge::update_cursor(ImGui::GetIO(), impl_->platform_bridge);
        impl_->frame_active = false;
        return foundation::Result::success();
    });
}

foundation::Result EditorImGuiHost::abort_frame() noexcept {
    if (impl_ == nullptr || !impl_->initialized)
        return foundation::Result::success();
    if (impl_->building_panels)
        return foundation::Result::failure(foundation::ResultCode::kInvalidState);
    return guard_editor_operation([&] {
        ImGui::SetCurrentContext(impl_->context);
        if (impl_->context->WithinFrameScope) {
            ImGui::ErrorRecoveryTryToRecoverState(&impl_->context->StackSizesInNewFrame);
            ImGui::EndFrame();
            // Multi-viewport backends require this once per frame even when
            // UI building was interrupted. Without it, the next NewFrame()
            // asserts that platform-window bookkeeping is incomplete.
            if (impl_->platform_bridge.viewports_enabled) {
                ImGui::UpdatePlatformWindows();
            }
        }
        for (ImGuiViewportP* viewport : impl_->context->Viewports) {
            viewport->DrawData = nullptr;
            viewport->DrawDataP.Clear();
        }
        impl_->frame_active = false;
        return foundation::Result::success();
    });
}

foundation::Result EditorImGuiHost::capture_draw_packet(ImGuiDrawPacket& packet) const {
    if (!impl_->initialized || impl_->frame_active) {
        return foundation::invalid_state("ImGui draw data requires a finalized frame");
    }

    ImGui::SetCurrentContext(impl_->context);
    const ImGuiViewport* main_viewport = ImGui::GetMainViewport();
    const ImDrawData* draw_data = main_viewport != nullptr ? main_viewport->DrawData : ImGui::GetDrawData();
    return capture_draw_data(draw_data, impl_->platform_bridge.window, impl_->frame_number, packet);
}

foundation::Result EditorImGuiHost::capture_draw_packets(std::vector<ImGuiDrawPacket>& packets) const {
    if (!impl_->initialized || impl_->frame_active) {
        return foundation::invalid_state("ImGui draw data requires a finalized frame");
    }

    try {
        ImGui::SetCurrentContext(impl_->context);
        std::vector<ImGuiDrawPacket> captured_packets;
        const ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
        captured_packets.reserve(static_cast<std::size_t>(platform_io.Viewports.Size));
        for (int index = 0; index < platform_io.Viewports.Size; ++index) {
            const ImGuiViewport* viewport = platform_io.Viewports[index];
            const platform::WindowId window = viewport_window_id(impl_->platform_bridge, viewport);
            if (viewport == nullptr || !window.is_valid() || viewport->DrawData == nullptr ||
                !viewport->DrawData->Valid) {
                continue;
            }
            ImGuiDrawPacket packet;
            if (const foundation::Result result =
                    capture_draw_data(viewport->DrawData, window, impl_->frame_number, packet);
                !result) {
                return result;
            }
            captured_packets.push_back(std::move(packet));
        }
        packets = std::move(captured_packets);
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("ImGui viewport draw packet allocation failed");
    } catch (const std::exception& exception) {
        return foundation::unknown_exception(exception);
    } catch (...) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                           "ImGui viewport draw packet capture threw an unknown exception");
    }
}

foundation::ResultValue<EditorPanelId> EditorImGuiHost::register_panel(std::unique_ptr<EditorPanel> panel) {
    if (!impl_->initialized) {
        return {foundation::invalid_state("EditorImGuiHost is not initialized"), {}};
    }
    if (impl_->building_panels) {
        return {foundation::invalid_state("Cannot register a panel from a panel callback"), {}};
    }
    if (panel == nullptr) {
        return {foundation::invalid_argument("Cannot register a null editor panel"), {}};
    }

    for (uint32_t index = 0; index < impl_->panels.size(); ++index) {
        Impl::PanelSlot& slot = impl_->panels[index];
        if (slot.panel == nullptr) {
            slot.panel = std::move(panel);
            slot.visible = true;
            return {foundation::Result::success(), EditorPanelId{index, slot.generation}};
        }
    }

    if (impl_->panels.size() == UINT32_MAX) {
        return {foundation::out_of_memory("Editor panel registry exhausted"), {}};
    }
    impl_->panels.push_back(Impl::PanelSlot{1, std::move(panel)});
    return {foundation::Result::success(),
            EditorPanelId{static_cast<uint32_t>(impl_->panels.size() - 1), impl_->panels.back().generation}};
}

foundation::Result EditorImGuiHost::unregister_panel(EditorPanelId panel) {
    if (!impl_->initialized) {
        return foundation::invalid_state("EditorImGuiHost is not initialized");
    }
    if (impl_->building_panels) {
        return foundation::invalid_state("Cannot unregister a panel from a panel callback");
    }
    if (panel.index >= impl_->panels.size()) {
        return foundation::not_found("Editor panel identity is not registered");
    }

    Impl::PanelSlot& slot = impl_->panels[panel.index];
    if (slot.panel == nullptr || slot.generation != panel.generation) {
        return foundation::not_found("Editor panel identity is stale");
    }
    slot.panel.reset();
    slot.visible = true;
    ++slot.generation;
    if (slot.generation == 0) {
        slot.generation = 1;
    }
    return foundation::Result::success();
}

foundation::Result EditorImGuiHost::set_panel_visible(EditorPanelId panel, bool visible) {
    if (!impl_->initialized) {
        return foundation::invalid_state("EditorImGuiHost is not initialized");
    }
    if (panel.index >= impl_->panels.size()) {
        return foundation::not_found("Editor panel identity is not registered");
    }

    Impl::PanelSlot& slot = impl_->panels[panel.index];
    if (slot.panel == nullptr || slot.generation != panel.generation) {
        return foundation::not_found("Editor panel identity is stale");
    }
    slot.visible = visible;
    return foundation::Result::success();
}

bool EditorImGuiHost::is_initialized() const noexcept {
    return impl_ != nullptr && impl_->initialized;
}

bool EditorImGuiHost::is_frame_active() const noexcept {
    return impl_ != nullptr && impl_->frame_active;
}

std::size_t EditorImGuiHost::panel_count() const noexcept {
    if (impl_ == nullptr) {
        return 0;
    }
    std::size_t count = 0;
    for (const Impl::PanelSlot& slot : impl_->panels) {
        count += slot.panel != nullptr ? 1u : 0u;
    }
    return count;
}

const void* EditorImGuiHost::draw_data() const noexcept {
    if (impl_ == nullptr || !impl_->initialized || impl_->frame_active) {
        return nullptr;
    }
    ImGui::SetCurrentContext(impl_->context);
    return ImGui::GetDrawData();
}
}  // namespace aeg::editor
