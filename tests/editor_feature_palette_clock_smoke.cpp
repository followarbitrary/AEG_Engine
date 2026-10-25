/**
 * @file editor_feature_palette_clock_smoke.cpp
 * @brief Domain, execution and full ClockId palette filtering.
 */
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <vector>

#include "editor_feature_graph_model.hpp"

namespace {
using namespace aeg::application;
using namespace aeg::feature;

constexpr ClockId kSimulationClock{0x100000007ull};
constexpr ClockId kPresentationClock{0x200000007ull};
constexpr ScopeId kLowScope{0x0000000000000007ull};
constexpr ScopeId kHighScope{0x8000000100000007ull};
constexpr FeatureDomainMask kLogicAndPhysics =
    to_mask(FeatureDomain::kLogic) | to_mask(FeatureDomain::kPhysics);

FeatureDefinition definition(uint64_t id, FeatureDomainMask domains,
                             ExecutionKind execution, ClockId clock) {
    FeatureDefinition result{};
    result.id = FeatureDefinitionId{id};
    result.version = {1, 2, 3};
    result.owner_module = ModuleIdentity{"com.aeg.palette.fixture"};
    auto& contract = result.contract;
    contract.definition_id = result.id;
    contract.version = result.version;
    contract.domains = domains;
    contract.execution = execution;
    contract.scope.id = ScopeId{"World"};
    contract.clock = aeg::feature::make_periodic_clock_contract(clock, 60, 4, LateDataPolicy::kReject,
        aeg::feature::DeadlinePolicy::kObserve, contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    contract.permissions.version = 1;
    contract.has_continuation = execution == ExecutionKind::kProcess;
    contract.ports = {{PortId{1}, PortDirection::kInput, ValueSchemaId{"Scalar.v1"}, 1}};
    if (domains == kLogicAndPhysics) {
        contract.structure = FeatureStructure::kComposite; contract.clock.work_budget_ns = 0;
        contract.children = {FeatureDefinitionId{12}, FeatureDefinitionId{15}};
    }
    return result;
}

FeatureCatalog catalog() {
    FeatureCatalog result;
    assert(result.export_definition(definition(11, kLogicAndPhysics, ExecutionKind::kCall,
                                               kSimulationClock)));
    assert(result.export_definition(definition(12, to_mask(FeatureDomain::kLogic),
                                               ExecutionKind::kCall, kSimulationClock)));
    assert(result.export_definition(definition(13, to_mask(FeatureDomain::kLogic),
                                               ExecutionKind::kProcess, kSimulationClock)));
    assert(result.export_definition(definition(14, to_mask(FeatureDomain::kRender),
                                               ExecutionKind::kContinuous, kPresentationClock)));
    assert(result.export_definition(definition(15, to_mask(FeatureDomain::kPhysics),
                                               ExecutionKind::kCall, kSimulationClock)));
    assert(result.export_definition(definition(16, to_mask(FeatureDomain::kAudio),
                                               ExecutionKind::kCall, kSimulationClock)));
    assert(result.export_definition(definition(17, to_mask(FeatureDomain::kStream),
                                               ExecutionKind::kContinuous, kSimulationClock)));
    assert(result.export_definition(definition(18, to_mask(FeatureDomain::kLogic),
                                               ExecutionKind::kCall, kPresentationClock)));
    return result;
}

void expect_ids(const EditorFeatureGraphModel& model, const FeaturePaletteFilter& filter,
                std::initializer_list<uint64_t> expected) {
    const auto entries = model.palette(filter);
    std::vector<uint64_t> actual;
    for (const auto& entry : entries) actual.push_back(entry.definition_id.value());
    assert(actual == std::vector<uint64_t>{expected});
}

void test_domain_execution_and_clock_filters_intersect() {
    EditorFeatureGraphModel model;
    assert(model.replace_catalog(catalog()));
    expect_ids(model, {}, {11, 12, 13, 14, 15, 16, 17, 18});
    expect_ids(model, {FeatureDomain::kLogic}, {11, 12, 13, 18});
    expect_ids(model, {FeatureDomain::kPhysics}, {11, 15});
    expect_ids(model, {FeatureDomain::kRender}, {14});
    expect_ids(model, {FeatureDomain::kAudio}, {16});
    expect_ids(model, {FeatureDomain::kStream}, {17});
    expect_ids(model, {std::nullopt, ExecutionKind::kProcess}, {13});
    expect_ids(model, {std::nullopt, ExecutionKind::kContinuous}, {14, 17});
    expect_ids(model, {std::nullopt, std::nullopt, kSimulationClock}, {11, 12, 13, 15, 16, 17});
    expect_ids(model, {std::nullopt, std::nullopt, kPresentationClock}, {14, 18});
    expect_ids(model, {FeatureDomain::kLogic, ExecutionKind::kCall, kSimulationClock}, {11, 12});
    expect_ids(model, {FeatureDomain::kLogic, ExecutionKind::kCall, kPresentationClock}, {18});
    expect_ids(model, {FeatureDomain::kPhysics, ExecutionKind::kCall, kSimulationClock}, {11, 15});
    expect_ids(model, {FeatureDomain::kAudio, ExecutionKind::kContinuous, kSimulationClock}, {});

    // All clocks is an empty optional, rather than the zero/invalid ClockId.
    expect_ids(model, {std::nullopt, std::nullopt, ClockId{}}, {});
    const auto all = model.palette({});
    assert(all.front().clock.value() == 0x100000007ull);
    assert(all[3].clock.value() == 0x200000007ull);
    assert(all.front().domains == kLogicAndPhysics);
}

void test_shared_predicate_distinguishes_all_clocks_from_explicit_unclocked() {
    const FeaturePaletteEntry unclocked{FeatureDefinitionId{91}, kLogicAndPhysics,
                                        ExecutionKind::kCall, ClockId{}};
    const FeaturePaletteEntry clocked{FeatureDefinitionId{92}, kLogicAndPhysics,
                                      ExecutionKind::kCall, kSimulationClock};
    const FeaturePaletteFilter all{};
    const FeaturePaletteFilter unclocked_only{std::nullopt, std::nullopt, ClockId{}};
    assert(feature_palette_matches(unclocked, all));
    assert(feature_palette_matches(clocked, all));
    assert(feature_palette_matches(unclocked, unclocked_only));
    assert(!feature_palette_matches(clocked, unclocked_only));
    assert(!feature_palette_matches(unclocked, {std::nullopt, std::nullopt, kSimulationClock}));
    assert(feature_palette_matches(clocked, {FeatureDomain::kPhysics, ExecutionKind::kCall,
                                             kSimulationClock}));
    assert(!feature_palette_matches(clocked, {FeatureDomain::kRender, ExecutionKind::kCall,
                                              kSimulationClock}));
    assert(!feature_palette_matches(clocked, {FeatureDomain::kLogic, ExecutionKind::kProcess,
                                              kSimulationClock}));
    assert(!feature_palette_matches(clocked, {FeatureDomain::kLogic, ExecutionKind::kCall,
                                              kPresentationClock}));
}

void test_catalog_replacement_copies_new_clock_and_preserves_catalog_order() {
    const auto original = catalog();
    const auto digest_before = original.contract_digest();
    const auto composite_before = original.entries().front();
    EditorFeatureGraphModel model;
    assert(model.replace_catalog(original));

    FeatureCatalog reordered;
    assert(reordered.export_definition(definition(18, to_mask(FeatureDomain::kLogic),
                                                  ExecutionKind::kCall, kPresentationClock)));
    assert(reordered.export_definition(definition(12, to_mask(FeatureDomain::kLogic),
                                                  ExecutionKind::kProcess, kPresentationClock)));
    assert(reordered.export_definition(definition(11, kLogicAndPhysics, ExecutionKind::kCall,
                                                  kSimulationClock)));
    assert(model.replace_catalog(reordered));
    expect_ids(model, {}, {18, 12, 11});
    expect_ids(model, {std::nullopt, std::nullopt, kSimulationClock}, {11});
    expect_ids(model, {std::nullopt, std::nullopt, kPresentationClock}, {18, 12});
    expect_ids(model, {FeatureDomain::kLogic, ExecutionKind::kProcess, kPresentationClock}, {12});
    const auto replacement = model.palette({});
    assert(replacement[1].clock.value() == 0x200000007ull);

    // Palette projection leaves the module-owned metadata and provenance intact.
    assert(original.contract_digest() == digest_before);
    const auto& composite = original.entries().front();
    assert(composite.clock == composite_before.clock);
    assert(composite.contract.clock.id == composite_before.contract.clock.id);
    assert(composite.owner_module == ModuleIdentity{"com.aeg.palette.fixture"});
    assert(composite.ports == composite_before.ports);
    assert(composite.contract_digest == composite_before.contract_digest);
    assert(model.replace_catalog(FeatureCatalog{}));
    expect_ids(model, {}, {});
}

void test_clock_filtering_preserves_runtime_source_chain() {
    FeatureNodeProvenance provenance{};
    provenance.definition_id = FeatureDefinitionId{11};
    provenance.node_id = FeatureNodeId{21};
    provenance.instance_id = FeatureInstanceId{31};
    provenance.process_id = ProcessId{41};
    provenance.invocation_id = InvocationId{51};
    provenance.owner_path.nodes = {FeatureNodeId{61}, FeatureNodeId{71}};
    provenance.clock = kSimulationClock;
    provenance.domains = kLogicAndPhysics;
    provenance.owner_module = ModuleIdentity{"com.aeg.palette.fixture"};
    EditorFeatureGraphModel model;
    assert(model.replace_catalog(catalog()));
    expect_ids(model, {FeatureDomain::kLogic, ExecutionKind::kCall, kPresentationClock}, {18});
    const auto path = EditorFeatureGraphModel::inspect(provenance);
    assert(path.definition_id == FeatureDefinitionId{11});
    assert(path.node_id == FeatureNodeId{21});
    assert(path.instance_id == FeatureInstanceId{31});
    assert(path.process_id == ProcessId{41});
    assert(path.invocation_id == InvocationId{51});
    assert((path.owner_path.nodes == std::vector{FeatureNodeId{61}, FeatureNodeId{71}}));
}

FeatureDefinition dimensional_definition(uint64_t id, FeatureDomainMask domains,
    ExecutionKind execution, ClockId clock, FeatureStructure structure, ScopeId scope) {
    auto value = definition(id, domains, execution, clock);
    value.contract.structure = structure;
    value.contract.clock.work_budget_ns = structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000;
    value.contract.scope.id = scope;
    value.contract.children.clear();
    if (structure == FeatureStructure::kComposite) {
        value.contract.children.push_back(FeatureDefinitionId{201});
        if ((domains & to_mask(FeatureDomain::kPhysics)) != 0)
            value.contract.children.push_back(FeatureDefinitionId{205});
    }
    return value;
}

FeatureCatalog dimensional_catalog() {
    FeatureCatalog result;
    const std::vector definitions{
        dimensional_definition(201, to_mask(FeatureDomain::kLogic), ExecutionKind::kCall,
            kSimulationClock, FeatureStructure::kAtomic, kLowScope),
        // A same-domain Composite must not be inferred to be Atomic from its domain mask.
        dimensional_definition(202, to_mask(FeatureDomain::kLogic), ExecutionKind::kCall,
            kSimulationClock, FeatureStructure::kComposite, kHighScope),
        dimensional_definition(203, kLogicAndPhysics, ExecutionKind::kCall,
            kSimulationClock, FeatureStructure::kComposite, kHighScope),
        dimensional_definition(204, to_mask(FeatureDomain::kLogic), ExecutionKind::kProcess,
            kSimulationClock, FeatureStructure::kAtomic, kHighScope),
        dimensional_definition(205, to_mask(FeatureDomain::kPhysics), ExecutionKind::kCall,
            kSimulationClock, FeatureStructure::kAtomic, kHighScope),
        dimensional_definition(206, to_mask(FeatureDomain::kLogic), ExecutionKind::kCall,
            kPresentationClock, FeatureStructure::kAtomic, kHighScope),
        dimensional_definition(207, to_mask(FeatureDomain::kLogic), ExecutionKind::kCall,
            kSimulationClock, FeatureStructure::kAtomic, kLowScope),
        dimensional_definition(208, to_mask(FeatureDomain::kLogic), ExecutionKind::kCall,
            kSimulationClock, FeatureStructure::kAtomic, kHighScope),
    };
    for (const auto& value : definitions) assert(result.export_definition(value));
    // Summary-only catalogs are formally accepted without a canonical contract.
    // Their zero ScopeId must remain distinguishable from the All scopes filter.
    FeatureCatalogEntry summary{};
    summary.definition_id = FeatureDefinitionId{209}; summary.version = {1, 0, 0};
    summary.domains = to_mask(FeatureDomain::kLogic); summary.clock = kSimulationClock;
    assert(result.add_entry(summary));
    return result;
}

void test_structure_and_full_scope_filters_intersect_all_existing_dimensions() {
    const auto source = dimensional_catalog();
    const auto digest = source.contract_digest();
    EditorFeatureGraphModel model;
    assert(model.replace_catalog(source));
    FeaturePaletteFilter filter{};
    filter.structure = FeatureStructure::kComposite;
    expect_ids(model, filter, {202, 203});
    filter.structure = FeatureStructure::kAtomic;
    expect_ids(model, filter, {201, 204, 205, 206, 207, 208, 209});
    filter.structure.reset(); filter.scope = kHighScope;
    expect_ids(model, filter, {202, 203, 204, 205, 206, 208});
    filter.scope = kLowScope;
    expect_ids(model, filter, {201, 207});
    filter.scope = ScopeId{};
    expect_ids(model, filter, {209});
    filter.scope.reset();
    expect_ids(model, filter, {201, 202, 203, 204, 205, 206, 207, 208, 209});

    // The first three aggregate fields retain their public ordering.
    filter = {FeatureDomain::kLogic, ExecutionKind::kCall, kSimulationClock,
              FeatureStructure::kComposite, kHighScope};
    expect_ids(model, filter, {202, 203});
    filter.structure = FeatureStructure::kAtomic;
    expect_ids(model, filter, {208});
    filter.execution = ExecutionKind::kProcess;
    expect_ids(model, filter, {204});
    filter.domain = FeatureDomain::kPhysics;
    expect_ids(model, filter, {});
    filter = {std::nullopt, std::nullopt, std::nullopt, std::nullopt, ScopeId{99}};
    expect_ids(model, filter, {});

    const auto projection = model.palette({});
    assert(projection[1].structure == FeatureStructure::kComposite);
    assert(projection[1].scope == kHighScope);
    assert(projection.front().scope == kLowScope);
    assert(source.contract_digest() == digest);
    assert(source.entries()[1].contract.structure == FeatureStructure::kComposite);
    assert(source.entries()[1].contract.scope.id == kHighScope);
}

void test_shared_predicate_keeps_atomic_zero_and_explicit_zero_scope_distinct_from_all() {
    const FeaturePaletteEntry old_aggregate{FeatureDefinitionId{301}, to_mask(FeatureDomain::kLogic),
                                            ExecutionKind::kCall, kSimulationClock};
    const FeaturePaletteEntry composite{FeatureDefinitionId{302}, to_mask(FeatureDomain::kLogic),
        ExecutionKind::kCall, kSimulationClock, FeatureStructure::kComposite, kHighScope};
    const FeaturePaletteFilter atomic_only{std::nullopt, std::nullopt, std::nullopt,
                                           FeatureStructure::kAtomic};
    const FeaturePaletteFilter zero_scope{std::nullopt, std::nullopt, std::nullopt,
                                          std::nullopt, ScopeId{}};
    assert(feature_palette_matches(old_aggregate, {}));
    assert(feature_palette_matches(composite, {}));
    assert(feature_palette_matches(old_aggregate, atomic_only));
    assert(!feature_palette_matches(composite, atomic_only));
    assert(feature_palette_matches(old_aggregate, zero_scope));
    assert(!feature_palette_matches(composite, zero_scope));
    assert(feature_palette_matches(composite, {FeatureDomain::kLogic, ExecutionKind::kCall,
        kSimulationClock, FeatureStructure::kComposite, kHighScope}));
    assert(!feature_palette_matches(composite, {FeatureDomain::kLogic, ExecutionKind::kCall,
        kSimulationClock, FeatureStructure::kComposite, kLowScope}));
}

void test_catalog_replacement_refreshes_structure_and_scope_without_reordering() {
    EditorFeatureGraphModel model;
    assert(model.replace_catalog(dimensional_catalog()));
    FeatureCatalog replacement;
    assert(replacement.export_definition(dimensional_definition(208, to_mask(FeatureDomain::kLogic),
        ExecutionKind::kCall, kSimulationClock, FeatureStructure::kComposite, kLowScope)));
    assert(replacement.export_definition(dimensional_definition(202, to_mask(FeatureDomain::kLogic),
        ExecutionKind::kCall, kSimulationClock, FeatureStructure::kAtomic, kHighScope)));
    assert(model.replace_catalog(replacement));
    expect_ids(model, {}, {208, 202});
    expect_ids(model, {std::nullopt, std::nullopt, std::nullopt,
        FeatureStructure::kComposite, kLowScope}, {208});
    expect_ids(model, {std::nullopt, std::nullopt, std::nullopt,
        FeatureStructure::kAtomic, kHighScope}, {202});
    expect_ids(model, {std::nullopt, std::nullopt, std::nullopt,
        FeatureStructure::kAtomic, kLowScope}, {});
    assert(model.replace_catalog(FeatureCatalog{}));
    expect_ids(model, {}, {});
}
}  // namespace

int main() {
    test_domain_execution_and_clock_filters_intersect();
    test_shared_predicate_distinguishes_all_clocks_from_explicit_unclocked();
    test_catalog_replacement_copies_new_clock_and_preserves_catalog_order();
    test_clock_filtering_preserves_runtime_source_chain();
    test_structure_and_full_scope_filters_intersect_all_existing_dimensions();
    test_shared_predicate_keeps_atomic_zero_and_explicit_zero_scope_distinct_from_all();
    test_catalog_replacement_refreshes_structure_and_scope_without_reordering();
    std::cout << "editor feature palette clock smoke passed\n";
    return 0;
}
