#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>

#include "asset_package.hpp"
#include "cooked_asset.hpp"
#include "runtime_asset_resolver.hpp"

namespace {
template <typename T>
void append(std::vector<std::byte>& bytes, T value) {
    const auto* first = reinterpret_cast<const std::byte*>(&value);
    bytes.insert(bytes.end(), first, first + sizeof(value));
}

void append_uuid(std::vector<std::byte>& bytes, aeg::asset::AssetId id) {
    for (const uint8_t value : id.value.bytes)
        append(bytes, value);
}

bool require(bool condition, const char* message) {
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}
}  // namespace

int main() {
    using namespace aeg;
    const auto material_id = asset::AssetId::parse("80112233-4455-6677-8899-aabbccddeeff");
    const auto texture_id = asset::AssetId::parse("81112233-4455-6677-8899-aabbccddeeff");
    const auto shader_id = asset::AssetId::parse("82112233-4455-6677-8899-aabbccddeeff");
    const auto missing_shader_id = asset::AssetId::parse("83112233-4455-6677-8899-aabbccddeeff");
    const auto wrong_binding_id = asset::AssetId::parse("84112233-4455-6677-8899-aabbccddeeff");
    const auto default_material_id = asset::AssetId::parse("85112233-4455-6677-8899-aabbccddeeff");
    if (!require(material_id && texture_id && shader_id && missing_shader_id && wrong_binding_id && default_material_id,
                 "material contract fixture IDs are invalid"))
        return 1;

    // AGM1 material: one valid texture and one authored parameter with no
    // shader schema mapping to the fixed TextureMaterialParameters::tint ABI.
    std::vector<std::byte> material_bytes;
    append<uint32_t>(material_bytes, 0x314d4741u);
    append<uint32_t>(material_bytes, 1u);
    append_uuid(material_bytes, shader_id.value);
    append<uint32_t>(material_bytes, 1u);
    // The current renderer has one canonical logical texture binding. Keep
    // this fixture on that binding so the parameter-schema diagnostic is the
    // first contract failure exercised below.
    append<uint64_t>(material_bytes, 1u);
    append_uuid(material_bytes, texture_id.value);
    append<uint32_t>(material_bytes, 1u);
    append<uint64_t>(material_bytes, 99u);
    append<uint8_t>(material_bytes, 0u);
    append<uint8_t>(material_bytes, 1u);

    // The other contract fixtures have no parameters, so their shader and
    // logical-binding diagnostics cannot be masked by the packing guard.
    auto missing_shader_bytes = material_bytes;
    auto wrong_binding_bytes = material_bytes;
    auto default_material_bytes = material_bytes;
    constexpr size_t kShaderOffset = sizeof(uint32_t) * 2u;
    constexpr size_t kBindingOffset = kShaderOffset + 16u + sizeof(uint32_t);
    constexpr size_t kParameterCountOffset = kBindingOffset + sizeof(uint64_t) + 16u;
    missing_shader_bytes.resize(kParameterCountOffset + sizeof(uint32_t));
    wrong_binding_bytes.resize(kParameterCountOffset + sizeof(uint32_t));
    default_material_bytes.resize(kParameterCountOffset + sizeof(uint32_t));
    // A zero shader UUID remains a malformed AGM1 fixture. The decoder owns
    // this fail-closed diagnostic before any GPU/host shader lookup.
    std::fill(missing_shader_bytes.begin() + kShaderOffset, missing_shader_bytes.begin() + kShaderOffset + 16u,
              std::byte{0});
    const uint32_t no_parameters = 0;
    std::memcpy(missing_shader_bytes.data() + kParameterCountOffset, &no_parameters, sizeof(no_parameters));
    std::memcpy(wrong_binding_bytes.data() + kParameterCountOffset, &no_parameters, sizeof(no_parameters));
    std::memcpy(default_material_bytes.data() + kParameterCountOffset, &no_parameters, sizeof(no_parameters));
    const uint64_t unsupported_binding = 2;
    std::memcpy(wrong_binding_bytes.data() + kBindingOffset, &unsupported_binding, sizeof(unsupported_binding));
    const auto decoded_fixture = asset::decode_material_payload(material_bytes);
    if (!decoded_fixture || decoded_fixture.value.parameters.size() != 1u) {
        if (decoded_fixture)
            std::cerr << "fixture parameter count=" << decoded_fixture.value.parameters.size() << '\n';
        else
            std::cerr << "fixture decode diagnostic: " << decoded_fixture.result.message << '\n';
        return 2;
    }

    std::vector<std::byte> texture_bytes(16u, std::byte{0xff});
    const uint32_t one = 1;
    const uint32_t four = 4;
    std::memcpy(texture_bytes.data(), &one, sizeof(one));
    std::memcpy(texture_bytes.data() + 4u, &one, sizeof(one));
    std::memcpy(texture_bytes.data() + 8u, &four, sizeof(four));

    const auto cooked_material =
        asset::encode_cooked_asset({material_id.value, asset::AssetKind::kMaterial, 1, {}, material_bytes});
    const auto cooked_missing_shader =
        asset::encode_cooked_asset({missing_shader_id.value, asset::AssetKind::kMaterial, 1, {}, missing_shader_bytes});
    const auto cooked_wrong_binding =
        asset::encode_cooked_asset({wrong_binding_id.value, asset::AssetKind::kMaterial, 1, {}, wrong_binding_bytes});
    const auto cooked_default_material = asset::encode_cooked_asset(
        {default_material_id.value, asset::AssetKind::kMaterial, 1, {}, default_material_bytes});
    const auto cooked_texture =
        asset::encode_cooked_asset({texture_id.value, asset::AssetKind::kTexture, 1, {}, std::move(texture_bytes)});
    if (!require(cooked_material && cooked_missing_shader && cooked_wrong_binding && cooked_default_material &&
                     cooked_texture,
                 "material contract fixture cook failed"))
        return 2;

    const std::vector<tooling::AssetPackageEntry> entries{
        {material_id.value, asset::AssetKind::kMaterial, 1, cooked_material.value},
        {missing_shader_id.value, asset::AssetKind::kMaterial, 1, cooked_missing_shader.value},
        {wrong_binding_id.value, asset::AssetKind::kMaterial, 1, cooked_wrong_binding.value},
        {default_material_id.value, asset::AssetKind::kMaterial, 1, cooked_default_material.value},
        {texture_id.value, asset::AssetKind::kTexture, 1, cooked_texture.value},
    };
    const auto path = std::filesystem::temp_directory_path() / "aeg_runtime_material_contract.aegbin";
    if (!require(static_cast<bool>(tooling::AssetPackageWriter{}.write(path, entries)),
                 "material contract package write failed"))
        return 3;
    auto resolver_result = application::runtime::RuntimeAssetResolver::open(path);
    if (!require(static_cast<bool>(resolver_result), "material contract package open failed"))
        return 4;
    auto resolver = std::move(resolver_result.value);
    const auto bound = resolver.bind_material(material_id.value);
    if (!require(static_cast<bool>(bound), "material contract bind failed"))
        return 5;
    const auto cached = resolver.cache()->load_material(material_id.value);
    if (!require(static_cast<bool>(cached) && cached.value.payload.parameters.size() == 1u,
                 "material payload parameter fixture did not decode"))
        return 6;
    const auto resolved = resolver.resolve_material(bound.value);
    if (!require(!resolved && resolved.result.code == foundation::ResultCode::kInvalidArgument &&
                     resolved.result.message.find("parameter") != std::string::npos,
                 "unmapped authored material parameter was not rejected"))
        return 6;

    const auto default_bound = resolver.bind_material(default_material_id.value);
    if (!require(static_cast<bool>(default_bound), "default material contract fixture did not bind"))
        return 7;

    // The host must publish the one shader UUID selected by the active
    // renderer before a material can enter the GPU path. A missing host
    // contract is not a material authoring error, while a different shader
    // UUID is an explicit contract mismatch.
    auto upload_context = application::runtime::RuntimeAssetResolver::UploadContext{};
    resolver.set_upload_context(upload_context);
    const auto missing_host_shader = resolver.resolve_material(default_bound.value);
    if (!require(!missing_host_shader && missing_host_shader.result.code == foundation::ResultCode::kNotReady &&
                     missing_host_shader.result.message.find("host shader") != std::string::npos,
                 "missing host material shader contract was not reported as not-ready"))
        return 8;
    upload_context.material_shader = missing_shader_id.value;
    resolver.set_upload_context(upload_context);
    const auto mismatching_host_shader = resolver.resolve_material(default_bound.value);
    if (!require(!mismatching_host_shader &&
                     mismatching_host_shader.result.code == foundation::ResultCode::kInvalidArgument &&
                     mismatching_host_shader.result.message.find("shader") != std::string::npos,
                 "mismatching host material shader contract was not rejected"))
        return 9;
    upload_context.material_shader = shader_id.value;
    resolver.set_upload_context(upload_context);
    const auto matching_host_shader = resolver.resolve_material(default_bound.value);
    if (!require(!matching_host_shader && matching_host_shader.result.code == foundation::ResultCode::kNotReady,
                 "matching host material shader contract did not reach the GPU readiness guard"))
        return 10;

    const auto missing_shader = resolver.bind_material(missing_shader_id.value);
    const auto wrong_binding = resolver.bind_material(wrong_binding_id.value);
    if (!require(missing_shader && wrong_binding, "material contract negative fixtures did not bind"))
        return 10;
    const auto missing_shader_result = resolver.resolve_material(missing_shader.value);
    if (!require(!missing_shader_result &&
                     missing_shader_result.result.code == foundation::ResultCode::kInvalidArgument &&
                     missing_shader_result.result.message.find("Material payload") != std::string::npos,
                 "malformed material shader payload was not rejected"))
        return 11;
    const auto wrong_binding_result = resolver.resolve_material(wrong_binding.value);
    if (!require(!wrong_binding_result &&
                     wrong_binding_result.result.code == foundation::ResultCode::kInvalidArgument &&
                     wrong_binding_result.result.message.find("canonical binding id 1") != std::string::npos,
                 "unsupported material binding was not rejected"))
        return 12;

    std::error_code error;
    std::filesystem::remove(path, error);
    std::cout << "runtime material contract smoke passed\n";
    return 0;
}
