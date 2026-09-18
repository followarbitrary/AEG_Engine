#include "editor_composition_storage.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace aeg::application {
namespace {

const LegacyIdentityMapping* find_mapping(std::span<const LegacyIdentityMapping> mappings, std::string_view source,
                                          uint64_t stage, uint64_t task) {
    for (const auto& mapping : mappings)
        if (mapping.source == source && mapping.stage == stage && mapping.task == task)
            return &mapping;
    return nullptr;
}

void unresolved(LegacyCompositionMigration& result, uint32_t line, std::string raw, std::string reason) {
    result.unresolved.push_back({line, std::move(raw), std::move(reason)});
}

pipeline::DocumentEdgeKind edge_kind(std::string_view source) {
    return source == "Game / Renderer" ? pipeline::DocumentEdgeKind::kRenderState
                                       : pipeline::DocumentEdgeKind::kOrdering;
}

}  // namespace

foundation::Result save_editor_composition(const std::filesystem::path& path, EditorCompositionDocument& editor) {
    if (path.empty())
        return foundation::invalid_argument("Composition document path is empty");
    const auto snapshot = editor.snapshot();
    if (!snapshot)
        return foundation::invalid_state("Composition document has no snapshot");
    if (const auto result = project::save_pipeline_document(path, *snapshot); !result)
        return result;
    editor.mark_saved();
    return foundation::Result::success();
}

foundation::ResultValue<LegacyCompositionMigration> import_legacy_pipeline_edits(
    const std::filesystem::path& path, const pipeline::PipelineDocument& baseline,
    std::span<const LegacyIdentityMapping> mappings, const pipeline::TypeCatalog& catalog) {
    (void)catalog;
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {foundation::not_found("Legacy Pipeline sidecar was not found"), {}};
    std::ostringstream bytes;
    bytes << input.rdbuf();
    LegacyCompositionMigration result{baseline, {}, bytes.str()};
    std::istringstream lines(result.original_bytes);
    std::string line;
    uint32_t line_number = 0;
    uint32_t version = 1;
    bool first = true;
    while (std::getline(lines, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (first) {
            first = false;
            std::istringstream header(line);
            std::string signature;
            if (header >> signature >> version && signature == "AEG_PIPELINE_EDITS") {
                if (version < 1 || version > 3)
                    unresolved(result, line_number, line, "unsupported legacy version");
                continue;
            }
            version = 1;
        }
        if (line.empty())
            continue;
        std::istringstream record(line);
        std::string kind;
        record >> kind;
        if (version == 1 && kind != "EDGE" && kind != "LAYOUT" && kind != "BLEND") {
            record.clear();
            record.str(line);
            kind = "EDGE";
        }
        if (kind == "EDGE") {
            std::string source;
            uint64_t ss = 0, st = 0, ds = 0, dt = 0;
            int add = 0;
            if (!(record >> std::quoted(source) >> ss >> st >> ds >> dt >> add)) {
                unresolved(result, line_number, line, "malformed EDGE record");
                continue;
            }
            const auto* from = find_mapping(mappings, source, ss, st);
            const auto* to = find_mapping(mappings, source, ds, dt);
            if (!from || !to || from->node_id.empty() || to->node_id.empty()) {
                unresolved(result, line_number, line, "stage/task identity is unavailable");
                continue;
            }
            const auto kind_value = edge_kind(source);
            auto existing = std::find_if(
                result.document.edges.begin(), result.document.edges.end(), [&](const pipeline::DocumentEdge& edge) {
                    return edge.kind == kind_value && edge.source.node_id == from->node_id &&
                           edge.destination.node_id == to->node_id && edge.source.port_id == from->port_id &&
                           edge.destination.port_id == to->port_id;
                });
            if (add == 0) {
                if (existing != result.document.edges.end())
                    result.document.edges.erase(existing);
                continue;
            }
            if (existing == result.document.edges.end()) {
                pipeline::DocumentEdge edge;
                edge.edge_id = "legacy:" + std::to_string(line_number);
                edge.kind = kind_value;
                edge.source = {from->node_id, kind_value == pipeline::DocumentEdgeKind::kOrdering ? 0 : from->port_id};
                edge.destination = {to->node_id, kind_value == pipeline::DocumentEdgeKind::kOrdering ? 0 : to->port_id};
                edge.binding_id = kind_value == pipeline::DocumentEdgeKind::kOrdering ? 0 : from->port_id;
                result.document.edges.push_back(std::move(edge));
            }
        } else if (kind == "LAYOUT") {
            std::string source;
            uint64_t stage = 0, task = 0;
            double x = 0, y = 0;
            if (!(record >> std::quoted(source) >> stage >> task >> x >> y)) {
                unresolved(result, line_number, line, "malformed LAYOUT record");
                continue;
            }
            const auto* mapping = find_mapping(mappings, source, stage, task);
            if (!mapping || mapping->node_id.empty() || !std::isfinite(x) || !std::isfinite(y)) {
                unresolved(result, line_number, line, "layout identity or coordinates are unavailable");
                continue;
            }
            result.document.layouts.push_back({mapping->node_id, x, y, true});
        } else if (kind == "BLEND") {
            std::string source;
            uint64_t stage = 0, task = 0, mode = 0;
            if (!(record >> std::quoted(source) >> stage >> task >> mode)) {
                unresolved(result, line_number, line, "malformed BLEND record");
                continue;
            }
            const auto* mapping = find_mapping(mappings, source, stage, task);
            if (!mapping || mapping->node_id.empty() || source != "Game / Renderer" || mode > 2) {
                unresolved(result, line_number, line, "blend semantics are unavailable");
                continue;
            }
            result.document.extensions.push_back({mapping->node_id, pipeline::OpaqueAuthoringEncoding::kToml, line});
        } else {
            unresolved(result, line_number, line, "unknown legacy record");
        }
    }
    return {foundation::Result::success(), std::move(result)};
}

foundation::Result save_legacy_migration_report(const std::filesystem::path& path,
                                                const LegacyCompositionMigration& migration) {
    if (path.empty())
        return foundation::invalid_argument("Migration report path is empty");
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        return foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot open migration report");
    output << "AEG_LEGACY_MIGRATION 1\n[original]\n" << migration.original_bytes;
    if (!migration.original_bytes.empty() && migration.original_bytes.back() != '\n')
        output << '\n';
    output << "[unresolved]\n";
    for (const auto& record : migration.unresolved)
        output << record.line << "\t" << record.reason << "\t" << record.raw << '\n';
    return output ? foundation::Result::success()
                  : foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot write migration report");
}

}  // namespace aeg::application
