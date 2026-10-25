#include "feature_implementation_navigation_fixture.hpp"

#if __has_include("editor_feature_implementation_resolver.hpp")
#include "editor_feature_implementation_resolver.hpp"
#endif

#include <iostream>

namespace {
using namespace aeg;
using namespace aeg::application;
using tests::navigation::require;

// The baseline still builds: the failure is the missing product operation,
// after real generation and canonical contract scanning have succeeded.
template<class Path, class Id>
foundation::ResultValue<CustomNodeDefinitionTarget> locate(const Path& project, Id id) {
    if constexpr (requires { locate_feature_implementation(project, id); })
        return locate_feature_implementation(project, id);
    else
        return {foundation::unsupported("FEATURE_IMPLEMENTATION_RESOLVER_MISSING"), {}};
}

void assert_target(const foundation::ResultValue<CustomNodeDefinitionTarget>& target,
                   const tests::navigation::Generated& generated) {
    require(bool(target), "Definition identity did not locate its actual implementation: " + target.result.message);
    require(std::filesystem::equivalent(target.value.path, generated.files.implementation_target.path) &&
                target.value.line == generated.execute_line && target.value.symbol == generated.symbol,
            "implementation navigation selected a same-name node, descriptor reference, comment, string or wrong source line");
}

void test_exact_identity_and_definition() {
    tests::navigation::Fixture fixture;
    const auto low = fixture.generate(feature::FeatureDefinitionId{uint64_t{7}}, "LowIdentity");
    const auto high = fixture.generate(feature::FeatureDefinitionId{uint64_t{0x8000000100000007ull}}, "HighIdentity");
    assert_target(locate(fixture.root, high.draft.entry.id), high);
    assert_target(locate(fixture.project_file, low.draft.entry.id), low);
    // A newer source-only draft is not an implementation authority. Its name
    // must not redirect a compiled definition's source navigation.
    auto draft = high.draft;
    draft.generated = false; draft.name = "../../OutsideProject";
    auto draft_file = high.files.files.back();
    draft_file.replace_filename("HighIdentity.flowgraph.draft.json");
    require(bool(write_editor_node_contract_atomic({draft_file, draft, {}, NodeContractSource::kEditorDraft}, fixture.root)),
            "could not prepare the newer editor draft");
    assert_target(locate(fixture.project_file, high.draft.entry.id), high);
    const auto unknown = locate(fixture.root, feature::FeatureDefinitionId{"No.Project.Source"});
    require(!unknown && unknown.result.code == foundation::ResultCode::kNotFound,
            "unknown/system definitions fabricated a project source location");
    const auto invalid = locate(fixture.root, feature::FeatureDefinitionId{});
    require(!invalid && invalid.result.code == foundation::ResultCode::kInvalidArgument,
            "null Definition identity was accepted");
    auto source_only = low.draft;
    source_only.generated = false;
    source_only.entry.id = feature::FeatureDefinitionId{"Navigation.DraftOnly"};
    source_only.entry.contract.definition_id = source_only.entry.id;
    const auto source_only_path = low.files.files.back().parent_path() / "DraftOnly.flowgraph.draft.json";
    require(bool(write_editor_node_contract_atomic({source_only_path, source_only, {}, NodeContractSource::kEditorDraft}, fixture.root)),
            "could not prepare source-only draft without generated code");
    const auto no_implementation = locate(fixture.root, source_only.entry.id);
    require(!no_implementation && no_implementation.result.code == foundation::ResultCode::kNotFound,
            "an ungenerated draft fabricated a project implementation");
}

void test_missing_ambiguous_and_nonregular_sources() {
    tests::navigation::Fixture fixture;
    const auto generated = fixture.generate(feature::FeatureDefinitionId{"Navigation.Source"}, "ActualSource");
    assert_target(locate(fixture.root, generated.draft.entry.id), generated);
    const auto source = generated.files.implementation_target.path;
    const auto original = tests::read_text(source);
    const auto signature = "aeg::foundation::ResultValue<aeg::feature::StateDelta> " + generated.symbol + "(";
    // Leave every descriptor/comment/string occurrence but remove the one
    // real declaration. A substring hit must not claim an implementation.
    auto missing_definition = original;
    std::size_t line_start = 0;
    for (uint32_t line = 1; line < generated.execute_line; ++line)
        line_start = missing_definition.find('\n', line_start) + 1;
    const auto definition = missing_definition.find(signature, line_start);
    require(definition == line_start, "actual execute line fixture moved unexpectedly");
    missing_definition.replace(definition, signature.size(), "aeg::foundation::ResultValue<aeg::feature::StateDelta> ActualSource::removed_execute(");
    tests::navigation::write(source, missing_definition);
    require(!locate(fixture.root, generated.draft.entry.id), "a descriptor reference or commented signature was treated as executable source");
    tests::navigation::write(source, original + "\n" + signature + "int another) {}\n");
    require(!locate(fixture.root, generated.draft.entry.id), "ambiguous execute definitions selected an arbitrary source line");
    tests::navigation::write(source, original);
    const auto contract = generated.files.files.back();
    const auto duplicate = contract.parent_path() / "OtherStem.flowgraph.contract.json";
    std::filesystem::copy_file(contract, duplicate);
    require(!locate(fixture.root, generated.draft.entry.id), "duplicate generated Definition identities selected an arbitrary contract file");
    std::filesystem::remove(duplicate);
    std::filesystem::remove(source);
    require(!locate(fixture.root, generated.draft.entry.id), "missing source fabricated an implementation path");
    std::filesystem::create_directory(source);
    require(!locate(fixture.root, generated.draft.entry.id), "a directory was accepted as an implementation file");
}
}  // namespace

int main() {
    unsigned failures = 0;
    const auto run = [&](const char* name, auto test) {
        try { test(); std::cout << name << " passed\n"; }
        catch (const std::exception& error) { ++failures; std::cerr << name << ": " << error.what() << '\n'; }
    };
    run("exact-id-and-real-execute-line", test_exact_identity_and_definition);
    run("missing-ambiguous-nonregular-source", test_missing_ambiguous_and_nonregular_sources);
    std::cout << "FEATURE_IMPLEMENTATION_RESOLVER_OBSERVATIONS=2 failed=" << failures << '\n';
    return failures ? 1 : 0;
}
