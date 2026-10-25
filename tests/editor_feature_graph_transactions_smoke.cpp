/** @brief Authoring transactions translate entry-point allocation failures into Results. */
#include "editor_feature_graph_document.hpp"
#include "editor_feature_graph_layout.hpp"

#include <cstdlib>
#include <iostream>
#include <new>
#include <string_view>

namespace {
bool fail_next_allocation = false;
bool allocation_was_failed = false;
std::size_t allocation_countdown = 1;

void* allocate(std::size_t size) {
    if (fail_next_allocation && --allocation_countdown == 0) {
        fail_next_allocation = false; allocation_was_failed = true;
        throw std::bad_alloc{};
    }
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc{};
}

} // namespace

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
using namespace aeg;
constexpr feature::FeatureNodeId kNode{uint64_t{0x8000000100000007ull}};

void require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

application::EditorFeatureGraphDocument make_document() {
    feature::FeatureGraphDocument graph;
    graph.asset_uuid = "00000000-0000-0000-0000-000000000072";
    graph.schema_version = 1; graph.graph_version = 11; graph.draft_revision = 13;
    graph.nodes = {{kNode, feature::FeatureDefinitionId{uint64_t{101}}, {1, 0, 0}}};
    application::EditorFeatureGraphDocument document{std::move(graph)};
    require(static_cast<bool>(document.execute("Existing history", [](auto& next) {
        ++next.graph_version; return foundation::Result::success();
    })), "Could not seed undo history");
    require(static_cast<bool>(document.execute("Existing redo", [](auto& next) {
        ++next.draft_revision; return foundation::Result::success();
    })) && static_cast<bool>(document.undo()), "Could not seed redo history");
    return document;
}

template <typename Operation>
void allocation_failure_is_atomic(const char* name, Operation operation) {
    auto document = make_document();
    application::FeatureEditorLayout layout;
    layout.node_positions.emplace(kNode.value(), std::pair{40.0f, 60.0f});
    const auto before = document.snapshot_owner();
    const auto revision = document.revision();
    const bool dirty = document.dirty(), undo = document.can_undo(), redo = document.can_redo();
    bool escaped = false;
    foundation::Result result;
    allocation_was_failed = false; allocation_countdown = 1; fail_next_allocation = true;
    try { result = operation(document, layout); }
    catch (const std::bad_alloc&) { escaped = true; }
    catch (...) { fail_next_allocation = false; require(false, "Transaction escaped an unexpected exception"); }
    fail_next_allocation = false;
    std::cout << name << " injected=" << allocation_was_failed << " escaped=" << escaped << '\n';
    require(allocation_was_failed, "Allocation failure injection did not reach the transaction");
    require(!escaped, "Authoring transaction let bad_alloc escape the Result boundary");
    require(result.code == foundation::ResultCode::kOutOfMemory, "Authoring transaction did not return OutOfMemory");
    require(document.snapshot_owner() == before && document.revision() == revision && document.dirty() == dirty &&
            document.can_undo() == undo && document.can_redo() == redo,
            "Failed authoring transaction changed its snapshot, revision or history");
    require(static_cast<bool>(document.redo()) && document.snapshot().draft_revision == before->draft_revision + 1,
            "Failed transaction damaged the pre-existing redo entry");
    require(static_cast<bool>(document.undo()) && document.snapshot_owner() == before,
            "Failed transaction damaged the exact undo snapshot");
}

void layout_allocation_sweep() {
    for (std::size_t ordinal = 1; ordinal <= 256; ++ordinal) {
        auto document = make_document();
        application::FeatureEditorLayout layout;
        layout.node_positions.emplace(kNode.value(), std::pair{40.0f, 60.0f});
        const auto before = document.snapshot_owner();
        const auto revision = document.revision();
        const bool dirty = document.dirty(), undo = document.can_undo(), redo = document.can_redo();
        bool escaped = false;
        foundation::Result result;
        allocation_was_failed = false; allocation_countdown = ordinal; fail_next_allocation = true;
        try { result = document.set_feature_layout(layout); }
        catch (const std::bad_alloc&) { escaped = true; }
        catch (...) { fail_next_allocation = false; require(false, "Layout sweep escaped an unexpected exception"); }
        fail_next_allocation = false;
        std::cout << "layout ordinal=" << ordinal << " injected=" << allocation_was_failed <<
            " escaped=" << escaped << " code=" << static_cast<int>(result.code) << '\n';
        require(!escaped, "A later layout allocation escaped the Result boundary");
        if (!allocation_was_failed) {
            require(static_cast<bool>(result), "Uninjected layout operation failed during the allocation sweep");
            const auto committed = document.snapshot_owner();
            require(committed != before && committed->layout_revision == before->layout_revision + 1 &&
                    committed->graph_version == before->graph_version && committed->draft_revision == before->draft_revision &&
                    committed->contract_digest == before->contract_digest && document.revision() == revision + 1 &&
                    !document.can_redo(), "Uninjected layout transaction did not commit exactly once");
            require(static_cast<bool>(document.undo()) && document.snapshot_owner() == before,
                    "Successful layout transaction did not retain its exact prior undo snapshot");
            require(static_cast<bool>(document.redo()) && document.snapshot_owner() == committed,
                    "Successful layout transaction did not retain its exact redo snapshot");
            std::cout << "layout sweep reached complete success at ordinal " << ordinal << '\n';
            return;
        }
        require(result.code == foundation::ResultCode::kOutOfMemory,
                "An injected layout allocation did not return OutOfMemory");
        require(document.snapshot_owner() == before && document.revision() == revision && document.dirty() == dirty &&
                document.can_undo() == undo && document.can_redo() == redo,
                "A failed layout allocation changed its snapshot, revision or history");
        require(static_cast<bool>(document.redo()) && document.snapshot().draft_revision == before->draft_revision + 1,
                "A failed layout allocation damaged the pre-existing redo content");
        require(static_cast<bool>(document.undo()) && document.snapshot_owner() == before,
                "A failed layout allocation damaged the exact prior undo snapshot");
    }
    require(false, "Layout allocation sweep never reached an uninjected complete transaction");
}
} // namespace

int main(int argc, char** argv) {
    const std::string_view selected = argc > 1 ? std::string_view{argv[1]} : std::string_view{};
    if (selected.empty() || selected == "--erase") allocation_failure_is_atomic("erase_nodes", [](auto& document, const auto&) {
        return document.erase_nodes(std::span<const feature::FeatureNodeId>{&kNode, 1});
    });
    if (selected.empty() || selected == "--clone") allocation_failure_is_atomic("clone_nodes", [](auto& document, const auto&) {
        return document.clone_nodes(std::span<const feature::FeatureNodeId>{&kNode, 1});
    });
    if (selected.empty() || selected == "--layout") allocation_failure_is_atomic("set_feature_layout", [](auto& document, const auto& layout) {
        return document.set_feature_layout(layout);
    });
    if (selected == "--layout-sweep") layout_allocation_sweep();
    require(selected.empty() || selected == "--erase" || selected == "--clone" || selected == "--layout" ||
            selected == "--layout-sweep", "Unknown transaction test case");
}
