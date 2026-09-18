/** @file cooked_asset.cpp */
#include "cooked_asset.hpp"

#include <flatbuffers/flatbuffers.h>

#include <exception>
#include <utility>

#include "cooked_asset_generated.h"

namespace aeg::asset {
namespace {
namespace fb = ::aeg::asset::fb;
[[nodiscard]] foundation::ResultValue<AssetId> decode_id(const flatbuffers::Vector<uint8_t>* bytes) {
    if (bytes == nullptr || bytes->size() != 16) {
        return {foundation::invalid_argument("Cooked asset UUID has an invalid length"), {}};
    }
    AssetId id{};
    for (flatbuffers::uoffset_t index = 0; index < bytes->size(); ++index) {
        id.value.bytes[index] = bytes->Get(index);
    }
    return {foundation::Result::success(), id};
}
[[nodiscard]] foundation::ResultValue<AssetKind> decode_kind(fb::AssetKind kind) {
    const auto value = static_cast<uint8_t>(kind);
    if (value > static_cast<uint8_t>(AssetKind::kShader)) {
        return {foundation::invalid_argument("Cooked asset has an unknown asset kind"), {}};
    }
    return {foundation::Result::success(), static_cast<AssetKind>(value)};
}
[[nodiscard]] fb::AssetKind encode_kind(AssetKind kind) noexcept {
    return static_cast<fb::AssetKind>(kind);
}
}  // namespace

foundation::ResultValue<std::vector<std::byte>> encode_cooked_asset(const CookedAsset& asset) {
    try {
        flatbuffers::FlatBufferBuilder builder;
        const auto id = builder.CreateVector(asset.id.value.bytes.data(), asset.id.value.bytes.size());
        std::vector<flatbuffers::Offset<fb::AssetReference>> dependencies;
        dependencies.reserve(asset.runtime_dependencies.size());
        for (const AssetId dependency : asset.runtime_dependencies) {
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
    return {foundation::Result::success(), std::move(decoded)};
}
}  // namespace aeg::asset
