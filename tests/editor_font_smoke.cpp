/**
 * @file editor_font_smoke.cpp
 * @brief Verifies editor font selection using installed system fonts.
 */
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <string>
#include <vector>

#include "application_platform.hpp"
#include "editor_font.hpp"
#include "editor_imgui.hpp"

namespace {
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
        assert(sample->LegacySize == 16.0f); // Preview is actual size, never magnified.
        for (ImWchar codepoint : {ImWchar(0x5668), ImWchar(0x6670), ImWchar(0x8BFB)}) {
            assert(sample->GetFontBaked(sample->LegacySize)->FindGlyphNoFallback(codepoint) != nullptr);
        }
        auto* chrome = static_cast<const ImFont*>(collection_host.preference_font(20));
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

    const EditorFontDescriptor invalid_descriptor{"invalid_font_data",
                                                  "Invalid Test Font",
                                                  EditorFontKind::kInterface,
                                                  false,
                                                  false,
                                                  false,
                                                  false};
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

    EditorImGuiHost host;
    EditorImGuiDesc imgui_desc{};
    imgui_desc.font_catalog = &catalog;
    imgui_desc.default_font_key = first_key;
    imgui_desc.font_size_pixels = 18.0f;
    assert(host.initialize(imgui_desc));
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
    assert(host.set_font(second_key, 20.0f));
    assert(host.active_font_key() == second_key);
    assert(host.preview_font_key().empty());
    assert(!host.set_font("invalid_font_data", 20.0f));
    assert(host.active_font_key() == second_key);
    assert(host.set_font(second_key, 20.0f));
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
    return 0;
}
