#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include "preferences/editor_preference_store.hpp"

namespace {
std::vector<char> read_all(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    input.seekg(0, std::ios::beg);
    std::vector<char> bytes(static_cast<std::size_t>(size));
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}
}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "aeg_editor_preference_store_smoke";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    const auto path = root / "settings" / "editor.preferences";

    aeg::application::EditorPreferenceStore store(path);
    aeg::application::EditorPreferenceFileData data{};
    data.active_font_key = "user_0123456789abcdef";
    data.font_size_pixels = 22.0f;
    data.user_fonts = {
        {"user_fedcba9876543210", "user_fedcba9876543210.otf"},
        {"user_0123456789abcdef", "user_0123456789abcdef.ttf"},
    };
    const auto write_result = store.write_atomic(data);
    if (!write_result) {
        std::fprintf(stderr, "write failed: %s\n", write_result.message.c_str());
    }
    assert(write_result);
    const auto loaded = store.load();
    assert(loaded);
    assert(loaded.value.active_font_key == data.active_font_key);
    assert(loaded.value.font_size_pixels == data.font_size_pixels);
    assert(loaded.value.user_fonts.size() == 2);
    assert(loaded.value.user_fonts[0].key == "user_0123456789abcdef");

    auto bytes = read_all(path);
    assert(bytes.size() > 8);
    bytes[0] = 'X';
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    assert(!store.load());

    assert(store.write_atomic(data));
    bytes = read_all(path);
    bytes.pop_back();
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    assert(!store.load());

    data.font_size_pixels = 100.0f;
    assert(!store.write_atomic(data));
    std::filesystem::remove_all(root, error);
    return 0;
}
