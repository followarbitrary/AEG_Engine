#pragma once

#include <string>
#include <vector>

#include "pipeline_authoring.hpp"

namespace aeg::application {

enum class EditorGraphKind : uint8_t { kPipeline, kFeature, kPass };

struct EditorGraphAddress final {
    EditorGraphKind kind{EditorGraphKind::kPipeline};
    std::string pipeline_node{};
    std::string feature_node{};
    std::string part_id{};
};

struct EditorPaletteEntry final {
    pipeline::AuthoringTypeKey type{};
    std::string label{};
    bool available{true};
    std::string reason{};
};

struct EditorDiagnosticTarget final {
    EditorGraphAddress graph{};
    std::string node_id{};
    std::string edge_id{};
    uint64_t port_id{};
    uint64_t parameter_id{};
};

[[nodiscard]] foundation::ResultValue<std::vector<EditorPaletteEntry>> build_composition_palette(
    const pipeline::PipelineDocument&, const pipeline::TypeCatalog&, const EditorGraphAddress&);

/** Creates a valid authoring-only contract with no implementation or ports. */
[[nodiscard]] pipeline::TypeCatalogEntry make_empty_custom_type(pipeline::AuthoringTypeKind, std::string_view name);

[[nodiscard]] foundation::ResultValue<EditorDiagnosticTarget> resolve_composition_diagnostic(
    const pipeline::PipelineDocument&, const pipeline::DocumentDiagnostic&);

}  // namespace aeg::application
