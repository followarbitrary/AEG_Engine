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
bool valid_entity_uuid(const std::array<uint8_t, 16>& uuid) {
    return std::any_of(uuid.begin(), uuid.end(), [](uint8_t byte) { return byte != 0; });
}
bool valid_kind(foundation::AssetKind kind) {
    return static_cast<uint8_t>(kind) <= static_cast<uint8_t>(foundation::AssetKind::kShader);
}

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
    bool uuid(foundation::AssetId& output) {
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
    void uuid(const foundation::AssetId& id) {
        for (uint8_t byte : id.value.bytes)
            value(byte);
    }
};
}  // namespace

bool has_scene_payload_header(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < sizeof(uint32_t))
        return false;
    uint32_t magic = 0;
    std::memcpy(&magic, bytes.data(), sizeof(magic));
    return magic == kMagic;
}

foundation::Result relocate_scene_assets(ScenePayload& payload, std::span<const uint64_t> stable_ids,
                                         const SceneAssetResolver& resolve) {
    if (stable_ids.size() != payload.references.size())
        return foundation::invalid_argument("Scene relocation stable-id count does not match references");
    if (!resolve)
        return foundation::invalid_argument("Scene relocation resolver is missing");
    for (std::size_t index = 0; index < payload.references.size(); ++index) {
        if (stable_ids[index] == 0)
            return foundation::invalid_argument("Scene relocation stable id is zero");
        const auto resolved = resolve(stable_ids[index]);
        if (!resolved)
            return resolved.result;
        if (resolved.value == foundation::AssetId{})
            return foundation::invalid_argument("Scene relocation resolved to zero AssetId");
        payload.references[index].asset = resolved.value;
    }
    return foundation::Result::success();
}

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
        if (!valid_entity_uuid(reference.scene_entity_uuid) || reference.component_type.empty() ||
            reference.component_type.size() > kMaxString || reference.component_version == 0 ||
            reference.field_id == 0 || reference.asset == foundation::AssetId{} || !valid_kind(reference.kind) ||
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
        if (!valid_entity_uuid(reference.scene_entity_uuid) || !reader.string(reference.component_type) ||
            reference.component_type.empty() || !reader.value(reference.component_version) ||
            !reader.value(reference.field_id) || reference.component_version == 0 || reference.field_id == 0 ||
            !reader.uuid(reference.asset) || reference.asset == foundation::AssetId{} || !reader.value(kind) ||
            kind > static_cast<uint8_t>(foundation::AssetKind::kShader) || !keys.insert(reference_key(reference)).second) {
            return {foundation::invalid_argument("Scene payload reference is invalid or duplicated"), {}};
        }
        reference.kind = static_cast<foundation::AssetKind>(kind);
        result.references.push_back(std::move(reference));
    }
    if (reader.offset != reader.bytes.size())
        return {foundation::invalid_argument("Scene payload has trailing bytes"), {}};
    return {foundation::Result::success(), std::move(result)};
}
}  // namespace aeg::asset
