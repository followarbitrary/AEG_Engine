/**
 * @file feature_contract_smoke.cpp
 * @brief Contract-level acceptance tests for the Feature five-domain model.
 */
#include <cassert>
#include <cmath>
#include <iostream>
#include <new>
#include <stdexcept>
#include <type_traits>

#include <feature_contract.hpp>
#include <feature_catalog.hpp>
#include <feature_parameters.hpp>
#include <feature_contract_metadata.hpp>
#include <feature_contract_json.hpp>
#include <limits>

namespace {
using namespace aeg::feature;

FeatureContract valid_atomic_contract(FeatureDomain domain,
                                      ExecutionKind execution = ExecutionKind::kCall,
                                      StatePolicy state = StatePolicy::kStateless) {
    FeatureContract contract{};
    contract.definition_id = FeatureDefinitionId("Test.Atomic");
    contract.version = {1, 0, 0};
    contract.structure = FeatureStructure::kAtomic;
    contract.domains = to_mask(domain);
    contract.execution = execution;
    contract.state = state;
    contract.scope = ScopeContract{ScopeId("World")};
    contract.clock = aeg::feature::make_periodic_clock_contract(ClockId("LogicTick"), 60, 2, LateDataPolicy::kReject,
        aeg::feature::DeadlinePolicy::kObserve, contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    contract.permissions.version = 1;
    contract.ports.push_back(PortContract{PortId("Input"), PortDirection::kInput,
                                          ValueSchemaId("Int.v1"), 1});
    contract.side_effects.push_back(SideEffectContract{SideEffectId("StateDelta"), 1,
                                                        SideEffectKind::kStateDelta,
                                                        CommitPolicy::kOwnerBoundary});
    return contract;
}

void test_state_payload_contract_validation_and_digest() {
    auto contract = valid_atomic_contract(FeatureDomain::kLogic, ExecutionKind::kCall, StatePolicy::kInstanceState);
    assert(validate_feature_contract(contract));
    auto invalid = contract;
    invalid.state_payload.max_size = 0;
    assert(!validate_feature_contract(invalid));
    invalid = contract;
    invalid.state_payload.schema_version = {};
    assert(!validate_feature_contract(invalid));
    invalid = contract;
    invalid.state = StatePolicy::kExternalSnapshot;
    invalid.state_payload.max_size = 1;
    assert(!validate_feature_contract(invalid));
    FeatureDefinition definition{};
    definition.id = contract.definition_id;
    definition.version = contract.version;
    definition.contract = contract;
    const auto digest = feature_contract_digest(definition);
    ++definition.contract.state_payload.max_size;
    assert(feature_contract_digest(definition) != digest);
    definition.contract = contract;
    definition.contract.state_payload.schema_version.minor = 1;
    assert(feature_contract_digest(definition) != digest);
}

void test_all_five_domains_are_leaf_domains() {
    static_assert(std::is_enum_v<FeatureDomain>);
    assert(to_mask(FeatureDomain::kLogic) != 0);
    assert(to_mask(FeatureDomain::kPhysics) != 0);
    assert(to_mask(FeatureDomain::kRender) != 0);
    assert(to_mask(FeatureDomain::kAudio) != 0);
    assert(to_mask(FeatureDomain::kStream) != 0);
    assert(kAllFeatureDomains == (to_mask(FeatureDomain::kLogic) |
                                  to_mask(FeatureDomain::kPhysics) |
                                  to_mask(FeatureDomain::kRender) |
                                  to_mask(FeatureDomain::kAudio) |
                                  to_mask(FeatureDomain::kStream)));
}

void test_valid_atomic_contracts() {
    for (const auto domain : {FeatureDomain::kLogic, FeatureDomain::kPhysics,
                              FeatureDomain::kRender, FeatureDomain::kAudio,
                              FeatureDomain::kStream}) {
        const auto result = validate_feature_contract(valid_atomic_contract(domain));
        assert(result);
    }
}

void test_invalid_execution_and_state_combinations() {
    auto call_process_state = valid_atomic_contract(FeatureDomain::kLogic,
                                                    ExecutionKind::kCall,
                                                    StatePolicy::kProcessState);
    assert(!validate_feature_contract(call_process_state));

    auto continuous_process_state = valid_atomic_contract(FeatureDomain::kLogic,
                                                          ExecutionKind::kContinuous,
                                                          StatePolicy::kProcessState);
    continuous_process_state.allows_child_process = false;
    assert(!validate_feature_contract(continuous_process_state));

    auto process_without_continuation = valid_atomic_contract(FeatureDomain::kLogic,
                                                              ExecutionKind::kProcess,
                                                              StatePolicy::kStateless);
    process_without_continuation.has_continuation = false;
    assert(!validate_feature_contract(process_without_continuation));
}

bool test_atomic_call_rejects_continuations() {
    const struct {
        StatePolicy state;
        const char* name;
    } cases[]{
        {StatePolicy::kStateless, "Stateless"},
        {StatePolicy::kInstanceState, "InstanceState"},
        {StatePolicy::kExternalSnapshot, "ExternalSnapshot"},
    };
    bool rejected_all = true;
    for (const auto& test : cases) {
        auto contract = valid_atomic_contract(FeatureDomain::kLogic, ExecutionKind::kCall, test.state);
        assert(validate_feature_contract(contract));
        contract.has_continuation = true;
        const auto rejected = validate_feature_contract(contract);
        if (rejected || rejected.code != aeg::foundation::ResultCode::kInvalidArgument) {
            std::cerr << "Atomic Call with " << test.name << " accepted a continuation\n";
            rejected_all = false;
        }
    }
    return rejected_all;
}

void test_clock_ports_and_side_effect_versions_are_required() {
    auto missing_clock = valid_atomic_contract(FeatureDomain::kLogic);
    missing_clock.clock.id = {};
    assert(!validate_feature_contract(missing_clock));

    auto missing_port_schema = valid_atomic_contract(FeatureDomain::kLogic);
    missing_port_schema.ports.front().schema = {};
    assert(!validate_feature_contract(missing_port_schema));

    auto missing_side_effect_version = valid_atomic_contract(FeatureDomain::kLogic);
    missing_side_effect_version.side_effects.front().version = 0;
    assert(!validate_feature_contract(missing_side_effect_version));

    auto duplicate_port = valid_atomic_contract(FeatureDomain::kLogic);
    duplicate_port.ports.push_back(duplicate_port.ports.front());
    assert(!validate_feature_contract(duplicate_port));

    auto duplicate_effect = valid_atomic_contract(FeatureDomain::kLogic);
    duplicate_effect.side_effects.push_back(duplicate_effect.side_effects.front());
    assert(!validate_feature_contract(duplicate_effect));
}

void test_composite_requires_leaf_domain_union() {
    auto composite = valid_atomic_contract(FeatureDomain::kLogic);
    composite.structure = FeatureStructure::kComposite; composite.clock.work_budget_ns = 0;
    composite.domains = to_mask(FeatureDomain::kLogic) | to_mask(FeatureDomain::kAudio);
    composite.children = {FeatureDefinitionId("Child.Logic"), FeatureDefinitionId("Child.Audio")};
    assert(validate_feature_contract(composite));

    composite.domains = 0;
    assert(!validate_feature_contract(composite));
}

void test_atomic_cannot_hide_children() {
    auto atomic = valid_atomic_contract(FeatureDomain::kLogic);
    atomic.children = {FeatureDefinitionId("Hidden.Child")};
    assert(!validate_feature_contract(atomic));
}

void test_composite_coordination_is_owned_by_a_process_child() {
    auto composite = valid_atomic_contract(FeatureDomain::kLogic);
    composite.structure = FeatureStructure::kComposite; composite.clock.work_budget_ns = 0;
    composite.children = {FeatureDefinitionId("Child.Logic")};
    composite.execution = ExecutionKind::kProcess;
    composite.state = StatePolicy::kProcessState;
    composite.has_continuation = true;
    assert(!validate_feature_contract(composite));
}

void test_contract_digest_is_shared_by_registry_and_catalog() {
    FeatureDefinition definition{};
    definition.id = FeatureDefinitionId("Test.Digest");
    definition.version = {1, 0, 0};
    definition.contract = valid_atomic_contract(FeatureDomain::kLogic);
    definition.contract.definition_id = definition.id;
    definition.contract.version = definition.version;

    FeatureCatalog catalog;
    assert(catalog.export_definition(definition));
    assert(catalog.entries().size() == 1);
    assert(catalog.entries().front().contract_digest == feature_contract_digest(definition));
}

void test_contract_digest_preserves_collection_boundaries() {
    FeatureDefinition children{};
    children.id = FeatureDefinitionId("Test.Digest.Children");
    children.version = {1, 0, 0};
    children.contract = valid_atomic_contract(FeatureDomain::kLogic);
    children.contract.definition_id = children.id;
    children.contract.version = children.version;
    children.contract.children = {FeatureDefinitionId("Child")};

    FeatureDefinition ports{};
    ports.id = children.id;
    ports.version = children.version;
    ports.contract = children.contract;
    ports.contract.children.clear();
    ports.contract.ports.push_back(PortContract{PortId("Child"), PortDirection::kOutput,
                                                ValueSchemaId("Schema"), 1});

    assert(feature_contract_digest(children) != feature_contract_digest(ports));
}

void test_parameter_envelope_codec_is_canonical_and_strict() {
    const std::vector<ParameterContract> schema{{ParameterId(7), 3, 2, true},
                                               {ParameterId(42), 1, 4, false}};
    const std::vector<FeatureParameterRecord> values{{ParameterId(42), 1, {9}},
                                                    {ParameterId(7), 3, {1, 2}}};
    const auto encoded = encode_feature_parameters(values, schema);
    assert(encoded);
    // little-endian count, followed by records in stable ParameterId order.
    assert(encoded.value.size() == 39 && encoded.value[0] == 2 && encoded.value[4] == 7);
    const auto decoded = decode_feature_parameters(encoded.value, schema);
    assert(decoded && decoded.value.size() == 2);
    assert(decoded.value[0] == values[1] && decoded.value[1] == values[0]);
    const std::vector<FeatureParameterRecord> reordered{values[1], values[0]};
    assert(encode_feature_parameters(reordered, schema).value == encoded.value);
    assert(validate_feature_parameter_blob(encoded.value, schema));

    auto corrupt = encoded.value;
    corrupt.push_back(0);
    assert(!decode_feature_parameters(corrupt, schema));
    corrupt = encoded.value;
    corrupt.pop_back();
    assert(!decode_feature_parameters(corrupt, schema));
    corrupt = {255, 255, 255, 255};
    assert(!decode_feature_parameters(corrupt, schema));
    assert(!encode_feature_parameters(std::vector<FeatureParameterRecord>{values[0]}, schema));
    assert(!encode_feature_parameters(std::vector<FeatureParameterRecord>{values[1], values[1]}, schema));
    auto invalid = values;
    invalid[1].version = 2;
    assert(!encode_feature_parameters(invalid, schema));
    invalid = values;
    invalid[1].payload = {1, 2, 3};
    assert(!encode_feature_parameters(invalid, schema));
    invalid = values;
    invalid[0].id = ParameterId(999);
    assert(!encode_feature_parameters(invalid, schema));
    assert(encode_feature_parameters({}, {}).value.empty());
    assert(decode_feature_parameters({}, {}).value.empty());
}

bool test_asset_parameter_kind_roundtrips_and_rejects_unknown_values() {
    const auto require = [](bool condition, const char* message) {
        if (!condition) std::cerr << message << '\n';
        return condition;
    };
    auto contract = valid_atomic_contract(FeatureDomain::kLogic);
    ParameterContract parameter{ParameterId{"Texture"}, 1, 16, false};
    ParameterMetadata metadata{};
    metadata.type = ParameterType::kAssetReference;
    metadata.default_value = aeg::foundation::AssetReference{"00112233-4455-6677-8899-aabbccddeeff"};
    metadata.constraint = AssetConstraint{false};
    parameter.metadata = metadata;
    contract.parameters.push_back(parameter);

    const auto encoded = encode_feature_contract_json(contract);
    if (!require(static_cast<bool>(encoded), "legacy asset contract did not encode")) return false;
    const auto legacy = decode_feature_contract_json(encoded.value);
    if (!require(legacy && !std::get<AssetConstraint>(legacy.value.parameters.back().metadata->constraint).kind,
                 "legacy asset contract acquired a kind")) return false;
    auto typed_json = encoded.value;
    const std::string old_constraint = "\"optional\":false";
    const auto old_constraint_position = typed_json.find(old_constraint);
    if (!require(old_constraint_position != std::string::npos, "asset nullability was not serialized")) return false;
    typed_json.replace(old_constraint_position, old_constraint.size(), "\"optional\":false,\"asset_kind\":1");

    const auto decoded = decode_feature_contract_json(typed_json);
    if (!require(static_cast<bool>(decoded), "typed asset contract did not decode")) return false;
    const auto roundtrip = encode_feature_contract_json(decoded.value);
    if (!require(roundtrip && roundtrip.value.find("\"asset_kind\":1") != std::string::npos,
                 "asset kind was lost during contract roundtrip")) return false;

    auto unknown_kind_json = typed_json;
    const auto kind_position = unknown_kind_json.find("\"asset_kind\":1");
    if (!require(kind_position != std::string::npos, "typed asset fixture has no kind")) return false;
    unknown_kind_json.replace(kind_position, std::string{"\"asset_kind\":1"}.size(), "\"asset_kind\":255");
    if (!require(!decode_feature_contract_json(unknown_kind_json), "unknown asset kind was accepted")) return false;
    for (const auto* invalid_kind : {"\"Texture\"", "null", "true", "1.0", "-1"}) {
        auto malformed = typed_json;
        malformed.replace(kind_position, std::string{"\"asset_kind\":1"}.size(),
                          std::string{"\"asset_kind\":"} + invalid_kind);
        if (!require(!decode_feature_contract_json(malformed), "nonnumeric/inexact asset kind was accepted")) return false;
    }

    auto typed = *decoded.value.parameters.back().metadata;
    auto untyped = typed;
    untyped.default_value.reset(); untyped.constraint = AssetConstraint{false};
    if (!require(parameter_metadata_digest(untyped) == 17723526721832114216ull,
                 "legacy asset metadata digest changed")) return false;
    auto mesh = untyped; mesh.constraint = AssetConstraint{false, aeg::foundation::AssetKind::kMesh};
    if (!require(parameter_metadata_digest(mesh) != parameter_metadata_digest(untyped),
                 "Mesh kind collided with undeclared kind")) return false;
    auto texture = mesh; texture.constraint = AssetConstraint{false, aeg::foundation::AssetKind::kTexture};
    if (!require(parameter_metadata_digest(mesh) != parameter_metadata_digest(texture),
                 "different asset kinds have identical digests")) return false;
    auto illegal = typed; illegal.constraint = AssetConstraint{false, static_cast<aeg::foundation::AssetKind>(255)};
    if (!require(!validate_parameter_metadata(illegal), "programmatic unknown asset kind was accepted")) return false;

    const auto reference = *typed.default_value;
    if (!require(static_cast<bool>(validate_parameter_value(reference, typed)), "schema-only validation unexpectedly resolves assets")) return false;
    if (!require(validate_parameter_value(reference, typed, {}).code == aeg::foundation::ResultCode::kInvalidState,
                 "missing typed asset resolver was not a configuration failure")) return false;
    unsigned resolutions{};
    const auto expected_id = aeg::foundation::AssetId::parse(std::get<aeg::foundation::AssetReference>(reference).uuid).value;
    const AssetKindResolver correct = [&](aeg::foundation::AssetId id) {
        ++resolutions;
        if (id != expected_id) return aeg::foundation::ResultValue<aeg::foundation::AssetKind>{aeg::foundation::not_found("unexpected asset"), {}};
        return aeg::foundation::ResultValue<aeg::foundation::AssetKind>{aeg::foundation::Result::success(), aeg::foundation::AssetKind::kTexture};
    };
    const AssetKindResolver wrong = [](aeg::foundation::AssetId) {
        return aeg::foundation::ResultValue<aeg::foundation::AssetKind>{aeg::foundation::Result::success(), aeg::foundation::AssetKind::kMesh};
    };
    const AssetKindResolver missing = [](aeg::foundation::AssetId) {
        return aeg::foundation::ResultValue<aeg::foundation::AssetKind>{aeg::foundation::not_found("missing asset"), {}};
    };
    if (!require(static_cast<bool>(validate_parameter_value(reference, typed, correct)), "matching asset kind was rejected")) return false;
    if (!require(validate_parameter_value(reference, typed, wrong).code == aeg::foundation::ResultCode::kInvalidArgument,
                 "wrong asset kind did not return invalid argument")) return false;
    if (!require(validate_parameter_value(reference, typed, missing).code == aeg::foundation::ResultCode::kNotFound,
                 "missing asset did not preserve resolver not found")) return false;
    const AssetKindResolver allocation_failure = [](aeg::foundation::AssetId) -> aeg::foundation::ResultValue<aeg::foundation::AssetKind> {
        throw std::bad_alloc{};
    };
    const AssetKindResolver callback_failure = [](aeg::foundation::AssetId) -> aeg::foundation::ResultValue<aeg::foundation::AssetKind> {
        throw std::runtime_error{"asset service failed"};
    };
    const AssetKindResolver nonstandard_failure = [](aeg::foundation::AssetId) -> aeg::foundation::ResultValue<aeg::foundation::AssetKind> {
        throw 17;
    };
    if (!require(validate_parameter_value(reference, typed, allocation_failure).code == aeg::foundation::ResultCode::kOutOfMemory,
                 "resolver allocation exception escaped its Result boundary")) return false;
    if (!require(validate_parameter_value(reference, typed, callback_failure).code == aeg::foundation::ResultCode::kInvalidState &&
                 validate_parameter_value(reference, typed, nonstandard_failure).code == aeg::foundation::ResultCode::kInvalidState,
                 "resolver callback exception escaped its Result boundary")) return false;
    auto nullable = typed; std::get<AssetConstraint>(nullable.constraint).optional = true;
    const auto previous = resolutions;
    if (!require(validate_parameter_value(aeg::foundation::AssetReference{}, nullable, correct) &&
                 validate_parameter_value(aeg::foundation::AssetReference{"00000000-0000-0000-0000-000000000000"}, nullable, correct) &&
                 resolutions == previous, "optional null reference invoked the resolver")) return false;
    if (!require(!validate_parameter_value(aeg::foundation::AssetReference{}, typed, correct), "required null reference was accepted")) return false;
    if (!require(!validate_parameter_value(std::string{"wrong variant"}, typed, correct), "semantic validation skipped structural type checks")) return false;
    if (!require(validate_parameter_value(reference, untyped, missing).code == aeg::foundation::ResultCode::kNotFound,
                 "Any asset reference bypassed the supplied resolver")) return false;
    auto unconstrained = untyped; unconstrained.constraint = {};
    if (!require(validate_parameter_value(reference, unconstrained, missing).code == aeg::foundation::ResultCode::kNotFound,
                 "unconstrained asset reference bypassed the supplied resolver")) return false;
    if (!require(validate_parameter_value(reference, untyped, {}) && validate_parameter_value(reference, untyped, wrong),
                 "Any asset lost its schema-only compatibility or rejected an existing kind")) return false;
    if (!require(validate_parameter_value(reference, untyped, correct) && resolutions == previous + 1,
                 "Any asset reference did not resolve its existence")) return false;
    const std::vector<ParameterContract> schema{{ParameterId{"Texture"}, 1, 16, true, typed}};
    if (!require(validate_feature_parameter_assets({}, schema, correct) && resolutions == previous + 2,
                 "effective default asset was not resolved")) return false;
    if (!require(!validate_feature_parameter_assets({}, schema, wrong), "default wrong-kind asset was accepted")) return false;
    if (!require(validate_feature_parameter_assets({}, schema, {}).code == aeg::foundation::ResultCode::kInvalidState,
                 "default typed reference did not reject missing resolver as configuration failure")) return false;
    if (!require(validate_feature_parameter_assets({}, schema, callback_failure).code == aeg::foundation::ResultCode::kInvalidState,
                 "default envelope resolver exception escaped its Result boundary")) return false;
    const auto bytes = serialize_parameter_value(reference, ParameterType::kAssetReference);
    const std::vector<FeatureParameterRecord> records{{schema.front().id, 1, bytes.value}};
    const auto blob = encode_feature_parameters(records, schema);
    if (!require(blob && validate_feature_parameter_assets(blob.value, schema, correct) &&
                 !validate_feature_parameter_assets(blob.value, schema, wrong), "explicit asset envelope skipped kind validation")) return false;
    return true;
}

bool test_no_default_schema_can_hold_its_fixed_wire_value() {
    using aeg::foundation::ParameterValue;
    struct FixedWireCase final {
        ParameterType type;
        ParameterValue value;
        uint32_t width;
        const char* name;
    };
    const FixedWireCase cases[]{
        {ParameterType::kBool, false, 1, "Bool"},
        {ParameterType::kInt32, int64_t{0}, 4, "Int32"},
        {ParameterType::kInt64, int64_t{0}, 8, "Int64"},
        {ParameterType::kUInt32, uint64_t{0}, 4, "UInt32"},
        {ParameterType::kUInt64, uint64_t{0}, 8, "UInt64"},
        {ParameterType::kFloat, 0.0, 4, "Float"},
        {ParameterType::kDouble, 0.0, 8, "Double"},
        {ParameterType::kAssetReference, aeg::foundation::AssetReference{}, 16, "AssetReference"},
        {ParameterType::kVector2, std::vector<double>{0, 0}, 16, "Vector2"},
        {ParameterType::kVector3, std::vector<double>{0, 0, 0}, 24, "Vector3"},
        {ParameterType::kVector4, std::vector<double>{0, 0, 0, 0}, 32, "Vector4"},
        {ParameterType::kColor, std::vector<double>{0, 0, 0, 0}, 32, "Color"},
    };
    bool passed = true;
    for (const auto& test : cases) {
        const auto payload = serialize_parameter_value(test.value, test.type);
        if (!payload || payload.value.size() != test.width) {
            std::cerr << test.name << " fixed wire fixture has the wrong width\n";
            passed = false;
            continue;
        }
        for (const bool required : {false, true}) {
            ParameterContract parameter{ParameterId{"FixedWidth"}, 1, test.width, required};
            parameter.metadata = ParameterMetadata{};
            parameter.metadata->type = test.type;
            if (!validate_feature_parameter_schema(std::vector<ParameterContract>{parameter})) {
                std::cerr << test.name << " exact fixed wire width was rejected without a default\n";
                passed = false;
            }
            --parameter.max_size;
            const auto rejected = validate_feature_parameter_schema(std::vector<ParameterContract>{parameter});
            if (rejected.code != aeg::foundation::ResultCode::kInvalidArgument) {
                std::cerr << test.name << " insufficient fixed wire width was accepted without a default (required="
                          << required << ")\n";
                passed = false;
            }
        }
    }
    return passed;
}

bool test_integer_metadata_rejects_ranges_without_representable_bounds() {
    struct InvalidRangeCase final {
        ParameterType type;
        NumericRange range;
        const char* name;
    };
    const InvalidRangeCase cases[]{
        {ParameterType::kInt32, {2147483648.0, 2147483648.0, 0, false}, "Int32 overflow"},
        {ParameterType::kUInt32, {-1, 0, 0, false}, "UInt32 negative lower bound"},
        {ParameterType::kUInt32, {4294967296.0, 4294967296.0, 0, false}, "UInt32 overflow"},
        {ParameterType::kInt64, {1e300, 1e300, 0, false}, "Int64 overflow"},
        {ParameterType::kInt64, {9223372036854775808.0, 9223372036854775808.0, 0, false}, "Int64 rounded upper limit"},
        {ParameterType::kUInt64, {-1, 0, 0, false}, "UInt64 negative lower bound"},
        {ParameterType::kUInt64, {18446744073709551616.0, 18446744073709551616.0, 0, false}, "UInt64 rounded upper limit"},
        {ParameterType::kInt64, {0.25, 0.75, 0, false}, "integer interval without an integer"},
    };
    bool passed = true;
    for (const auto& test : cases) {
        ParameterMetadata metadata{};
        metadata.type = test.type;
        metadata.constraint = test.range;
        if (validate_parameter_metadata(metadata).code != aeg::foundation::ResultCode::kInvalidArgument) {
            std::cerr << test.name << " was accepted without a default\n";
            passed = false;
        }
    }
    ParameterMetadata minimum{};
    minimum.type = ParameterType::kInt64;
    minimum.constraint = NumericRange{-9223372036854775808.0, -9223372036854775808.0, 0, false};
    if (!validate_parameter_metadata(minimum) ||
        !validate_parameter_value((std::numeric_limits<int64_t>::min)(), minimum)) {
        std::cerr << "representable Int64 minimum was rejected\n";
        passed = false;
    }
    return passed;
}

bool test_vector_metadata_rejects_component_ranges_that_cannot_be_unit_length() {
    bool passed = true;
    for (const auto type : {ParameterType::kVector2, ParameterType::kVector3,
                            ParameterType::kVector4, ParameterType::kColor}) {
        ParameterMetadata metadata{};
        metadata.type = type;
        for (const auto range : {NumericRange{1, 2, 0, false}, NumericRange{-2, -1, 0, false},
                                 NumericRange{0, 0.1, 0, false}, NumericRange{0, 0, 0, false},
                                 NumericRange{-1, 1, 2, false}, NumericRange{0, 0.9, 2, false}}) {
            metadata.constraint = VectorConstraint{range, false};
            if (!validate_parameter_metadata(metadata)) {
                std::cerr << "non-unit vector component range was rejected\n";
                passed = false;
            }
            std::get<VectorConstraint>(metadata.constraint).normalized = true;
            if (validate_parameter_metadata(metadata).code != aeg::foundation::ResultCode::kInvalidArgument) {
                std::cerr << "unsatisfiable unit vector component range [" << range.min << ',' << range.max
                          << "] was accepted for type " << static_cast<unsigned>(type) << '\n';
                passed = false;
            }
        }
        metadata.constraint = VectorConstraint{NumericRange{0, 1, 1, false}, true};
        if (!validate_parameter_metadata(metadata)) {
            std::cerr << "satisfiable stepped unit vector component range was rejected\n";
            passed = false;
        }
        metadata.constraint = VectorConstraint{NumericRange{0.5, 1, 0, false}, true};
        if (!validate_parameter_metadata(metadata)) {
            std::cerr << "satisfiable positive unit vector component range was rejected\n";
            passed = false;
        }
    }
    ParameterMetadata direction{};
    direction.type = ParameterType::kVector2;
    for (const double length : {1.0000005, 0.9999995}) {
        const double component = length / std::sqrt(2.0);
        const std::vector<double> value{component, component};
        auto tolerant = direction;
        tolerant.constraint = VectorConstraint{NumericRange{component, component, 0, false}, true};
        if (!validate_parameter_metadata(tolerant) || !validate_parameter_value(value, tolerant)) {
            std::cerr << "unit magnitude tolerance was lost during metadata validation\n";
            passed = false;
        }
    }
    direction.constraint = VectorConstraint{NumericRange{0, 0.9999995, 1, false}, true};
    if (!validate_parameter_metadata(direction) ||
        !validate_parameter_value(std::vector<double>{0.9999995, 0}, direction)) {
        std::cerr << "accepted step and unit tolerances were lost at a clipped final grid point\n";
        passed = false;
    }
    return passed;
}
}  // namespace

int main() {
    using namespace aeg::feature;
    const auto int32_bytes = serialize_parameter_value(int64_t{0x12345678}, ParameterType::kInt32);
    assert(int32_bytes && int32_bytes.value == std::vector<uint8_t>({0x78, 0x56, 0x34, 0x12}));
    const auto float_bytes = serialize_parameter_value(1.0, ParameterType::kFloat);
    assert(float_bytes && float_bytes.value == std::vector<uint8_t>({0, 0, 0x80, 0x3f}));
    ParameterMetadata finite{};
    finite.type = ParameterType::kDouble;
    finite.constraint = NumericRange{0, 1, 0, true};
    assert(!validate_parameter_value((std::numeric_limits<double>::quiet_NaN)(), finite));
    static_assert(std::is_same_v<decltype(ParameterMetadata{}.default_value),
                                 std::optional<aeg::foundation::ParameterValue>>);
    assert(!serialize_parameter_value(std::string("wrong"), ParameterType::kInt32));
    assert(!serialize_parameter_value(int64_t{1} << 40, ParameterType::kInt32));
    assert(!serialize_parameter_value(uint64_t{1} << 40, ParameterType::kUInt32));
    assert(!serialize_parameter_value((std::numeric_limits<double>::infinity)(), ParameterType::kFloat));
    assert(!deserialize_parameter_value(std::vector<uint8_t>{2}, ParameterType::kBool));
    assert(!deserialize_parameter_value(std::vector<uint8_t>{1, 0}, ParameterType::kBool));
    assert(!deserialize_parameter_value(std::vector<uint8_t>{1, 2, 3}, ParameterType::kInt32));
    assert(!deserialize_parameter_value({}, ParameterType::kDouble));
    assert(!serialize_parameter_value(std::vector<double>{1, 2}, ParameterType::kVector3));
    const aeg::foundation::AssetReference asset{"00112233-4455-6677-8899-aabbccddeeff"};
    const auto asset_bytes = serialize_parameter_value(asset, ParameterType::kAssetReference);
    assert(asset_bytes && asset_bytes.value == std::vector<uint8_t>({0, 0x11, 0x22, 0x33,
        0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff}));
    const auto asset_value = deserialize_parameter_value(asset_bytes.value, ParameterType::kAssetReference);
    assert(asset_value && std::get<aeg::foundation::AssetReference>(asset_value.value) == asset);
    assert(!serialize_parameter_value(aeg::foundation::AssetReference{"bad"}, ParameterType::kAssetReference));
    ParameterContract typed{ParameterId("Speed"), 1, 4, true};
    typed.metadata = ParameterMetadata{};
    typed.metadata->type = ParameterType::kFloat;
    typed.metadata->default_value = 1.0;
    typed.metadata->constraint = NumericRange{0, 4, 0, false};
    const std::vector<ParameterContract> typed_schema{typed};
    const auto defaults = encode_feature_parameters({}, typed_schema);
    assert(defaults && !defaults.value.empty());
    auto fractional_float = typed;
    fractional_float.metadata->default_value = 0.1;
    fractional_float.metadata->constraint = NumericRange{0, 0.1, 0, false};
    assert(validate_feature_parameter_schema(std::vector<ParameterContract>{fractional_float}));
    auto stepped_float = typed;
    stepped_float.metadata->default_value = 100.0;
    stepped_float.metadata->constraint = NumericRange{0, 100, 0.1, false};
    assert(validate_feature_parameter_schema(std::vector<ParameterContract>{stepped_float}));
    const auto off_grid = serialize_parameter_value(99.95, ParameterType::kFloat);
    assert(off_grid && !encode_feature_parameters(std::vector<FeatureParameterRecord>{{typed.id,1,off_grid.value}},
                                                 std::vector<ParameterContract>{stepped_float}));
    const auto records = decode_feature_parameters({}, typed_schema);
    assert(records && records.value.size() == 1 && records.value[0].payload == std::vector<uint8_t>({0,0,0x80,0x3f}));
    auto invalid_typed = typed;
    invalid_typed.metadata->default_value = 8.0;
    assert(!validate_feature_parameter_schema(std::vector<ParameterContract>{invalid_typed}));
    invalid_typed = typed;
    invalid_typed.max_size = 3;
    assert(!validate_feature_parameter_schema(std::vector<ParameterContract>{invalid_typed}));
    invalid_typed = typed;
    invalid_typed.metadata->constraint = NumericRange{4, 0, -1, false};
    assert(!validate_feature_parameter_schema(std::vector<ParameterContract>{invalid_typed}));
    const auto excessive = serialize_parameter_value(8.0, ParameterType::kFloat);
    assert(excessive && !encode_feature_parameters(std::vector<FeatureParameterRecord>{{typed.id, 1, excessive.value}}, typed_schema));
    auto definition = FeatureDefinition{};
    definition.id = FeatureDefinitionId("Test.Atomic");
    definition.version = {1,0,0};
    definition.contract = valid_atomic_contract(FeatureDomain::kLogic);
    definition.contract.parameters = typed_schema;
    const auto original_digest = feature_contract_digest(definition);
    definition.contract.parameters[0].metadata->display_name = "Different label";
    assert(feature_contract_digest(definition) == original_digest);
    definition.contract.parameters[0].metadata->default_value = 2.0;
    assert(feature_contract_digest(definition) != original_digest);
    assert(serialize_parameter_value(int64_t{-2}, ParameterType::kInt32).value == std::vector<uint8_t>({0xfe,0xff,0xff,0xff}));
    assert(serialize_parameter_value(1.0, ParameterType::kDouble).value == std::vector<uint8_t>({0,0,0,0,0,0,0xf0,0x3f}));
    const auto signed_limit = serialize_parameter_value((std::numeric_limits<int64_t>::min)(), ParameterType::kInt64);
    assert(signed_limit && std::get<int64_t>(deserialize_parameter_value(signed_limit.value, ParameterType::kInt64).value) == (std::numeric_limits<int64_t>::min)());
    const auto unsigned_limit = serialize_parameter_value((std::numeric_limits<uint64_t>::max)(), ParameterType::kUInt64);
    assert(unsigned_limit && unsigned_limit.value == std::vector<uint8_t>(8,0xff));
    assert(std::get<uint64_t>(deserialize_parameter_value(unsigned_limit.value, ParameterType::kUInt64).value) == (std::numeric_limits<uint64_t>::max)());
    ParameterMetadata exact_integer{};
    exact_integer.type = ParameterType::kInt64;
    exact_integer.constraint = NumericRange{0,9007199254740992.0,0,false};
    assert(!validate_parameter_value(int64_t{9007199254740993}, exact_integer));
    ParameterMetadata direction{};
    direction.type = ParameterType::kVector3;
    direction.constraint = VectorConstraint{{NumericRange{0,1,0,true}},true};
    direction.default_value = std::vector<double>{1,0,0};
    assert(validate_parameter_metadata(direction));
    assert(!validate_parameter_value(std::vector<double>{0,0,0}, direction));
    assert(!validate_parameter_value(std::vector<double>{1,0,(std::numeric_limits<double>::quiet_NaN)()}, direction));
    auto duplicate_choices = ParameterMetadata{};
    duplicate_choices.type = ParameterType::kEnum;
    duplicate_choices.constraint = EnumChoices{{"Walk","Walk"},{}};
    assert(!validate_parameter_metadata(duplicate_choices));
    duplicate_choices.constraint = EnumChoices{{"Walk","Run"},{}};
    duplicate_choices.default_value = std::string("Invalid");
    assert(!validate_parameter_metadata(duplicate_choices));
    auto bad_regex = ParameterMetadata{};
    bad_regex.type = ParameterType::kString;
    bad_regex.constraint = StringPattern{"[",0,0,false,false};
    assert(!validate_parameter_metadata(bad_regex));
    assert(!deserialize_parameter_value(std::vector<uint8_t>{2,0,0,0,'a'}, ParameterType::kString));
    auto required_no_default = typed;
    required_no_default.metadata->default_value.reset();
    assert(!decode_feature_parameters({}, std::vector<ParameterContract>{required_no_default}));
    test_state_payload_contract_validation_and_digest();
    test_all_five_domains_are_leaf_domains();
    test_valid_atomic_contracts();
    test_invalid_execution_and_state_combinations();
    if (!test_atomic_call_rejects_continuations()) {
        std::cerr << "Atomic Call continuation contract validation failed\n";
        return 1;
    }
    test_clock_ports_and_side_effect_versions_are_required();
    test_composite_requires_leaf_domain_union();
    test_atomic_cannot_hide_children();
    test_composite_coordination_is_owned_by_a_process_child();
    test_contract_digest_is_shared_by_registry_and_catalog();
    test_contract_digest_preserves_collection_boundaries();
    test_parameter_envelope_codec_is_canonical_and_strict();
    if (!test_asset_parameter_kind_roundtrips_and_rejects_unknown_values()) return 1;
    const bool fixed_width = test_no_default_schema_can_hold_its_fixed_wire_value();
    const bool integer_ranges = test_integer_metadata_rejects_ranges_without_representable_bounds();
    const bool vector_ranges = test_vector_metadata_rejects_component_ranges_that_cannot_be_unit_length();
    if (!fixed_width || !integer_ranges || !vector_ranges) return 1;
    std::cout << "feature contract smoke passed\n";
    return 0;
}
