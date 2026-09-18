/** @file scene_payload.cpp */
#include "scene_payload.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <utility>

namespace aeg::asset {
namespace {
constexpr uint32_t kMagic = 0x314e5341u;  // ASN1
constexpr uint32_t kMaxCount = 1'000'000;
constexpr uint32_t kMaxString = 16u * 1024u * 1024u;
constexpr std::size_t kMaxSceneBlob = 64u * 1024u * 1024u;

std::string reference_key(const SceneAssetReference& reference) {
    std::string key(reinterpret_cast<const char*>(reference.scene_entity_uuid.data()),
                    reference.scene_entity_uuid.size());
    key.append(reference.component_type);
    key.push_back('\0');
    key.append(std::to_string(reference.field_id));
    return key;
}

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
    bool blob(std::vector<std::byte>& output) {
        uint32_t size = 0;
        if (!value(size) || size > kMaxSceneBlob || size > bytes.size() - offset)
            return false;
        output.assign(bytes.begin() + offset, bytes.begin() + offset + size);
        offset += size;
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
        for (auto& byte : output.value.bytes)
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
    void blob(std::span<const std::byte> blob) {
        value(static_cast<uint32_t>(blob.size()));
        bytes.insert(bytes.end(), blob.begin(), blob.end());
    }
    void string(std::string_view text) {
        blob(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
    }
    void uuid(const AssetId& id) {
        for (uint8_t byte : id.value.bytes)
            value(byte);
    }
};
}  // namespace

foundation::ResultValue<std::vector<std::byte>> encode_scene_payload(const ScenePayload& payload) {
    if (payload.schema_version == 0 || payload.serialized_scene.size() > kMaxSceneBlob ||
        payload.serialized_scene.size() > UINT32_MAX || payload.references.size() > kMaxCount) {
        return {foundation::invalid_argument("Scene payload identity or size is invalid"), {}};
    }
    std::unordered_set<std::string> keys;
    Writer writer{};
    writer.value(kMagic);
    writer.value(payload.schema_version);
    writer.blob(payload.serialized_scene);
    writer.value(static_cast<uint32_t>(payload.references.size()));
    for (const SceneAssetReference& reference : payload.references) {
        if (reference.component_type.empty() || reference.field_id == 0 || reference.asset == AssetId{} ||
            !keys.insert(reference_key(reference)).second) {
            return {foundation::invalid_argument("Scene payload reference is invalid or duplicated"), {}};
        }
        for (uint8_t byte : reference.scene_entity_uuid)
            writer.value(byte);
        writer.string(reference.component_type);
        writer.value(reference.component_version);
        writer.value(reference.field_id);
        writer.uuid(reference.asset);
        writer.value(static_cast<uint8_t>(reference.kind));
    }
    return {foundation::Result::success(), std::move(writer.bytes)};
}

foundation::ResultValue<ScenePayload> decode_scene_payload(std::span<const std::byte> bytes) {
    Reader reader{bytes};
    ScenePayload result{};
    uint32_t magic = 0, count = 0;
    if (!reader.value(magic) || !reader.value(result.schema_version) || magic != kMagic || result.schema_version == 0 ||
        !reader.blob(result.serialized_scene) || !reader.value(count) || count > kMaxCount) {
        return {foundation::invalid_argument("Scene payload header is invalid"), {}};
    }
    std::unordered_set<std::string> keys;
    result.references.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        SceneAssetReference reference{};
        uint8_t kind = 0;
        for (uint8_t& byte : reference.scene_entity_uuid)
            if (!reader.value(byte))
                return {foundation::invalid_argument("Scene payload entity UUID is truncated"), {}};
        if (!reader.string(reference.component_type) || reference.component_type.empty() ||
            !reader.value(reference.component_version) || !reader.value(reference.field_id) ||
            reference.field_id == 0 || !reader.uuid(reference.asset) || !reader.value(kind) ||
            kind > static_cast<uint8_t>(AssetKind::kShader) || !keys.insert(reference_key(reference)).second) {
            return {foundation::invalid_argument("Scene payload reference is invalid or duplicated"), {}};
        }
        reference.kind = static_cast<AssetKind>(kind);
        result.references.push_back(std::move(reference));
    }
    if (reader.offset != reader.bytes.size())
        return {foundation::invalid_argument("Scene payload has trailing bytes"), {}};
    return {foundation::Result::success(), std::move(result)};
}
}  // namespace aeg::asset
