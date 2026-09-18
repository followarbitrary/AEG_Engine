#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <vector>

#include "editor_imgui.hpp"
#include "preferences/editor_preference_controller.hpp"

namespace {
std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    input.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}
}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "aeg_editor_preference_controller_smoke";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    const auto bundled_path = std::filesystem::path(AEG_ENGINE_SOURCE_ROOT) / "ThirdParty" / "Fonts" / "SourceCodePro" /
                              "SourceCodePro-Variable.ttf";
    const auto import_path = std::filesystem::path(AEG_ENGINE_SOURCE_ROOT) / "ThirdParty" / "Fonts" / "SourceCodePro" /
                             "SourceCodePro-Regular.otf";
    const auto bundled_bytes = read_file(bundled_path);
    assert(!bundled_bytes.empty());

    aeg::editor::EditorFontCatalog bundled;
    aeg::editor::EditorFontDescriptor bundled_descriptor{
        "bundled", "Bundled", aeg::editor::EditorFontKind::kInterface, false, false, true, true};
    assert(bundled.register_font(bundled_descriptor, bundled_bytes));

    aeg::application::EditorFontRepository repository;
    assert(repository.initialize(root / "Fonts"));
    aeg::application::EditorPreferenceStore store(root / "Preferences" / "editor.preferences");
    aeg::editor::EditorImGuiHost host;
    aeg::editor::EditorImGuiDesc host_desc{};
    host_desc.font_catalog = &bundled;
    host_desc.default_font_key = "bundled";
    host_desc.font_size_pixels = 18.0f;
    assert(host.initialize(host_desc));

    std::filesystem::path selected = import_path;
    uint32_t refresh_count = 0;
    aeg::application::EditorPreferenceController controller({
        &repository,
        &store,
        &host,
        [&refresh_count](std::string_view, float) {
            ++refresh_count;
            return aeg::foundation::Result::success();
        },
        [&selected]() -> aeg::foundation::ResultValue<std::optional<std::filesystem::path>> {
            return {aeg::foundation::Result::success(), selected};
        },
    });
    assert(controller.initialize(bundled, "bundled", 18.0f));
    assert(controller.request_import_font());
    auto* model = controller.model();
    assert(model != nullptr);
    assert(model->state().fonts.size() == 2);
    const auto& assets = controller.catalog().assets();
    assert(assets.size() == 2);
    const auto* imported = assets[1].descriptor.key.starts_with("user_") ? &assets[1] : &assets[0];
    assert(imported->descriptor.is_user_font);
    assert(model->select_font(imported->descriptor.key));
    assert(controller.preview(model->state()));
    assert(refresh_count != 0);
    const auto first_apply = controller.apply(model->state());
    if (!first_apply)
        std::fprintf(stderr, "first apply failed: %s\n", first_apply.message.c_str());
    assert(first_apply);

    assert(model->select_font("bundled"));
    assert(controller.request_remove_font(imported->descriptor.key));
    assert(controller.cancel(model->state()));
    assert(controller.catalog().find(imported->descriptor.key) != nullptr);

    assert(model->select_font("bundled"));
    assert(controller.request_remove_font(imported->descriptor.key));
    assert(controller.apply(model->state()));
    assert(controller.catalog().find(imported->descriptor.key) == nullptr);
    assert(repository.scan().value.find(imported->descriptor.key) == nullptr);
    assert(host.shutdown());
    std::filesystem::remove_all(root, error);
    return 0;
}
