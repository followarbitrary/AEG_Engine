/**
 * @file feature_clock_contract_smoke.cpp
 * @brief Explicit Clock contract admission regression coverage.
 */
#include <exception>
#include <iostream>
#include <limits>
#include <string>

#include <nlohmann/json.hpp>

#include <feature_contract.hpp>
#include <feature_contract_json.hpp>
#include <feature_clock_host.hpp>
#include <feature_graph.hpp>

namespace {
using namespace aeg::feature;

FeatureContract atomic_call_with_legacy_clock() {
    FeatureContract contract{};
    contract.definition_id = FeatureDefinitionId("Test.ClockContract.AtomicCall");
    contract.version = {1, 0, 0};
    contract.structure = FeatureStructure::kAtomic;
    contract.domains = to_mask(FeatureDomain::kLogic);
    contract.execution = ExecutionKind::kCall;
    contract.state = StatePolicy::kStateless;
    contract.scope = ScopeContract{ScopeId("World")};
    // The current API can express only these four Clock fields. Missing explicit
    // timing, capability version and work budget must not gain implicit approval.
    contract.clock = ClockContract{ClockId("LogicTick"), 60, 2, LateDataPolicy::kReject};
    contract.permissions.version = 1;
    contract.ports.push_back(PortContract{PortId("Input"), PortDirection::kInput,
                                        ValueSchemaId("Int.v1"), 1});
    contract.side_effects.push_back(SideEffectContract{SideEffectId("StateDelta"), 1,
                                                      SideEffectKind::kStateDelta,
                                                      CommitPolicy::kOwnerBoundary});
    return contract;
}

bool expect_invalid_argument(const FeatureContract& contract, const char* case_name) {
    const auto result = validate_feature_contract(contract);
    if (result) {
        std::cerr << case_name << ": validation accepted the contract\n";
        return false;
    }
    if (result.code != aeg::foundation::ResultCode::kInvalidArgument) {
        std::cerr << case_name << ": expected InvalidArgument, got result code "
                  << static_cast<int>(result.code) << ": " << result.message << '\n';
        return false;
    }
    return true;
}

bool require(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

FeatureContract explicit_atomic_call() {
    auto contract = atomic_call_with_legacy_clock();
    contract.clock.capability_version = {1, 0, 0};
    contract.clock.timing = ClockTiming{0, 20'000'000, DeadlinePolicy::kRejectCommit};
    contract.clock.work_budget_ns = 250'000;
    return contract;
}

uint64_t digest(const FeatureContract& contract) {
    FeatureDefinition definition{};
    definition.id = contract.definition_id;
    definition.version = contract.version;
    definition.contract = contract;
    return feature_contract_digest(definition);
}

bool test_explicit_clock_contract_and_profile() {
    bool passed = require(static_cast<bool>(validate_feature_contract(explicit_atomic_call())),
                          "complete explicit Atomic Call Clock was rejected");
    auto observed = explicit_atomic_call();
    observed.clock.timing->deadline_policy = DeadlinePolicy::kObserve;
    passed &= require(static_cast<bool>(validate_feature_contract(observed)),
                      "explicit Observe policy was rejected");

    const auto clock = make_periodic_clock_contract(ClockId("Profile60"), 60, 2,
        LateDataPolicy::kQueue, DeadlinePolicy::kObserve, 250'000, 0);
    passed &= require(clock.capability_version == Version{1, 0, 0} && clock.timing &&
                      clock.timing->phase_ns == 0 &&
                      clock.timing->relative_deadline_ns == 16'666'667 &&
                      clock.timing->deadline_policy == DeadlinePolicy::kObserve &&
                      clock.work_budget_ns == 250'000 && clock.frequency_hz == 60 &&
                      clock.max_backlog == 2 && clock.late_data_policy == LateDataPolicy::kQueue,
                      "periodic profile did not declare the exact ceil-period Clock contract");
    return passed;
}

bool test_clock_contract_missing_and_invalid_fields() {
    const struct {
        const char* name;
        void (*mutate)(ClockContract&);
    } cases[]{
        {"missing Clock capability version", [](ClockContract& c) { c.capability_version = {}; }},
        {"unsupported Clock capability version", [](ClockContract& c) { c.capability_version = {2, 0, 0}; }},
        {"missing Clock timing", [](ClockContract& c) { c.timing.reset(); }},
        {"missing Clock deadline policy", [](ClockContract& c) { c.timing->deadline_policy = DeadlinePolicy::kUnspecified; }},
        {"unknown Clock deadline policy", [](ClockContract& c) { c.timing->deadline_policy = static_cast<DeadlinePolicy>(255); }},
        {"zero Clock relative deadline", [](ClockContract& c) { c.timing->relative_deadline_ns = 0; }},
        {"zero Atomic work budget", [](ClockContract& c) { c.work_budget_ns = 0; }},
        {"budget beyond relative deadline", [](ClockContract& c) { c.work_budget_ns = c.timing->relative_deadline_ns + 1; }},
        {"phase at exact 1000 Hz period", [](ClockContract& c) { c.frequency_hz = 1000; c.timing->phase_ns = 1'000'000; }},
        {"phase beyond fractional 60 Hz period", [](ClockContract& c) { c.timing->phase_ns = 16'666'667; }},
        {"overflow-sized phase", [](ClockContract& c) { c.timing->phase_ns = (std::numeric_limits<uint64_t>::max)(); }},
    };
    bool passed = true;
    for (const auto& test : cases) {
        auto contract = explicit_atomic_call();
        test.mutate(contract.clock);
        passed &= expect_invalid_argument(contract, test.name);
    }
    auto largest_integral_phase = explicit_atomic_call();
    largest_integral_phase.clock.timing->phase_ns = 16'666'666;
    passed &= require(static_cast<bool>(validate_feature_contract(largest_integral_phase)),
                      "phase strictly below a fractional period was rejected");
    auto exact_budget = explicit_atomic_call();
    exact_budget.clock.work_budget_ns = exact_budget.clock.timing->relative_deadline_ns;
    passed &= require(static_cast<bool>(validate_feature_contract(exact_budget)),
                      "budget equal to its relative deadline was rejected");

    auto composite = explicit_atomic_call();
    composite.structure = FeatureStructure::kComposite;
    composite.children = {FeatureDefinitionId("Test.ClockContract.Child")};
    composite.clock.work_budget_ns = 0;
    composite.ports.clear();
    composite.side_effects.clear();
    passed &= require(static_cast<bool>(validate_feature_contract(composite)),
                      "structural Composite with zero callback budget was rejected");
    composite.clock.work_budget_ns = 1;
    passed &= expect_invalid_argument(composite, "Composite callback budget must be zero");
    return passed;
}

bool test_every_new_clock_field_changes_digest() {
    const auto original = explicit_atomic_call();
    const auto original_digest = digest(original);
    const struct {
        const char* name;
        void (*mutate)(ClockContract&);
    } cases[]{
        {"capability major omitted from digest", [](ClockContract& c) { ++c.capability_version.major; }},
        {"capability minor omitted from digest", [](ClockContract& c) { ++c.capability_version.minor; }},
        {"capability patch omitted from digest", [](ClockContract& c) { ++c.capability_version.patch; }},
        {"timing presence omitted from digest", [](ClockContract& c) { c.timing.reset(); }},
        {"phase omitted from digest", [](ClockContract& c) { ++c.timing->phase_ns; }},
        {"relative deadline omitted from digest", [](ClockContract& c) { ++c.timing->relative_deadline_ns; }},
        {"deadline policy omitted from digest", [](ClockContract& c) { c.timing->deadline_policy = DeadlinePolicy::kObserve; }},
        {"work budget omitted from digest", [](ClockContract& c) { ++c.work_budget_ns; }},
    };
    bool passed = true;
    for (const auto& test : cases) {
        auto changed = original;
        test.mutate(changed.clock);
        passed &= require(digest(changed) != original_digest, test.name);
    }
    return passed;
}

bool test_native_unknown_structure_and_execution_are_rejected() {
    auto invalid_structure = explicit_atomic_call();
    invalid_structure.structure = static_cast<FeatureStructure>(255);
    invalid_structure.execution = ExecutionKind::kProcess;
    invalid_structure.state = StatePolicy::kInstanceState;
    invalid_structure.clock.work_budget_ns = 0;
    bool passed = expect_invalid_argument(invalid_structure, "native unknown Structure with zero Process budget");
    auto invalid_execution = explicit_atomic_call();
    invalid_execution.execution = static_cast<ExecutionKind>(255);
    passed &= expect_invalid_argument(invalid_execution, "native unknown Execution");
    auto invalid_state = explicit_atomic_call();
    invalid_state.execution = ExecutionKind::kProcess;
    invalid_state.state = static_cast<StatePolicy>(255);
    invalid_state.has_continuation = false;
    passed &= expect_invalid_argument(invalid_state, "native unknown StatePolicy bypassing Process continuation");
    for (const uint64_t budget : {0ull, 250'000ull}) {
        auto clock = explicit_atomic_call().clock;
        clock.work_budget_ns = budget;
        const auto result = validate_clock_contract(clock, static_cast<FeatureStructure>(255));
        passed &= require(!result && result.code == aeg::foundation::ResultCode::kInvalidArgument,
                          "standalone Clock validator accepted an unknown Structure");
    }
    return passed;
}

bool expect_invalid_json(const nlohmann::json& json, const char* case_name) {
    const auto decoded = decode_feature_contract_json(json.dump());
    if (decoded || decoded.result.code != aeg::foundation::ResultCode::kInvalidArgument) {
        std::cerr << case_name << ": malformed Clock JSON was not rejected as InvalidArgument\n";
        return false;
    }
    return true;
}

bool test_explicit_clock_json_roundtrip_and_required_keys() {
    const auto original = explicit_atomic_call();
    const auto encoded = encode_feature_contract_json(original);
    if (!require(static_cast<bool>(encoded), "explicit Clock JSON encoding failed")) return false;
    const auto json = nlohmann::json::parse(encoded.value);
    const auto& clock = json.at("clock");
    if (!require(clock.contains("capability_version") && clock.contains("timing") &&
                 clock.contains("work_budget_ns") && clock.at("timing").is_object() &&
                 clock.at("timing").contains("phase_ns") &&
                 clock.at("timing").contains("relative_deadline_ns") &&
                 clock.at("timing").contains("deadline_policy"),
                 "canonical JSON omitted a required explicit Clock field")) return false;
    bool passed = true;
    const auto decoded = decode_feature_contract_json(encoded.value);
    if (require(static_cast<bool>(decoded), "complete explicit Clock JSON failed to decode")) {
        passed &= require(static_cast<bool>(validate_feature_contract(decoded.value)) &&
                          digest(decoded.value) == digest(original),
                          "Clock JSON roundtrip changed the contract or lost its digest");
    } else {
        passed = false;
    }
    for (const auto* key : {"capability_version", "timing", "work_budget_ns"}) {
        auto missing = json;
        missing["clock"].erase(key);
        passed &= expect_invalid_json(missing, key);
    }
    for (const auto* key : {"phase_ns", "relative_deadline_ns", "deadline_policy"}) {
        auto missing = json;
        missing["clock"]["timing"].erase(key);
        passed &= expect_invalid_json(missing, key);
    }
    for (const auto* key : {"major", "minor", "patch"}) {
        auto missing = json;
        missing["clock"]["capability_version"].erase(key);
        passed &= expect_invalid_json(missing, key);
    }
    return passed;
}

bool test_clock_json_rejects_invalid_numbers_and_enums() {
    const auto encoded = encode_feature_contract_json(explicit_atomic_call());
    if (!require(static_cast<bool>(encoded), "Clock JSON malformed-value fixture encoding failed")) return false;
    const auto original = nlohmann::json::parse(encoded.value);
    bool passed = true;
    for (const auto* key : {"phase_ns", "relative_deadline_ns"}) {
        for (const auto& invalid : {nlohmann::json(-1), nlohmann::json(0.5),
                                   nlohmann::json(true), nlohmann::json("18446744073709551616")}) {
            auto changed = original;
            changed["clock"]["timing"][key] = invalid;
            passed &= expect_invalid_json(changed, key);
        }
    }
    for (const auto& invalid : {nlohmann::json(-1), nlohmann::json(0.5),
                               nlohmann::json(true), nlohmann::json("18446744073709551616")}) {
        auto changed = original;
        changed["clock"]["work_budget_ns"] = invalid;
        passed &= expect_invalid_json(changed, "invalid work_budget_ns");
    }
    for (const auto invalid_policy : {0, 255, -1}) {
        auto changed = original;
        changed["clock"]["timing"]["deadline_policy"] = invalid_policy;
        passed &= expect_invalid_json(changed, "invalid deadline_policy");
    }
    auto version_overflow = original;
    version_overflow["clock"]["capability_version"]["major"] = 65'536;
    passed &= expect_invalid_json(version_overflow, "capability version overflow");
    auto zero_deadline = original;
    zero_deadline["clock"]["timing"]["relative_deadline_ns"] = 0;
    passed &= expect_invalid_json(zero_deadline, "zero JSON deadline");
    auto zero_budget = original;
    zero_budget["clock"]["work_budget_ns"] = 0;
    passed &= expect_invalid_json(zero_budget, "zero Atomic JSON budget");
    auto null_timing = original;
    null_timing["clock"]["timing"] = nullptr;
    passed &= expect_invalid_json(null_timing, "null Clock timing");
    auto zero_version = original;
    zero_version["clock"]["capability_version"]["major"] = 0;
    passed &= expect_invalid_json(zero_version, "zero capability version");

    // A literal beyond uint64_t may parse as a floating JSON number. It must
    // still fail checked conversion instead of wrapping or saturating.
    auto overflow_text = original.dump();
    const auto budget_key = overflow_text.find("\"work_budget_ns\":");
    if (!require(budget_key != std::string::npos, "canonical JSON budget key was not found")) return false;
    const auto number_start = budget_key + std::string("\"work_budget_ns\":").size();
    const auto number_end = overflow_text.find_first_of(",}", number_start);
    overflow_text.replace(number_start, number_end - number_start, "18446744073709551616");
    const auto overflow = decode_feature_contract_json(overflow_text);
    passed &= require(!overflow && overflow.result.code == aeg::foundation::ResultCode::kInvalidArgument,
                      "JSON numeric uint64 overflow was accepted");
    return passed;
}

bool test_missing_explicit_clock_contract_is_rejected() {
    return expect_invalid_argument(atomic_call_with_legacy_clock(),
                                   "missing explicit Clock timing/version/budget");
}

bool test_invalid_definition_identity_is_rejected() {
    auto contract = explicit_atomic_call();
    contract.definition_id = {};
    return expect_invalid_argument(contract, "invalid definition identity control");
}

FeatureDefinition public_host_definition() {
    FeatureDefinition definition{};
    definition.id = FeatureDefinitionId("Test.PublicClockHost.Feature");
    definition.version = {1, 0, 0};
    definition.owner_module = ModuleIdentity("Test.PublicClockHost.Module");
    definition.contract = explicit_atomic_call();
    definition.contract.definition_id = definition.id;
    return definition;
}

bool test_public_clock_host_does_not_approve_definitions() {
    const auto host = make_clock_host();
    if (!require(static_cast<bool>(host) && static_cast<bool>(host.value),
                 "public bounded Clock host creation failed")) return false;
    FeatureRegistry registry(host.value);
    const auto original_digest = registry.contract_digest();
    const auto definition = public_host_definition();
    const auto registered = registry.register_definition(definition);
    bool passed = require(!registered && registered.code == aeg::foundation::ResultCode::kUnsupported &&
                          !registry.find_definition(definition.id) && registry.contract_digest() == original_digest,
                          "public Clock host silently approved a definition");
    FeatureRegistry unhosted(ClockHost{});
    passed &= require(!unhosted.register_definition(definition),
                      "empty public Clock host approved a definition");
    return passed;
}

bool test_catalog_registry_compiles_without_execution_authority() {
    const auto definition = public_host_definition();
    FeatureCatalog catalog;
    if (!require(static_cast<bool>(catalog.export_definition(definition)),
                 "public catalog fixture export failed")) return false;
    auto snapshot = FeatureRegistry::from_catalog(catalog);
    if (!require(static_cast<bool>(snapshot), "public catalog registry creation failed")) return false;
    const auto* imported = snapshot.value.find_definition(definition.id);
    bool passed = require(imported && !imported->execute && imported->owner_module == definition.owner_module &&
                          snapshot.value.contract_digest() == catalog.contract_digest(),
                          "catalog registry lost contract provenance/digest or gained executable code");
    FeatureGraphDocument graph{};
    graph.asset_uuid = "44444444-4444-4444-8444-444444444444";
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.contract_digest = catalog.contract_digest();
    graph.nodes.push_back({FeatureNodeId(1), definition.id, definition.version});
    const auto compiled = FeatureGraphCompiler{}.compile(graph, snapshot.value);
    passed &= require(compiled && compiled.value.provenance.size() == 1 &&
                      compiled.value.provenance.front().owner_module == definition.owner_module,
                      "catalog registry could not compile its actual FeatureGraph");
    auto executable = definition;
    executable.execute = [](const InvocationToken&, const StateSnapshot&, std::span<const uint8_t>,
                            std::span<const uint8_t>) {
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), {}};
    };
    passed &= require(!snapshot.value.replace_definition(executable),
                      "catalog registry allowed metadata to acquire executable code");
    executable.id = executable.contract.definition_id = FeatureDefinitionId("Test.PublicClockHost.Peer");
    passed &= require(!snapshot.value.register_definition(executable),
                      "catalog registry granted registration authority for its admitted Clock");
    const auto expanded = snapshot.value.expand(definition.id, {});
    if (!require(static_cast<bool>(expanded), "catalog registry expansion failed")) return false;
    FeatureRuntime runtime(snapshot.value);
    passed &= require(!runtime.activate(expanded.value, definition.contract.scope.id),
                      "catalog registry granted Runtime execution authority");
    passed &= require(snapshot.value.contract_digest() == catalog.contract_digest(),
                      "failed catalog registry mutations changed its accepted contracts");
    return passed;
}

bool test_catalog_registry_rejects_unapproved_and_incomplete_contracts() {
    auto oversized = public_host_definition();
    oversized.contract.clock.frequency_hz = 192001;
    FeatureCatalog catalog;
    if (!require(static_cast<bool>(catalog.export_definition(oversized)),
                 "over-limit catalog fixture is not a valid declarative contract")) return false;
    const auto rejected = FeatureRegistry::from_catalog(catalog);
    bool passed = require(!rejected && rejected.result.code == aeg::foundation::ResultCode::kInvalidArgument &&
                          !rejected.value.find_definition(oversized.id),
                          "catalog registry accepted a Clock beyond the fixed host limits");
    auto entry = catalog.entries().front();
    entry.contract = {};
    FeatureCatalog incomplete;
    if (!require(static_cast<bool>(incomplete.add_entry(std::move(entry))),
                 "summary-only catalog fixture could not be created")) return false;
    const auto missing = FeatureRegistry::from_catalog(incomplete);
    passed &= require(!missing && missing.result.code == aeg::foundation::ResultCode::kInvalidArgument &&
                      !missing.value.find_definition(oversized.id),
                      "catalog registry accepted summary-only metadata as a complete contract");
    return passed;
}
}  // namespace

int main() {
    const struct {
        const char* name;
        bool (*run)();
    } cases[]{
        {"missing explicit Clock", test_missing_explicit_clock_contract_is_rejected},
        {"invalid identity", test_invalid_definition_identity_is_rejected},
        {"explicit Clock/profile", test_explicit_clock_contract_and_profile},
        {"invalid Clock fields", test_clock_contract_missing_and_invalid_fields},
        {"Clock digest", test_every_new_clock_field_changes_digest},
        {"native unknown enum", test_native_unknown_structure_and_execution_are_rejected},
        {"Clock JSON keys/roundtrip", test_explicit_clock_json_roundtrip_and_required_keys},
        {"Clock JSON malformed values", test_clock_json_rejects_invalid_numbers_and_enums},
        {"public Clock host approval boundary", test_public_clock_host_does_not_approve_definitions},
        {"catalog registry compilation/authority", test_catalog_registry_compiles_without_execution_authority},
        {"catalog registry contract rejection", test_catalog_registry_rejects_unapproved_and_incomplete_contracts},
    };
    int failures = 0;
    for (const auto& test : cases) {
        try {
            failures += !test.run();
        } catch (const std::exception& exception) {
            std::cerr << test.name << ": unexpected exception: " << exception.what() << '\n';
            ++failures;
        } catch (...) {
            std::cerr << test.name << ": unexpected non-standard exception\n";
            ++failures;
        }
    }
    if (failures != 0) {
        std::cerr << "Feature Clock contract smoke failed: " << failures << " case(s)\n";
        return 1;
    }
    return 0;
}
