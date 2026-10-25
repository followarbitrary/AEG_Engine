#include "dynamic_module.hpp"
#include "feature_clock_admission.hpp"
#include "feature_runtime.hpp"
#include "module_bundle.hpp"
#include "module.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
void succeeds(const aeg::foundation::Result& result, const char* operation) {
    require(bool(result), std::string{operation} + ": " + result.message);
}
}

int main(int argc, char** argv) {
    using namespace aeg;
    try {
        require(argc == 4, "usage: verifier verify|reject-contract manifest expected-revision");
        const bool reject_contract = std::string_view{argv[1]} == "reject-contract";
        require(reject_contract || std::string_view{argv[1]} == "verify", "unknown verifier mode");
        const auto revision = static_cast<uint16_t>(std::stoul(argv[3]));
        const auto bundle = module::validate_module_bundle(argv[2]);
        succeeds(bundle.result, "verify the actual bundle's metadata/DLL/manifest hashes");
        const auto clock = feature::detail::make_clock_host_context(feature::detail::ClockHostLimits{});
        succeeds(clock.result, "create bounded verifier Clock host");
        feature::FeatureRegistry registry{clock.value};
        module::ModuleRegistry modules{registry};
        module::DynamicModuleLoader loader{modules};
        module::DynamicModuleHandle handle;
        succeeds(loader.load(bundle.value, handle), "load the actual factory-produced DLL");
        if (reject_contract) {
            {
                const auto imported = loader.make_feature_module(handle);
                require(!imported && imported.result.code == foundation::ResultCode::kInvalidArgument &&
                            imported.result.message == "DLL Feature contracts differ from the verified sidecar",
                        "internally valid stale metadata was not rejected against the actual new DLL contract");
            }
            succeeds(loader.unload(handle), "release the rejected DLL import without leaking a lease");
            std::cout << "CONTRACT_EXPORTER_MISMATCH_REJECTED\n";
            return 0;
        }
        const auto& metadata = bundle.value.metadata;
        require(metadata.schema_version == module::kModuleMetadataSchemaVersion &&
                    metadata.sdk_contract_major == module::kModuleSdkContractMajor && metadata.abi_major == 2 &&
                    metadata.build_id == "contract-exporter-fixture" && metadata.target == "windows-x64-vulkan" &&
                    metadata.configuration == "Debug" && metadata.feature_catalog.entries().size() == 1,
                "exported metadata has the wrong identity, wire versions or catalog count");
        const auto& entry = metadata.feature_catalog.entries().front();
        const feature::FeatureDefinitionId feature_id{"Exporter.Fixture.Feature"};
        feature::FeatureDefinition metadata_definition;
        metadata_definition.id = entry.definition_id;
        metadata_definition.version = entry.version;
        metadata_definition.owner_module = entry.owner_module;
        metadata_definition.contract = entry.contract;
        require(entry.definition_id == feature_id && entry.version == feature::Version{revision, 0, 0} &&
                    entry.contract.ports.size() == 1 &&
                    entry.contract.ports.front().schema == feature::ValueSchemaId{
                        std::string{"Exporter.Byte."} + std::to_string(revision)} &&
                    entry.contract_digest == feature::feature_contract_digest(metadata_definition),
                "C++-only factory change did not reach the actual exported contract");
        auto imported = loader.make_feature_module(handle);
        succeeds(imported.result, "compare the actual DLL factory contract with canonical metadata");
        require(imported.value.feature_definitions.size() == 1 &&
                    feature::feature_contract_digest(imported.value.feature_definitions.front()) == entry.contract_digest,
                "actual DLL and exported source metadata contract differ");
        succeeds(module::register_static_module(modules, std::move(imported.value)), "admit the actual DLL definition");
        imported = {};
        {
            feature::FeatureRuntime runtime{registry};
            const auto expanded = registry.expand(feature_id, {});
            succeeds(expanded.result, "expand the imported Feature");
            const auto owner = runtime.activate(expanded.value, feature::ScopeId{"World"});
            succeeds(owner.result, "activate the imported Feature");
            succeeds(runtime.dispatch_call(owner.value), "execute and commit the actual DLL callback");
            const auto* instance = runtime.find_instance(owner.value);
            require(instance && instance->state.payload == std::vector<uint8_t>{static_cast<uint8_t>(revision)} &&
                        instance->state.commit_count == 1,
                    "the actual DLL did not commit the expected factory revision exactly once");
            succeeds(runtime.shutdown_execution(), "retire actual Runtime work");
        }
        succeeds(modules.unregister_module(metadata.module.id), "unregister the imported DLL definitions");
        succeeds(loader.unload(handle), "release the actual DLL and its callback code leases");
        std::cout << "CONTRACT_EXPORTER_BUNDLE_COMPLETE revision=" << revision << " callback-commits=1\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
