/** @file cooked_asset.cpp */
#include "cooked_asset.hpp"

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <exception>
#include <utility>

#include "cooked_asset_generated.h"

namespace aeg::asset {
namespace {
namespace fb = ::aeg::asset::fb;
std::vector<foundation::AssetId> canonical_dependencies(std::span<const foundation::AssetId> dependencies) {
    std::vector<foundation::AssetId> result(dependencies.begin(), dependencies.end());
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}
bool valid_kind(foundation::AssetKind kind) {
    return static_cast<uint8_t>(kind) <= static_cast<uint8_t>(foundation::AssetKind::kShader);
}
[[nodiscard]] foundation::ResultValue<foundation::AssetId> decode_id(const flatbuffers::Vector<uint8_t>* bytes) {
    if (bytes == nullptr || bytes->size() != 16) {
        return {foundation::invalid_argument("Cooked asset UUID has an invalid length"), {}};
    }
    foundation::AssetId id{};
    for (flatbuffers::uoffset_t index = 0; index < bytes->size(); ++index) {
        id.value.bytes[index] = bytes->Get(index);
    }
    if (id == foundation::AssetId{})
        return {foundation::invalid_argument("Cooked asset UUID is zero"), {}};
    return {foundation::Result::success(), id};
}
[[nodiscard]] foundation::ResultValue<foundation::AssetKind> decode_kind(fb::AssetKind kind) {
    const auto value = static_cast<uint8_t>(kind);
    if (value > static_cast<uint8_t>(foundation::AssetKind::kShader)) {
        return {foundation::invalid_argument("Cooked asset has an unknown asset kind"), {}};
    }
    return {foundation::Result::success(), static_cast<foundation::AssetKind>(value)};
}
[[nodiscard]] fb::AssetKind encode_kind(foundation::AssetKind kind) noexcept {
    return static_cast<fb::AssetKind>(kind);
}
}  // namespace

foundation::ResultValue<std::vector<std::byte>> encode_cooked_asset(const CookedAsset& asset) {
    try {
        if (asset.id == foundation::AssetId{} || asset.schema_version == 0 || !valid_kind(asset.kind)) {
            return {foundation::invalid_argument("Cooked asset identity or schema is invalid"), {}};
        }
        if (std::any_of(asset.runtime_dependencies.begin(), asset.runtime_dependencies.end(),
                        [](const foundation::AssetId& id) { return id == foundation::AssetId{}; }))
            return {foundation::invalid_argument("Cooked asset runtime dependency UUID is zero"), {}};
        const auto dependencies_canonical = canonical_dependencies(asset.runtime_dependencies);
        flatbuffers::FlatBufferBuilder builder;
        const auto id = builder.CreateVector(asset.id.value.bytes.data(), asset.id.value.bytes.size());
        std::vector<flatbuffers::Offset<fb::AssetReference>> dependencies;
        dependencies.reserve(dependencies_canonical.size());
        for (const foundation::AssetId dependency : dependencies_canonical) {
            const auto dependency_id =
                builder.CreateVector(dependency.value.bytes.data(), dependency.value.bytes.size());
            dependencies.push_back(fb::CreateAssetReference(builder, dependency_id));
        }
        const auto dependency_vector = builder.CreateVector(dependencies);
        const auto payload =
            builder.CreateVector(reinterpret_cast<const uint8_t*>(asset.payload.data()), asset.payload.size());
        const auto root = fb::CreateCookedAsset(builder, id, encode_kind(asset.kind), asset.schema_version,
                                                dependency_vector, payload);
        fb::FinishCookedAssetBuffer(builder, root);
        return {foundation::Result::success(),
                {reinterpret_cast<const std::byte*>(builder.GetBufferPointer()),
                 reinterpret_cast<const std::byte*>(builder.GetBufferPointer()) + builder.GetSize()}};
    } catch (const std::exception& exception) {
        return {foundation::Result::failure(foundation::ResultCode::kOutOfMemory, exception.what()), {}};
    }
}

foundation::ResultValue<CookedAsset> decode_cooked_asset(std::span<const std::byte> bytes) {
    flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    if (!fb::VerifyCookedAssetBuffer(verifier)) {
        return {foundation::invalid_argument("Cooked asset FlatBuffer verification failed"), {}};
    }
    const fb::CookedAsset* encoded = fb::GetCookedAsset(bytes.data());
    const auto id = decode_id(encoded->id());
    const auto kind = decode_kind(encoded->kind());
    if (!id)
        return {id.result, {}};
    if (!kind)
        return {kind.result, {}};
    if (encoded->payload() == nullptr) {
        return {foundation::invalid_argument("Cooked asset payload is missing"), {}};
    }
    CookedAsset decoded{};
    decoded.id = id.value;
    decoded.kind = kind.value;
    if (encoded->schema_version() == 0)
        return {foundation::invalid_argument("Cooked asset schema version is zero"), {}};
    decoded.schema_version = encoded->schema_version();
    decoded.payload.assign(reinterpret_cast<const std::byte*>(encoded->payload()->Data()),
                           reinterpret_cast<const std::byte*>(encoded->payload()->Data()) + encoded->payload()->size());
    if (const auto* dependencies = encoded->runtime_dependencies(); dependencies != nullptr) {
        decoded.runtime_dependencies.reserve(dependencies->size());
        for (const fb::AssetReference* dependency : *dependencies) {
            if (dependency == nullptr) {
                return {foundation::invalid_argument("Cooked asset contains a null dependency"), {}};
            }
            const auto dependency_id = decode_id(dependency->id());
            if (!dependency_id)
                return {dependency_id.result, {}};
            decoded.runtime_dependencies.push_back(dependency_id.value);
        }
    }
    if (!valid_kind(decoded.kind))
        return {foundation::invalid_argument("Cooked asset kind is invalid"), {}};
    if (std::any_of(decoded.runtime_dependencies.begin(), decoded.runtime_dependencies.end(),
                    [](const foundation::AssetId& id) { return id == foundation::AssetId{}; }))
        return {foundation::invalid_argument("Cooked asset runtime dependency UUID is zero"), {}};
    decoded.runtime_dependencies = canonical_dependencies(decoded.runtime_dependencies);
    return {foundation::Result::success(), std::move(decoded)};
}
}  // namespace aeg::asset
