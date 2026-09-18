#include "game_build_manifest.hpp"

#include <cstring>

namespace aeg::asset {
namespace {
constexpr uint32_t kMagic = 0x314d4247u;  // GBM1
constexpr uint32_t kMaxString = 16u * 1024u * 1024u;
constexpr uint32_t kMaxPipelines = 4096;

struct Reader final {
    std::span<const std::byte> bytes{};
    std::size_t offset{};
    template <typename T>
    bool value(T& output) {
        if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
            return false;
        std::memcpy(&output, bytes.data() + offset, sizeof(T));
        offset += sizeof(T);
        return true;
    }
    bool string(std::string& output) {
        uint32_t size = 0;
        if (!value(size) || size > kMaxString || size > bytes.size() - offset)
            return false;
        output.assign(reinterpret_cast<const char*>(bytes.data() + offset), size);
        offset += size;
        return true;
    }
    bool uuid(AssetId& output) {
        for (uint8_t& byte : output.value.bytes)
            if (!value(byte))
                return false;
        return true;
    }
};
struct Writer final {
    std::vector<std::byte> bytes{};
    template <typename T>
    void value(T value) {
        const auto* ptr = reinterpret_cast<const std::byte*>(&value);
        bytes.insert(bytes.end(), ptr, ptr + sizeof(T));
    }
    void string(const std::string& value) {
        value.length() <= UINT32_MAX ? this->value(static_cast<uint32_t>(value.length())) : this->value(uint32_t{0});
        bytes.insert(bytes.end(), reinterpret_cast<const std::byte*>(value.data()),
                     reinterpret_cast<const std::byte*>(value.data()) + value.size());
    }
    void uuid(const AssetId& id) {
        for (const uint8_t byte : id.value.bytes)
            value(byte);
    }
};
bool valid_string(const std::string& value) {
    return !value.empty() && value.size() <= kMaxString;
}
}  // namespace

foundation::ResultValue<std::vector<std::byte>> encode_game_build_manifest(const GameBuildManifest& manifest) {
    if (manifest.schema_version == 0 || !valid_string(manifest.project_uuid) || !valid_string(manifest.module_id) ||
        !valid_string(manifest.build_id) || !valid_string(manifest.module_contract_digest) ||
        !valid_string(manifest.package_digest) || !valid_string(manifest.target) ||
        !valid_string(manifest.configuration) || !valid_string(manifest.package_name) ||
        manifest.startup_scene == AssetId{} || manifest.startup_pipelines.size() > kMaxPipelines) {
        return {foundation::invalid_argument("Game build manifest identity or size is invalid"), {}};
    }
    Writer writer{};
    writer.value(kMagic);
    writer.value(manifest.schema_version);
    writer.string(manifest.project_uuid);
    writer.string(manifest.module_id);
    writer.string(manifest.build_id);
    writer.string(manifest.module_contract_digest);
    writer.string(manifest.package_digest);
    writer.uuid(manifest.startup_scene);
    writer.value(static_cast<uint32_t>(manifest.startup_pipelines.size()));
    for (const AssetId id : manifest.startup_pipelines) {
        if (id == AssetId{})
            return {foundation::invalid_argument("Game build manifest has an invalid pipeline"), {}};
        writer.uuid(id);
    }
    writer.string(manifest.target);
    writer.string(manifest.configuration);
    writer.string(manifest.package_name);
    return {foundation::Result::success(), std::move(writer.bytes)};
}

foundation::ResultValue<GameBuildManifest> decode_game_build_manifest(std::span<const std::byte> bytes) {
    Reader reader{bytes};
    GameBuildManifest result{};
    uint32_t magic = 0, count = 0;
    if (!reader.value(magic) || !reader.value(result.schema_version) || magic != kMagic || result.schema_version == 0 ||
        !reader.string(result.project_uuid) || !reader.string(result.module_id) || !reader.string(result.build_id) ||
        !reader.string(result.module_contract_digest) || !reader.string(result.package_digest) ||
        !reader.uuid(result.startup_scene) || result.startup_scene == AssetId{} || !reader.value(count) ||
        count > kMaxPipelines) {
        return {foundation::invalid_argument("Game build manifest header is invalid"), {}};
    }
    if (!valid_string(result.project_uuid) || !valid_string(result.module_id) || !valid_string(result.build_id) ||
        !valid_string(result.module_contract_digest) || !valid_string(result.package_digest)) {
        return {foundation::invalid_argument("Game build manifest identity is invalid"), {}};
    }
    result.startup_pipelines.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        AssetId id{};
        if (!reader.uuid(id) || id == AssetId{})
            return {foundation::invalid_argument("Game build pipeline identity is invalid"), {}};
        result.startup_pipelines.push_back(id);
    }
    if (!reader.string(result.target) || !reader.string(result.configuration) || !reader.string(result.package_name) ||
        !valid_string(result.target) || !valid_string(result.configuration) || !valid_string(result.package_name) ||
        reader.offset != reader.bytes.size()) {
        return {foundation::invalid_argument("Game build manifest body is invalid"), {}};
    }
    return {foundation::Result::success(), std::move(result)};
}
}  // namespace aeg::asset
