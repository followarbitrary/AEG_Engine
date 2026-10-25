#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

#include "../Engine/Module/abi/module_api.h"
#include "module_bundle.hpp"

namespace {
template <typename Metadata>
constexpr bool has_legacy_catalog = requires { &Metadata::catalog; };

bool write_text(const std::filesystem::path& path, const std::string& value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    return static_cast<bool>(output);
}

std::string manifest_json(const aeg::module::ModuleMetadata& metadata, std::string_view metadata_digest,
                          std::string_view binary_digest, std::string_view metadata_path = "module.metadata.json",
                          std::string_view binary_path = "game.dll", std::string_view build_id = "build-1",
                          std::string_view target = "windows-x64-vulkan", std::string_view configuration = "Debug",
                          std::string_view contract_digest = {}) {
    const std::string contract = contract_digest.empty() ? metadata.contract_digest : std::string(contract_digest);
    return "{\"manifest_schema_version\":1,\"metadata_path\":\"" + std::string(metadata_path) +
           "\",\"binary_path\":\"" + std::string(binary_path) + "\",\"build_id\":\"" + std::string(build_id) +
           "\",\"target\":\"" + std::string(target) + "\",\"configuration\":\"" + std::string(configuration) +
           "\",\"contract_digest\":\"" + contract + "\",\"metadata_sha256\":\"" + std::string(metadata_digest) +
           "\",\"binary_sha256\":\"" + std::string(binary_digest) + "\"}\n";
}
}  // namespace

int main() {
    using namespace aeg;
    if (module::kModuleMetadataSchemaVersion != 3 || module::kModuleSdkContractMajor != 3 || AEG_MODULE_ABI_MAJOR != 2)
        return 51;
    if (has_legacy_catalog<module::ModuleMetadata>) {
        std::cerr << "module metadata still exposes the legacy Pipeline/Pass catalog\n";
        return 40;
    }
    // The metadata boundary must retain the contract used to validate editor ports.
    const auto summary_only = module::decode_module_metadata(R"({
      "schema_version":3,"abi_major":2,"abi_minor":0,"sdk_contract_major":3,"sdk_contract_minor":0,
      "build_id":"dev","contract_digest":"","target":"windows-x64-vulkan","configuration":"Debug",
      "module":{"id":"42","name":"Ports","version":{"major":1,"minor":0,"patch":0},"dependencies":[]},
      "features":[{"id":"100","owner_module":"42","version":{"major":1,"minor":0,"patch":0},"structure":"atomic",
        "domains":["logic"],"execution":"call","state":"stateless","scope":"200",
        "clock":{"id":"300","frequency_hz":60,"max_backlog":4,"late_data_policy":"reject"},
        "permissions_version":1,"ports":[{"id":"400","direction":"output","schema":"500","version":2}]}]
    })");
    if (summary_only) { std::cerr << "summary-only metadata bypassed its required Clock contract\n"; return 52; }
    feature::FeatureDefinition authoring_definition{};
    authoring_definition.id = feature::FeatureDefinitionId(100);
    authoring_definition.version = {1, 0, 0}; authoring_definition.owner_module = feature::ModuleIdentity(42);
    auto& authoring_contract = authoring_definition.contract;
    authoring_contract.definition_id = authoring_definition.id; authoring_contract.version = authoring_definition.version;
    authoring_contract.domains = feature::to_mask(feature::FeatureDomain::kLogic);
    authoring_contract.scope.id = feature::ScopeId(200);
    authoring_contract.clock = feature::make_periodic_clock_contract(feature::ClockId(300), 60, 4,
        feature::LateDataPolicy::kReject, feature::DeadlinePolicy::kObserve, 250000, 0);
    authoring_contract.permissions.version = 1;
    authoring_contract.ports = {{feature::PortId(400), feature::PortDirection::kOutput, feature::ValueSchemaId(500), 2}};
    module::ModuleMetadata authoring_metadata{};
    authoring_metadata.module = {feature::ModuleIdentity(42), "Ports", {1, 0, 0}, {}};
    authoring_metadata.build_id = "dev"; authoring_metadata.target = "windows-x64-vulkan";
    authoring_metadata.configuration = "Debug";
    if (!authoring_metadata.feature_catalog.export_definition(authoring_definition)) return 53;
    const auto authoring_bytes = module::encode_module_metadata(authoring_metadata);
    const auto authoring = authoring_bytes ? module::decode_module_metadata(authoring_bytes.value)
        : foundation::ResultValue<module::ModuleMetadata>{authoring_bytes.result, {}};
    const auto authoring_roundtrip = authoring ? module::encode_module_metadata(authoring.value)
                                               : foundation::ResultValue<std::string>{authoring.result, {}};
    const auto restored = authoring_roundtrip ? module::decode_module_metadata(authoring_roundtrip.value)
                                             : foundation::ResultValue<module::ModuleMetadata>{authoring_roundtrip.result, {}};
    if (!restored || authoring_roundtrip.value.find("\"ports\":[{\"direction\":") == std::string::npos) {
        std::cerr << "Feature metadata lost its declared port contract\n";
        return 30;
    }
    auto typed_entry = restored.value.feature_catalog.entries().front();
    feature::ParameterContract speed{feature::ParameterId("Speed"), 1, 4, true};
    speed.metadata = feature::ParameterMetadata{};
    speed.metadata->type = feature::ParameterType::kFloat;
    speed.metadata->default_value = 1.0;
    speed.metadata->constraint = feature::NumericRange{0, 4, 0, false};
    speed.metadata->display_name = "Movement speed";
    speed.metadata->ui_group = "Movement";
    typed_entry.contract.definition_id = typed_entry.definition_id;
    typed_entry.contract.version = typed_entry.version;
    typed_entry.contract.domains = typed_entry.domains;
    typed_entry.contract.execution = typed_entry.execution;
    typed_entry.contract.state = typed_entry.state;
    typed_entry.contract.scope.id = feature::ScopeId(200);
    typed_entry.contract.clock = aeg::feature::make_periodic_clock_contract(typed_entry.clock, 60, 4, feature::LateDataPolicy::kReject,
        aeg::feature::DeadlinePolicy::kObserve, typed_entry.contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    typed_entry.contract.permissions.version = 1;
    typed_entry.contract.ports = typed_entry.ports;
    typed_entry.contract.parameters = {speed};
    typed_entry.state = feature::StatePolicy::kInstanceState;
    typed_entry.contract.state = typed_entry.state;
    typed_entry.contract.state_payload = {{3, 2, 1}, 65};
    const auto add_typed = [&](const char* id, uint32_t max_size, feature::ParameterType type,
                               foundation::ParameterValue value, feature::ParameterConstraint constraint) {
        feature::ParameterContract parameter{feature::ParameterId(id),1,max_size,true};
        parameter.metadata = feature::ParameterMetadata{};
        parameter.metadata->type = type;
        parameter.metadata->default_value = std::move(value);
        parameter.metadata->constraint = std::move(constraint);
        typed_entry.contract.parameters.push_back(std::move(parameter));
    };
    add_typed("Mode",32,feature::ParameterType::kEnum,std::string("Walk"),feature::EnumChoices{{"Walk","Run"},{"Slow","Fast"}});
    add_typed("Label",32,feature::ParameterType::kString,std::string("abc"),feature::StringPattern{"[a-z]+",1,8,false,false});
    add_typed("Asset",16,feature::ParameterType::kAssetReference,foundation::AssetReference{"00112233-4455-6677-8899-aabbccddeeff"},feature::AssetConstraint{false});
    add_typed("Direction",24,feature::ParameterType::kVector3,std::vector<double>{1,0,0},feature::VectorConstraint{{feature::NumericRange{0,1,0,true}},true});
    add_typed("Enabled",1,feature::ParameterType::kBool,false,std::monostate{});

    feature::FeatureDefinition typed_definition{};
    typed_definition.id = typed_entry.definition_id;
    typed_definition.version = typed_entry.version;
    typed_definition.owner_module = typed_entry.owner_module;
    typed_definition.contract = typed_entry.contract;
    typed_entry.contract_digest = feature::feature_contract_digest(typed_definition);
    auto typed_module = restored.value;
    typed_module.feature_catalog = {};
    if (!typed_module.feature_catalog.add_entry(typed_entry)) return 31;
    const auto typed_json = module::encode_module_metadata(typed_module);
    const auto typed_restored = typed_json ? module::decode_module_metadata(typed_json.value)
                                         : foundation::ResultValue<module::ModuleMetadata>{typed_json.result, {}};
    if (!typed_restored || typed_restored.value.feature_catalog.entries()[0].contract.parameters != typed_entry.contract.parameters ||
        typed_restored.value.feature_catalog.entries()[0].contract.state_payload != typed_entry.contract.state_payload ||
        !feature::same_clock_schedule(typed_restored.value.feature_catalog.entries()[0].contract.clock, typed_entry.contract.clock) ||
        typed_restored.value.feature_catalog.entries()[0].contract.clock.work_budget_ns != typed_entry.contract.clock.work_budget_ns) {
        std::cerr << "Feature module metadata lost parameter or state semantics\n";
        if (!typed_restored) std::cerr << typed_restored.result.message << '\n';
        return 32;
    }
    const auto state_limit_position = typed_json.value.find("\"max_size\":65");
    if (state_limit_position == std::string::npos) return 35;
    auto bad_state_json = typed_json.value;
    bad_state_json.replace(state_limit_position, std::string("\"max_size\":65").size(), "\"max_size\":0");
    if (module::decode_module_metadata(bad_state_json)) return 36;
    auto bad_default_json = typed_json.value;
    const auto default_position = bad_default_json.find("\"default_value\"");
    const auto default_value_position = bad_default_json.find("\"value\":\"1\"", default_position);
    if (default_value_position == std::string::npos) return 33;
    bad_default_json.replace(default_value_position, std::string("\"value\":\"1\"").size(), "\"value\":\"1junk\"");
    if (module::decode_module_metadata(bad_default_json)) return 34;
    auto wrong_schema = typed_json.value;
    const auto schema_position = wrong_schema.find("\"schema_version\":3");
    if (schema_position == std::string::npos) return 41;
    wrong_schema.replace(schema_position, std::string("\"schema_version\":3").size(), "\"schema_version\":4");
    if (module::decode_module_metadata(wrong_schema)) { std::cerr << "unsupported metadata schema accepted\n"; return 42; }
    auto old_catalog = typed_json.value; old_catalog.insert(1, "\"catalog\":[],");
    if (module::decode_module_metadata(old_catalog) || module::decode_module_metadata("{\"build_id\":\"b\",\"contract_digest\":\"d\"}")) return 43;
    auto old_metadata = typed_module; old_metadata.schema_version = 1;
    if (module::encode_module_metadata(old_metadata)) return 44;
    old_metadata.schema_version = 2;
    if (module::encode_module_metadata(old_metadata)) return 45;
    old_metadata = typed_module; old_metadata.sdk_contract_major = 2;
    if (module::encode_module_metadata(old_metadata)) return 46;
    for (const auto field : {"schema_version", "sdk_contract_major"}) {
        auto old_json = typed_json.value;
        const auto key = std::string("\"") + field + "\":3";
        const auto offset = old_json.find(key);
        if (offset == std::string::npos) return 47;
        old_json.replace(offset, key.size(), std::string("\"") + field + "\":2");
        if (module::decode_module_metadata(old_json)) return 48;
    }
    for (const auto field : {"capability_version", "timing", "work_budget_ns"}) {
        auto old_clock = typed_json.value;
        const auto offset = old_clock.find(std::string("\"") + field + "\"");
        if (offset == std::string::npos) return 49;
        // Renaming a required key keeps the JSON valid and proves no default is supplied.
        old_clock.insert(offset + 1, "legacy_");
        if (module::decode_module_metadata(old_clock)) return 50;
    }
    const auto root = std::filesystem::temp_directory_path() / "aeg_module_metadata_smoke_bundle";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    if (error)
        return 1;

    module::ModuleMetadata metadata{};
    metadata.module.id = module::ModuleId("com.aeg.test.metadata");
    metadata.module.name = "\xE6\xB5\x8B\xE8\xAF\x95\xE6\xA8\xA1\xE5\x9D\x97";
    metadata.module.version = {1, 2, 3};
    metadata.build_id = "build-1";
    metadata.target = "windows-x64-vulkan";
    metadata.configuration = "Debug";
    metadata.abi_major = AEG_MODULE_ABI_MAJOR;
    metadata.abi_minor = AEG_MODULE_ABI_MINOR;

    feature::FeatureDefinition render_definition{};
    render_definition.id = feature::FeatureDefinitionId{std::numeric_limits<uint64_t>::max() - 1u};
    render_definition.version = {1, 0, 0}; render_definition.owner_module = metadata.module.id;
    auto& render_contract = render_definition.contract;
    render_contract.definition_id = render_definition.id; render_contract.version = render_definition.version;
    render_contract.domains = feature::to_mask(feature::FeatureDomain::kRender);
    render_contract.execution = feature::ExecutionKind::kContinuous;
    render_contract.state = feature::StatePolicy::kInstanceState;
    render_contract.scope.id = feature::ScopeId("Frame");
    render_contract.clock = feature::make_periodic_clock_contract(feature::ClockId("RenderFrame"), 60, 2,
        feature::LateDataPolicy::kUseLatest, feature::DeadlinePolicy::kObserve, 250000, 0);
    render_contract.permissions.version = 1;
    render_contract.ports = {{feature::PortId("SceneColor"), feature::PortDirection::kInput,
                              feature::ValueSchemaId("Texture.v1"), 1}};
    const auto add_feature = metadata.feature_catalog.export_definition(render_definition);
    if (!add_feature)
        return 2;
    auto summary_metadata = metadata;
    auto summary_entry = summary_metadata.feature_catalog.entries().front();
    summary_entry.contract = {};
    summary_metadata.feature_catalog = {};
    if (!summary_metadata.feature_catalog.add_entry(std::move(summary_entry)) ||
        module::encode_module_metadata(summary_metadata)) return 54;

    // The metadata boundary must preserve the complete Feature contract.  A
    // summary-only catalog is insufficient for editor authoring and runtime
    // validation because structure, scope, clock policy, permissions,
    // side-effects, failure/retry policy, and composite children are all
    // semantic parts of the contract.
    feature::FeatureDefinition authored{};
    authored.id = feature::FeatureDefinitionId("Game.Logic.Composite");
    authored.version = {2, 3, 4};
    authored.owner_module = metadata.module.id;
    authored.contract.definition_id = authored.id;
    authored.contract.version = authored.version;
    authored.contract.structure = feature::FeatureStructure::kComposite; authored.contract.clock.work_budget_ns = 0;
    authored.contract.domains = feature::to_mask(feature::FeatureDomain::kLogic);
    authored.contract.execution = feature::ExecutionKind::kCall;
    authored.contract.state = feature::StatePolicy::kStateless;
    authored.contract.scope.id = feature::ScopeId("World");
    authored.contract.clock = aeg::feature::make_periodic_clock_contract(feature::ClockId("Simulation"), 60, 8, feature::LateDataPolicy::kQueue,
        aeg::feature::DeadlinePolicy::kObserve, authored.contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    authored.contract.permissions = {0x55u, 7u};
    authored.contract.parameters.push_back({feature::ParameterId("Config"), 5u, 16u, true});
    authored.contract.side_effects.push_back(
        {feature::SideEffectId("Damage"), 3u, feature::SideEffectKind::kEvent,
         feature::CommitPolicy::kOutbox});
    authored.contract.failure = feature::FailurePolicy::kRetry;
    authored.contract.retry = {4u, 2u};
    authored.contract.children.push_back(feature::FeatureDefinitionId("Game.Logic.Child"));
    const auto export_feature = metadata.feature_catalog.export_definition(authored);
    if (!export_feature)
        return 2;
    const auto before_catalog_digest = metadata.feature_catalog.contract_digest();
    if (before_catalog_digest == 0u)
        return 2;
    auto stale_contract = metadata.feature_catalog.entries().back();
    stale_contract.contract_digest ^= 1u;
    if (feature::FeatureCatalog{}.add_entry(std::move(stale_contract))) {
        std::cerr << "Feature catalog accepted a stale contract digest\n";
        return 31;
    }

    const auto contract = module::module_contract_sha256(metadata);
    if (!contract || contract.value.size() != 64u)
        return 3;
    metadata.contract_digest = contract.value;
    const auto encoded = module::encode_module_metadata(metadata);
    if (!encoded || encoded.value.find("18446744073709551614") == std::string::npos ||
        encoded.value.find("features") == std::string::npos)
        return 4;
    const auto decoded = module::decode_module_metadata(encoded.value);
    if (!decoded || decoded.value.module.name != metadata.module.name ||
        decoded.value.feature_catalog.entries().size() != 2u ||
        decoded.value.feature_catalog.entries().front().ports.size() != 1u ||
        encoded.value.find("\"catalog\"") != std::string::npos) {
        std::cerr << "Feature metadata basic roundtrip failed\n";
        return 5;
    }
    const auto& decoded_feature = decoded.value.feature_catalog.entries().back();
    if (decoded_feature.contract_digest != feature::feature_contract_digest(authored) ||
        decoded_feature.owner_module != metadata.module.id ||
        decoded_feature.contract.definition_id != authored.id ||
        decoded_feature.contract.version != authored.version ||
        decoded_feature.contract.structure != feature::FeatureStructure::kComposite ||
        decoded_feature.contract.scope.id != authored.contract.scope.id ||
        decoded_feature.contract.clock.frequency_hz != 60u ||
        decoded_feature.contract.clock.max_backlog != 8u ||
        decoded_feature.contract.clock.late_data_policy != feature::LateDataPolicy::kQueue ||
        decoded_feature.contract.permissions.capability_mask != 0x55u ||
        decoded_feature.contract.permissions.version != 7u ||
        decoded_feature.contract.parameters.size() != 1u ||
        decoded_feature.contract.parameters.front().id != feature::ParameterId("Config") ||
        decoded_feature.contract.parameters.front().version != 5u ||
        decoded_feature.contract.parameters.front().max_size != 16u ||
        !decoded_feature.contract.parameters.front().required ||
        decoded_feature.contract.side_effects.size() != 1u ||
        decoded_feature.contract.failure != feature::FailurePolicy::kRetry ||
        decoded_feature.contract.retry.max_attempts != 4u ||
        decoded_feature.contract.children.size() != 1u ||
        decoded_feature.contract.has_continuation ||
        decoded_feature.contract.allows_child_process ||
        decoded.value.feature_catalog.contract_digest() != before_catalog_digest) {
        std::cerr << "feature roundtrip mismatch: decoded=" << static_cast<bool>(decoded)
                  << " entries=" << (decoded ? decoded.value.feature_catalog.entries().size() : 0u)
                  << " digest=" << (decoded ? decoded_feature.contract_digest : 0u)
                  << " expected=" << feature::feature_contract_digest(authored)
                  << " aggregate=" << (decoded ? decoded.value.feature_catalog.contract_digest() : 0u)
                  << " before=" << before_catalog_digest << '\n';
        return 5;
    }
    const auto content_hash = module::module_content_sha256("abc");
    if (!content_hash || content_hash.value != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
        return 6;

    const auto metadata_path = root / "module.metadata.json";
    const auto binary_path = root / "game.dll";
    const auto manifest_path = root / "module.manifest.json";
    if (!write_text(metadata_path, encoded.value) || !write_text(binary_path, "binary-v1"))
        return 7;
    const auto metadata_digest = module::module_content_sha256(encoded.value);
    const auto binary_digest = module::module_content_sha256("binary-v1");
    if (!metadata_digest || !binary_digest ||
        !write_text(manifest_path, manifest_json(metadata, metadata_digest.value, binary_digest.value)))
        return 8;

    const auto valid_bundle = module::validate_module_bundle(manifest_path);
    if (!valid_bundle || valid_bundle.value.binary_path != binary_path ||
        valid_bundle.value.metadata_sha256 != metadata_digest.value)
        return 9;

    if (!write_text(binary_path, "binary-tampered") || module::validate_module_bundle(manifest_path))
        return 10;
    if (!write_text(binary_path, "binary-v1") || !write_text(metadata_path, encoded.value + " ") ||
        module::validate_module_bundle(manifest_path))
        return 11;
    if (!write_text(metadata_path, encoded.value) ||
        !write_text(manifest_path,
                    manifest_json(metadata, metadata_digest.value, binary_digest.value, "../module.metadata.json")) ||
        module::validate_module_bundle(manifest_path))
        return 12;
    if (!write_text(manifest_path, manifest_json(metadata, metadata_digest.value, binary_digest.value,
                                                 "module.metadata.json", "game.dll", "wrong-build")) ||
        module::validate_module_bundle(manifest_path))
        return 13;
    if (!write_text(manifest_path, manifest_json(metadata, metadata_digest.value, binary_digest.value,
                                                 "module.metadata.json", "game.dll", "build-1", "wrong-target")) ||
        module::validate_module_bundle(manifest_path))
        return 14;
    if (!write_text(manifest_path,
                    manifest_json(metadata, metadata_digest.value, binary_digest.value, "module.metadata.json",
                                  "game.dll", "build-1", "windows-x64-vulkan", "Debug", "deadbeef")) ||
        module::validate_module_bundle(manifest_path))
        return 15;

    std::filesystem::remove_all(root, error);
    std::cout << "module metadata smoke passed\n";
    return 0;
}
