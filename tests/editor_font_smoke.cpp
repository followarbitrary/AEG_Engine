/**
 * @file editor_font_smoke.cpp
 * @brief Verifies editor font selection using installed system fonts.
 */
#include <imgui.h>

#include <cassert>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "application_platform.hpp"
#include "editor_font.hpp"
#include "editor_imgui.hpp"
#include "font/editor_system_font_catalog.hpp"

namespace {
struct AtlasAllocationWatch {
    ImGuiMemAllocFunc allocate{};
    ImGuiMemFreeFunc release{};
    void* user_data{};
    void* texture_data{};
    bool texture_released{false};

    static void* alloc(std::size_t size, void* context) {
        auto& watch = *static_cast<AtlasAllocationWatch*>(context);
        return watch.allocate(size, watch.user_data);
    }

    static void free(void* allocation, void* context) {
        auto& watch = *static_cast<AtlasAllocationWatch*>(context);
        if (allocation == watch.texture_data)
            watch.texture_released = true;
        watch.release(allocation, watch.user_data);
    }
};

[[nodiscard]] std::vector<std::byte> read_font_file(const aeg::application::SystemFontRecord& record) {
    const std::filesystem::path path = std::filesystem::u8path(record.file_path);
    std::ifstream stream(path, std::ios::binary);
    assert(stream.good());
    stream.seekg(0, std::ios::end);
    const std::streampos end = stream.tellg();
    assert(end > 0);
    std::vector<std::byte> data(static_cast<std::size_t>(end));
    stream.seekg(0, std::ios::beg);
    stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    assert(stream.good() || stream.eof());
    return data;
}
}  // namespace

int main() {
    using aeg::application::SystemFontRecord;
    using aeg::editor::EditorFontCatalog;
    using aeg::editor::EditorFontDescriptor;
    using aeg::editor::EditorFontKind;
    using aeg::editor::EditorImGuiDesc;
    using aeg::editor::EditorImGuiFrameDesc;
    using aeg::editor::EditorImGuiHost;
    using aeg::editor::ImGuiFontAtlasData;
    using aeg::platform::WindowExtent;
    using aeg::platform::WindowId;

    const auto records_result = aeg::application::enumerate_system_fonts();
    assert(records_result);
    assert(records_result.value.size() >= 2);

    EditorFontCatalog catalog;
    std::string first_key;
    std::string second_key;
    uint32_t registered_count = 0;
    for (const SystemFontRecord& record : records_result.value) {
        const std::string key = "system_test_" + std::to_string(registered_count);
        const EditorFontDescriptor descriptor{key,
                                              record.family_name,
                                              record.family_name.find("Mono") != std::string::npos
                                                  ? EditorFontKind::kMonospace
                                                  : EditorFontKind::kInterface,
                                              record.supports_chinese,
                                              record.supports_ligatures,
                                              registered_count == 0,
                                              true};
        if (!catalog.register_font(descriptor, read_font_file(record), record.face_index)) {
            continue;
        }
        if (registered_count == 0) {
            first_key = key;
        } else if (registered_count == 1) {
            second_key = key;
        }
        ++registered_count;
        if (registered_count == 2) {
            break;
        }
    }
    assert(registered_count == 2);

    const SystemFontRecord* collection_primary_record = nullptr;
    const SystemFontRecord* collection_fallback_record = nullptr;
    for (const SystemFontRecord& record : records_result.value) {
        if (collection_primary_record == nullptr && !record.supports_chinese) {
            collection_primary_record = &record;
        }
        if (collection_fallback_record == nullptr && record.supports_chinese && record.face_index != 0) {
            collection_fallback_record = &record;
        }
    }
    if (collection_primary_record != nullptr && collection_fallback_record != nullptr) {
        EditorFontCatalog collection_catalog;
        const EditorFontDescriptor collection_primary_descriptor{"collection_primary",
                                                                 collection_primary_record->family_name,
                                                                 EditorFontKind::kInterface,
                                                                 collection_primary_record->supports_chinese,
                                                                 collection_primary_record->supports_ligatures,
                                                                 true,
                                                                 true};
        const EditorFontDescriptor collection_fallback_descriptor{"collection_fallback",
                                                                  collection_fallback_record->family_name,
                                                                  EditorFontKind::kInterface,
                                                                  collection_fallback_record->supports_chinese,
                                                                  collection_fallback_record->supports_ligatures,
                                                                  false,
                                                                  true};
        assert(collection_catalog.register_font(collection_primary_descriptor,
                                                read_font_file(*collection_primary_record),
                                                collection_primary_record->face_index));
        assert(collection_catalog.register_font(collection_fallback_descriptor,
                                                read_font_file(*collection_fallback_record),
                                                collection_fallback_record->face_index));

        EditorImGuiHost collection_host;
        EditorImGuiDesc collection_desc{};
        collection_desc.font_catalog = &collection_catalog;
        collection_desc.default_font_key = "collection_primary";
        collection_desc.font_size_pixels = 18.0f;
        collection_desc.enable_preference_fonts = true;
        assert(collection_host.initialize(collection_desc));
        assert(ImGui::GetIO().Fonts->Sources.Size >= 2);
        assert(ImGui::GetIO().Fonts->Sources[1].FontNo == collection_fallback_record->face_index);
        // The exact preview text previously showed replacement glyphs for
        // characters outside ImGui's simplified-common subset.
        assert(collection_host.set_preview_font("collection_primary", 16));
        auto* sample = const_cast<ImFont*>(static_cast<const ImFont*>(collection_host.preview_font()));
        assert(sample->LegacySize == 16.0f);  // Preview is actual size, never magnified.
        for (ImWchar codepoint : {ImWchar(0x5668), ImWchar(0x6670), ImWchar(0x8BFB)}) {
            assert(sample->GetFontBaked(sample->LegacySize)->FindGlyphNoFallback(codepoint) != nullptr);
        }
        auto* chrome = static_cast<const ImFont*>(collection_host.preference_font(20));
        assert(chrome != nullptr);  // 20 identifies the body role, even when its raster size is 18.
        assert(chrome->LegacySize == 18);
        for (const float role : {16.0f, 20.0f, 24.0f, 32.0f})
            assert(collection_host.preference_font(role) != nullptr);
        const float chrome_size = chrome->LegacySize;
        assert(collection_host.set_preview_font("collection_primary", 48));
        assert(collection_host.active_font_size_pixels() == 18);
        // Editing the draft changes only Preview. Settings chrome remains on
        // the applied font until Apply/Save updates the active host font.
        assert(static_cast<const ImFont*>(collection_host.preference_font(20))->LegacySize == chrome_size);
        assert(static_cast<const ImFont*>(collection_host.preview_font())->LegacySize == 48);
        assert(collection_host.set_font("collection_primary", 24));
        assert(static_cast<const ImFont*>(collection_host.preference_font(20))->LegacySize == 24);
        assert(static_cast<const ImFont*>(collection_host.preview_font())->LegacySize == 48);
        assert(collection_host.shutdown());
    }

    const EditorFontDescriptor invalid_descriptor{
        "invalid_font_data", "Invalid Test Font", EditorFontKind::kInterface, false, false, false, false};
    std::vector<std::byte> invalid_font_data = read_font_file(records_result.value.front());
    assert(invalid_font_data.size() >= 12);
    const auto read_u16_be = [&invalid_font_data](std::size_t offset) {
        return static_cast<uint16_t>(static_cast<uint8_t>(invalid_font_data[offset])) << 8u |
               static_cast<uint16_t>(static_cast<uint8_t>(invalid_font_data[offset + 1]));
    };
    const std::size_t table_count = read_u16_be(4);
    assert(invalid_font_data.size() >= 12u + table_count * 16u);
    for (std::size_t table_index = 0; table_index < table_count; ++table_index) {
        const std::size_t tag_offset = 12u + table_index * 16u;
        for (std::size_t byte_index = 0; byte_index < 4; ++byte_index) {
            invalid_font_data[tag_offset + byte_index] = static_cast<std::byte>(0x78u);
        }
    }
    assert(catalog.register_font(invalid_descriptor, invalid_font_data));
    auto short_descriptor = invalid_descriptor;
    short_descriptor.key = "short_font_data";
    assert(catalog.register_font(short_descriptor, std::vector<std::byte>(16)));
    auto invalid_face_descriptor = invalid_descriptor;
    invalid_face_descriptor.key = "invalid_face_index";
    const auto* valid_asset = catalog.find(first_key);
    assert(valid_asset != nullptr);
    const auto valid_font_data = valid_asset->font_data;
    const auto valid_face_index = valid_asset->face_index;
    const auto inspected = aeg::editor::EditorFontInspector{}.inspect(valid_font_data, valid_face_index);
    assert(inspected);
    assert(!inspected.value.family_name.empty());
    assert(!inspected.value.style_name.empty());
    assert(!aeg::editor::EditorFontInspector{}.inspect(std::span<const std::byte>{}));
    // The vendored Source Code Pro OTF exercises the CFF outline path through
    // the same backend-neutral inspector and ImGui FreeType loader.
    const auto otf_path = std::filesystem::path(AEG_ENGINE_SOURCE_ROOT) / "ThirdParty" / "Fonts" / "SourceCodePro" /
                          "SourceCodePro-Regular.otf";
    std::ifstream otf_stream(otf_path, std::ios::binary);
    assert(otf_stream.good());
    otf_stream.seekg(0, std::ios::end);
    const auto otf_end = otf_stream.tellg();
    assert(otf_end > 0);
    std::vector<std::byte> otf_data(static_cast<std::size_t>(otf_end));
    otf_stream.seekg(0, std::ios::beg);
    otf_stream.read(reinterpret_cast<char*>(otf_data.data()), static_cast<std::streamsize>(otf_data.size()));
    assert(otf_stream.good() || otf_stream.eof());
    auto otf_descriptor = aeg::editor::EditorFontInspector{}.inspect(otf_data);
    assert(otf_descriptor);
    otf_descriptor.value.key = "cff_otf";
    otf_descriptor.value.supports_chinese = false;
    EditorFontCatalog otf_catalog;
    assert(otf_catalog.register_font(otf_descriptor.value, otf_data));
    EditorImGuiHost otf_host;
    EditorImGuiDesc otf_desc{};
    otf_desc.font_catalog = &otf_catalog;
    otf_desc.default_font_key = "cff_otf";
    otf_desc.font_size_pixels = 18.0f;
    assert(otf_host.initialize(otf_desc));
    ImGuiFontAtlasData otf_atlas;
    assert(otf_host.capture_font_atlas(otf_atlas));
    assert(otf_atlas.is_valid());
    assert(otf_host.shutdown());
    assert(catalog.register_font(invalid_face_descriptor, valid_font_data, std::numeric_limits<uint32_t>::max()));

    EditorImGuiHost host;
    EditorImGuiDesc imgui_desc{};
    imgui_desc.font_catalog = &catalog;
    imgui_desc.default_font_key = first_key;
    imgui_desc.font_size_pixels = 18.0f;
    assert(host.initialize(imgui_desc));
    // Rejected external bytes must return Results before reaching ImGui's asserts.
    ImGui::GetIO().ConfigErrorRecoveryEnableAssert = true;
    assert(host.active_font_key() == first_key);
    assert(host.active_font_size_pixels() == 18.0f);

    assert(!host.set_font("missing_font", 18.0f));
    assert(!host.set_font(second_key, 0.0f));
    assert(host.set_preview_font(second_key, 20.0f));
    assert(host.active_font_key() == first_key);
    assert(host.active_font_size_pixels() == 18.0f);
    assert(host.preview_font_key() == second_key);
    assert(host.preview_font_size_pixels() == 20.0f);
    const ImFont* preview_font = static_cast<const ImFont*>(host.preview_font());
    assert(preview_font != nullptr);
    assert(ImGui::GetIO().FontDefault != nullptr);
    assert(ImGui::GetIO().FontDefault->LegacySize == 18.0f);
    assert(preview_font->LegacySize == 20.0f);

    // A rejected preview must leave the current atlas usable on the very next
    // frame, without reloading a good font to repair the failed operation.
    const aeg::editor::ImGuiTextureHandle texture{1, 1};
    assert(host.set_font_texture(texture));
    ImFontAtlas* previous_atlas = ImGui::GetIO().Fonts;
    ImFont* previous_default = ImGui::GetIO().FontDefault;
    const void* previous_display = host.display_font();
    const ImTextureID previous_texture = previous_atlas->TexRef.GetTexID();
    ImGuiFontAtlasData previous_pixels;
    assert(host.capture_font_atlas(previous_pixels));
    assert(!host.set_preview_font("invalid_font_data", 24.0f));
    assert(ImGui::GetIO().Fonts == previous_atlas);
    assert(ImGui::GetIO().Fonts->IsBuilt());
    assert(ImGui::GetIO().FontDefault == previous_default);
    assert(host.preview_font() == preview_font);
    assert(host.display_font() == previous_display);
    assert(host.preview_font_key() == second_key);
    assert(host.preview_font_size_pixels() == 20.0f);
    assert(ImGui::GetIO().Fonts->TexRef.GetTexID() == previous_texture);
    ImGuiFontAtlasData retained_pixels;
    assert(host.capture_font_atlas(retained_pixels));
    assert(retained_pixels.rgba8_pixels == previous_pixels.rgba8_pixels);

    EditorImGuiFrameDesc recovery_frame{};
    recovery_frame.window = WindowId{0, 1};
    recovery_frame.display_extent = WindowExtent{1280, 720};
    assert(host.begin_frame(recovery_frame));
    ImGui::Begin("Failed preview recovery");
    ImGui::TextUnformatted("The existing editor font is still usable.");
    ImGui::PushFont(const_cast<ImFont*>(preview_font));
    ImGui::TextUnformatted("The previous preview is still usable.");
    ImGui::PopFont();
    ImGui::End();
    assert(host.end_frame());
    aeg::editor::ImGuiDrawPacket recovery_packet;
    assert(host.capture_draw_packet(recovery_packet));

    // Finalized draw packets remain valid until the next begin_frame, even
    // when a successful font change replaces their atlas in between.
    AtlasAllocationWatch allocation_watch;
    ImGui::GetAllocatorFunctions(&allocation_watch.allocate, &allocation_watch.release, &allocation_watch.user_data);
    allocation_watch.texture_data = ImGui::GetIO().Fonts->TexData;
    ImGui::SetAllocatorFunctions(AtlasAllocationWatch::alloc, AtlasAllocationWatch::free, &allocation_watch);
    assert(host.set_font(second_key, 20.0f));
    assert(!allocation_watch.texture_released);
    aeg::editor::ImGuiDrawPacket previous_frame_packet;
    assert(host.capture_draw_packet(previous_frame_packet));
    assert(previous_frame_packet.commands.size() == recovery_packet.commands.size());
    assert(host.active_font_key() == second_key);
    assert(host.preview_font_key().empty());
    assert(!host.set_font("invalid_font_data", 20.0f));
    assert(host.active_font_key() == second_key);
    assert(ImGui::GetIO().Fonts->IsBuilt());
    assert(host.begin_frame(recovery_frame));
    assert(allocation_watch.texture_released);
    ImGui::SetAllocatorFunctions(allocation_watch.allocate, allocation_watch.release, allocation_watch.user_data);
    assert(host.end_frame());
    ImGuiFontAtlasData atlas;
    assert(host.capture_font_atlas(atlas));
    assert(atlas.is_valid());

    EditorImGuiFrameDesc frame_desc{};
    frame_desc.window = WindowId{0, 1};
    frame_desc.display_extent = WindowExtent{1280, 720};
    assert(host.begin_frame(frame_desc));
    assert(!host.set_font(first_key, 16.0f));
    assert(host.end_frame());
    assert(host.set_font(second_key, 28.0f));
    assert(host.begin_frame(frame_desc));
    ImGui::Begin("Font size regression");
    assert(ImGui::GetFontSize() == 28.0f);
    ImGui::Selectable("Selected asset", true);
    assert(ImGui::GetItemRectSize().y >= ImGui::CalcTextSize("Selected asset").y);
    ImGui::End();
    assert(host.end_frame());
    assert(host.shutdown());

    // Preferences keeps additional heading/icon/preview font pointers. Repeated
    // failures must preserve those as well, including after a rendered frame.
    imgui_desc.enable_preference_fonts = true;
    assert(host.initialize(imgui_desc));
    ImGui::GetIO().ConfigErrorRecoveryEnableAssert = true;
    assert(host.set_preview_font(second_key, 20.0f));
    assert(host.set_font_texture(texture));
    const void* preference_heading = host.preference_font(24);
    const void* preference_preview = host.preview_font();
    EditorImGuiHost other_host;
    assert(other_host.initialize());
    assert(other_host.set_font_texture(texture));
    for (int attempt = 0; attempt < 3; ++attempt) {
        for (const char* key : {"invalid_font_data", "short_font_data", "invalid_face_index"}) {
            assert(!host.set_preview_font(key, 22.0f));
            assert(!host.set_font(key, 22.0f));
        }
        assert(host.preference_font(24) == preference_heading);
        assert(host.preview_font() == preference_preview);
        assert(host.active_font_key() == first_key);
        assert(host.preview_font_key() == second_key);
        assert(host.begin_frame(recovery_frame));
        ImGui::Begin("Preferences failure recovery");
        ImGui::PushFont(static_cast<ImFont*>(const_cast<void*>(preference_heading)));
        ImGui::TextUnformatted("Settings heading");
        ImGui::PopFont();
        ImGui::PushFont(static_cast<ImFont*>(const_cast<void*>(preference_preview)));
        ImGui::TextUnformatted("Preview remains available");
        ImGui::PopFont();
        ImGui::End();
        assert(host.end_frame());
        assert(host.capture_draw_packet(recovery_packet));
    }
    assert(host.set_preview_font(first_key, 24.0f));
    assert(host.set_font(second_key, 24.0f));
    assert(host.begin_frame(recovery_frame));
    assert(host.end_frame());
    ImGui::GetIO().Fonts->Clear();
    assert(!host.begin_frame(recovery_frame));
    assert(host.shutdown());
    assert(other_host.begin_frame(recovery_frame));
    ImGui::Begin("Other host survives preferences rebuild and shutdown");
    ImGui::TextUnformatted("Independent context is still usable");
    ImGui::End();
    assert(other_host.end_frame());
    assert(other_host.capture_draw_packet(recovery_packet));
    assert(other_host.shutdown());

    // A malformed merged fallback is rejected after a valid primary was added
    // to the candidate atlas. The built-in active font remains renderable.
    EditorFontCatalog fallback_catalog;
    EditorFontDescriptor primary_descriptor{"primary", "Primary", EditorFontKind::kInterface};
    auto fallback_descriptor = invalid_descriptor;
    fallback_descriptor.key = "invalid_chinese_fallback";
    fallback_descriptor.supports_chinese = true;
    assert(fallback_catalog.register_font(primary_descriptor, valid_font_data, valid_face_index));
    assert(fallback_catalog.register_font(fallback_descriptor, invalid_font_data));
    EditorImGuiDesc fallback_desc{};
    fallback_desc.font_catalog = &fallback_catalog;
    assert(host.initialize(fallback_desc));
    ImGui::GetIO().ConfigErrorRecoveryEnableAssert = true;
    assert(host.set_font_texture(texture));
    assert(!host.set_font("primary", 18.0f));
    assert(host.active_font_key().empty());
    assert(host.begin_frame(recovery_frame));
    ImGui::Begin("Rejected fallback recovery");
    ImGui::TextUnformatted("Built-in font is preserved");
    ImGui::End();
    assert(host.end_frame());
    assert(host.capture_draw_packet(recovery_packet));
    assert(host.shutdown());

    // Exercise the actual catalog consumed by Settings, including all installed
    // collection faces; incompatible faces must never enter the selectable list.
    const auto system_catalog = aeg::application::EditorSystemFontCatalog{}.load();
    assert(system_catalog);
    assert(system_catalog.value.catalog.font_count() >= 2);
    for (const auto& asset : system_catalog.value.catalog.assets()) {
        assert(aeg::editor::validate_editor_font_data(asset.font_data, asset.face_index));
        assert(asset.descriptor.display_name != "Symbol");
        assert(asset.descriptor.display_name != "Wingdings");
        assert(asset.descriptor.display_name != "Webdings");
    }
    return 0;
}
