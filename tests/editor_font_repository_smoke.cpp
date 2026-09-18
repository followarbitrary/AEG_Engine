#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "font/editor_font_repository.hpp"

namespace {
std::filesystem::path find_system_font() {
    const char* windir = std::getenv("WINDIR");
    const auto root = std::filesystem::path(windir ? windir : "C:/Windows") / "Fonts";
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        if (error)
            break;
        if (!entry.is_regular_file(error))
            continue;
        const auto extension = entry.path().extension().string();
        if (extension == ".ttf" || extension == ".TTF" || extension == ".otf" || extension == ".OTF")
            return entry.path();
    }
    return {};
}
}  // namespace

int main() {
    const auto source = find_system_font();
    if (source.empty())
        return 0;  // Fontless CI images cannot exercise import.

    const auto root = std::filesystem::temp_directory_path() / "aeg_editor_font_repository_smoke";
    std::error_code error;
    std::filesystem::remove_all(root, error);

    aeg::application::EditorFontRepository repository;
    assert(repository.initialize(root));
    const auto staged = repository.stage_import(source);
    assert(staged);
    assert(staged.value.descriptor.is_user_font);
    assert(staged.value.descriptor.key.starts_with("user_"));
    assert(repository.scan().value.font_count() == 0);
    repository.rollback();
    assert(repository.scan().value.font_count() == 0);

    const auto staged_again = repository.stage_import(source);
    assert(staged_again);
    assert(!repository.stage_import(source));
    assert(repository.commit());
    const auto committed = repository.scan();
    assert(committed);
    assert(committed.value.font_count() == 1);
    const auto* asset = committed.value.find(staged_again.value.descriptor.key);
    assert(asset != nullptr);
    assert(asset->descriptor.source == aeg::editor::EditorFontSource::kUser);

    assert(repository.stage_remove(staged_again.value.descriptor.key));
    repository.rollback();
    assert(repository.scan().value.font_count() == 1);
    assert(repository.stage_remove(staged_again.value.descriptor.key));
    assert(repository.commit());
    assert(repository.scan().value.font_count() == 0);

    const auto invalid = root / "invalid.txt";
    {
        std::ofstream output(invalid);
        output << "not a font";
    }
    assert(!repository.stage_import(invalid));
    std::filesystem::remove_all(root, error);
    return 0;
}
