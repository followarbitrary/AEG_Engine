/**
 * @file feature_graph_document_io_smoke.cpp
 * @brief FeatureGraph-only authoring document persistence checks.
 */
#include <cassert>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <concurrency.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>

#include <feature_graph_io.hpp>
#include "feature_graph_source_storage.hpp"

namespace {
using namespace aeg::feature;
using namespace aeg::application::authoring;

FeatureGraphDocument make_graph() {
    FeatureGraphDocument graph{};
    graph.asset_uuid = "asset://io";
    graph.schema_version = 1;
    graph.graph_version = 4;
    graph.draft_revision = 7;
    graph.layout_revision = 9;
    graph.module_requirements = {"gameplay@1"};
    graph.nodes.push_back({FeatureNodeId("Root"), FeatureDefinitionId("Logic.Root"), {1, 0, 0},
                           OwnerPath{{FeatureNodeId("Owner")}}, {8, 13}});
    graph.edges.push_back({FeatureNodeId("Root"), PortId("Out"), FeatureNodeId("Root"), PortId("In"),
                           EdgeKind::kEvent, ClockId("LogicTick"), ClockId("LogicTick"), 2, 3, 4});
    graph.process_subgraphs.push_back({FeatureNodeId("Root"), {"Idle"}, {"Idle->Idle"}, 3, "Idle", {}});
    graph.input_bindings = {"Input.Fire"};
    graph.variadic_slots = {"Children"};
    graph.extensions = {"vendor:key"};
    graph.contract_digest = 42;
    return graph;
}

int crash_probe(std::string_view mode, const std::filesystem::path& root, std::string_view stage) {
    using namespace aeg::application::authoring;
    std::filesystem::create_directories(root);
    const auto file = root / "Graph.aegfeature";
    if (mode == "--prepare-crash-probe") {
        auto graph = make_graph();
        graph.graph_version = 1;
        if (!save_feature_graph_document(file, graph)) return 11;
        graph.graph_version = 2;
        if (!save_feature_graph_document(file, graph)) return 12;
        graph.graph_version = 3;
        return save_feature_graph_document(root / "Candidate.aegfeature", graph) ? 0 : 13;
    }
    if (mode == "--verify-crash-probe") {
        const auto current = load_feature_graph_source(file);
        const auto backup = load_feature_graph_document(root / "Graph.aegfeature.bak");
        if (!current || !backup || current.value.document.graph_version != 2 || backup.value.graph_version != 1)
            return 21;
        auto next = current.value.document;
        ++next.graph_version;
        const auto saved = save_feature_graph_source(file, next, &current.value.revision);
        if (!saved || saved.value != inspect_feature_graph_file_revision(file).value) return 22;
        std::cout << "SOURCE_CRASH_RECOVERY_COMPLETE " << stage << '\n';
        return 0;
    }
    if (mode != "--crash-probe") return 30;
    const auto before = load_feature_graph_source(file);
    const auto candidate = load_feature_graph_source(root / "Candidate.aegfeature");
    if (!before || !candidate) return 31;
    const auto result = aeg::application::authoring::detail::publish_feature_graph_source(file, candidate.value.revision.contents,
        before.value.revision, [&](const auto& path) {
            const auto filename = path.filename().string();
            if (stage == "after-source-temp-flush" && filename.starts_with("Graph.aegfeature.tmp-"))
                std::_Exit(71); // Deliberately bypass destructors, as a terminated writer does.
            if (stage == "after-backup-temp-flush" && filename.starts_with("Graph.aegfeature.bak.tmp-"))
                std::_Exit(72);
            return load_feature_graph_document(path).result;
        });
    (void)result;
    return 32; // A requested interrupted-write phase must actually have been reached.
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 4) return crash_probe(argv[1], argv[2], argv[3]);
    const auto root = std::filesystem::temp_directory_path() / "aeg_feature_graph_io_smoke";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    assert(!error);
    const auto file = root / "Graph.aegfeature";
    const auto source = make_graph();
    assert(save_feature_graph_document(file, source));
    const auto loaded = load_feature_graph_document(file);
    assert(loaded);
    assert(loaded.value.asset_uuid == source.asset_uuid);
    assert(loaded.value.nodes.size() == 1 && loaded.value.nodes.front().parameter_blob == source.nodes.front().parameter_blob);
    assert(loaded.value.edges.size() == 1 && loaded.value.edges.front().kind == EdgeKind::kEvent);
    assert(loaded.value.process_subgraphs.front().state_schema_version == 3);
    assert(loaded.value.process_subgraphs.front().initial_state == "Idle");
    assert(loaded.value.contract_digest == 42);

    // Replacing an author document must retain the previous complete source.
    auto revised = source;
    ++revised.graph_version;
    assert(save_feature_graph_document(file, revised));
    auto backup = file;
    backup += ".bak";
    if (!std::filesystem::exists(backup)) {
        std::cerr << "FeatureGraph replacement lost its recoverable previous version\n";
        return 1;
    }
    const auto previous = load_feature_graph_document(backup);
    if (!previous || previous.value.graph_version != source.graph_version) {
        std::cerr << "FeatureGraph backup is not the previous complete source\n";
        return 1;
    }

    const auto editor_a = load_feature_graph_source(file);
    const auto editor_b = load_feature_graph_source(file);
    assert(editor_a && editor_b);
    auto edited_a = editor_a.value.document;
    ++edited_a.graph_version;
    assert(save_feature_graph_document(file, edited_a, editor_a.value.revision));
    const auto committed_a = inspect_feature_graph_file_revision(file);
    assert(committed_a);
    auto edited_b = editor_b.value.document;
    edited_b.graph_version += 9;
    const auto conflict = save_feature_graph_document(file, edited_b, editor_b.value.revision);
    if (conflict || conflict.code != aeg::foundation::ResultCode::kOutOfDate ||
        inspect_feature_graph_file_revision(file).value != committed_a.value ||
        edited_b.graph_version != editor_b.value.document.graph_version + 9) {
        std::cerr << "FeatureGraph concurrent save did not preserve both disk and local drafts\n";
        return 1;
    }
    const auto good_backup = inspect_feature_graph_file_revision(backup);
    assert(good_backup && good_backup.value.exists);
    auto damaged_bytes = committed_a.value.contents;
    const auto payload_position = damaged_bytes.find("asset://io");
    assert(payload_position != std::string::npos);
    damaged_bytes[payload_position] = 'X'; // Valid TOML syntax, invalid integrity.
    {
        std::ofstream damaged(file, std::ios::binary | std::ios::trunc);
        damaged.write(damaged_bytes.data(), static_cast<std::streamsize>(damaged_bytes.size()));
    }
    if (load_feature_graph_document(file)) {
        std::cerr << "FeatureGraph accepted corrupted author data as a new graph\n";
        return 1;
    }
    const auto damaged_revision = inspect_feature_graph_file_revision(file);
    assert(damaged_revision);
    assert(!recover_feature_graph_document(file, committed_a.value)); // Stale recovery must not overwrite.
    auto stale_backup = good_backup.value;
    stale_backup.contents += '\n';
    const auto stale_recovery = recover_feature_graph_document(file, damaged_revision.value, &stale_backup);
    assert(!stale_recovery && stale_recovery.code == aeg::foundation::ResultCode::kOutOfDate &&
        inspect_feature_graph_file_revision(file).value == damaged_revision.value);
    assert(recover_feature_graph_document(file, damaged_revision.value, &good_backup.value));
    const auto recovered = load_feature_graph_document(file);
    if (!recovered || recovered.value.graph_version != revised.graph_version ||
        inspect_feature_graph_file_revision(backup).value != good_backup.value) {
        std::cerr << "FeatureGraph recovery damaged its validated backup\n";
        return 1;
    }
    bool damaged_source_preserved = false;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (!entry.path().filename().string().starts_with("Graph.aegfeature.recovery-original-")) continue;
        const auto archived = inspect_feature_graph_file_revision(entry.path());
        damaged_source_preserved = archived && archived.value == damaged_revision.value;
        if (damaged_source_preserved) break;
    }
    if (!damaged_source_preserved) {
        std::cerr << "FeatureGraph recovery discarded the displaced damaged source\n";
        return 1;
    }

    // A second live writer must fail while the first owns the actual file lock.
    const auto live_source = load_feature_graph_source(file);
    assert(live_source);
    std::mutex gate_mutex;
    std::condition_variable gate;
    bool held = false, release = false;
    aeg::foundation::Result first_writer;
    aeg::concurrency::WorkerExecutor holder_executor(
        aeg::concurrency::WorkerExecutorDesc{aeg::concurrency::ExecutorDomain::kIoWorker, 1, false});
    aeg::concurrency::TaskGroup holder_group;
    const auto holder_submission = holder_group.submit(holder_executor, aeg::concurrency::TaskDesc{
        .execute = [&](const aeg::concurrency::TaskContext&) {
        bool paused = false;
        first_writer = aeg::application::authoring::detail::publish_feature_graph_source(
            file, live_source.value.revision.contents, live_source.value.revision,
            [&](const auto& candidate_path) {
                if (!paused && candidate_path.filename().string().find(".tmp-") != std::string::npos) {
                    paused = true;
                    std::unique_lock lock(gate_mutex);
                    held = true;
                    gate.notify_all();
                    gate.wait(lock, [&] { return release; });
                }
                return load_feature_graph_document(candidate_path).result;
            });
        return first_writer;
        },
        .domain = aeg::concurrency::ExecutorDomain::kIoWorker});
    bool observed_lock = false;
    {
        std::unique_lock lock(gate_mutex);
        observed_lock = gate.wait_for(lock, std::chrono::seconds(5), [&] { return held; });
    }
    auto competing_graph = live_source.value.document;
    ++competing_graph.graph_version;
    const auto competing_writer = observed_lock ? save_feature_graph_source(file, competing_graph, &live_source.value.revision)
        : aeg::foundation::ResultValue<FeatureGraphFileRevision>{aeg::foundation::invalid_state("writer gate was not reached"), {}};
    {
        std::lock_guard lock(gate_mutex);
        release = true;
    }
    gate.notify_all();
    const auto holder_result = holder_group.wait(5'000'000'000ull);
    if (!holder_submission || !holder_result || !observed_lock || !first_writer || competing_writer ||
        competing_writer.result.code != aeg::foundation::ResultCode::kNotReady ||
        inspect_feature_graph_file_revision(file).value != live_source.value.revision) {
        std::cerr << "FeatureGraph live writers did not serialize without publishing the losing draft\n";
        return 1;
    }

    const auto unicode_file = root / std::filesystem::path{u8"节点-来源.aegfeature"};
    auto unicode_graph = source;
    unicode_graph.module_requirements = {"模块.来源@1"};
    assert(save_feature_graph_document(unicode_file, unicode_graph));
    const auto unicode_reopened = load_feature_graph_document(unicode_file);
    if (!unicode_reopened || unicode_reopened.value.module_requirements != unicode_graph.module_requirements) {
        std::cerr << "FeatureGraph Unicode path/value did not round-trip\n";
        return 1;
    }

    auto future_graph = source;
    future_graph.schema_version = 999;
    const auto before_future_save = inspect_feature_graph_file_revision(file);
    if (save_feature_graph_document(file, future_graph) ||
        inspect_feature_graph_file_revision(file).value != before_future_save.value) {
        std::cerr << "FeatureGraph accepted an unsupported future source schema\n";
        return 1;
    }

    {
        std::ofstream legacy(file, std::ios::trunc);
        legacy << "schema_version = 1\nasset_uuid = \"asset://old\"\npipeline_nodes = []\n";
    }
    assert(!load_feature_graph_document(file));
    {
        std::ofstream future(file, std::ios::binary | std::ios::trunc);
        future << "schema_version = 999\ngraph_version = 1\nasset_uuid = \"asset://future\"\n";
    }
    if (load_feature_graph_document(file)) {
        std::cerr << "FeatureGraph loaded an unsupported future source schema\n";
        return 1;
    }
    {
        std::ofstream unknown(file, std::ios::binary | std::ios::trunc);
        unknown << "schema_version = 1\ngraph_version = 1\nasset_uuid = \"asset://unknown-node\"\n"
                   "[[nodes]]\nnode_id = 1\ndefinition_id = 1\nversion = [1, 0, 0]\n"
                   "future_binding = \"must-not-disappear\"\n";
    }
    const auto unknown_before = inspect_feature_graph_file_revision(file);
    if (load_feature_graph_document(file) || !unknown_before ||
        inspect_feature_graph_file_revision(file).value != unknown_before.value) {
        std::cerr << "FeatureGraph silently accepted an unknown nested source field\n";
        return 1;
    }

    {
        std::ofstream malformed(file, std::ios::trunc);
        malformed << "schema_version = -1\n"
                     "graph_version = 1\n"
                     "asset_uuid = \"asset://negative-schema\"\n";
    }
    assert(!load_feature_graph_document(file));

    {
        std::ofstream malformed(file, std::ios::trunc);
        malformed << "schema_version = 1\n"
                     "graph_version = \"123junk\"\n"
                     "asset_uuid = \"asset://partial-number\"\n";
    }
    assert(!load_feature_graph_document(file));

    {
        std::ofstream malformed(file, std::ios::trunc);
        malformed << "schema_version = 1\n"
                     "graph_version = 1\n"
                     "asset_uuid = \"asset://invalid-edge\"\n"
                     "[[edges]]\n"
                     "source_node = 1\n"
                     "source_port = 1\n"
                     "target_node = 2\n"
                     "target_port = 2\n"
                     "kind = -1\n"
                     "source_clock = 1\n"
                     "target_clock = 1\n"
                     "version = 1\n"
                     "capacity = 1\n"
                     "deadline_ticks = 1\n";
    }
    assert(!load_feature_graph_document(file));

    {
        std::ofstream malformed(file, std::ios::trunc);
        malformed << "schema_version = 1\n"
                     "graph_version = 1\n"
                     "asset_uuid = \"asset://overflow\"\n"
                     "[[edges]]\n"
                     "source_node = 1\n"
                     "source_port = 1\n"
                     "target_node = 2\n"
                     "target_port = 2\n"
                     "kind = 0\n"
                     "source_clock = 1\n"
                     "target_clock = 1\n"
                     "version = 1\n"
                     "capacity = 4294967296\n"
                     "deadline_ticks = 1\n";
    }
    assert(!load_feature_graph_document(file));
    std::filesystem::remove_all(root, error);
    std::cout << "feature graph document IO smoke passed\n";
    return 0;
}
