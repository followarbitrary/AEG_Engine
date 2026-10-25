#include <cassert>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

#include "asset_package.hpp"
#include "cooked_asset.hpp"
#include "font_payload.hpp"
#include "runtime_asset_cache.hpp"

namespace {
using namespace aeg;

foundation::AssetId id(const char* text) {
    const auto parsed = foundation::AssetId::parse(text);
    assert(parsed);
    return parsed.value;
}
}  // namespace

int main() {
    const auto font_id = id("00112233-4455-6677-8899-aabbccddeeff");
    const std::vector<std::byte> source_bytes{std::byte{0x00}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00},
                                              std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
                                              std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    const auto payload = asset::encode_font_payload({0, source_bytes});
    assert(payload);
    const auto cooked = asset::encode_cooked_asset({font_id, foundation::AssetKind::kFont, 1, {}, payload.value});
    assert(cooked);
    const auto package_path = std::filesystem::temp_directory_path() / "aeg_runtime_font_cache_smoke.aegbin";
    assert(tooling::AssetPackageWriter{}.write(package_path, {{font_id, foundation::AssetKind::kFont, 1, cooked.value}}));
    const auto package = asset::AssetPackageReader::open(package_path);
    assert(package);
    application::runtime::RuntimeAssetCache cache{std::make_shared<asset::AssetPackageReader>(package.value)};
    const auto first = cache.load_font(font_id);
    assert(first && first.value.payload.face_index == 0 && first.value.payload.font_bytes == source_bytes);
    const auto second = cache.load_font(font_id);
    assert(second && second.value.handle == first.value.handle);
    const auto truncated =
        asset::decode_font_payload(std::span<const std::byte>{payload.value.data(), payload.value.size() - 1});
    assert(!truncated && truncated.result.code == foundation::ResultCode::kInvalidArgument);
    std::filesystem::remove(package_path);
    return 0;
}
