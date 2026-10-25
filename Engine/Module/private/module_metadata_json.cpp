#include "module_metadata_json.hpp"
#include "module_api.h"
#include "../../Feature/private/feature_json_codec.hpp"
#include "../../Serialization/private/json_value_codec.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <locale>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace aeg::module::detail {
namespace {
using Json = nlohmann::json;

constexpr std::size_t kMaxMetadataBytes = 64u * 1024u * 1024u;

using namespace serialization::detail;

Json descriptor_json(const ModuleDescriptor& value) {
    Json result{{"id", id_json(value.id)},
                {"name", value.name},
                {"version", version_json({value.version.major, value.version.minor, value.version.patch})},
                {"dependencies", Json::array()}};
    std::vector<ModuleDependency> dependencies = value.dependencies;
    std::sort(dependencies.begin(), dependencies.end(),
              [](const auto& left, const auto& right) { return left.module.value() < right.module.value(); });
    for (const auto& item : dependencies)
        result["dependencies"].push_back(
            Json{{"module", id_json(item.module)}, {"minimum_version", version_json({item.minimum_version.major, item.minimum_version.minor, item.minimum_version.patch})}});
    return result;
}
foundation::ResultValue<ModuleDescriptor> descriptor_value(const Json& value) {
    ModuleDescriptor result{};
    const auto id = id_value<ModuleId>(value, "id");
    const auto name = string_value(value, "name");
    const auto version = version_value(value, "version");
    if (!id || !name || !version)
        return {invalid("module"), {}};
    result.id = id.value;
    result.name = name.value;
    result.version = {version.value.major, version.value.minor, version.value.patch};
    for (const auto& item : value.at("dependencies")) {
        const auto module = id_value<ModuleId>(item, "module");
        const auto minimum = version_value(item, "minimum_version");
        if (!module || !minimum)
            return {invalid("dependency"), {}};
        result.dependencies.push_back({module.value, {minimum.value.major, minimum.value.minor, minimum.value.patch}});
    }
    return {foundation::Result::success(), std::move(result)};
}

Json metadata_json(const ModuleMetadata& input) {
    Json result{{"abi_major", input.abi_major},
                {"abi_minor", input.abi_minor},
                {"build_id", input.build_id},
                {"features", Json::array()},
                {"configuration", input.configuration},
                {"contract_digest", input.contract_digest},
                {"module", descriptor_json(input.module)},
                {"schema_version", input.schema_version},
                {"sdk_contract_major", input.sdk_contract_major},
                {"sdk_contract_minor", input.sdk_contract_minor},
                {"target", input.target}};
    for (const auto& entry : input.feature_catalog.entries())
        result["features"].push_back(feature::detail::encode_catalog_entry_json(entry));
    return result;
}

std::string sha256(std::string_view bytes) {
    constexpr uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
    // The implementation is kept deterministic and private; nlohmann handles
    // JSON syntax while this small digest avoids an engine crypto dependency.
    std::vector<uint8_t> input(bytes.begin(), bytes.end());
    const uint64_t bits = static_cast<uint64_t>(input.size()) * 8u;
    input.push_back(0x80u);
    while (input.size() % 64u != 56u)
        input.push_back(0);
    for (int shift = 7; shift >= 0; --shift)
        input.push_back(static_cast<uint8_t>(bits >> (shift * 8)));
    uint32_t s[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    auto r = [](uint32_t x, uint32_t n) { return (x >> n) | (x << (32u - n)); };
    for (size_t off = 0; off < input.size(); off += 64) {
        uint32_t w[64]{};
        for (uint32_t i = 0; i < 16; i++)
            w[i] = (uint32_t(input[off + i * 4]) << 24) | (uint32_t(input[off + i * 4 + 1]) << 16) |
                   (uint32_t(input[off + i * 4 + 2]) << 8) | input[off + i * 4 + 3];
        for (uint32_t i = 16; i < 64; i++) {
            uint32_t a = r(w[i - 15], 7) ^ r(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t b = r(w[i - 2], 17) ^ r(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + a + w[i - 7] + b;
        }
        uint32_t a = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];
        for (uint32_t i = 0; i < 64; i++) {
            uint32_t q = r(e, 6) ^ r(e, 11) ^ r(e, 25), ch = (e & f) ^ ((~e) & g), t1 = h + q + ch + k[i] + w[i],
                     p = r(a, 2) ^ r(a, 13) ^ r(a, 22), maj = (a & b) ^ (a & c) ^ (b & c), t2 = p + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        s[0] += a;
        s[1] += b;
        s[2] += c;
        s[3] += d;
        s[4] += e;
        s[5] += f;
        s[6] += g;
        s[7] += h;
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (auto item : s)
        out << std::setw(8) << item;
    return out.str();
}

}  // namespace

foundation::ResultValue<std::string> encode_metadata_json(const ModuleMetadata& metadata) {
    if (metadata.schema_version != kModuleMetadataSchemaVersion || metadata.abi_major != AEG_MODULE_ABI_MAJOR ||
        metadata.sdk_contract_major != kModuleSdkContractMajor)
        return {foundation::invalid_argument("module metadata version is invalid"), {}};
    for (const auto& entry : metadata.feature_catalog.entries()) {
        if (!entry.contract.definition_id.is_valid())
            return {foundation::invalid_argument("module metadata requires a complete Feature contract"), {}};
        if (const auto valid = feature::validate_feature_contract(entry.contract); !valid) return {valid, {}};
    }
    const auto result = metadata_json(metadata).dump();
    if (result.size() > kMaxMetadataBytes)
        return {foundation::invalid_argument("module metadata is too large"), {}};
    return {foundation::Result::success(), result + "\n"};
}
foundation::ResultValue<ModuleMetadata> decode_metadata_json(std::string_view json) {
    if (json.empty() || json.size() > kMaxMetadataBytes)
        return {foundation::invalid_argument("module metadata size is invalid"), {}};
    try {
        const auto root = Json::parse(json);
        if (!root.is_object())
            return {invalid("root"), {}};
        ModuleMetadata result{};
        const auto build = string_value(root, "build_id");
        const auto contract = string_value(root, "contract_digest");
        if (!build || !contract)
            return {foundation::invalid_argument("module metadata is missing required fields"), {}};
        result.build_id = build.value;
        result.contract_digest = contract.value;
        const auto schema = u32_value<uint32_t>(root, "schema_version");
        const auto abi_major = u32_value<uint32_t>(root, "abi_major");
        const auto abi_minor = u32_value<uint32_t>(root, "abi_minor");
        const auto sdk_major = u32_value<uint32_t>(root, "sdk_contract_major");
        const auto sdk_minor = u32_value<uint32_t>(root, "sdk_contract_minor");
        const auto target = string_value(root, "target");
        const auto configuration = string_value(root, "configuration");
        if (!schema || !abi_major || !abi_minor || !sdk_major || !sdk_minor || !target || !configuration ||
            !root.contains("module") || !root.contains("features"))
            return {invalid("header"), {}};
        if (schema.value != kModuleMetadataSchemaVersion || abi_major.value != AEG_MODULE_ABI_MAJOR || sdk_major.value != kModuleSdkContractMajor)
            return {invalid("metadata schema/ABI/SDK version"), {}};
        const auto descriptor = descriptor_value(root.at("module"));
        if (!descriptor || !root.at("features").is_array() || root.contains("catalog"))
            return {invalid("catalog"), {}};
        result.schema_version = schema.value;
        result.abi_major = abi_major.value;
        result.abi_minor = abi_minor.value;
        result.sdk_contract_major = sdk_major.value;
        result.sdk_contract_minor = sdk_minor.value;
        result.target = target.value;
        result.configuration = configuration.value;
        result.module = descriptor.value;
        if (root.contains("features")) {
            for (const auto& item : root.at("features")) {
                const auto entry = feature::detail::decode_catalog_entry_json(item);
                if (!entry)
                    return {entry.result, {}};
                if (!entry.value.contract.definition_id.is_valid())
                    return {invalid("complete Feature contract"), {}};
                if (entry.value.owner_module != result.module.id)
                    return {invalid("feature owner module"), {}};
                if (const auto add = result.feature_catalog.add_entry(entry.value); !add)
                    return {add, {}};
            }
        }
        return {foundation::Result::success(), std::move(result)};
    } catch (const std::exception& exception) {
        return {foundation::invalid_argument(std::string("module metadata JSON is invalid: ") + exception.what()), {}};
    }
}
foundation::ResultValue<std::string> metadata_sha256(std::string_view bytes) {
    return {foundation::Result::success(), sha256(bytes)};
}
}  // namespace aeg::module::detail
