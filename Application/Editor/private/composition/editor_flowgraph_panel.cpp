#include "editor_flowgraph_panel.hpp"
#include "editor_custom_node_workflow.hpp"
#include "editor_flowgraph_geometry.hpp"
#include "editor_node_contract_storage.hpp"

#include <imgui_internal.h>

#include <array>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <optional>
#include <sstream>
#include <unordered_map>

namespace aeg::application {
namespace {

// The palette uses a small, stable payload rather than the display label. This
// lets a renamed catalog entry keep creating the exact authoring type that was
// dragged, and prevents accidental insertion of an item from another graph.
struct FlowGraphPalettePayload final {
    uint8_t kind{};
    uint64_t type_id{};
};

struct FlowGraphCategoryPayload final {
    uint8_t kind{};
};

constexpr const char* palette_payload_type = "AEG.FlowGraph.NodeType";
constexpr const char* palette_category_payload_type = "AEG.FlowGraph.NodeCategory";

ImU32 node_color(EditorGraphNodeKind kind, bool unknown) {
    if (unknown)
        return IM_COL32(90, 96, 104, 255);
    switch (kind) {
        case EditorGraphNodeKind::kPipeline: return IM_COL32(46, 109, 150, 255);
        case EditorGraphNodeKind::kStage:
        case EditorGraphNodeKind::kFeaturePoint: return IM_COL32(62, 96, 129, 255);
        case EditorGraphNodeKind::kFeature: return IM_COL32(46, 137, 104, 255);
        case EditorGraphNodeKind::kPass: return IM_COL32(112, 83, 160, 255);
        case EditorGraphNodeKind::kResource: return IM_COL32(155, 111, 48, 255);
        case EditorGraphNodeKind::kPart: return IM_COL32(83, 93, 100, 255);
    }
    return IM_COL32(70, 80, 90, 255);
}

ImU32 node_header_color(EditorGraphNodeKind kind, bool unknown) {
    if (unknown)
        return IM_COL32(91, 101, 112, 255);
    switch (kind) {
        case EditorGraphNodeKind::kPipeline: return IM_COL32(67, 157, 218, 255);
        case EditorGraphNodeKind::kFeature: return IM_COL32(61, 177, 126, 255);
        case EditorGraphNodeKind::kPass: return IM_COL32(242, 144, 44, 255);
        case EditorGraphNodeKind::kResource: return IM_COL32(43, 164, 226, 255);
        case EditorGraphNodeKind::kPart: return IM_COL32(99, 112, 130, 255);
        default: return IM_COL32(78, 137, 181, 255);
    }
}

ImU32 port_color(pipeline::PortValueKind kind) {
    switch (kind) {
        case pipeline::PortValueKind::kArtifact: return IM_COL32(66, 197, 192, 255);
        case pipeline::PortValueKind::kTexture:
        case pipeline::PortValueKind::kRenderResource: return IM_COL32(181, 83, 255, 255);
        case pipeline::PortValueKind::kBuffer: return IM_COL32(56, 156, 230, 255);
        case pipeline::PortValueKind::kRenderState: return IM_COL32(255, 196, 57, 255);
    }
    return IM_COL32(165, 176, 187, 255);
}

const EditorGraphNodeView* find_node(const EditorCompositionGraphView& view, std::string_view id) {
    const auto found = std::find_if(view.nodes.begin(), view.nodes.end(),
                                    [id](const auto& node) { return node.node_id == id; });
    return found == view.nodes.end() ? nullptr : &*found;
}

const pipeline::ResourceNodeDocument* find_resource(const EditorCompositionDocument& document,
                                                    std::string_view node_id) {
    const auto snapshot = document.snapshot();
    const auto found = std::find_if(snapshot->resources.begin(), snapshot->resources.end(),
                                    [node_id](const auto& resource) { return resource.node_id == node_id; });
    return found == snapshot->resources.end() ? nullptr : &*found;
}

const pipeline::ParameterBlock* parameters_for_node(const EditorCompositionDocument& document,
                                                    std::string_view node_id) {
    const auto snapshot = document.snapshot();
    for (const auto& node : snapshot->pipeline_nodes)
        if (node.node_id == node_id)
            return &node.parameters;
    for (const auto& node : snapshot->feature_nodes)
        if (node.node_id == node_id)
            return &node.parameters;
    for (const auto& node : snapshot->pass_nodes)
        if (node.node_id == node_id)
            return &node.parameters;
    return nullptr;
}

const pipeline::TypeCatalogEntry* type_for_node(const EditorCompositionDocument& document,
                                                const EditorGraphAddress& address,
                                                const EditorGraphNodeView& node) {
    const auto snapshot = document.snapshot();
    if (node.kind == EditorGraphNodeKind::kPipeline) {
        const auto found = std::find_if(snapshot->pipeline_nodes.begin(), snapshot->pipeline_nodes.end(),
                                        [&](const auto& value) { return value.node_id == node.node_id; });
        return found == snapshot->pipeline_nodes.end()
                   ? nullptr
                   : document.catalog().find({pipeline::AuthoringTypeKind::kPipeline, found->type.value()});
    }
    if (node.kind == EditorGraphNodeKind::kFeature) {
        const auto found = std::find_if(snapshot->feature_nodes.begin(), snapshot->feature_nodes.end(),
                                        [&](const auto& value) { return value.node_id == node.node_id; });
        return found == snapshot->feature_nodes.end()
                   ? nullptr
                   : document.catalog().find({pipeline::AuthoringTypeKind::kFeature, found->type.value()});
    }
    if (node.kind == EditorGraphNodeKind::kPass) {
        const auto found = std::find_if(snapshot->pass_nodes.begin(), snapshot->pass_nodes.end(),
                                        [&](const auto& value) { return value.node_id == node.node_id; });
        return found == snapshot->pass_nodes.end()
                   ? nullptr
                   : document.catalog().find({pipeline::AuthoringTypeKind::kRenderPass, found->type.value()});
    }
    (void)address;
    return nullptr;
}

std::string graph_scope(const EditorGraphAddress& address) {
    return std::to_string(static_cast<uint32_t>(address.kind)) + ":" + address.pipeline_node + ":" +
           address.feature_node + ":" + address.part_id;
}

// A Pass graph entered from a Feature can target its sole render part
// directly.  When a Feature declares multiple render parts there is no safe
// implicit choice, so the empty part id keeps the aggregate view and the
// graph selector remains the explicit part chooser.
std::string single_render_part(const EditorCompositionDocument& document, std::string_view feature_node_id) {
    const auto snapshot = document.snapshot();
    const auto feature = std::find_if(snapshot->feature_nodes.begin(), snapshot->feature_nodes.end(),
                                      [&](const auto& value) { return value.node_id == feature_node_id; });
    if (feature == snapshot->feature_nodes.end())
        return {};
    const auto* entry = document.catalog().find({pipeline::AuthoringTypeKind::kFeature, feature->type.value()});
    if (entry == nullptr)
        return {};
    const auto* declaration = std::get_if<pipeline::FeatureAuthoringDeclaration>(&entry->declaration);
    if (declaration == nullptr)
        return {};
    std::string result;
    for (const auto& part : declaration->parts) {
        if (part.domain != pipeline::FeatureDomain::kRender)
            continue;
        if (!result.empty())
            return {};
        result = part.name;
    }
    return result;
}

float node_height(const EditorGraphNodeView& node) {
    const auto inputs = static_cast<std::size_t>(std::count_if(node.ports.begin(), node.ports.end(),
                                                                [](const auto& port) { return port.input; }));
    const auto outputs = node.ports.size() - inputs;
    return flowgraph::node_layout(inputs, outputs).height();
}

bool overlaps(const ImVec2& candidate, const ImVec2& size, const ImVec2& other, const ImVec2& other_size,
              float gap = 18.0f) {
    return candidate.x < other.x + other_size.x + gap && candidate.x + size.x + gap > other.x &&
           candidate.y < other.y + other_size.y + gap && candidate.y + size.y + gap > other.y;
}

float point_segment_distance(ImVec2 point, ImVec2 a, ImVec2 b) {
    const ImVec2 delta{b.x - a.x, b.y - a.y};
    const float length_sq = delta.x * delta.x + delta.y * delta.y;
    if (length_sq <= 0.001f)
        return std::hypot(point.x - a.x, point.y - a.y);
    const float t = std::clamp(((point.x - a.x) * delta.x + (point.y - a.y) * delta.y) / length_sq, 0.0f, 1.0f);
    const ImVec2 projection{a.x + delta.x * t, a.y + delta.y * t};
    return std::hypot(point.x - projection.x, point.y - projection.y);
}

ImVec2 find_free_position(const EditorGraphNodeView& node, const std::vector<std::pair<ImVec2, ImVec2>>& occupied) {
    constexpr float kWidth = 220.0f;
    constexpr float kGap = 28.0f;
    const ImVec2 size{kWidth, node_height(node)};
    for (std::size_t row = 0; row < 10000; ++row) {
        for (std::size_t column = 0; column < 8; ++column) {
            const ImVec2 candidate{32.0f + static_cast<float>(column) * (kWidth + kGap),
                                   32.0f + static_cast<float>(row) * (150.0f + kGap)};
            bool free = true;
            for (const auto& [other, other_size] : occupied) {
                if (overlaps(candidate, size, other, other_size)) {
                    free = false;
                    break;
                }
            }
            if (free)
                return candidate;
        }
    }
    return {32.0f, 32.0f};
}

void draw_ellipsis(ImDrawList* draw_list, ImVec2 min, float max_x, std::string_view text, ImU32 color) {
    std::string visible{text};
    const float width = std::max(0.0f, max_x - min.x);
    if (ImGui::CalcTextSize(visible.c_str()).x > width) {
        constexpr std::string_view suffix = "...";
        while (!visible.empty() && ImGui::CalcTextSize((visible + std::string{suffix}).c_str()).x > width) {
            // Remove one complete UTF-8 code point. Popping one byte could
            // leave a malformed label and make ImGui measure/render it
            // inconsistently, especially for Chinese node names.
            visible.pop_back();
            while (!visible.empty() && (static_cast<unsigned char>(visible.back()) & 0xC0u) == 0x80u)
                visible.pop_back();
        }
        visible += suffix;
    }
    draw_list->AddText(min, color, visible.c_str());
}

std::string graph_type_prefix(EditorGraphKind kind) {
    switch (kind) {
        case EditorGraphKind::kPipeline: return "Pipeline";
        case EditorGraphKind::kFeature: return "Feature";
        case EditorGraphKind::kPass: return "Pass";
    }
    return "Node";
}

bool catalog_has_name(const pipeline::TypeCatalog& catalog, pipeline::AuthoringTypeKind kind,
                      std::string_view name) {
    for (const auto& entry : catalog.entries())
        if (entry.key.kind == kind && entry.name == name)
            return true;
    return false;
}

std::string unique_type_name(const pipeline::TypeCatalog& catalog, pipeline::AuthoringTypeKind kind,
                             std::string name) {
    if (name.empty())
        name = "Editor.Custom." + graph_type_prefix(kind == pipeline::AuthoringTypeKind::kPipeline
                                                          ? EditorGraphKind::kPipeline
                                                          : kind == pipeline::AuthoringTypeKind::kFeature
                                                                ? EditorGraphKind::kFeature
                                                                : EditorGraphKind::kPass);
    if (name.find('.') == std::string::npos)
        name = "Editor.Custom." + name;
    const std::string original = name;
    for (uint32_t suffix = 2; catalog_has_name(catalog, kind, name) ||
                               (kind == pipeline::AuthoringTypeKind::kPipeline &&
                                catalog.find({kind, pipeline::PipelineId(name).value()}) != nullptr) ||
                               (kind == pipeline::AuthoringTypeKind::kFeature &&
                                catalog.find({kind, pipeline::FeatureId(name).value()}) != nullptr) ||
                               (kind == pipeline::AuthoringTypeKind::kRenderPass &&
                                catalog.find({kind, pipeline::RenderPassTypeId(name).value()}) != nullptr);
         ++suffix)
        name = original + std::to_string(suffix);
    return name;
}

std::string make_node_id(const pipeline::PipelineDocument& snapshot, std::string base) {
    std::replace_if(base.begin(), base.end(), [](char character) {
        return !std::isalnum(static_cast<unsigned char>(character)) && character != '_';
    }, '_');
    if (base.empty())
        base = "CustomNode";
    std::string candidate = base;
    const auto exists = [&snapshot](std::string_view id) {
        return std::any_of(snapshot.pipeline_nodes.begin(), snapshot.pipeline_nodes.end(),
                           [id](const auto& node) { return node.node_id == id; }) ||
               std::any_of(snapshot.feature_nodes.begin(), snapshot.feature_nodes.end(),
                           [id](const auto& node) { return node.node_id == id; }) ||
               std::any_of(snapshot.pass_nodes.begin(), snapshot.pass_nodes.end(),
                           [id](const auto& node) { return node.node_id == id; });
    };
    for (uint32_t suffix = 2; exists(candidate); ++suffix)
        candidate = base + "_" + std::to_string(suffix);
    return candidate;
}

foundation::Result create_empty_node(EditorCompositionDocument& document, const EditorGraphAddress& address,
                                     std::string requested_name, std::string* created_node_id,
                                     std::string* created_type_name = nullptr,
                                     pipeline::AuthoringTypeKey* created_type_key = nullptr) {
    using namespace pipeline;
    const auto kind = address.kind == EditorGraphKind::kPipeline
                          ? AuthoringTypeKind::kPipeline
                          : address.kind == EditorGraphKind::kFeature ? AuthoringTypeKind::kFeature
                                                                       : AuthoringTypeKind::kRenderPass;
    const std::string type_name = unique_type_name(document.catalog(), kind, std::move(requested_name));
    auto entry = make_empty_custom_type(kind, type_name);
    const auto entry_key = entry.key;
    std::optional<pipeline::TypeCatalogEntry> owner_replacement;

    if (address.kind == EditorGraphKind::kPass) {
        const auto snapshot = document.snapshot();
        const auto feature = std::find_if(snapshot->feature_nodes.begin(), snapshot->feature_nodes.end(),
                                          [&](const auto& node) { return node.node_id == address.feature_node; });
        if (feature == snapshot->feature_nodes.end())
            return foundation::not_found("Pass owner Feature node does not exist");
        const auto* owner = document.catalog().find({AuthoringTypeKind::kFeature, feature->type.value()});
        if (owner == nullptr)
            return foundation::not_found("Pass owner Feature type is unavailable");
        auto owner_copy = *owner;
        auto* declaration = std::get_if<FeatureAuthoringDeclaration>(&owner_copy.declaration);
        if (declaration == nullptr)
            return foundation::invalid_argument("Pass owner Feature declaration is invalid");
        if (!address.part_id.empty()) {
            const auto part = std::find_if(declaration->parts.begin(), declaration->parts.end(),
                                           [&](const auto& value) { return value.name == address.part_id; });
            if (part == declaration->parts.end() || part->domain != FeatureDomain::kRender)
                return foundation::invalid_argument("Pass part is not a declared render part");
        } else if (!declaration->parts.empty()) {
            return foundation::invalid_argument("Pass insertion requires a declared render part");
        }
        const bool already_allowed =
            std::find(declaration->allowed_pass_types.begin(), declaration->allowed_pass_types.end(),
                      RenderPassTypeId(entry.key.id)) != declaration->allowed_pass_types.end();
        // Registering a Pass from the FlowGraph is an explicit authoring
        // action. Built-in Features therefore receive the same editable
        // contract treatment as editor-owned Features; the generated Pass
        // still has to provide its runtime implementation before execution.
        if (!already_allowed) {
            declaration->allow_add_passes = true;
            declaration->allowed_pass_types.push_back(RenderPassTypeId(entry.key.id));
        }
        owner_replacement = std::move(owner_copy);
    } else {
        if (address.kind == EditorGraphKind::kFeature) {
            const auto snapshot = document.snapshot();
            const auto pipeline = std::find_if(snapshot->pipeline_nodes.begin(), snapshot->pipeline_nodes.end(),
                                               [&](const auto& node) { return node.node_id == address.pipeline_node; });
            if (pipeline == snapshot->pipeline_nodes.end())
                return foundation::not_found("Feature owner Pipeline node does not exist");
            const auto* pipeline_entry =
                document.catalog().find({AuthoringTypeKind::kPipeline, pipeline->type.value()});
            const auto* declaration = pipeline_entry == nullptr
                                          ? nullptr
                                          : std::get_if<PipelineAuthoringDeclaration>(&pipeline_entry->declaration);
            if (declaration == nullptr || declaration->feature_points.empty())
                return foundation::invalid_argument("Pipeline has no FeaturePoint for a custom Feature");
        }
    }

    std::string attachment;
    if (address.kind == EditorGraphKind::kFeature) {
        const auto snapshot = document.snapshot();
        const auto pipeline = std::find_if(snapshot->pipeline_nodes.begin(), snapshot->pipeline_nodes.end(),
                                           [&](const auto& node) { return node.node_id == address.pipeline_node; });
        if (pipeline == snapshot->pipeline_nodes.end())
            return foundation::not_found("Feature owner Pipeline node does not exist");
        const auto* pipeline_entry = document.catalog().find({AuthoringTypeKind::kPipeline, pipeline->type.value()});
        const auto* declaration = pipeline_entry == nullptr
                                      ? nullptr
                                      : std::get_if<PipelineAuthoringDeclaration>(&pipeline_entry->declaration);
        if (declaration == nullptr || declaration->feature_points.empty())
            return foundation::invalid_argument("Pipeline has no FeaturePoint for a custom Feature");
        attachment = std::to_string(declaration->feature_points.front().id.value());
    } else if (address.kind == EditorGraphKind::kPass) {
        attachment = address.part_id;
    }
    const auto snapshot = document.snapshot();
    const std::string node_id = make_node_id(*snapshot, "Custom_" + graph_type_prefix(address.kind));
    std::ostringstream marker;
    uint64_t owner_feature = 0;
    if (address.kind == EditorGraphKind::kPass) {
        const auto owner = std::find_if(snapshot->feature_nodes.begin(), snapshot->feature_nodes.end(),
                                        [&](const auto& node) { return node.node_id == address.feature_node; });
        if (owner != snapshot->feature_nodes.end())
            owner_feature = owner->type.value();
    }
    marker << "AEG_FLOWGRAPH_EMPTY 1 " << static_cast<uint32_t>(kind) << ' ' << std::quoted(type_name) << ' '
           << owner_feature << ' ' << std::quoted(node_id);
    pipeline::TypeCatalog next_catalog;
    for (const auto& existing : document.catalog().entries()) {
        if (owner_replacement && existing.key == owner_replacement->key)
            continue;
        if (const auto result = next_catalog.add(existing); !result)
            return result;
    }
    if (owner_replacement) {
        if (const auto result = next_catalog.add(*owner_replacement); !result)
            return result;
    }
    if (const auto result = next_catalog.add(entry); !result)
        return result;
    EditorCompositionCommands commands(document);
    const auto result = commands.add_type_with_catalog(address, entry_key, node_id, std::move(next_catalog),
                                                       std::move(attachment), marker.str());
    if (!result)
        return result;
    if (created_node_id != nullptr)
        *created_node_id = node_id;
    if (created_type_name != nullptr)
        *created_type_name = type_name;
    if (created_type_key != nullptr)
        *created_type_key = entry_key;
    return foundation::Result::success();
}

}  // namespace

void EditorFlowGraphPanel::draw(EditorCompositionDocument& document, const EditorGraphAddress& address,
                                const std::filesystem::path& project_root) {
    draw(document, address, project_root, Actions{});
}

void EditorFlowGraphPanel::draw(EditorCompositionDocument& document, const EditorGraphAddress& address,
                                const std::filesystem::path& project_root, const Actions& actions) {
    project_root_ = project_root;
    if (!address_initialized_ || document_identity_ != &document) {
        address_ = address;
        address_initialized_ = true;
        document_identity_ = &document;
        last_child_address_ = {};
        has_last_child_address_ = false;
        restore_child_after_pipeline_switch_ = false;
        position_scope_ = graph_scope(address_);
        migrated_pass_layout_scope_.clear();
        positions_.clear();
        selected_node_.clear();
        selected_edge_.clear();
        connect_source_node_.clear();
        connect_source_port_ = 0;
        connect_source_slot_ = 0;
        canvas_pan_ = {};
        canvas_pan_active_ = false;
        canvas_pan_candidate_ = false;
    }
    if (position_scope_ != graph_scope(address_)) {
        position_scope_ = graph_scope(address_);
        migrated_pass_layout_scope_.clear();
        positions_.clear();
        selected_node_.clear();
        selected_edge_.clear();
        connect_source_node_.clear();
        connect_source_port_ = 0;
        connect_source_slot_ = 0;
        canvas_pan_ = {};
        canvas_pan_active_ = false;
        canvas_pan_candidate_ = false;
    }

    // A native FlowGraph window can outlive the graph tab that opened it.  In
    // that case an old child address may refer to a type name (or a deleted
    // feature) instead of a live feature instance.  Do not leave the user in
    // an empty error window: recover to the owning pipeline graph and let the
    // graph selector provide the valid child entries again.
    const auto document_snapshot = document.snapshot();
    if (address_.kind != EditorGraphKind::kPipeline) {
        const auto pipeline = std::find_if(document_snapshot->pipeline_nodes.begin(),
                                          document_snapshot->pipeline_nodes.end(),
                                          [&](const auto& node) {
                                              return node.node_id == address_.pipeline_node;
                                          });
        const auto feature = std::find_if(document_snapshot->feature_nodes.begin(),
                                          document_snapshot->feature_nodes.end(),
                                          [&](const auto& node) {
                                              return node.node_id == address_.feature_node;
                                          });
        // Feature graphs are the collection view for a Pipeline's Features,
        // so their address intentionally carries an empty feature_node.  Pass
        // graphs still require a concrete Feature owner.  Keep the recovery
        // guard strict for invalid non-empty selections without rejecting the
        // valid collection address used by Pipeline double-click navigation.
        const bool feature_required = address_.kind == EditorGraphKind::kPass || !address_.feature_node.empty();
        if (pipeline == document_snapshot->pipeline_nodes.end() ||
            (feature_required && (feature == document_snapshot->feature_nodes.end() ||
                                  feature->pipeline_node != pipeline->node_id)) ||
            (address_.kind == EditorGraphKind::kPass &&
             feature->pipeline_node != address_.pipeline_node)) {
            address_ = {EditorGraphKind::kPipeline, {}, {}, {}};
            position_scope_ = graph_scope(address_);
            positions_.clear();
            selected_node_.clear();
            selected_edge_.clear();
            connect_source_node_.clear();
            connect_source_port_ = 0;
            connect_source_slot_ = 0;
            last_action_ = "Recovered an unavailable child graph; showing its Pipeline graph";
        }
    }
    if (address_.kind != EditorGraphKind::kPipeline) {
        last_child_address_ = address_;
        has_last_child_address_ = true;
    }
    const auto view_result = project_composition_graph(*document.snapshot(), document.catalog(), address_);
    const auto graph_label = [&] {
        if (address_.kind == EditorGraphKind::kPipeline)
            return std::string{"Pipeline"};
        if (address_.kind == EditorGraphKind::kFeature)
            return std::string{"Feature / "} + address_.pipeline_node;
        return address_.part_id.empty() ? std::string{"Pass / "} + address_.feature_node
                                        : std::string{"Pass / "} + address_.feature_node + " / " + address_.part_id;
    }();
    ImGui::SetNextWindowSizeConstraints(ImVec2{960.0f, 640.0f}, ImVec2{FLT_MAX, FLT_MAX});
    const ImGuiWindowFlags flowgraph_window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                                    (actions.native_window ? ImGuiWindowFlags_NoMove
                                                                            : ImGuiWindowFlags_None) |
                                                    ImGuiWindowFlags_NoScrollbar |
                                                    ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2{aeg::editor::kEditorWindowContentPadding,
                               aeg::editor::kEditorWindowContentPadding});
    ImGui::Begin("Flow Graph###FlowGraph", nullptr, flowgraph_window_flags);
    ImGui::PopStyleVar();
    if (actions.native_window) {
        const std::size_t data_flow_count = view_result
                                                 ? static_cast<std::size_t>(std::count_if(
                                                       view_result.value.edges.begin(), view_result.value.edges.end(),
                                                       [](const auto& edge) { return !edge.fixed; }))
                                                 : 0U;
        const std::size_t structural_relation_count =
            view_result ? view_result.value.edges.size() - data_flow_count : 0U;
        std::string status = graph_label + "  |  " +
                             std::to_string(view_result ? view_result.value.nodes.size() : 0) + " nodes  /  " +
                             std::to_string(data_flow_count) + " data flows";
        if (structural_relation_count != 0U)
            status += "  /  " + std::to_string(structural_relation_count) + " structural relations";
        aeg::editor::draw_editor_window_chrome("Flow Graph", status, actions.window_actions);
    }
    if (!view_result) {
        ImGui::TextColored(ImVec4{1.0f, 0.35f, 0.25f, 1.0f}, "Graph unavailable: %s",
                           view_result.result.message.c_str());
        ImGui::End();
        return;
    }
    const auto& view = view_result.value;
    const std::size_t data_flow_count = static_cast<std::size_t>(std::count_if(
        view.edges.begin(), view.edges.end(), [](const auto& edge) { return !edge.fixed; }));
    const std::size_t structural_relation_count = view.edges.size() - data_flow_count;
    node_definition_service_.set_project_documents({&document});

    if (ImGui::BeginChild("##flowgraph-tabs", ImVec2{0.0f, 32.0f}, false,
                          ImGuiWindowFlags_NoScrollbar)) {
        const bool on_pipeline_graph = address_.kind == EditorGraphKind::kPipeline;
        const char* active_tab = on_pipeline_graph ? "Pipeline Graph" : graph_label.c_str();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.08f, 0.18f, 0.23f, 1.0f});
        if (ImGui::Button("Main Graph") && !on_pipeline_graph) {
            last_child_address_ = address_;
            has_last_child_address_ = true;
            restore_child_after_pipeline_switch_ = true;
            address_ = {EditorGraphKind::kPipeline, {}, {}, {}};
            position_scope_ = graph_scope(address_);
            positions_.clear();
            selected_node_.clear();
            selected_edge_.clear();
            connect_source_node_.clear();
            connect_source_port_ = connect_source_slot_ = 0;
            last_action_ = "Opened Pipeline graph";
        }
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.10f, 0.27f, 0.30f, 1.0f});
        if (ImGui::Button(active_tab) && on_pipeline_graph && has_last_child_address_) {
            address_ = last_child_address_;
            restore_child_after_pipeline_switch_ = false;
            position_scope_ = graph_scope(address_);
            positions_.clear();
            selected_node_.clear();
            selected_edge_.clear();
            connect_source_node_.clear();
            connect_source_port_ = connect_source_slot_ = 0;
            last_action_ = "Restored child graph";
        }
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::Button("+");
    }
    ImGui::EndChild();
    ImGui::TextDisabled("Home  >  Pipeline");
    ImGui::SameLine();
    ImGui::TextDisabled(">  %s", graph_label.c_str());
    ImGui::Separator();
    ImGui::Text("%s Graph", graph_label.c_str());
    ImGui::SameLine();
    if (structural_relation_count == 0U) {
        ImGui::TextDisabled("%zu nodes / %zu data flows", view.nodes.size(), data_flow_count);
    } else {
        ImGui::TextDisabled("%zu nodes / %zu data flows / %zu structural relations", view.nodes.size(),
                            data_flow_count, structural_relation_count);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("zoom %.2f", zoom_);
    ImGui::SameLine();
    if (ImGui::Button("Validate##flowgraph")) {
        last_action_ = document.diagnostics().empty() ? "FlowGraph is valid" :
                       "FlowGraph has " + std::to_string(document.diagnostics().size()) + " diagnostic(s)";
    }
    ImGui::SameLine();
    if (!actions.save_draft)
        ImGui::BeginDisabled();
    if (ImGui::Button("Save Draft##flowgraph") && actions.save_draft) {
        const auto result = actions.save_draft();
        last_action_ = result ? "FlowGraph draft saved" : result.message;
    }
    if (!actions.save_draft)
        ImGui::EndDisabled();
    ImGui::SameLine();
    const bool apply_enabled = static_cast<bool>(actions.apply_to_viewport) && document.diagnostics().empty();
    if (!apply_enabled)
        ImGui::BeginDisabled();
    if (ImGui::Button("Apply to Viewport##flowgraph") && actions.apply_to_viewport) {
        const auto result = actions.apply_to_viewport();
        last_action_ = result ? "Applied FlowGraph to viewport" : result.message;
    }
    if (!apply_enabled)
        ImGui::EndDisabled();
    ImGui::SameLine();
    if (!actions.restore_default_viewport)
        ImGui::BeginDisabled();
    if (ImGui::Button("Restore Default##flowgraph") && actions.restore_default_viewport) {
        const auto result = actions.restore_default_viewport();
        last_action_ = result ? "Restored default viewport pipeline" : result.message;
    }
    if (!actions.restore_default_viewport)
        ImGui::EndDisabled();
    if (actions.runtime_status) {
        const std::string status = actions.runtime_status();
        if (!status.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", status.c_str());
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(show_runtime_inspect_ ? "Authoring Graph##flowgraph" : "Runtime Inspect##flowgraph"))
        show_runtime_inspect_ = !show_runtime_inspect_;
    ImGui::SameLine();
    ImGui::BeginDisabled(!document.can_undo());
    if (ImGui::SmallButton("Undo##flowgraph")) {
        const auto result = document.undo();
        last_action_ = result ? "Undid FlowGraph edit" : result.message;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!document.can_redo());
    if (ImGui::SmallButton("Redo##flowgraph")) {
        const auto result = document.redo();
        last_action_ = result ? "Redid FlowGraph edit" : result.message;
    }
    ImGui::EndDisabled();
    for (const auto& diagnostic : document.diagnostics()) {
        if (diagnostic.node_id.empty())
            continue;
        ImGui::NewLine();
        ImGui::TextColored(ImVec4{1.0f, 0.72f, 0.22f, 1.0f}, "! %s", diagnostic.code.c_str());
        break;
    }
    ImGui::Separator();

    if (show_runtime_inspect_) {
        ImGui::TextDisabled("Runtime Inspect is read-only and shows the currently active executable plans.");
        if (!actions.runtime_inspect) {
            ImGui::TextDisabled("Runtime plan inspection is unavailable until a project runtime is initialized.");
        } else {
            const auto snapshots = actions.runtime_inspect();
            if (snapshots.empty()) {
                ImGui::TextDisabled("No active runtime plan. Apply a valid FlowGraph or start the project runtime.");
            } else {
                for (const auto& snapshot : snapshots) {
                    ImGui::PushID(snapshot.source.c_str());
                    const std::string header = snapshot.source + "  |  revision " + std::to_string(snapshot.revision) +
                                               "  |  " + std::to_string(snapshot.nodes.size()) + " nodes  |  " +
                                               std::to_string(snapshot.dependencies.size()) + " dependencies";
                    if (ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                        if (!snapshot.graph_asset.empty()) {
                            ImGui::Text("Applied asset: %s / version %llu", snapshot.graph_asset.c_str(),
                                        static_cast<unsigned long long>(snapshot.graph_version));
                            ImGui::TextDisabled("Semantic digest: %s", snapshot.semantic_digest.c_str());
                        } else {
                            ImGui::TextDisabled("Author source mapping is unavailable for this active plan.");
                        }
                        ImGui::BeginChild("RuntimeInspectPlan", ImVec2{0.0f, 180.0f}, true,
                                          ImGuiWindowFlags_HorizontalScrollbar);
                        if (snapshot.nodes.empty()) {
                            ImGui::TextDisabled("The active plan contains no executable nodes.");
                        } else {
                            ImGui::Columns(6, "RuntimeInspectColumns", true);
                            ImGui::TextUnformatted("Index");
                            ImGui::NextColumn();
                            ImGui::TextUnformatted("Stage / Task");
                            ImGui::NextColumn();
                            ImGui::TextUnformatted("In");
                            ImGui::NextColumn();
                            ImGui::TextUnformatted("Out");
                            ImGui::NextColumn();
                            ImGui::TextUnformatted("Kind");
                            ImGui::NextColumn();
                            ImGui::TextUnformatted("Author source");
                            ImGui::NextColumn();
                            ImGui::Separator();
                            for (const auto& node : snapshot.nodes) {
                                ImGui::Text("%u", node.index);
                                ImGui::NextColumn();
                                ImGui::Text("0x%llx / 0x%llx", static_cast<unsigned long long>(node.stage_id),
                                            static_cast<unsigned long long>(node.task_id));
                                ImGui::NextColumn();
                                ImGui::Text("%u", node.incoming_dependencies);
                                ImGui::NextColumn();
                                ImGui::Text("%u", node.outgoing_dependencies);
                                ImGui::NextColumn();
                                ImGui::TextUnformatted(node.synthetic ? "Synthetic" : "Executable");
                                ImGui::NextColumn();
                                ImGui::TextUnformatted(node.author_node.empty() ? "Unmapped" : node.author_node.c_str());
                                ImGui::NextColumn();
                            }
                            ImGui::Columns(1);
                        }
                        if (ImGui::TreeNode("Dependencies")) {
                            for (const auto& dependency : snapshot.dependencies) {
                                ImGui::Text("%u -> %u  %s  0x%llx", dependency.source, dependency.destination,
                                            dependency.kind == pipeline::PipelineDependencyKind::kArtifact ? "Artifact" :
                                                                                                              "Fixed ordering",
                                            static_cast<unsigned long long>(dependency.artifact));
                            }
                            ImGui::TreePop();
                        }
                        ImGui::EndChild();
                    }
                    ImGui::PopID();
                }
            }
        }
        ImGui::Separator();
        ImGui::End();
        return;
    }
    if (address_.kind != EditorGraphKind::kPipeline) {
        if (ImGui::Button("Back")) {
            address_ = address_.kind == EditorGraphKind::kPass
                           ? EditorGraphAddress{EditorGraphKind::kFeature, address_.pipeline_node, {}, {}}
                           : EditorGraphAddress{EditorGraphKind::kPipeline, {}, {}, {}};
            position_scope_ = graph_scope(address_);
            positions_.clear();
            selected_node_.clear();
            selected_edge_.clear();
            connect_source_node_.clear();
            connect_source_port_ = connect_source_slot_ = 0;
            last_action_ = "Returned to parent graph";
        }
        ImGui::SameLine();
        if (address_.kind == EditorGraphKind::kFeature) {
            ImGui::TextDisabled("Pipeline / Features");
        } else {
            ImGui::TextDisabled("Pipeline / %s%s", address_.feature_node.c_str(),
                                address_.kind == EditorGraphKind::kPass ? (" / " + address_.part_id).c_str() : "");
        }
        ImGui::Separator();
    }
    const float graph_selector_width = std::clamp(ImGui::GetContentRegionAvail().x * 0.42f, 180.0f, 330.0f);
    ImGui::TextDisabled("Graph");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(graph_selector_width);
    if (ImGui::BeginCombo("##FlowGraphSelector", graph_label.c_str())) {
        const auto snapshot = document.snapshot();
        if (ImGui::Selectable("Pipeline", address_.kind == EditorGraphKind::kPipeline)) {
            if (address_.kind != EditorGraphKind::kPipeline) {
                last_child_address_ = address_;
                has_last_child_address_ = true;
                restore_child_after_pipeline_switch_ = true;
            } else {
                restore_child_after_pipeline_switch_ = false;
            }
            address_ = {EditorGraphKind::kPipeline, {}, {}, {}};
            positions_.clear();
            position_scope_ = graph_scope(address_);
            selected_node_.clear();
            connect_source_node_.clear();
            connect_source_port_ = 0;
            connect_source_slot_ = 0;
        }
        // Feature is a collection graph owned by a Pipeline.  Keep one
        // selector entry per Pipeline so the label and selected state match
        // the strict projection (all Feature cards for that Pipeline).
        for (const auto& pipeline : snapshot->pipeline_nodes) {
            const std::string label = "Feature / " + pipeline.node_id;
            if (ImGui::Selectable(label.c_str(), address_.kind == EditorGraphKind::kFeature &&
                                                       address_.pipeline_node == pipeline.node_id &&
                                                       address_.feature_node.empty())) {
                const bool restore_child =
                    restore_child_after_pipeline_switch_ && has_last_child_address_ &&
                    last_child_address_.kind != EditorGraphKind::kPipeline &&
                    last_child_address_.pipeline_node == pipeline.node_id &&
                    last_child_address_.kind == EditorGraphKind::kFeature;
                address_ = restore_child ? last_child_address_
                                         : EditorGraphAddress{EditorGraphKind::kFeature, pipeline.node_id, {}, {}};
                restore_child_after_pipeline_switch_ = false;
                positions_.clear();
                position_scope_ = graph_scope(address_);
                selected_node_.clear();
                connect_source_node_.clear();
                connect_source_port_ = 0;
                connect_source_slot_ = 0;
            }
            for (const auto& feature : snapshot->feature_nodes) {
                if (feature.pipeline_node != pipeline.node_id)
                    continue;
                // Pass graphs are backed by the authored pass instances, not
                // only by optional type-declaration parts. Built-in
                // Deferred/Forward Features intentionally use an empty part
                // id, so the menu keeps their real Pass graph reachable.
                std::vector<std::string> pass_graph_parts;
                for (const auto& pass : snapshot->pass_nodes) {
                    if (pass.feature_node != feature.node_id ||
                        std::find(pass_graph_parts.begin(), pass_graph_parts.end(), pass.part_id) !=
                            pass_graph_parts.end())
                        continue;
                    pass_graph_parts.push_back(pass.part_id);
                }
                const auto* feature_type = document.catalog().find(
                    {pipeline::AuthoringTypeKind::kFeature, feature.type.value()});
                const auto* declaration = feature_type == nullptr
                                              ? nullptr
                                              : std::get_if<pipeline::FeatureAuthoringDeclaration>(
                                                    &feature_type->declaration);
                if (declaration != nullptr) {
                    for (const auto& part : declaration->parts) {
                        if (std::find(pass_graph_parts.begin(), pass_graph_parts.end(), part.name) ==
                            pass_graph_parts.end())
                            pass_graph_parts.push_back(part.name);
                    }
                    // Keep an empty Pass canvas reachable after the user
                    // removes every instance from an editable feature.
                    if (pass_graph_parts.empty() &&
                        (declaration->allow_add_passes || !declaration->allowed_pass_types.empty() ||
                         !declaration->default_passes.empty()))
                        pass_graph_parts.emplace_back();
                }
                for (const auto& part_id : pass_graph_parts) {
                    const std::string part_label =
                        part_id.empty() ? "Pass / " + feature.node_id
                                        : "Pass / " + feature.node_id + " / " + part_id;
                    if (!ImGui::Selectable(part_label.c_str(), address_.kind == EditorGraphKind::kPass &&
                                                                address_.pipeline_node == feature.pipeline_node &&
                                                                address_.feature_node == feature.node_id &&
                                                                address_.part_id == part_id))
                        continue;
                    address_ = {EditorGraphKind::kPass, feature.pipeline_node, feature.node_id, part_id};
                    restore_child_after_pipeline_switch_ = false;
                    positions_.clear();
                    position_scope_ = graph_scope(address_);
                    selected_node_.clear();
                    connect_source_node_.clear();
                    connect_source_port_ = 0;
                    connect_source_slot_ = 0;
                }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    const std::array<float, 5> zoom_levels{{0.5f, 0.75f, 1.0f, 1.5f, 2.0f}};
    const std::string zoom_label = std::to_string(static_cast<int>(zoom_ * 100.0f)) + "%";
    ImGui::SetNextItemWidth(78.0f);
    if (ImGui::BeginCombo("##flowgraph-zoom", zoom_label.c_str())) {
        for (const float level : zoom_levels) {
            const std::string label = std::to_string(static_cast<int>(level * 100.0f)) + "%";
            if (ImGui::Selectable(label.c_str(), std::abs(zoom_ - level) < 0.01f))
                zoom_ = level;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Focus##flowgraph"))
        last_action_ = selected_node_.empty() ? "Select a node to focus" : "Use F to focus the selected node";
    ImGui::SameLine();
    ImGui::TextDisabled("Grid");
    ImGui::SameLine();
    ImGui::TextDisabled("Drag blank canvas to pan (left or middle mouse)");

    // FlowGraph is a graph-authoring workspace rather than a toolbar attached
    // to the scene viewport. Keep the three authoring surfaces visible together
    // so the source of a node, its data-flow canvas, and its contract can be
    // understood without opening another window.
    palette_width_ = std::clamp(palette_width_, 180.0f, 380.0f);
    bool palette_drag_source_active = false;
    const auto column_widths = flowgraph::distribute_columns(ImGui::GetContentRegionAvail().x,
                                                              palette_width_, 320.0f);
    palette_width_ = column_widths.palette;
    const bool flowgraph_table = ImGui::BeginTable("##flowgraph-workspace", 3,
                                                   ImGuiTableFlags_BordersInnerV |
                                                       ImGuiTableFlags_Resizable |
                                                       ImGuiTableFlags_SizingStretchProp);
    if (flowgraph_table) {
    ImGui::TableSetupColumn("##flowgraph-palette", ImGuiTableColumnFlags_WidthFixed, palette_width_);
    ImGui::TableSetupColumn("##flowgraph-canvas-column", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("##flowgraph-inspector", ImGuiTableColumnFlags_WidthFixed,
                           column_widths.inspector);

    ImGui::TableNextColumn();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{4.0f, 2.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{10.0f, 8.0f});
    if (ImGui::BeginChild("##flowgraph-palette", ImVec2{0.0f, 0.0f},
                          ImGuiChildFlags_AlwaysUseWindowPadding)) {
        // Keep the palette visually close to the reference: a compact title
        // row, a full-width search field, then grouped rows with consistent
        // icon/label alignment. The catalog remains the source of truth for
        // which rows are actually insertable in the current graph.
        ImGui::TextUnformatted("Node Palette");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 14.0f);
        ImGui::TextDisabled("‹");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##flowgraph-palette-search", "Search nodes...", palette_search_.data(),
                                 palette_search_.size());
        ImGui::Separator();
        const std::string palette_search{palette_search_.data()};
        const auto contains_palette_search = [&](std::string_view label) {
            if (palette_search.empty())
                return true;
            std::string lower_label{label};
            std::string lower_search = palette_search;
            std::transform(lower_label.begin(), lower_label.end(), lower_label.begin(),
                           [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
            std::transform(lower_search.begin(), lower_search.end(), lower_search.begin(),
                           [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
            return lower_label.find(lower_search) != std::string::npos;
        };
        const auto palette = build_composition_palette(*document.snapshot(), document.catalog(), address_);
        if (palette && palette.value.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(129, 157, 169, 255));
            if (address_.kind == EditorGraphKind::kPass)
                ImGui::TextWrapped("当前没有已注册的 Pass。可以从下方 Render Pass 分类拖入画布注册新 Pass。\n");
            else
                ImGui::TextWrapped("当前没有已注册的节点。可以从下方分类拖入画布注册新节点。\n");
            ImGui::PopStyleColor();
        }
        const ImVec4 pipeline_color{0.68f, 0.40f, 0.95f, 1.0f};
        const ImVec4 feature_color{0.72f, 0.42f, 0.95f, 1.0f};
        const ImVec4 pass_color{1.0f, 0.57f, 0.18f, 1.0f};
        const ImVec4 resource_color{0.17f, 0.68f, 0.95f, 1.0f};
        const ImVec4 utility_color{0.82f, 0.87f, 0.91f, 1.0f};

        const auto draw_palette_icon = [&](const ImVec2& min, std::string_view icon,
                                           const ImVec4& color) {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 center{min.x + 12.0f, min.y + 12.0f};
            const ImU32 tint = ImGui::GetColorU32(color);
            if (icon == "pipeline") {
                draw->AddRectFilled({min.x + 4.0f, min.y + 4.0f}, {min.x + 20.0f, min.y + 10.0f}, tint, 2.0f);
                draw->AddRectFilled({min.x + 4.0f, min.y + 9.0f}, {min.x + 20.0f, min.y + 15.0f}, tint, 2.0f);
                draw->AddRectFilled({min.x + 4.0f, min.y + 14.0f}, {min.x + 20.0f, min.y + 20.0f}, tint, 2.0f);
            } else if (icon == "frame") {
                draw->AddRect({min.x + 4.0f, min.y + 4.0f}, {min.x + 20.0f, min.y + 20.0f}, tint, 2.0f, 0, 2.0f);
                draw->AddRect({min.x + 7.0f, min.y + 7.0f}, {min.x + 17.0f, min.y + 17.0f}, tint, 1.0f, 0, 1.0f);
            } else if (icon == "output") {
                draw->AddRectFilled({min.x + 5.0f, min.y + 4.0f}, {min.x + 9.0f, min.y + 20.0f}, tint, 1.0f);
                draw->AddTriangleFilled({min.x + 9.0f, min.y + 4.0f}, {min.x + 21.0f, min.y + 12.0f},
                                        {min.x + 9.0f, min.y + 20.0f}, tint);
            } else if (icon == "cube") {
                draw->AddRectFilled({min.x + 4.0f, min.y + 6.0f}, {min.x + 20.0f, min.y + 18.0f}, tint, 3.0f);
                draw->AddLine({min.x + 4.0f, min.y + 6.0f}, center, IM_COL32(255, 255, 255, 100), 1.0f);
            } else if (icon == "pass") {
                draw->AddRectFilled({min.x + 4.0f, min.y + 4.0f}, {min.x + 20.0f, min.y + 20.0f}, tint, 2.0f);
                draw->AddRectFilled({min.x + 8.0f, min.y + 8.0f}, {min.x + 11.0f, min.y + 16.0f},
                                    IM_COL32(255, 255, 255, 190), 1.0f);
                draw->AddLine({min.x + 12.0f, min.y + 9.0f}, {min.x + 17.0f, min.y + 12.0f},
                              IM_COL32(255, 255, 255, 190), 1.5f);
                draw->AddLine({min.x + 17.0f, min.y + 12.0f}, {min.x + 12.0f, min.y + 15.0f},
                              IM_COL32(255, 255, 255, 190), 1.5f);
            } else if (icon == "subgraph") {
                draw->AddRect({min.x + 4.0f, min.y + 4.0f}, {min.x + 20.0f, min.y + 20.0f}, tint, 1.0f,
                              ImDrawFlags_None, 2.0f);
            } else if (icon == "resource") {
                draw->AddRectFilled({min.x + 4.0f, min.y + 7.0f}, {min.x + 20.0f, min.y + 18.0f}, tint, 3.0f);
                draw->AddEllipse({center.x, min.y + 7.0f}, {8.0f, 3.0f}, tint, 0.0f, 24, 1.5f);
            } else if (icon == "folder") {
                draw->AddRectFilled({min.x + 3.0f, min.y + 7.0f}, {min.x + 21.0f, min.y + 19.0f}, tint, 2.0f);
                draw->AddRectFilled({min.x + 5.0f, min.y + 4.0f}, {min.x + 13.0f, min.y + 9.0f}, tint, 2.0f);
            } else if (icon == "plus") {
                draw->AddLine({center.x - 8.0f, center.y}, {center.x + 8.0f, center.y}, tint, 2.0f);
                draw->AddLine({center.x, center.y - 8.0f}, {center.x, center.y + 8.0f}, tint, 2.0f);
            } else if (icon == "multiply") {
                draw->AddLine({min.x + 5.0f, min.y + 5.0f}, {min.x + 19.0f, min.y + 19.0f}, tint, 2.0f);
                draw->AddLine({min.x + 19.0f, min.y + 5.0f}, {min.x + 5.0f, min.y + 19.0f}, tint, 2.0f);
            } else if (icon == "split") {
                draw->AddCircleFilled({min.x + 7.0f, center.y}, 3.0f, tint, 12);
                draw->AddLine({min.x + 10.0f, center.y}, {min.x + 16.0f, min.y + 7.0f}, tint, 1.7f);
                draw->AddLine({min.x + 10.0f, center.y}, {min.x + 16.0f, min.y + 17.0f}, tint, 1.7f);
                draw->AddCircleFilled({min.x + 19.0f, min.y + 7.0f}, 2.5f, tint, 12);
                draw->AddCircleFilled({min.x + 19.0f, min.y + 17.0f}, 2.5f, tint, 12);
            } else if (icon == "merge") {
                draw->AddCircleFilled({min.x + 6.0f, min.y + 7.0f}, 2.5f, tint, 12);
                draw->AddCircleFilled({min.x + 6.0f, min.y + 17.0f}, 2.5f, tint, 12);
                draw->AddLine({min.x + 9.0f, min.y + 7.0f}, {min.x + 15.0f, center.y}, tint, 1.7f);
                draw->AddLine({min.x + 9.0f, min.y + 17.0f}, {min.x + 15.0f, center.y}, tint, 1.7f);
                draw->AddLine({min.x + 15.0f, center.y}, {min.x + 19.0f, center.y}, tint, 1.7f);
                draw->AddCircleFilled({min.x + 20.0f, center.y}, 2.5f, tint, 12);
            } else if (icon == "reroute") {
                draw->AddCircleFilled({min.x + 6.0f, center.y}, 3.0f, tint, 12);
                draw->AddLine({min.x + 9.0f, center.y}, {min.x + 19.0f, center.y}, tint, 1.7f);
                draw->AddCircleFilled({min.x + 19.0f, center.y}, 2.5f, tint, 12);
            } else if (icon == "comment") {
                draw->AddRectFilled({min.x + 3.0f, min.y + 4.0f}, {min.x + 21.0f, min.y + 17.0f}, tint, 3.0f);
                draw->AddTriangleFilled({min.x + 7.0f, min.y + 16.0f}, {min.x + 7.0f, min.y + 22.0f},
                                        {min.x + 13.0f, min.y + 16.0f}, tint);
            } else {
                draw->AddCircleFilled(center, 7.0f, tint, 16);
            }
        };
        const auto draw_section = [&](const char* title, const ImVec4& title_color) {
            ImGui::Dummy({0.0f, 3.0f});
            ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(13, 29, 39, 210));
            ImGui::Selectable((std::string("v  ") + title).c_str(), false, ImGuiSelectableFlags_SpanAllColumns,
                             {0.0f, 24.0f});
            ImGui::PopStyleColor();
            (void)title_color;
        };
        const auto draw_reference_row = [&](const char* label, const char* icon, const ImVec4& color,
                                            const EditorPaletteEntry* entry,
                                            std::optional<pipeline::AuthoringTypeKind> category_kind = std::nullopt) {
            if (!contains_palette_search(label))
                return;
            ImGui::PushID(label);
            const ImVec2 row_min = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##palette-row", {ImGui::GetContentRegionAvail().x, 24.0f});
            const bool hovered = ImGui::IsItemHovered();
            if (hovered)
                ImGui::GetWindowDrawList()->AddRectFilled(row_min,
                    {row_min.x + ImGui::GetContentRegionAvail().x, row_min.y + 24.0f},
                    IM_COL32(24, 45, 57, 190), 3.0f);
            draw_palette_icon({row_min.x + 3.0f, row_min.y}, icon, color);
            // A category row is always a registration source, even when a
            // catalog entry happens to share its display name. Registered
            // instances remain available through the Add Node menu and the
            // concrete rows below the category.
            const bool can_drag_category = category_kind.has_value() &&
                                           *category_kind ==
                                               (address_.kind == EditorGraphKind::kPipeline
                                                    ? pipeline::AuthoringTypeKind::kPipeline
                                                    : address_.kind == EditorGraphKind::kFeature
                                                          ? pipeline::AuthoringTypeKind::kFeature
                                                          : pipeline::AuthoringTypeKind::kRenderPass);
            const bool can_drag_registered = entry != nullptr && entry->available && !category_kind;
            ImGui::GetWindowDrawList()->AddText({row_min.x + 32.0f, row_min.y + 4.0f},
                                                can_drag_registered || can_drag_category
                                                    ? IM_COL32(222, 232, 238, 255)
                                                    : IM_COL32(112, 139, 151, 255), label);
            if ((can_drag_registered || can_drag_category) &&
                ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID |
                                           ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
                if (can_drag_registered) {
                    const FlowGraphPalettePayload payload{static_cast<uint8_t>(entry->type.kind), entry->type.id};
                    ImGui::SetDragDropPayload(palette_payload_type, &payload, sizeof(payload));
                } else {
                    const FlowGraphCategoryPayload payload{static_cast<uint8_t>(*category_kind)};
                    ImGui::SetDragDropPayload(palette_category_payload_type, &payload, sizeof(payload));
                }
                palette_drag_source_active = true;
                // Render a stable card in the foreground while the pointer is
                // still over the palette. This makes the drag affordance
                // visible before the pointer reaches the canvas and avoids
                // ImGui's default floating text tooltip.
                const ImVec2 mouse = ImGui::GetIO().MousePos;
                const auto preview_point = flowgraph::drag_node_top_left({mouse.x, mouse.y});
                const ImVec2 preview_min{preview_point.x, preview_point.y};
                const ImVec2 preview_max{preview_min.x + 220.0f, preview_min.y + 58.0f};
                ImDrawList* foreground = ImGui::GetForegroundDrawList();
                foreground->AddRectFilled({preview_min.x + 4.0f, preview_min.y + 5.0f},
                                          {preview_max.x + 4.0f, preview_max.y + 5.0f},
                                          IM_COL32(0, 0, 0, 110), 6.0f);
                foreground->AddRectFilled(preview_min, preview_max, ImGui::GetColorU32(color), 6.0f);
                foreground->AddRect(preview_min, preview_max, IM_COL32(112, 226, 218, 235), 6.0f, 0, 2.0f);
                draw_ellipsis(foreground, {preview_min.x + 10.0f, preview_min.y + 9.0f},
                              preview_max.x - 10.0f, label, IM_COL32(245, 252, 253, 255));
                foreground->AddText({preview_min.x + 10.0f, preview_min.y + 34.0f},
                                    IM_COL32(194, 222, 228, 245),
                                    can_drag_category ? "Release to register" : "Release to place");
                ImGui::EndDragDropSource();
            }
            if (can_drag_registered && ImGui::IsItemHovered() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                palette_search_.fill('\0');
                const auto count = std::min(entry->label.size(), palette_search_.size() - 1u);
                std::copy_n(entry->label.data(), count, palette_search_.data());
                palette_search_[count] = '\0';
                add_node_popup_requested_ = true;
            }
            if (hovered && entry == nullptr && !category_kind) {
                if (palette && palette.value.empty() && address_.kind == EditorGraphKind::kPass)
                    ImGui::SetTooltip("当前 Feature 的 Pass 契约禁止添加节点（allow_add_passes=false）。");
                else
                    ImGui::SetTooltip("此项只是节点类别示意，没有可拖入的已注册节点契约。");
            }
            if (hovered && category_kind && can_drag_category)
                ImGui::SetTooltip("拖入画布以注册一个新的编辑器节点，并生成可编辑的代码骨架。");
            ImGui::PopID();
        };
        const auto find_entry = [&](pipeline::AuthoringTypeKind kind, std::string_view label) -> const EditorPaletteEntry* {
            if (!palette)
                return nullptr;
            std::string needle{label};
            std::transform(needle.begin(), needle.end(), needle.begin(),
                           [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
            const auto found = std::find_if(palette.value.begin(), palette.value.end(), [&](const auto& entry) {
                if (!entry.available || entry.type.kind != kind)
                    return false;
                std::string candidate = entry.label;
                std::transform(candidate.begin(), candidate.end(), candidate.begin(),
                               [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
                return candidate == needle;
            });
            return found == palette.value.end() ? nullptr : &*found;
        };
        const auto draw_catalog_entries = [&](pipeline::AuthoringTypeKind kind, const ImVec4& color,
                                              std::string_view reference_label, const char* icon) {
            if (!palette)
                return;
            std::string reference{reference_label};
            std::transform(reference.begin(), reference.end(), reference.begin(),
                           [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
            for (const auto& entry : palette.value) {
                if (!entry.available || entry.type.kind != kind)
                    continue;
                std::string candidate = entry.label;
                std::transform(candidate.begin(), candidate.end(), candidate.begin(),
                               [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
                if (candidate != reference)
                    draw_reference_row(entry.label.c_str(), icon, color, &entry);
            }
        };
        draw_section("Pipeline", pipeline_color);
        draw_reference_row("Pipeline", "pipeline", pipeline_color,
                           find_entry(pipeline::AuthoringTypeKind::kPipeline, "Pipeline"),
                           pipeline::AuthoringTypeKind::kPipeline);
        draw_catalog_entries(pipeline::AuthoringTypeKind::kPipeline, pipeline_color, "Pipeline", "pipeline");
        draw_reference_row("Frame Input", "frame", ImVec4{0.13f, 0.68f, 0.95f, 1.0f}, nullptr);
        draw_reference_row("Final Output", "output", ImVec4{0.26f, 0.85f, 0.42f, 1.0f}, nullptr);
        draw_section("Feature", feature_color);
        draw_reference_row("Feature", "cube", feature_color,
                           find_entry(pipeline::AuthoringTypeKind::kFeature, "Feature"),
                           pipeline::AuthoringTypeKind::kFeature);
        draw_catalog_entries(pipeline::AuthoringTypeKind::kFeature, feature_color, "Feature", "cube");
        draw_reference_row("Sub Graph", "subgraph", feature_color, nullptr);
        draw_section("Pass", pass_color);
        draw_reference_row("Render Pass", "pass", pass_color,
                           find_entry(pipeline::AuthoringTypeKind::kRenderPass, "Render Pass"),
                           pipeline::AuthoringTypeKind::kRenderPass);
        draw_catalog_entries(pipeline::AuthoringTypeKind::kRenderPass, pass_color, "Render Pass", "pass");
        draw_reference_row("Compute Pass", "pass", pass_color, nullptr);
        draw_reference_row("Raster Pass", "pass", pass_color, nullptr);
        draw_reference_row("Post Process Pass", "pass", pass_color, nullptr);
        draw_section("Resource", resource_color);
        draw_reference_row("Texture", "resource", resource_color, nullptr);
        draw_reference_row("Buffer", "resource", resource_color, nullptr);
        draw_reference_row("Scene Resource", "cube", resource_color, nullptr);
        draw_reference_row("External Resource", "folder", ImVec4{0.93f, 0.78f, 0.35f, 1.0f}, nullptr);
        draw_section("Utility", utility_color);
        draw_reference_row("Add", "plus", utility_color, nullptr);
        draw_reference_row("Multiply", "multiply", utility_color, nullptr);
        draw_reference_row("Split", "split", utility_color, nullptr);
        draw_reference_row("Merge", "merge", utility_color, nullptr);
        draw_reference_row("Reroute", "reroute", utility_color, nullptr);
        draw_reference_row("Comment", "comment", utility_color, nullptr);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleVar();
    ImGui::TableNextColumn();

    bool flow_toolbar_has_item = false;
    float flow_toolbar_line_width = 0.0f;
    float flow_toolbar_row_width = 0.0f;
    const auto flow_toolbar_button = [&](const char* label) {
        const float width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (!flow_toolbar_has_item) {
            flow_toolbar_has_item = true;
            flow_toolbar_line_width = ImGui::GetContentRegionAvail().x;
            flow_toolbar_row_width = width;
        } else if (flow_toolbar_row_width + ImGui::GetStyle().ItemSpacing.x + width <= flow_toolbar_line_width) {
            ImGui::SameLine();
            flow_toolbar_row_width += ImGui::GetStyle().ItemSpacing.x + width;
        } else {
            flow_toolbar_row_width = width;
        }
        return ImGui::Button(label);
    };
    if (flow_toolbar_button("Add Node"))
        ImGui::OpenPopup("FlowGraphAddNode");
    if (flow_toolbar_button("Create Empty Node")) {
        const std::string default_name = "Game.Custom." + graph_type_prefix(address_.kind);
        custom_type_name_.fill('\0');
        std::memcpy(custom_type_name_.data(), default_name.data(),
                    std::min(default_name.size(), custom_type_name_.size() - 1u));
        custom_type_name_initialized_ = true;
        ImGui::OpenPopup("FlowGraphCreateEmptyNode");
    }
    if (ImGui::BeginPopup("FlowGraphCreateEmptyNode")) {
        ImGui::TextUnformatted("Create an authoring placeholder");
        ImGui::TextDisabled("The node has no implementation or ports yet.");
        ImGui::InputText("Type name", custom_type_name_.data(), custom_type_name_.size());
        const bool can_create = custom_type_name_initialized_ && custom_type_name_[0] != '\0';
        if (ImGui::Button("Create", ImVec2{120.0f, 0.0f}) && can_create) {
            std::string created_node_id;
            const auto result = create_empty_node(document, address_, custom_type_name_.data(), &created_node_id);
            last_action_ = result ? "Created empty " + graph_type_prefix(address_.kind) + " node"
                                  : result.message;
            if (result) {
                selected_node_ = std::move(created_node_id);
                pending_layout_node_ = selected_node_;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2{120.0f, 0.0f}))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("FlowGraphAddNode")) {
        ImGui::InputTextWithHint("##FlowGraphPaletteSearch", "Search node types...", palette_search_.data(),
                                 palette_search_.size());
        const std::string search{palette_search_.data()};
        const auto contains_search = [&](const std::string& label) {
            if (search.empty()) return true;
            std::string lower_label = label;
            std::string lower_search = search;
            std::transform(lower_label.begin(), lower_label.end(), lower_label.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::transform(lower_search.begin(), lower_search.end(), lower_search.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return lower_label.find(lower_search) != std::string::npos;
        };
        const auto palette = build_composition_palette(*document.snapshot(), document.catalog(), address_);
        if (!palette) {
            ImGui::TextDisabled("Node catalog unavailable: %s", palette.result.message.c_str());
        } else if (palette.value.empty()) {
            ImGui::TextDisabled("No compatible node types are declared for this graph.");
        } else {
            bool any_visible = false;
            for (std::size_t palette_index = 0; palette_index < palette.value.size(); ++palette_index) {
                const auto& entry = palette.value[palette_index];
                if (!entry.available || !contains_search(entry.label))
                    continue;
                any_visible = true;
                if (!ImGui::Selectable(entry.label.c_str()))
                    continue;
                std::string base = entry.label.empty() ? entry.type.id == 0 ? "Node" : std::to_string(entry.type.id)
                                                       : entry.label;
                std::replace_if(base.begin(), base.end(), [](char character) {
                    return !std::isalnum(static_cast<unsigned char>(character)) && character != '_';
                }, '_');
                std::string node_id = base;
                node_id += "_" + std::to_string(document.revision() + palette_index + 1);
                const auto snapshot = document.snapshot();
                const auto exists = [&](std::string_view id) {
                    return std::any_of(snapshot->pipeline_nodes.begin(), snapshot->pipeline_nodes.end(),
                                       [id](const auto& node) { return node.node_id == id; }) ||
                           std::any_of(snapshot->feature_nodes.begin(), snapshot->feature_nodes.end(),
                                       [id](const auto& node) { return node.node_id == id; }) ||
                           std::any_of(snapshot->pass_nodes.begin(), snapshot->pass_nodes.end(),
                                       [id](const auto& node) { return node.node_id == id; });
                };
                while (exists(node_id))
                    node_id += "_1";
                std::string attachment;
                if (address_.kind == EditorGraphKind::kFeature) {
                    const auto pipeline_node = std::find_if(
                        snapshot->pipeline_nodes.begin(), snapshot->pipeline_nodes.end(),
                        [&](const auto& node) { return node.node_id == address_.pipeline_node; });
                    const auto* pipeline_entry = pipeline_node == snapshot->pipeline_nodes.end()
                                                     ? nullptr
                                                     : document.catalog().find({pipeline::AuthoringTypeKind::kPipeline,
                                                                                pipeline_node->type.value()});
                    const auto* declaration = pipeline_entry == nullptr
                                                  ? nullptr
                                                  : std::get_if<pipeline::PipelineAuthoringDeclaration>(&pipeline_entry->declaration);
                    if (declaration != nullptr && !declaration->feature_points.empty())
                        attachment = std::to_string(declaration->feature_points.front().id.value());
                } else if (address_.kind == EditorGraphKind::kPass) {
                    attachment = address_.part_id;
                }
                EditorCompositionCommands commands(document);
                const auto result = commands.add_type(address_, entry.type, node_id, std::move(attachment));
                last_action_ = result ? "Added " + entry.label : result.message;
                if (result) {
                    selected_node_ = node_id;
                    pending_layout_node_ = node_id;
                }
                ImGui::CloseCurrentPopup();
                break;
            }
            if (!any_visible)
                ImGui::TextDisabled("No node type matches the search.");
        }
        ImGui::EndPopup();
    }
    const auto* selected_toolbar_node = find_node(view, selected_node_);
    const bool selected_node_editable = selected_toolbar_node != nullptr && !selected_toolbar_node->fixed &&
                                        selected_toolbar_node->kind != EditorGraphNodeKind::kPart;
    ImGui::BeginDisabled(!selected_node_editable);
    if (flow_toolbar_button("Delete Selected") && selected_node_editable) {
        EditorCompositionCommands commands(document);
        const auto result = commands.erase_node(selected_node_);
        last_action_ = result ? "Deleted " + selected_node_ : result.message;
        if (result)
            selected_node_.clear();
    }
    ImGui::EndDisabled();
    const auto selected_edge_view = std::find_if(view.edges.begin(), view.edges.end(),
                                                 [&](const auto& edge) { return edge.edge_id == selected_edge_; });
    const bool selected_edge_editable = selected_edge_view != view.edges.end() && !selected_edge_view->fixed;
    const auto disconnect_edge_view = [&](const EditorGraphEdgeView& edge) {
        EditorCompositionCommands commands(document);
        if (edge.backing_edge_ids.empty())
            return commands.disconnect(edge.edge_id);

        // A strict Pass edge may stand for producer -> hidden Resource and
        // hidden Resource -> consumer. Removing the consumer relation is the
        // user-visible operation. Keep the producer when another Pass still
        // consumes that Resource, and remove it only when the relay becomes
        // completely unused.
        const auto before = document.snapshot();
        if (edge.backing_edge_ids.size() < 2U)
            return commands.disconnect(edge.backing_edge_ids.front());
        const auto producer = std::find_if(before->edges.begin(), before->edges.end(), [&](const auto& candidate) {
            return candidate.edge_id == edge.backing_edge_ids.front();
        });
        const auto consumer = std::find_if(before->edges.begin(), before->edges.end(), [&](const auto& candidate) {
            return candidate.edge_id == edge.backing_edge_ids.back();
        });
        if (producer == before->edges.end() || consumer == before->edges.end())
            return foundation::not_found("collapsed resource flow no longer exists");
        const std::string resource_node = consumer->source.node_id;
        auto result = commands.disconnect(consumer->edge_id);
        if (!result)
            return result;
        const auto after = document.snapshot();
        const bool has_other_consumers = std::any_of(
            after->edges.begin(), after->edges.end(), [&](const auto& candidate) {
                return candidate.kind == pipeline::DocumentEdgeKind::kRenderResource &&
                       candidate.source.node_id == resource_node;
            });
        if (!has_other_consumers)
            result = commands.disconnect(producer->edge_id);
        return result;
    };
    if (!selected_edge_editable)
        ImGui::BeginDisabled();
    const bool disconnect_selected = flow_toolbar_button("Disconnect Selected");
    if (!selected_edge_editable)
        ImGui::EndDisabled();
    if (disconnect_selected && !selected_edge_.empty() && selected_edge_editable) {
        const auto result = disconnect_edge_view(*selected_edge_view);
        last_action_ = result ? "Disconnected " + selected_edge_ : result.message;
        if (result)
            selected_edge_.clear();
    }
    if (flow_toolbar_button(connect_source_node_.empty() ? "Connect From Selected" : "Cancel Connect")) {
        if (connect_source_node_.empty())
            connect_source_node_ = selected_node_;
        else {
            connect_source_node_.clear();
            connect_source_port_ = 0;
            connect_source_slot_ = 0;
        }
    }
    if (!connect_source_node_.empty())
        ImGui::TextColored(ImVec4{0.45f, 0.9f, 0.85f, 1.0f},
                           connect_source_port_ == 0 ? "Source selected: %s; click one of its output ports"
                                                     : "Output selected: %s.%llu; click a destination input",
                           connect_source_node_.c_str(),
                           static_cast<unsigned long long>(connect_source_port_));
    if (!last_action_.empty())
        ImGui::TextDisabled("%s", last_action_.c_str());

    const auto draw_selection_editor = [&]() {
    if (ImGui::CollapsingHeader("Selection", ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto* selected = find_node(view, selected_node_);
        if (selected == nullptr) {
            ImGui::TextDisabled("Select a node to edit its declared inputs and parameters.");
        } else {
            ImGui::Text("%s", selected->node_id.c_str());
            if (selected->kind == EditorGraphNodeKind::kResource) {
                const auto* resource = find_resource(document, selected->node_id);
                if (resource == nullptr) {
                    ImGui::TextDisabled("Resource declaration is unavailable.");
                } else {
                    const bool external_input = resource->external_input;
                    const char* class_name = resource->resource_constraints.resource_class ==
                                                     pipeline::ResourceClass::kBuffer
                                                 ? "Buffer"
                                                 : "Texture";
                    ImGui::TextDisabled("%s resource  |  %s", class_name,
                                        external_input ? "external input" : "graph-produced");
                    const auto snapshot = document.snapshot();
                    const auto binding = std::find_if(
                        snapshot->input_bindings.begin(), snapshot->input_bindings.end(),
                        [&](const auto& value) {
                            return value.node_id == selected->node_id && value.port_id == resource->resource_id.value() &&
                                   value.slot_id == 0;
                        });
                    const auto source = binding == snapshot->input_bindings.end()
                                            ? pipeline::InputSourceKind::kUnset
                                            : binding->source;
                    if (external_input) {
                        ImGui::TextDisabled("Source (an external Texture/Buffer may have no upstream node)");
                        ImGui::SameLine();
                        const std::string source_id = "##resource-source-" + selected->node_id;
                        const char* source_label = source == pipeline::InputSourceKind::kAsset ? "Asset" :
                            source == pipeline::InputSourceKind::kNone ? "None" : "Asset";
                        ImGui::SetNextItemWidth(150.0f);
                        if (ImGui::BeginCombo(source_id.c_str(), source_label)) {
                            if (ImGui::Selectable("Asset", source == pipeline::InputSourceKind::kAsset)) {
                                pipeline::InputBindingDocument next_binding{};
                                if (binding != snapshot->input_bindings.end()) next_binding = *binding;
                                next_binding.node_id = selected->node_id;
                                next_binding.port_id = resource->resource_id.value();
                                next_binding.source = pipeline::InputSourceKind::kAsset;
                                next_binding.status = pipeline::InputBindingStatus::kBound;
                                const auto result = EditorCompositionCommands(document).set_input_source(next_binding);
                                last_action_ = result ? "Resource asset source updated" : result.message;
                                ImGui::CloseCurrentPopup();
                            }
                            if (ImGui::Selectable("None", source == pipeline::InputSourceKind::kNone)) {
                                pipeline::InputBindingDocument next_binding{};
                                next_binding.node_id = selected->node_id;
                                next_binding.port_id = resource->resource_id.value();
                                next_binding.source = pipeline::InputSourceKind::kNone;
                                next_binding.status = pipeline::InputBindingStatus::kBound;
                                const auto result = EditorCompositionCommands(document).set_input_source(next_binding);
                                last_action_ = result ? "Resource source cleared" : result.message;
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::EndCombo();
                        }
                        if (source == pipeline::InputSourceKind::kAsset) {
                            const std::string buffer_key = selected->node_id + ":resource";
                            auto& buffer = input_buffers_[buffer_key];
                            if (buffer[0] == '\0' && binding != snapshot->input_bindings.end()) {
                                const auto count = std::min(binding->asset.uuid.size(), buffer.size() - 1u);
                                std::copy_n(binding->asset.uuid.data(), count, buffer.data());
                                buffer[count] = '\0';
                            }
                            ImGui::SetNextItemWidth(300.0f);
                            const bool edited = ImGui::InputText(("AssetId##" + buffer_key).c_str(), buffer.data(),
                                                                  buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
                            if (edited || (ImGui::IsItemDeactivatedAfterEdit() && ImGui::IsItemEdited())) {
                                pipeline::InputBindingDocument asset_binding{};
                                asset_binding.node_id = selected->node_id;
                                asset_binding.port_id = resource->resource_id.value();
                                asset_binding.source = pipeline::InputSourceKind::kAsset;
                                asset_binding.asset.uuid = buffer.data();
                                asset_binding.status = pipeline::InputBindingStatus::kBound;
                                const auto result = EditorCompositionCommands(document).set_input_source(asset_binding);
                                last_action_ = result ? "Resource asset updated" : result.message;
                            }
                        }
                    } else {
                        ImGui::TextDisabled("Produced by graph data flow; connect an upstream producer to Resource.");
                    }
                }
            } else if (const auto* type = type_for_node(document, address_, *selected); type != nullptr) {
                const bool editor_owned =
                    type->owner_module.value() == pipeline::ModuleId("Editor.FlowGraph").value();
                if (editor_owned) {
                    ImGui::Separator();
                    ImGui::TextUnformatted("Node contract");
                    ImGui::TextDisabled("Configure ports and computation points in the node definition editor.");
                    const auto input_count = static_cast<std::size_t>(std::count_if(
                        type->ports.begin(), type->ports.end(), [](const auto& port) {
                            return port.direction == pipeline::PortDirection::kInput;
                        }));
                    const auto output_count = static_cast<std::size_t>(std::count_if(
                        type->ports.begin(), type->ports.end(), [](const auto& port) {
                            return port.direction == pipeline::PortDirection::kOutput;
                        }));
                    const auto compute_count = type->parameters.size();
                    ImGui::TextDisabled("%zu inputs, %zu outputs, %zu compute points",
                                       input_count, output_count, compute_count);
                    if (ImGui::Button("Edit Node Definition")) {
                        definition_node_ = selected->node_id;
                        definition_popup_requested_ = true;
                        last_action_ = "Opening definition: " + selected->node_id;
                    }
                }
                const auto* parameters = parameters_for_node(document, selected->node_id);
                for (const auto& parameter : type->parameters) {
                    const std::string label = parameter.display_name.empty() ? parameter.name : parameter.display_name;
                    const pipeline::ParameterValue* configured_value = nullptr;
                    if (parameters != nullptr) {
                        const auto configured = parameters->find(parameter.id);
                        if (configured != parameters->end())
                            configured_value = &configured->second;
                    }
                    pipeline::ParameterValue value = configured_value == nullptr ? parameter.default_value
                                                                                   : *configured_value;
                    bool changed = false;
                    ImGui::PushID(static_cast<int>(parameter.id));
                    ImGui::TextDisabled("%s", label.c_str());
                    ImGui::SameLine();
                    switch (parameter.kind) {
                        case pipeline::ParameterKind::kBool: {
                            bool current = std::get_if<bool>(&value) ? *std::get_if<bool>(&value) : false;
                            changed = ImGui::Checkbox("##parameter", &current);
                            value = current;
                            break;
                        }
                        case pipeline::ParameterKind::kInt64: {
                            int64_t current = std::get_if<int64_t>(&value) ? *std::get_if<int64_t>(&value) : 0;
                            changed = ImGui::InputScalar("##parameter", ImGuiDataType_S64, &current);
                            value = current;
                            break;
                        }
                        case pipeline::ParameterKind::kUInt64: {
                            uint64_t current = std::get_if<uint64_t>(&value) ? *std::get_if<uint64_t>(&value) : 0;
                            changed = ImGui::InputScalar("##parameter", ImGuiDataType_U64, &current);
                            value = current;
                            break;
                        }
                        case pipeline::ParameterKind::kFloat64: {
                            double current = std::get_if<double>(&value) ? *std::get_if<double>(&value) : 0.0;
                            changed = ImGui::InputDouble("##parameter", &current);
                            value = current;
                            break;
                        }
                        case pipeline::ParameterKind::kString: {
                            const std::string key = selected->node_id + ":parameter:" + std::to_string(parameter.id);
                            auto& buffer = input_buffers_[key];
                            if (buffer[0] == '\0') {
                                if (const auto* text = std::get_if<std::string>(&value)) {
                                    const auto count = std::min(text->size(), buffer.size() - 1u);
                                    std::copy_n(text->data(), count, buffer.data());
                                    buffer[count] = '\0';
                                }
                            }
                            changed = ImGui::InputText("##parameter", buffer.data(), buffer.size(),
                                                       ImGuiInputTextFlags_EnterReturnsTrue);
                            value = std::string{buffer.data()};
                            break;
                        }
                        case pipeline::ParameterKind::kEnum: {
                            const std::string current = std::get_if<std::string>(&value) ? *std::get_if<std::string>(&value)
                                                                                           : std::string{};
                            if (ImGui::BeginCombo("##parameter", current.empty() ? "Select" : current.c_str())) {
                                for (const auto& option : parameter.enum_values) {
                                    if (ImGui::Selectable(option.c_str(), option == current)) {
                                        value = option;
                                        changed = true;
                                    }
                                }
                                ImGui::EndCombo();
                            }
                            break;
                        }
                        case pipeline::ParameterKind::kAsset: {
                            const std::string key = selected->node_id + ":asset-parameter:" + std::to_string(parameter.id);
                            auto& buffer = input_buffers_[key];
                            if (buffer[0] == '\0') {
                                if (const auto* asset = std::get_if<pipeline::AssetReference>(&value)) {
                                    const auto count = std::min(asset->uuid.size(), buffer.size() - 1u);
                                    std::copy_n(asset->uuid.data(), count, buffer.data());
                                    buffer[count] = '\0';
                                }
                            }
                            changed = ImGui::InputText("##parameter", buffer.data(), buffer.size(),
                                                       ImGuiInputTextFlags_EnterReturnsTrue);
                            value = pipeline::AssetReference{buffer.data()};
                            break;
                        }
                        case pipeline::ParameterKind::kVector: {
                            std::vector<double> current = std::get_if<std::vector<double>>(&value)
                                                              ? *std::get_if<std::vector<double>>(&value)
                                                              : std::vector<double>{3, 0.0};
                            if (current.empty())
                                current.resize(3, 0.0);
                            for (std::size_t component = 0; component < current.size(); ++component) {
                                if (component != 0)
                                    ImGui::SameLine();
                                changed |= ImGui::InputDouble(("##parameter-" + std::to_string(component)).c_str(),
                                                              &current[component], 0.0, 0.0, "%.3f");
                            }
                            value = std::move(current);
                            break;
                        }
                    }
                    ImGui::PopID();
                    if (changed) {
                        const auto result = EditorCompositionCommands(document).set_parameter(selected->node_id,
                                                                                                 parameter.id,
                                                                                                 std::move(value));
                        last_action_ = result ? "Parameter updated" : result.message;
                    }
                }
                for (const auto& port : selected->ports) {
                    if (!port.input) continue;
                    const auto snapshot = document.snapshot();
                    const auto binding = std::find_if(snapshot->input_bindings.begin(), snapshot->input_bindings.end(),
                        [&](const auto& value) { return value.node_id == selected->node_id && value.port_id == port.port_id &&
                                                         value.slot_id == port.slot_id; });
                    // In a strict Pass graph, a Resource relay is hidden and
                    // projected as a Pass-to-Pass edge. Its binding belongs to
                    // the hidden Resource document node, so looking only at
                    // input_bindings would incorrectly show every required
                    // consumer input as Unset.
                    const auto projected_edge = std::find_if(
                        view.edges.begin(), view.edges.end(), [&](const auto& edge) {
                            return edge.destination_node == selected->node_id &&
                                   edge.destination_port == port.port_id && edge.destination_slot == port.slot_id &&
                                   !edge.fixed;
                        });
                    const bool has_projected_data_edge = projected_edge != view.edges.end();
                    const auto source = has_projected_data_edge
                                            ? pipeline::InputSourceKind::kEdge
                                            : binding == snapshot->input_bindings.end()
                                                  ? pipeline::InputSourceKind::kUnset
                                                  : binding->source;
                    const auto contract_port = std::find_if(type->ports.begin(), type->ports.end(),
                                                            [&](const auto& candidate) {
                                                                return candidate.id == port.port_id && candidate.input;
                                                            });
                    ImGui::TextDisabled("Input %s%s", port.label.c_str(), port.required ? " (required)" : " (optional)");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(170.0f);
                    const char* source_label = source == pipeline::InputSourceKind::kEdge ? "Data edge" :
                        source == pipeline::InputSourceKind::kAsset ? "Asset" : source == pipeline::InputSourceKind::kLiteral ? "Literal" :
                        source == pipeline::InputSourceKind::kDefault ? "Default" : source == pipeline::InputSourceKind::kContext ? "Context" : "Unset";
                    const std::string source_id = "##source-" + std::to_string(port.port_id) + "-" +
                                                  std::to_string(port.slot_id);
                    if (has_projected_data_edge) {
                        ImGui::BeginDisabled();
                        ImGui::SetNextItemWidth(170.0f);
                        if (ImGui::BeginCombo(source_id.c_str(), source_label))
                            ImGui::EndCombo();
                        ImGui::EndDisabled();
                        ImGui::SameLine();
                        ImGui::TextDisabled("Provided by graph edge");
                    } else if (ImGui::BeginCombo(source_id.c_str(), source_label)) {
                        const std::array<std::pair<pipeline::InputSourceKind, const char*>, 6> choices{{
                            {pipeline::InputSourceKind::kUnset, "Unset"}, {pipeline::InputSourceKind::kEdge, "Data edge"},
                            {pipeline::InputSourceKind::kAsset, "Asset"}, {pipeline::InputSourceKind::kLiteral, "Literal"},
                            {pipeline::InputSourceKind::kDefault, "Default"}, {pipeline::InputSourceKind::kContext, "Context"}}};
                        for (const auto& [choice, label] : choices) {
                            const bool enabled = choice != pipeline::InputSourceKind::kEdge &&
                                (choice == pipeline::InputSourceKind::kUnset ||
                                 (port.allowed_sources & pipeline::input_source_mask(choice)) != 0);
                            if (!enabled)
                                ImGui::BeginDisabled();
                            if (ImGui::Selectable(label, source == choice, 0, ImVec2{0, 0}) && enabled) {
                                pipeline::InputBindingDocument next_binding{};
                                next_binding.node_id = selected->node_id; next_binding.port_id = port.port_id;
                                next_binding.slot_id = port.slot_id;
                                next_binding.source = choice; next_binding.status = choice == pipeline::InputSourceKind::kUnset
                                    ? pipeline::InputBindingStatus::kUnset : pipeline::InputBindingStatus::kBound;
                                if (binding != snapshot->input_bindings.end()) next_binding = *binding;
                                next_binding.source = choice; next_binding.status = choice == pipeline::InputSourceKind::kUnset
                                    ? pipeline::InputBindingStatus::kUnset : pipeline::InputBindingStatus::kBound;
                                if (choice == pipeline::InputSourceKind::kDefault) {
                                    if (contract_port != type->ports.end() && contract_port->input->default_value) {
                                        next_binding.default_id = contract_port->input->default_value->id;
                                        next_binding.default_revision = contract_port->input->default_value->revision;
                                    }
                                }
                                if (choice == pipeline::InputSourceKind::kContext && next_binding.context_key.empty()) {
                                    if (contract_port != type->ports.end() && !contract_port->input->context_keys.empty())
                                        next_binding.context_key = contract_port->input->context_keys.front();
                                }
                                EditorCompositionCommands commands(document);
                                const auto result = commands.set_input_source(next_binding);
                                last_action_ = result ? "Input source updated" : result.message;
                                ImGui::CloseCurrentPopup();
                            }
                            if (!enabled) {
                                ImGui::EndDisabled();
                                ImGui::TextDisabled("  %s (not declared)", label);
                            }
                        }
                        ImGui::EndCombo();
                    }
                    if (source == pipeline::InputSourceKind::kAsset) {
                        const std::string buffer_key = selected->node_id + ":" + std::to_string(port.port_id) + ":" +
                                                       std::to_string(port.slot_id);
                        auto& buffer = input_buffers_[buffer_key];
                        if (buffer[0] == '\0' && binding != snapshot->input_bindings.end()) {
                            const std::size_t count = std::min(binding->asset.uuid.size(), buffer.size() - 1u);
                            std::copy_n(binding->asset.uuid.data(), count, buffer.data());
                            buffer[count] = '\0';
                        }
                        ImGui::SetNextItemWidth(300.0f);
                        const bool asset_edited = ImGui::InputText(("AssetId##" + buffer_key).c_str(), buffer.data(),
                                                                   buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
                        if (asset_edited || (ImGui::IsItemDeactivatedAfterEdit() && ImGui::IsItemEdited())) {
                            pipeline::InputBindingDocument asset_binding{};
                            asset_binding.node_id = selected->node_id;
                            asset_binding.port_id = port.port_id;
                            asset_binding.slot_id = port.slot_id;
                            asset_binding.source = pipeline::InputSourceKind::kAsset;
                            asset_binding.asset.uuid = buffer.data();
                            asset_binding.status = pipeline::InputBindingStatus::kBound;
                            EditorCompositionCommands commands(document);
                            const auto result = commands.set_input_source(asset_binding);
                            last_action_ = result ? "Asset source updated" : result.message;
                        }
                    } else if (source == pipeline::InputSourceKind::kDefault && contract_port != type->ports.end() &&
                               contract_port->input->default_value) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("Default #%llu rev %llu",
                                           static_cast<unsigned long long>(contract_port->input->default_value->id),
                                           static_cast<unsigned long long>(contract_port->input->default_value->revision));
                    } else if (source == pipeline::InputSourceKind::kContext && contract_port != type->ports.end()) {
                        std::string context = binding == snapshot->input_bindings.end() ? std::string{}
                                                                                          : binding->context_key;
                        const std::string context_id = "##context-" + std::to_string(port.port_id) + "-" +
                                                       std::to_string(port.slot_id);
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(180.0f);
                        if (ImGui::BeginCombo(context_id.c_str(), context.empty() ? "Select context" : context.c_str())) {
                            for (const auto& key : contract_port->input->context_keys) {
                                if (!ImGui::Selectable(key.c_str(), key == context))
                                    continue;
                                pipeline::InputBindingDocument context_binding =
                                    binding == snapshot->input_bindings.end() ? pipeline::InputBindingDocument{} : *binding;
                                context_binding.node_id = selected->node_id;
                                context_binding.port_id = port.port_id;
                                context_binding.slot_id = port.slot_id;
                                context_binding.source = pipeline::InputSourceKind::kContext;
                                context_binding.context_key = key;
                                context_binding.status = pipeline::InputBindingStatus::kBound;
                                const auto result = EditorCompositionCommands(document).set_input_source(context_binding);
                                last_action_ = result ? "Context source updated" : result.message;
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::EndCombo();
                        }
                    } else if (source == pipeline::InputSourceKind::kLiteral && contract_port != type->ports.end() &&
                               contract_port->input->literal_schema) {
                        const auto& schema = *contract_port->input->literal_schema;
                        pipeline::ParameterValue value = binding == snapshot->input_bindings.end()
                                                             ? schema.default_value
                                                             : binding->literal;
                        bool changed = false;
                        if (schema.kind == pipeline::ParameterKind::kBool) {
                            bool value_bool = std::get_if<bool>(&value) ? *std::get_if<bool>(&value) : false;
                            changed = ImGui::Checkbox(("##literal-" + std::to_string(port.port_id) + "-" +
                                                       std::to_string(port.slot_id)).c_str(), &value_bool);
                            value = value_bool;
                        } else if (schema.kind == pipeline::ParameterKind::kFloat64) {
                            double value_double = std::get_if<double>(&value) ? *std::get_if<double>(&value) : 0.0;
                            changed = ImGui::InputDouble(("##literal-" + std::to_string(port.port_id) + "-" +
                                                          std::to_string(port.slot_id)).c_str(), &value_double);
                            value = value_double;
                        } else if (schema.kind == pipeline::ParameterKind::kString) {
                            const std::string key = selected->node_id + ":literal:" + std::to_string(port.port_id) + ":" +
                                                    std::to_string(port.slot_id);
                            auto& buffer = input_buffers_[key];
                            if (buffer[0] == '\0' && binding != snapshot->input_bindings.end()) {
                                const auto* text = std::get_if<std::string>(&binding->literal);
                                if (text != nullptr)
                                    std::copy_n(text->data(), std::min(text->size(), buffer.size() - 1u), buffer.data());
                            }
                            changed = ImGui::InputText(("##literal-" + key).c_str(), buffer.data(), buffer.size(),
                                                       ImGuiInputTextFlags_EnterReturnsTrue);
                            value = std::string{buffer.data()};
                        } else {
                            ImGui::TextDisabled("Literal schema: %s", schema.display_name.empty() ? schema.name.c_str()
                                                                                                    : schema.display_name.c_str());
                        }
                        if (changed) {
                            pipeline::InputBindingDocument literal_binding =
                                binding == snapshot->input_bindings.end() ? pipeline::InputBindingDocument{} : *binding;
                            literal_binding.node_id = selected->node_id;
                            literal_binding.port_id = port.port_id;
                            literal_binding.slot_id = port.slot_id;
                            literal_binding.source = pipeline::InputSourceKind::kLiteral;
                            literal_binding.literal = std::move(value);
                            literal_binding.status = pipeline::InputBindingStatus::kBound;
                            const auto result = EditorCompositionCommands(document).set_input_source(literal_binding);
                            last_action_ = result ? "Literal source updated" : result.message;
                        }
                    }
                }
                for (const auto& group : type->variadic_inputs) {
                    std::vector<const pipeline::VariadicInputSlotDocument*> slots;
                    const auto snapshot = document.snapshot();
                    for (const auto& slot : snapshot->variadic_slots)
                        if (slot.node_id == selected->node_id && slot.group_id == group.id)
                            slots.push_back(&slot);
                    std::sort(slots.begin(), slots.end(), [](const auto* left, const auto* right) {
                        return left->order < right->order;
                    });
                    ImGui::Separator();
                    ImGui::Text("%s (%zu/%u)", group.name.c_str(), slots.size(), group.max_slots);
                    ImGui::SameLine();
                    EditorCompositionCommands commands(document);
                    if (ImGui::SmallButton(("+##variadic-" + std::to_string(group.id)).c_str())) {
                        const auto result = commands.add_variadic_slot(selected->node_id, group.id);
                        last_action_ = result ? "Added input slot" : result.message;
                    }
                    ImGui::SameLine();
                    const bool can_remove = slots.size() > group.min_slots;
                    if (!can_remove) ImGui::BeginDisabled();
                    if (ImGui::SmallButton(("-##variadic-" + std::to_string(group.id)).c_str()) && !slots.empty()) {
                        const auto result = commands.remove_variadic_slot(selected->node_id, group.id, slots.back()->slot_id);
                        last_action_ = result ? "Removed input slot" : result.message;
                    }
                    if (!can_remove) ImGui::EndDisabled();
                    for (std::size_t index = 0; index < slots.size(); ++index) {
                        ImGui::Text("Slot %zu  #%llu", index + 1,
                                    static_cast<unsigned long long>(slots[index]->slot_id));
                        ImGui::SameLine();
                        if (index > 0 && ImGui::SmallButton(("↑##slot-" + std::to_string(slots[index]->slot_id)).c_str()))
                            last_action_ = commands.move_variadic_slot(selected->node_id, group.id, slots[index]->slot_id, -1)
                                ? "Moved input slot" : "Cannot move input slot";
                        ImGui::SameLine();
                        if (index + 1 < slots.size() && ImGui::SmallButton(("↓##slot-" + std::to_string(slots[index]->slot_id)).c_str()))
                            last_action_ = commands.move_variadic_slot(selected->node_id, group.id, slots[index]->slot_id, 1)
                                ? "Moved input slot" : "Cannot move input slot";
                    }
                }
            }
        }
    }
    };
    const float remaining_height = std::max(0.0f, ImGui::GetContentRegionAvail().y);
    const float diagnostics_height = std::clamp(remaining_height * 0.24f, 112.0f, 190.0f);
    const float canvas_height = std::max(96.0f, remaining_height - diagnostics_height);
    // Give the canvas an explicit viewport width. A zero width child lets its
    // off-screen node hit targets become the table column's minimum width,
    // pushing the Inspector past the right edge of the editor.
    const float canvas_width = std::max(240.0f, ImGui::GetContentRegionAvail().x);
    const ImVec2 canvas_size{canvas_width, canvas_height};
    const auto add_palette_type_at = [&](const FlowGraphPalettePayload& payload, ImVec2 graph_position) {
        const auto kind = static_cast<pipeline::AuthoringTypeKind>(payload.kind);
        const auto* entry = document.catalog().find({kind, payload.type_id});
        if (entry == nullptr) {
            last_action_ = "The dragged node type is not available in this catalog";
            return false;
        }
        const auto snapshot = document.snapshot();
        std::string base = entry->display_name.empty() ? entry->name : entry->display_name;
        std::replace_if(base.begin(), base.end(), [](char character) {
            return !std::isalnum(static_cast<unsigned char>(character)) && character != '_';
        }, '_');
        const std::string node_id = make_node_id(*snapshot, base);
        std::string attachment;
        if (address_.kind == EditorGraphKind::kFeature) {
            const auto pipeline_node = std::find_if(snapshot->pipeline_nodes.begin(), snapshot->pipeline_nodes.end(),
                                                    [&](const auto& node) { return node.node_id == address_.pipeline_node; });
            const auto* pipeline_entry = pipeline_node == snapshot->pipeline_nodes.end()
                                             ? nullptr
                                             : document.catalog().find({pipeline::AuthoringTypeKind::kPipeline,
                                                                        pipeline_node->type.value()});
            const auto* declaration = pipeline_entry == nullptr
                                          ? nullptr
                                          : std::get_if<pipeline::PipelineAuthoringDeclaration>(&pipeline_entry->declaration);
            if (declaration != nullptr && !declaration->feature_points.empty())
                attachment = std::to_string(declaration->feature_points.front().id.value());
        } else if (address_.kind == EditorGraphKind::kPass) {
            attachment = address_.part_id;
        }
        const auto result = EditorCompositionCommands(document).add_type(address_, entry->key, node_id, attachment);
        last_action_ = result ? "Added " + entry->display_name : result.message;
        if (!result)
            return false;
        selected_node_ = node_id;
        positions_[node_id] = {graph_position, true};
        static_cast<void>(document.set_layout(node_id, graph_position.x, graph_position.y));
        return true;
    };
    const auto register_category_at = [&](const FlowGraphCategoryPayload& payload, ImVec2 graph_position) {
        const auto category = static_cast<pipeline::AuthoringTypeKind>(payload.kind);
        const bool pipeline_category = category == pipeline::AuthoringTypeKind::kPipeline;
        const bool feature_category = category == pipeline::AuthoringTypeKind::kFeature;
        const bool valid_category = address_.kind == EditorGraphKind::kPipeline
                                        ? pipeline_category
                                        : address_.kind == EditorGraphKind::kFeature
                                              ? feature_category
                                              : category == pipeline::AuthoringTypeKind::kRenderPass;
        if (!valid_category) {
            last_action_ = "This node category is only available in its matching graph";
            return false;
        }
        EditorGraphAddress registration_address = address_;
        const std::string requested_name = "Game.Custom." +
                                           (feature_category ? std::string{"Feature"} :
                                            pipeline_category ? std::string{"Pipeline"} :
                                                                graph_type_prefix(address_.kind));
        std::string created_node_id;
        std::string created_type_name;
        pipeline::AuthoringTypeKey created_type_key{};
        const auto created = create_empty_node(document, registration_address, requested_name, &created_node_id,
                                               &created_type_name, &created_type_key);
        if (!created) {
            last_action_ = created.message;
            return false;
        }
        selected_node_ = created_node_id;
        pending_layout_node_ = created_node_id;
        positions_[created_node_id] = {graph_position, true};
        static_cast<void>(document.set_layout(created_node_id, graph_position.x, graph_position.y));

        const auto* entry = document.catalog().find(created_type_key);
        if (entry == nullptr) {
            last_action_ = "Registered node, but its catalog contract is unavailable";
            return true;
        }
        if (project_root_.empty()) {
            scaffold_status_ = "Registered " + created_type_name + ". Open a project to generate its code stub.";
            last_action_ = "Registered " + created_type_name;
            return true;
        }
        CustomNodeScaffoldRequest request;
        request.project_root = project_root_;
        request.namespace_name = "game";
        const auto separator = created_type_name.find_last_of('.');
        request.type_name = created_type_name.substr(separator == std::string::npos ? 0 : separator + 1);
        std::replace_if(request.type_name.begin(), request.type_name.end(), [](char character) {
            return !std::isalnum(static_cast<unsigned char>(character)) && character != '_';
        }, '_');
        request.type_name = "FlowGraph_" + request.type_name;
        const auto generated = scaffold_custom_node(request, *entry);
        if (!generated) {
            scaffold_status_ = "Registered " + created_type_name + ", but code generation failed: " +
                               generated.result.message;
            last_action_ = "Registered " + created_type_name;
            return true;
        }
        scaffold_implementation_ = generated.value.implementation_target.path;
        scaffold_status_ = "Registered " + created_type_name + " and generated " +
                           std::to_string(generated.value.files.size()) + " code files.";
        last_action_ = "Registered " + created_type_name + " and generated its code stub";
        return true;
    };
    std::optional<FlowGraphPalettePayload> palette_drag_preview{};
    ImVec2 palette_drag_position{};
    std::optional<FlowGraphCategoryPayload> palette_category_drag_preview{};
    ImVec2 palette_category_drag_position{};
    // The graph owns its camera offset, so the child remains a fixed viewport
    // and never steals the main window's scroll gesture. New nodes are placed
    // on deterministic free grid cells and remain reachable through panning.
    if (ImGui::BeginChild("##flowgraph-canvas", canvas_size, true,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        // The child is a fixed viewport. Keep its scroll position at zero and
        // move the graph with a signed camera offset; relying on ImGui's
        // ScrollMax makes a small graph impossible to pan.
        ImGuiWindow* canvas_window = ImGui::GetCurrentWindow();
        const ImVec2 viewport_min = ImGui::GetCursorScreenPos();
        const ImVec2 content_max{canvas_window->InnerRect.Max.x - canvas_window->WindowPadding.x,
                                 canvas_window->InnerRect.Max.y - canvas_window->WindowPadding.y};
        const ImVec2 extent{std::max(0.0f, content_max.x - viewport_min.x),
                            std::max(0.0f, content_max.y - viewport_min.y)};
        const ImVec2 graph_origin{viewport_min.x + canvas_pan_.x, viewport_min.y + canvas_pan_.y};
        const ImGuiIO& io = ImGui::GetIO();
        // A child window is not itself an ImGui item after the cursor has
        // moved into its contents, so use an explicit target rectangle. This
        // makes drops work on the grid, including empty areas between nodes.
        if (ImGui::BeginDragDropTargetCustom(ImRect{viewport_min, content_max},
                                              ImGui::GetID("##flowgraph-canvas-drop"))) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                    palette_payload_type, ImGuiDragDropFlags_AcceptBeforeDelivery |
                                               ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
                payload != nullptr && payload->DataSize == static_cast<int>(sizeof(FlowGraphPalettePayload))) {
                FlowGraphPalettePayload palette_payload{};
                std::memcpy(&palette_payload, payload->Data, sizeof(palette_payload));
                const ImVec2 mouse = io.MousePos;
                const auto graph_point = flowgraph::graph_position_from_mouse(
                    {mouse.x, mouse.y}, {graph_origin.x, graph_origin.y}, zoom_);
                const ImVec2 graph_position{graph_point.x, graph_point.y};
                if (payload->IsDelivery())
                    add_palette_type_at(palette_payload, graph_position);
                else {
                    palette_drag_preview = palette_payload;
                    palette_drag_position = graph_position;
                }
            }
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                    palette_category_payload_type, ImGuiDragDropFlags_AcceptBeforeDelivery |
                                                         ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
                payload != nullptr && payload->DataSize == static_cast<int>(sizeof(FlowGraphCategoryPayload))) {
                FlowGraphCategoryPayload category_payload{};
                std::memcpy(&category_payload, payload->Data, sizeof(category_payload));
                const ImVec2 mouse = io.MousePos;
                const auto graph_point = flowgraph::graph_position_from_mouse(
                    {mouse.x, mouse.y}, {graph_origin.x, graph_origin.y}, zoom_);
                const ImVec2 graph_position{graph_point.x, graph_point.y};
                if (payload->IsDelivery())
                    register_category_at(category_payload, graph_position);
                else {
                    palette_category_drag_preview = category_payload;
                    palette_category_drag_position = graph_position;
                }
            }
            ImGui::EndDragDropTarget();
        }
        const bool space_pan = ImGui::IsKeyDown(ImGuiKey_Space);
        const bool canvas_keyboard_focus = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                                           !ImGui::IsAnyItemActive();
        const bool command_modifier = io.KeyCtrl || io.KeySuper;
        const bool focus_selected_request = canvas_keyboard_focus && ImGui::IsKeyPressed(ImGuiKey_F);
        const bool focus_all_request = canvas_keyboard_focus && ImGui::IsKeyPressed(ImGuiKey_A);
        if (canvas_keyboard_focus && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            connect_source_node_.clear();
            connect_source_port_ = 0;
            connect_source_slot_ = 0;
            selected_edge_.clear();
            last_action_ = "Cancelled FlowGraph selection";
        }
        if (canvas_keyboard_focus && command_modifier && ImGui::IsKeyPressed(ImGuiKey_Z)) {
            const auto result = document.undo();
            last_action_ = result ? "Undid FlowGraph edit" : result.message;
        } else if (canvas_keyboard_focus && command_modifier && ImGui::IsKeyPressed(ImGuiKey_Y)) {
            const auto result = document.redo();
            last_action_ = result ? "Redid FlowGraph edit" : result.message;
        } else if (canvas_keyboard_focus && command_modifier && ImGui::IsKeyPressed(ImGuiKey_S)) {
            if (actions.save_draft) {
                const auto result = actions.save_draft();
                last_action_ = result ? "FlowGraph draft saved" : result.message;
            } else {
                last_action_ = "FlowGraph save is unavailable";
            }
        }
        if (canvas_keyboard_focus && command_modifier && ImGui::IsKeyPressed(ImGuiKey_D) &&
            !selected_node_.empty()) {
            const auto* selected = find_node(view, selected_node_);
            if (selected == nullptr || selected->fixed || selected->kind == EditorGraphNodeKind::kPart) {
                last_action_ = "The selected node cannot be duplicated";
            } else {
                std::string copy_id = selected_node_ + "_Copy";
                const auto snapshot_for_duplicate = document.snapshot();
                const auto exists = [&](std::string_view id) {
                    return std::any_of(snapshot_for_duplicate->pipeline_nodes.begin(), snapshot_for_duplicate->pipeline_nodes.end(),
                                       [id](const auto& value) { return value.node_id == id; }) ||
                           std::any_of(snapshot_for_duplicate->feature_nodes.begin(), snapshot_for_duplicate->feature_nodes.end(),
                                       [id](const auto& value) { return value.node_id == id; }) ||
                           std::any_of(snapshot_for_duplicate->pass_nodes.begin(), snapshot_for_duplicate->pass_nodes.end(),
                                       [id](const auto& value) { return value.node_id == id; });
                };
                for (uint32_t suffix = 2; exists(copy_id); ++suffix)
                    copy_id = selected_node_ + "_Copy" + std::to_string(suffix);
                const auto result = EditorCompositionCommands(document).duplicate_node(selected_node_, copy_id);
                last_action_ = result ? "Duplicated " + selected_node_ : result.message;
                if (result) {
                    selected_node_ = std::move(copy_id);
                    pending_layout_node_ = selected_node_;
                }
            }
        }
        if (canvas_keyboard_focus && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            const auto selected_edge = std::find_if(view.edges.begin(), view.edges.end(),
                                                    [&](const auto& edge) { return edge.edge_id == selected_edge_; });
            if (selected_edge != view.edges.end() && !selected_edge->fixed) {
                const auto result = disconnect_edge_view(*selected_edge);
                last_action_ = result ? "Disconnected " + selected_edge_ : result.message;
                if (result)
                    selected_edge_.clear();
            } else if (!selected_node_.empty()) {
                const auto* selected = find_node(view, selected_node_);
                if (selected == nullptr || selected->fixed || selected->kind == EditorGraphNodeKind::kPart) {
                    last_action_ = "The selected node cannot be deleted";
                } else {
                    const auto node_id = selected_node_;
                    const auto result = EditorCompositionCommands(document).erase_node(node_id);
                    last_action_ = result ? "Deleted " + node_id : result.message;
                    if (result)
                        selected_node_.clear();
                }
            }
        }
        // Detached/native FlowGraph windows use a separate ImGui viewport. In
        // that path the child-window hover query can briefly refer to the
        // parent viewport while the pointer is still inside the visible
        // canvas. Keep the screen-space rectangle as a fallback, but still
        // require the FlowGraph root/children to own the hover. This keeps a
        // popup or another native viewport from panning the graph underneath.
        const bool mouse_in_canvas = io.MousePos.x >= viewport_min.x && io.MousePos.x <= content_max.x &&
                                     io.MousePos.y >= viewport_min.y && io.MousePos.y <= content_max.y;
        ImGuiContext* imgui_context = ImGui::GetCurrentContext();
        ImGuiWindow* hovered_window = imgui_context != nullptr ? imgui_context->HoveredWindow : nullptr;
        const bool flowgraph_hovered = hovered_window == nullptr || hovered_window == canvas_window ||
                                       ImGui::IsWindowChildOf(hovered_window, canvas_window, false, true) ||
                                       ImGui::IsWindowChildOf(canvas_window, hovered_window, false, true);
        const bool popup_open = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        const bool canvas_hovered = mouse_in_canvas && flowgraph_hovered && !popup_open;
        const bool begin_middle_pan = canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
        const bool begin_modifier_pan = canvas_hovered && space_pan &&
                                        ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                                        !ImGui::IsAnyItemActive();
        const bool begin_empty_left_candidate = canvas_hovered && !space_pan &&
                                                ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                                                !ImGui::IsAnyItemActive();
        const bool pan_was_active = canvas_pan_active_;
        if (!canvas_pan_active_ && (begin_middle_pan || begin_modifier_pan)) {
            canvas_pan_active_ = true;
            canvas_pan_button_ = begin_middle_pan ? ImGuiMouseButton_Middle : ImGuiMouseButton_Left;
        }
        if (!canvas_pan_active_ && !canvas_pan_candidate_ && begin_empty_left_candidate) {
            // Defer deciding whether this is a canvas gesture until the
            // current frame has reconstructed node and edge hit regions.
            canvas_pan_candidate_ = true;
            canvas_pan_press_position_ = io.MousePos;
        }
        if (canvas_pan_active_) {
            if (ImGui::IsMouseDown(canvas_pan_button_)) {
                // Capture the gesture until release, even when the pointer
                // leaves the child viewport. This keeps a long pan from
                // stopping at the canvas edge.
                canvas_pan_.x += io.MouseDelta.x;
                canvas_pan_.y += io.MouseDelta.y;
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            } else {
                canvas_pan_active_ = false;
            }
        }
        // A pan is a viewport gesture. Suppress every left-button graph
        // semantic handler for its duration so Space+left cannot select a
        // node, start a wire, or double-click an edge at the same time.
        bool suppress_graph_left_gestures = pan_was_active || canvas_pan_active_ || space_pan;
        const float grid = 24.0f * zoom_;
        const float grid_x = flowgraph::first_grid_line_offset(-canvas_pan_.x, grid);
        const float grid_y = flowgraph::first_grid_line_offset(-canvas_pan_.y, grid);
        for (float x = viewport_min.x + grid_x; x <= content_max.x; x += grid)
            draw_list->AddLine({x, viewport_min.y}, {x, content_max.y},
                               IM_COL32(35, 49, 57, 180));
        for (float y = viewport_min.y + grid_y; y <= content_max.y; y += grid)
            draw_list->AddLine({viewport_min.x, y}, {content_max.x, y},
                               IM_COL32(35, 49, 57, 180));
        std::unordered_map<std::string, ImVec2> anchors;
        std::unordered_map<std::string, ImVec2> port_anchors;
        std::vector<std::pair<ImVec2, ImVec2>> occupied;
        const auto snapshot = document.snapshot();
        bool refresh_positions_after_reorder = false;
        // Older documents persisted one layout for every hidden Resource
        // relay.  Once the Pass graph became a strict Pass-only projection,
        // those coordinates left real Pass cards outside the viewport (and
        // made the graph look as if only a subset of its nodes existed).
        // Migrate that stale projection once, using the visible dependency
        // graph below to place every Pass in a readable left-to-right layout.
        bool migrate_stale_pass_layout = false;
        if (address_.kind == EditorGraphKind::kPass && snapshot != nullptr &&
            migrated_pass_layout_scope_ != position_scope_) {
            const auto is_visible = [&](std::string_view node_id) {
                return std::any_of(view.nodes.begin(), view.nodes.end(),
                                   [node_id](const auto& node) { return node.node_id == node_id; });
            };
            const bool has_hidden_resource_layout = std::any_of(
                snapshot->layouts.begin(), snapshot->layouts.end(), [&](const auto& layout) {
                    return !is_visible(layout.node_id) &&
                           std::any_of(snapshot->resources.begin(), snapshot->resources.end(),
                                       [&](const auto& resource) { return resource.node_id == layout.node_id; });
                });
            if (has_hidden_resource_layout && !view.nodes.empty()) {
                const float safe_zoom = std::max(zoom_, 0.01f);
                const float graph_width = extent.x / safe_zoom;
                const float graph_height = extent.y / safe_zoom;
                const bool has_visible_layout_outside_view = std::any_of(
                    view.nodes.begin(), view.nodes.end(), [&](const auto& node) {
                        const auto saved = std::find_if(
                            snapshot->layouts.begin(), snapshot->layouts.end(),
                            [&](const auto& layout) { return layout.node_id == node.node_id; });
                        if (saved == snapshot->layouts.end())
                            return false;
                        return saved->x < -128.0 || saved->y < -128.0 ||
                               saved->x > static_cast<double>(graph_width + 128.0f) ||
                               saved->y > static_cast<double>(graph_height + 128.0f);
                    });
                migrate_stale_pass_layout = has_visible_layout_outside_view;
            }
        }
        if (migrate_stale_pass_layout) {
            for (const auto& node : view.nodes)
                positions_.erase(node.node_id);
            migrated_pass_layout_scope_ = position_scope_;
            last_action_ = "Migrated the Pass graph layout to the visible Pass nodes";
        }
        for (const auto& node : view.nodes) {
            auto& position = positions_[node.node_id];
            if (position.initialized)
                continue;
            if (migrate_stale_pass_layout)
                continue;
            const auto saved_layout = std::find_if(snapshot->layouts.begin(), snapshot->layouts.end(),
                                                   [&](const auto& layout) { return layout.node_id == node.node_id; });
            if (saved_layout != snapshot->layouts.end()) {
                position.value = {static_cast<float>(saved_layout->x), static_cast<float>(saved_layout->y)};
                position.initialized = true;
            }
        }
        if (!pending_layout_node_.empty())
            positions_[pending_layout_node_].initialized = false;
        // Preserve every position that already belongs to this graph before
        // placing new nodes.  The view is sorted by node id, so assigning as
        // we iterate would let a newly-created node take the position of an
        // existing node that happens to sort after it.
        for (const auto& node : view.nodes) {
            const auto found = positions_.find(node.node_id);
            if (found != positions_.end() && found->second.initialized)
                occupied.emplace_back(found->second.value, ImVec2{220.0f, node_height(node)});
        }
        // New graphs are placed by dependency depth so the first frame reads
        // like the reference data-flow view: producers on the left, consumers
        // on the right.  Saved/manual coordinates still win and are never
        // rearranged here.
        std::unordered_map<std::string, std::size_t> layers;
        for (const auto& node : view.nodes)
            layers.emplace(node.node_id, 0);
        for (std::size_t pass = 0; pass < view.nodes.size(); ++pass) {
            bool changed = false;
            for (const auto& edge : view.edges) {
                const auto source = layers.find(edge.source_node);
                const auto destination = layers.find(edge.destination_node);
                if (source == layers.end() || destination == layers.end())
                    continue;
                const auto proposed = source->second + 1;
                if (destination->second < proposed) {
                    destination->second = proposed;
                    changed = true;
                }
            }
            if (!changed)
                break;
        }
        std::unordered_map<std::size_t, std::size_t> layer_rows;
        for (const auto& node : view.nodes) {
            auto& position = positions_[node.node_id];
            const ImVec2 size{220.0f, node_height(node)};
            if (!position.initialized) {
                const std::size_t layer = layers[node.node_id];
                std::size_t row = layer_rows[layer]++;
                position.value = {32.0f + static_cast<float>(layer) * (220.0f + 92.0f),
                                  32.0f + static_cast<float>(row) * (150.0f + 28.0f)};
                while (std::any_of(occupied.begin(), occupied.end(), [&](const auto& other) {
                    return overlaps(position.value, size, other.first, other.second);
                })) {
                    position.value.y += 178.0f;
                }
                position.initialized = true;
                occupied.emplace_back(position.value, size);
                // Materialize the first deterministic placement immediately.
                // This keeps a newly-created node away from existing nodes on
                // the next frame and makes the layout survive a document
                // reload instead of recomputing over an old card.
                static_cast<void>(document.set_layout(node.node_id, position.value.x, position.value.y));
            }
            const ImVec2 min{graph_origin.x + position.value.x * zoom_, graph_origin.y + position.value.y * zoom_};
            const ImVec2 max{min.x + size.x * zoom_, min.y + size.y * zoom_};
            anchors.emplace(node.node_id, ImVec2{(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f});
            const auto card = flowgraph::node_layout(
                static_cast<std::size_t>(std::count_if(node.ports.begin(), node.ports.end(),
                                                       [](const auto& port) { return port.input; })),
                static_cast<std::size_t>(std::count_if(node.ports.begin(), node.ports.end(),
                                                       [](const auto& port) { return !port.input; })));
            std::size_t input_row = 0;
            std::size_t output_row = 0;
            for (const auto& port : node.ports) {
                const auto row = port.input ? input_row++ : output_row++;
                const auto anchor = port.input ? card.input_anchor(row) : card.output_anchor(row);
                port_anchors.emplace(flowgraph::port_anchor_key(node.node_id, port.input, port.port_id,
                                                                  port.slot_id),
                                     ImVec2{min.x + anchor.x * zoom_, min.y + anchor.y * zoom_});
            }
        }
        if (palette_drag_preview.has_value()) {
            const auto kind = static_cast<pipeline::AuthoringTypeKind>(palette_drag_preview->kind);
            const auto* entry = document.catalog().find({kind, palette_drag_preview->type_id});
            const std::string title = entry == nullptr
                                          ? "Unknown node"
                                          : (entry->display_name.empty() ? entry->name : entry->display_name);
            const ImVec2 min{graph_origin.x + palette_drag_position.x * zoom_,
                             graph_origin.y + palette_drag_position.y * zoom_};
            const ImVec2 max{min.x + 220.0f * zoom_, min.y + 58.0f * zoom_};
            const ImU32 fill = kind == pipeline::AuthoringTypeKind::kPipeline ? IM_COL32(52, 116, 157, 44)
                              : kind == pipeline::AuthoringTypeKind::kFeature ? IM_COL32(52, 147, 112, 44)
                              : IM_COL32(125, 94, 171, 44);
            // The foreground card follows the cursor for the entire gesture.
            // On the canvas, add only a quiet placement target so the two
            // previews never stack on top of each other.
            draw_list->AddRectFilled(min, max, fill, 6.0f);
            draw_list->AddRect(min, max, IM_COL32(112, 226, 218, 215), 6.0f, 0, 2.0f);
            if (!palette_drag_source_active) {
                draw_ellipsis(draw_list, {min.x + 10.0f * zoom_, min.y + 10.0f * zoom_},
                              max.x - 10.0f * zoom_, title, IM_COL32(245, 252, 253, 255));
                draw_list->AddText({min.x + 10.0f * zoom_, min.y + 34.0f * zoom_},
                                   IM_COL32(194, 222, 228, 245), "Release to place");
            }
        }
        if (palette_category_drag_preview.has_value()) {
            const auto kind = static_cast<pipeline::AuthoringTypeKind>(palette_category_drag_preview->kind);
            const char* title = kind == pipeline::AuthoringTypeKind::kPipeline
                                    ? "New Pipeline"
                                    : kind == pipeline::AuthoringTypeKind::kFeature ? "New Feature" : "New Render Pass";
            const ImVec2 min{graph_origin.x + palette_category_drag_position.x * zoom_,
                             graph_origin.y + palette_category_drag_position.y * zoom_};
            const ImVec2 max{min.x + 220.0f * zoom_, min.y + 58.0f * zoom_};
            const ImU32 fill = kind == pipeline::AuthoringTypeKind::kPipeline ? IM_COL32(52, 116, 157, 60)
                              : kind == pipeline::AuthoringTypeKind::kFeature ? IM_COL32(52, 147, 112, 60)
                              : IM_COL32(242, 144, 44, 60);
            draw_list->AddRectFilled(min, max, fill, 6.0f);
            draw_list->AddRect(min, max, IM_COL32(112, 226, 218, 215), 6.0f, 0, 2.0f);
            if (!palette_drag_source_active) {
                draw_ellipsis(draw_list, {min.x + 10.0f * zoom_, min.y + 10.0f * zoom_},
                              max.x - 10.0f * zoom_, title, IM_COL32(245, 252, 253, 255));
                draw_list->AddText({min.x + 10.0f * zoom_, min.y + 34.0f * zoom_},
                                   IM_COL32(194, 222, 228, 245), "Release to register");
            }
        }
        if (focus_selected_request || focus_all_request) {
            bool have_focus_bounds = false;
            ImVec2 focus_min{};
            ImVec2 focus_max{};
            const auto include_bounds = [&](const ImVec2& position, const ImVec2& size) {
                const ImVec2 min{position.x * zoom_, position.y * zoom_};
                const ImVec2 max{min.x + size.x * zoom_, min.y + size.y * zoom_};
                if (!have_focus_bounds) {
                    focus_min = min;
                    focus_max = max;
                    have_focus_bounds = true;
                } else {
                    focus_min.x = std::min(focus_min.x, min.x);
                    focus_min.y = std::min(focus_min.y, min.y);
                    focus_max.x = std::max(focus_max.x, max.x);
                    focus_max.y = std::max(focus_max.y, max.y);
                }
            };
            if (focus_selected_request && !selected_node_.empty()) {
                const auto found = positions_.find(selected_node_);
                const auto selected_view = find_node(view, selected_node_);
                if (found != positions_.end() && found->second.initialized && selected_view != nullptr)
                    include_bounds(found->second.value, ImVec2{220.0f, node_height(*selected_view)});
            } else if (focus_all_request) {
                for (const auto& node : view.nodes) {
                    const auto found = positions_.find(node.node_id);
                    if (found != positions_.end() && found->second.initialized)
                        include_bounds(found->second.value, ImVec2{220.0f, node_height(node)});
                }
            }
            if (have_focus_bounds) {
                const ImVec2 focus_center{(focus_min.x + focus_max.x) * 0.5f,
                                         (focus_min.y + focus_max.y) * 0.5f};
                canvas_pan_ = {extent.x * 0.5f - focus_center.x, extent.y * 0.5f - focus_center.y};
            } else if (focus_selected_request) {
                last_action_ = "Select a node before using F";
            } else {
                last_action_ = "The graph has no nodes to focus";
            }
        }
        pending_layout_node_.clear();
        const auto connect_ports = [&](std::string_view source_id, uint64_t source_port, uint64_t source_slot,
                                       std::string_view destination_id, uint64_t destination_port,
                                       uint64_t destination_slot, uint64_t binding_id,
                                       pipeline::DocumentEdgeKind edge_kind) {
            EditorCompositionCommands commands(document);
            EditorConnectionRequest request{};
            request.edge_id = "ui:" + std::string(source_id) + ":" + std::string(destination_id) + ":" +
                              std::to_string(source_port) + ":" + std::to_string(destination_port) + ":" +
                              std::to_string(source_slot) + ":" + std::to_string(destination_slot) + ":" +
                              std::to_string(document.revision());
            request.kind = edge_kind;
            request.source_node = std::string(source_id);
            request.source_port = source_port;
            request.source_slot = source_slot;
            request.destination_node = std::string(destination_id);
            request.destination_port = destination_port;
            request.destination_slot = destination_slot;
            request.binding_id = binding_id;
            return commands.connect(request);
        };
        bool graph_item_hovered = false;
        bool edge_double_click_handled = false;
        for (const auto& edge : view.edges) {
            const auto source_port_anchor = port_anchors.find(flowgraph::port_anchor_key(
                edge.source_node, false, edge.source_port, edge.source_slot));
            const auto destination_port_anchor = port_anchors.find(flowgraph::port_anchor_key(
                edge.destination_node, true, edge.destination_port, edge.destination_slot));
            const auto source_anchor = anchors.find(edge.source_node);
            const auto destination_anchor = anchors.find(edge.destination_node);
            const bool use_structural_anchors = edge.fixed && source_port_anchor == port_anchors.end() &&
                                                destination_port_anchor == port_anchors.end() &&
                                                source_anchor != anchors.end() && destination_anchor != anchors.end();
            if (!use_structural_anchors &&
                (source_port_anchor == port_anchors.end() || destination_port_anchor == port_anchors.end()))
                continue;
            const ImVec2 source_position = use_structural_anchors ? source_anchor->second : source_port_anchor->second;
            const ImVec2 destination_position = use_structural_anchors ? destination_anchor->second
                                                                         : destination_port_anchor->second;
            const auto* source_node = find_node(view, edge.source_node);
            const EditorGraphPortView* source_port = nullptr;
            if (source_node != nullptr) {
                const auto found_port = std::find_if(source_node->ports.begin(), source_node->ports.end(),
                                                     [&](const auto& port) {
                                                         return !port.input && port.port_id == edge.source_port &&
                                                                port.slot_id == edge.source_slot;
                                                     });
                if (found_port != source_node->ports.end())
                    source_port = &*found_port;
            }
            const ImU32 flow_color = source_port != nullptr ? port_color(source_port->value_kind)
                                                              : IM_COL32(72, 192, 185, 240);
            const ImU32 color = edge.edge_id == selected_edge_ ? IM_COL32(255, 220, 100, 255)
                                  : edge.fixed || edge.kind == pipeline::DocumentEdgeKind::kOrdering
                                      ? IM_COL32(190, 150, 62, 230)
                                      : flow_color;
            const float curve = std::max(60.0f, std::abs(destination_position.x - source_position.x) * 0.45f);
            draw_list->AddBezierCubic(source_position, {source_position.x + curve, source_position.y},
                                      {destination_position.x - curve, destination_position.y}, destination_position,
                                      color, edge.edge_id == selected_edge_ ? 3.0f : 2.0f);
            if (ImGui::IsWindowHovered() &&
                ((!suppress_graph_left_gestures && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) ||
                 ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
                const ImVec2 mouse = ImGui::GetIO().MousePos;
                ImVec2 previous = source_position;
                bool hit = false;
                for (int sample = 1; sample <= 20; ++sample) {
                    const float t = static_cast<float>(sample) / 20.0f;
                    const float u = 1.0f - t;
                    const ImVec2 current{
                            u * u * u * source_position.x + 3.0f * u * u * t * (source_position.x + curve) +
                            3.0f * u * t * t * (destination_position.x - curve) + t * t * t * destination_position.x,
                        u * u * u * source_position.y + 3.0f * u * u * t * source_position.y +
                            3.0f * u * t * t * destination_position.y + t * t * t * destination_position.y};
                    if (point_segment_distance(mouse, previous, current) <= 8.0f) { hit = true; break; }
                    previous = current;
                }
                if (hit) {
                    graph_item_hovered = true;
                    selected_edge_ = edge.edge_id;
                    if (!suppress_graph_left_gestures &&
                        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !edge_double_click_handled) {
                        edge_double_click_handled = true;
                        if (edge.fixed) {
                            last_action_ = edge.kind == pipeline::DocumentEdgeKind::kOrdering
                                                ? "Pipeline structure is fixed by the node contract"
                                                : "Feature binding is fixed by the pipeline contract";
                        } else {
                            const auto result = disconnect_edge_view(edge);
                            last_action_ = result ? "Disconnected " + edge.edge_id : result.message;
                            if (result)
                                selected_edge_.clear();
                        }
                    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                        ImGui::OpenPopup("FlowGraphEdgeContext");
                    }
                }
            }
        }
        if (canvas_pan_candidate_) {
            const bool left_still_down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
            bool press_over_node = false;
            if (left_still_down && !space_pan) {
                for (const auto& node : view.nodes) {
                    const auto position = positions_.find(node.node_id);
                    if (position == positions_.end() || !position->second.initialized)
                        continue;
                    const ImVec2 min{graph_origin.x + position->second.value.x * zoom_,
                                     graph_origin.y + position->second.value.y * zoom_};
                    const ImVec2 max{min.x + 220.0f * zoom_,
                                     min.y + node_height(node) * zoom_};
                    if (canvas_pan_press_position_.x >= min.x && canvas_pan_press_position_.x <= max.x &&
                        canvas_pan_press_position_.y >= min.y && canvas_pan_press_position_.y <= max.y) {
                        press_over_node = true;
                        break;
                    }
                }
            }
            if (!left_still_down || space_pan || press_over_node || graph_item_hovered) {
                canvas_pan_candidate_ = false;
            } else {
                canvas_pan_candidate_ = false;
                canvas_pan_active_ = true;
                canvas_pan_button_ = ImGuiMouseButton_Left;
                canvas_pan_.x += io.MouseDelta.x;
                canvas_pan_.y += io.MouseDelta.y;
                suppress_graph_left_gestures = true;
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            }
        }
        if (!connect_source_node_.empty() && connect_source_port_ != 0) {
            const auto source = port_anchors.find(flowgraph::port_anchor_key(
                connect_source_node_, false, connect_source_port_, connect_source_slot_));
            if (source != port_anchors.end() && ImGui::IsWindowHovered() && !suppress_graph_left_gestures)
                draw_list->AddLine(source->second, ImGui::GetIO().MousePos, IM_COL32(255, 220, 100, 190), 2.0f);
        }
        for (const auto& node : view.nodes) {
            auto& position = positions_[node.node_id];
            const auto card = flowgraph::node_layout(
                static_cast<std::size_t>(std::count_if(node.ports.begin(), node.ports.end(),
                                                       [](const auto& port) { return port.input; })),
                static_cast<std::size_t>(std::count_if(node.ports.begin(), node.ports.end(),
                                                       [](const auto& port) { return !port.input; })));
            const float height = card.height();
            const ImVec2 min{graph_origin.x + position.value.x * zoom_, graph_origin.y + position.value.y * zoom_};
            const ImVec2 max{min.x + card.width * zoom_, min.y + height * zoom_};
            const float radius = 6.0f * zoom_;
            draw_list->AddRectFilled(min, max, IM_COL32(14, 25, 34, 255), radius);
            const ImVec2 header_max{max.x, min.y + card.header_height * zoom_};
            draw_list->AddRectFilled(min, header_max, node_header_color(node.kind, node.unknown), radius);
            draw_list->AddRectFilled({min.x, header_max.y - radius}, {max.x, header_max.y},
                                     node_header_color(node.kind, node.unknown));
            // A quiet category rail and a darker body keep the card readable
            // when many nodes are visible together; the header remains the
            // only high-contrast area.
            draw_list->AddRectFilled({min.x, header_max.y}, {min.x + 4.0f * zoom_, max.y},
                                     node_color(node.kind, node.unknown));
            draw_list->AddLine({min.x + 4.0f * zoom_, header_max.y},
                               {min.x + 4.0f * zoom_, max.y}, IM_COL32(7, 15, 21, 220), 1.0f);
            draw_list->AddRect(min, max, node.node_id == selected_node_ ? IM_COL32(255, 220, 100, 255)
                                                                         : IM_COL32(20, 26, 30, 255),
                               radius, 0, node.node_id == selected_node_ ? 2.0f : 1.0f);
            // A compact category mark gives the card the same visual grammar
            // as the reference graph without changing the stable node title.
            draw_list->AddRectFilled({min.x + 9.0f * zoom_, min.y + 8.0f * zoom_},
                                     {min.x + 21.0f * zoom_, min.y + 20.0f * zoom_},
                                     node_header_color(node.kind, node.unknown), 2.0f * zoom_);
            ImGui::SetCursorScreenPos(min);
            ImGui::InvisibleButton(("##node-" + node.node_id).c_str(), {max.x - min.x, max.y - min.y});
            if (!suppress_graph_left_gestures && ImGui::IsItemActive() &&
                ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                position.value = {position.value.x + ImGui::GetIO().MouseDelta.x / zoom_,
                                  position.value.y + ImGui::GetIO().MouseDelta.y / zoom_};
                if (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f)
                    static_cast<void>(document.set_layout(node.node_id, position.value.x, position.value.y));
            }
            if (!suppress_graph_left_gestures && ImGui::IsItemClicked()) {
                // Node bodies select and drag only. Data flow is created from
                // an explicit output port to an explicit input port; guessing
                // a compatible pair from a body click can silently wire the
                // wrong source when a node exposes multiple ports.
                selected_node_ = node.node_id;
            }
            if (ImGui::IsItemHovered())
                graph_item_hovered = true;
            if (!suppress_graph_left_gestures && ImGui::IsItemHovered() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                selected_node_ = node.node_id;
                if (node.kind == EditorGraphNodeKind::kStage || node.kind == EditorGraphNodeKind::kFeaturePoint) {
                    last_action_ = "Pipeline structure is read-only";
                } else {
                bool entered_child_graph = false;
                if (node.kind == EditorGraphNodeKind::kPipeline && address_.kind == EditorGraphKind::kPipeline) {
                    // Pipeline graphs are the first layer in the hierarchy.
                    // Double-clicking a Pipeline enters its Feature
                    // collection; the empty feature_node is intentional and
                    // lets the graph show every Feature owned by that
                    // Pipeline rather than picking an arbitrary child.
                    address_ = {EditorGraphKind::kFeature, node.node_id, {}, {}};
                    position_scope_ = graph_scope(address_);
                    positions_.clear();
                    selected_node_.clear();
                    connect_source_node_.clear();
                    connect_source_port_ = 0;
                    connect_source_slot_ = 0;
                    last_action_ = "Entered Feature graph: " + node.node_id;
                    entered_child_graph = true;
                }
                if (!entered_child_graph && node.kind == EditorGraphNodeKind::kFeature &&
                    address_.kind == EditorGraphKind::kFeature) {
                    // Feature graphs are the second layer. Their Feature
                    // cards own the Pass graph; a sole render part is picked
                    // automatically, while multi-part Features remain in
                    // the aggregate view until the user chooses a part.
                    address_ = {EditorGraphKind::kPass, address_.pipeline_node, node.node_id,
                                single_render_part(document, node.node_id)};
                    position_scope_ = graph_scope(address_);
                    positions_.clear();
                    selected_node_.clear();
                    connect_source_node_.clear();
                    connect_source_port_ = 0;
                    connect_source_slot_ = 0;
                    last_action_ = "Entered Pass graph: " + node.node_id;
                    entered_child_graph = true;
                }
                if (!entered_child_graph && node.kind == EditorGraphNodeKind::kPart &&
                    address_.kind == EditorGraphKind::kFeature) {
                    constexpr std::string_view part_prefix{"::part::"};
                    if (node.node_id.starts_with(address_.feature_node + std::string{part_prefix})) {
                        const auto part_name = node.node_id.substr(address_.feature_node.size() + part_prefix.size());
                        if (!part_name.empty()) {
                            address_ = {EditorGraphKind::kPass, address_.pipeline_node, address_.feature_node,
                                        part_name};
                            position_scope_ = graph_scope(address_);
                            positions_.clear();
                            selected_node_.clear();
                            connect_source_node_.clear();
                            connect_source_port_ = 0;
                            connect_source_slot_ = 0;
                            last_action_ = "Entered Pass graph: " + part_name;
                            entered_child_graph = true;
                        }
                    }
                }
                if (!entered_child_graph) {
                    definition_node_ = node.node_id;
                    definition_popup_requested_ = true;
                    last_action_ = "Opening definition: " + node.node_id;
                }
                }
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                selected_node_ = node.node_id;
            const std::string context_id = "FlowGraphNodeContext##" + node.node_id;
            if (ImGui::BeginPopupContextItem(context_id.c_str())) {
                ImGui::TextDisabled("%s", node.node_id.c_str());
                ImGui::Separator();
                const bool has_node_definition = node.kind != EditorGraphNodeKind::kStage &&
                                                 node.kind != EditorGraphNodeKind::kFeaturePoint;
                if (ImGui::MenuItem("Open Node Definition", nullptr, false, has_node_definition)) {
                    definition_node_ = node.node_id;
                    definition_popup_requested_ = true;
                    last_action_ = "Opening definition: " + node.node_id;
                    ImGui::CloseCurrentPopup();
                }
                const bool can_enter_feature = node.kind == EditorGraphNodeKind::kPipeline &&
                                               address_.kind == EditorGraphKind::kPipeline;
                if (ImGui::MenuItem("Enter Feature Graph", nullptr, false, can_enter_feature)) {
                    address_ = {EditorGraphKind::kFeature, node.node_id, {}, {}};
                    positions_.clear();
                    position_scope_ = graph_scope(address_);
                    selected_node_.clear();
                    connect_source_node_.clear();
                    connect_source_port_ = 0;
                    connect_source_slot_ = 0;
                    last_action_ = "Entered Feature graph: " + node.node_id;
                    ImGui::CloseCurrentPopup();
                }
                const auto part_name_for_node = [&]() -> std::string {
                    if (node.kind != EditorGraphNodeKind::kPart || address_.kind != EditorGraphKind::kFeature)
                        return {};
                    const std::string prefix = address_.feature_node + "::part::";
                    if (!node.node_id.starts_with(prefix))
                        return {};
                    return node.node_id.substr(prefix.size());
                }();
                const bool can_enter_pass =
                    (node.kind == EditorGraphNodeKind::kFeature && address_.kind == EditorGraphKind::kFeature) ||
                    !part_name_for_node.empty();
                if (ImGui::MenuItem("Enter Pass Graph", nullptr, false, can_enter_pass)) {
                    const std::string feature_part = node.kind == EditorGraphNodeKind::kFeature
                                                         ? single_render_part(document, node.node_id)
                                                         : part_name_for_node;
                    address_ = {EditorGraphKind::kPass, address_.pipeline_node,
                                node.kind == EditorGraphNodeKind::kFeature ? node.node_id : address_.feature_node,
                                feature_part};
                    position_scope_ = graph_scope(address_);
                    positions_.clear();
                    selected_node_.clear();
                    connect_source_node_.clear();
                    connect_source_port_ = 0;
                    connect_source_slot_ = 0;
                    last_action_ = "Entered Pass graph: " +
                                   (node.kind == EditorGraphNodeKind::kFeature ? node.node_id : part_name_for_node);
                    ImGui::CloseCurrentPopup();
                }
                const bool can_reorder_pass = node.kind == EditorGraphNodeKind::kPass && !node.fixed;
                if (can_reorder_pass) {
                    if (ImGui::MenuItem("Move Pass Up")) {
                        const auto result = EditorCompositionCommands(document).move_pass(node.node_id, -1);
                        last_action_ = result ? "Moved Pass up" : result.message;
                        refresh_positions_after_reorder = refresh_positions_after_reorder || static_cast<bool>(result);
                        ImGui::CloseCurrentPopup();
                    }
                    if (ImGui::MenuItem("Move Pass Down")) {
                        const auto result = EditorCompositionCommands(document).move_pass(node.node_id, 1);
                        last_action_ = result ? "Moved Pass down" : result.message;
                        refresh_positions_after_reorder = refresh_positions_after_reorder || static_cast<bool>(result);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::Separator();
                }
                const bool can_delete = !node.fixed && node.kind != EditorGraphNodeKind::kPart;
                if (ImGui::MenuItem("Duplicate Node", nullptr, false, can_delete)) {
                    std::string copy_id = node.node_id + "_Copy";
                    const auto snapshot_for_duplicate = document.snapshot();
                    auto exists = [&](std::string_view id) {
                        return std::any_of(snapshot_for_duplicate->pipeline_nodes.begin(), snapshot_for_duplicate->pipeline_nodes.end(),
                                           [id](const auto& value) { return value.node_id == id; }) ||
                               std::any_of(snapshot_for_duplicate->feature_nodes.begin(), snapshot_for_duplicate->feature_nodes.end(),
                                           [id](const auto& value) { return value.node_id == id; }) ||
                               std::any_of(snapshot_for_duplicate->pass_nodes.begin(), snapshot_for_duplicate->pass_nodes.end(),
                                           [id](const auto& value) { return value.node_id == id; });
                    };
                    for (uint32_t suffix = 2; exists(copy_id); ++suffix)
                        copy_id = node.node_id + "_Copy" + std::to_string(suffix);
                    EditorCompositionCommands commands(document);
                    const auto result = commands.duplicate_node(node.node_id, copy_id);
                    last_action_ = result ? "Duplicated " + node.node_id : result.message;
                    if (result) { selected_node_ = copy_id; pending_layout_node_ = copy_id; }
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::MenuItem("Delete Node", nullptr, false, can_delete)) {
                    EditorCompositionCommands commands(document);
                    const auto result = commands.erase_node(node.node_id);
                    last_action_ = result ? "Deleted " + node.node_id : result.message;
                    if (result)
                        selected_node_.clear();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            if (ImGui::IsItemHovered()) {
                const char* double_click_hint =
                    node.kind == EditorGraphNodeKind::kPipeline && address_.kind == EditorGraphKind::kPipeline
                        ? "double-click to enter Feature graph"
                        : node.kind == EditorGraphNodeKind::kFeature && address_.kind == EditorGraphKind::kFeature
                              ? "double-click to enter Pass graph"
                              : "double-click to open definition";
                ImGui::SetTooltip("%s\nDrag to move; %s", node.node_id.c_str(), double_click_hint);
            }
            const ImVec2 node_title_min{min.x + 29.0f * zoom_, min.y + 8.0f * zoom_};
            const ImVec2 node_title_max{max.x - 10.0f * zoom_, node_title_min.y + ImGui::GetTextLineHeight()};
            // Display names may be full renderer-qualified stage/point IDs.
            // Keep the card readable at the normal zoom while retaining the
            // authored ID in the tooltip and Inspector.
            const std::string compact_node_name = flowgraph::compact_label(
                node.display_name.empty() ? node.node_id : node.display_name);
            draw_ellipsis(draw_list, node_title_min, node_title_max.x, compact_node_name,
                          IM_COL32(235, 245, 250, 255));
            if (node.ports.empty()) {
                const ImVec2 empty_min{min.x + 16.0f * zoom_,
                                       min.y + (card.header_height + card.padding + 2.0f) * zoom_};
                const ImVec2 empty_max{max.x - 10.0f * zoom_, empty_min.y + ImGui::GetTextLineHeight()};
                const char* description = node.kind == EditorGraphNodeKind::kStage ? "Pipeline stage"
                                                : node.kind == EditorGraphNodeKind::kFeaturePoint
                                                      ? "Feature extension point"
                                                      : "No declared ports";
                draw_ellipsis(draw_list, empty_min, empty_max.x, description,
                              IM_COL32(200, 220, 230, 255));
            } else {
                std::size_t input_row = 0;
                std::size_t output_row = 0;
                for (const auto& port : node.ports) {
                    const auto row = port.input ? input_row++ : output_row++;
                    const auto anchor = port.input ? card.input_anchor(row) : card.output_anchor(row);
                    const ImU32 color = port_color(port.value_kind);
                    const float port_x = port.input ? min.x : max.x;
                    const float port_y = min.y + anchor.y * zoom_;
                    draw_list->AddCircleFilled({port_x, port_y}, 5.0f * zoom_, IM_COL32(9, 20, 28, 255));
                    draw_list->AddCircleFilled({port_x, port_y}, 3.5f * zoom_, color);
                    ImGui::SetCursorScreenPos({port_x - 9.0f * zoom_, port_y - 9.0f * zoom_});
                    const std::string port_id = "##port-" +
                        flowgraph::port_anchor_key(node.node_id, port.input, port.port_id, port.slot_id);
                    ImGui::InvisibleButton(port_id.c_str(), {18.0f * zoom_, 18.0f * zoom_});
                    if (!suppress_graph_left_gestures && !port.input && ImGui::IsItemActive() &&
                        ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                        connect_source_node_ = node.node_id;
                        connect_source_port_ = port.port_id;
                        connect_source_slot_ = port.slot_id;
                        selected_node_ = node.node_id;
                    }
                    const bool release_on_input = !suppress_graph_left_gestures && port.input &&
                                                  ImGui::IsItemHovered() &&
                                                  ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
                                                  !connect_source_node_.empty();
                    if (!suppress_graph_left_gestures &&
                        (ImGui::IsItemClicked(ImGuiMouseButton_Left) || release_on_input)) {
                        if (!port.input) {
                            if (connect_source_node_ == node.node_id && connect_source_port_ == port.port_id) {
                                connect_source_node_.clear();
                                connect_source_port_ = 0;
                                connect_source_slot_ = 0;
                            } else {
                                connect_source_node_ = node.node_id;
                                connect_source_port_ = port.port_id;
                                connect_source_slot_ = 0;
                                selected_node_ = node.node_id;
                                last_action_ = "Output selected: " + node.node_id + "." + port.label;
                            }
                        } else if (!connect_source_node_.empty() && connect_source_port_ != 0 &&
                                   connect_source_node_ != node.node_id) {
                            const auto* source_node = find_node(view, connect_source_node_);
                            const EditorGraphPortView* source_port = nullptr;
                            if (source_node != nullptr) {
                                const auto found_source_port = std::find_if(
                                    source_node->ports.begin(), source_node->ports.end(), [&](const auto& candidate) {
                                        return !candidate.input && candidate.port_id == connect_source_port_;
                                    });
                                if (found_source_port != source_node->ports.end())
                                    source_port = &*found_source_port;
                            }
                            if (source_port == nullptr) {
                                last_action_ = "Selected output is no longer available";
                                continue;
                            }
                            if (source_port->binding_id != port.binding_id || source_port->edge_kind != port.edge_kind) {
                                last_action_ = "The selected ports have incompatible data contracts";
                                continue;
                            }
                            const auto result = connect_ports(connect_source_node_, connect_source_port_,
                                                              connect_source_slot_, node.node_id, port.port_id, port.slot_id,
                                                              source_port->binding_id, source_port->edge_kind);
                            last_action_ = result ? "Connected " + connect_source_node_ + "." +
                                                       std::to_string(connect_source_port_) + " -> " + node.node_id +
                                                       "." + port.label
                                                  : result.message;
                            if (result) {
                                connect_source_node_.clear();
                                connect_source_port_ = 0;
                                connect_source_slot_ = 0;
                            }
                        }
                    }
                    if (ImGui::IsItemHovered())
                        graph_item_hovered = true;
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s%s\n%s", port.input ? "Input: " : "Output: ", port.label.c_str(),
                                          port.required ? "Required input" : "Optional input");
                    const std::string compact_port_name = flowgraph::compact_label(port.label);
                    const float label_y = port_y - ImGui::GetTextLineHeight() * 0.5f;
                    if (port.input) {
                        const ImVec2 label_min{min.x + 16.0f * zoom_, label_y};
                        // Keep a real visual gutter between the input and
                        // output lanes. The previous 52%/48% split let long
                        // labels (for example "Scene Lighting" and "Scene
                        // Color") touch or visually merge in the same row.
                        const float input_lane_max = min.x + card.width * 0.48f * zoom_;
                        draw_ellipsis(draw_list, label_min, input_lane_max, compact_port_name,
                                      IM_COL32(220, 235, 240, 255));
                    } else {
                        const float output_lane_min = min.x + card.width * 0.52f * zoom_;
                        const ImVec2 label_min{output_lane_min, label_y};
                        draw_ellipsis(draw_list, label_min, max.x - 16.0f * zoom_, compact_port_name,
                                      IM_COL32(220, 235, 240, 255));
                    }
                }
            }
        }
        if (refresh_positions_after_reorder)
            positions_.clear();
        // Clicking empty canvas space cancels an in-progress wire and clears
        // the edge selection. Escape and the Cancel Connect toolbar command
        // remain available for keyboard and explicit cancellation.
        if (ImGui::IsWindowHovered() && !suppress_graph_left_gestures &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !graph_item_hovered) {
            if (!connect_source_node_.empty()) {
                connect_source_node_.clear();
                connect_source_port_ = 0;
                connect_source_slot_ = 0;
                last_action_ = "Cancelled connection";
            }
            selected_edge_.clear();
        }
        if (ImGui::BeginPopupContextWindow("FlowGraphCanvasContext",
                                           ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
            if (ImGui::MenuItem("Add Node")) {
                palette_search_.fill('\0');
                add_node_popup_requested_ = true;
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Create Empty Node")) {
                const std::string default_name = "Game.Custom." + graph_type_prefix(address_.kind);
                custom_type_name_.fill('\0');
                std::memcpy(custom_type_name_.data(), default_name.data(),
                            std::min(default_name.size(), custom_type_name_.size() - 1u));
                custom_type_name_initialized_ = true;
                empty_node_popup_requested_ = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("FlowGraphEdgeContext")) {
            ImGui::TextDisabled("%s", selected_edge_.c_str());
            const bool edge_editable = selected_edge_view != view.edges.end() && !selected_edge_view->fixed;
            if (!edge_editable)
                ImGui::BeginDisabled();
            if (ImGui::MenuItem("Disconnect Edge")) {
                const auto result = disconnect_edge_view(*selected_edge_view);
                last_action_ = result ? "Disconnected edge" : result.message;
                if (result) selected_edge_.clear();
                ImGui::CloseCurrentPopup();
            }
            if (!edge_editable)
                ImGui::EndDisabled();
            if (!edge_editable)
                ImGui::TextDisabled("Execution ordering is fixed by the node contract.");
            ImGui::EndPopup();
        }
        if (ImGui::IsWindowHovered() && ImGui::GetIO().KeyCtrl && ImGui::GetIO().MouseWheel != 0.0f)
            zoom_ = std::clamp(zoom_ + ImGui::GetIO().MouseWheel * 0.1f, 0.4f, 2.0f);
    }
    ImGui::EndChild();
    // The canvas context menu lives in a child window. Defer opening the
    // actual editor popups to the parent window so Dear ImGui resolves the
    // popup IDs in the same scope as the toolbar popups.
    if (add_node_popup_requested_) {
        ImGui::OpenPopup("FlowGraphAddNode");
        add_node_popup_requested_ = false;
    }
    if (empty_node_popup_requested_) {
        ImGui::OpenPopup("FlowGraphCreateEmptyNode");
        empty_node_popup_requested_ = false;
    }
    const float diagnostics_width = std::max(240.0f, ImGui::GetContentRegionAvail().x);
    const bool diagnostics_visible = ImGui::BeginChild("##flowgraph-diagnostics",
                                                       ImVec2{diagnostics_width, diagnostics_height}, true);
    if (diagnostics_visible) {
        const std::size_t error_count = static_cast<std::size_t>(std::count_if(
            document.diagnostics().begin(), document.diagnostics().end(), [](const auto& diagnostic) {
                return diagnostic.code.rfind("input.", 0) == 0;
            }));
        const std::size_t warning_count = document.diagnostics().size() - error_count;
        ImGui::TextUnformatted("Compiler Diagnostics");
        ImGui::SameLine();
        if (document.diagnostics().empty()) {
            ImGui::TextColored(ImVec4{0.32f, 0.9f, 0.65f, 1.0f}, "Compiled");
        } else {
            ImGui::TextColored(ImVec4{1.0f, 0.72f, 0.22f, 1.0f}, "Needs attention");
            ImGui::SameLine();
            ImGui::TextDisabled("%zu error(s), %zu warning(s)", error_count, warning_count);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("This panel reports graph validation and compiler readiness.\n"
                              "Use Locate to jump to the affected node or edge.");
        ImGui::Separator();
        if (ImGui::BeginTabBar("##flowgraph-diagnostics-tabs")) {
            if (ImGui::BeginTabItem("Compiler Diagnostics")) {
                if (document.diagnostics().empty()) {
                    ImGui::TextDisabled("No diagnostics. The graph is ready to compile.");
                } else {
                    std::size_t diagnostic_index = 0;
                    for (const auto& diagnostic : document.diagnostics()) {
                        const ImVec4 color = diagnostic.code.rfind("input.", 0) == 0
                                                 ? ImVec4{1.0f, 0.35f, 0.3f, 1.0f}
                                                 : ImVec4{1.0f, 0.75f, 0.25f, 1.0f};
                        ImGui::PushID(static_cast<int>(diagnostic_index++));
                        ImGui::PushStyleColor(ImGuiCol_Text, color);
                        ImGui::Text("%s", diagnostic.code.c_str());
                        ImGui::PopStyleColor();
                        ImGui::SameLine();
                        ImGui::TextWrapped("%s", diagnostic.message.c_str());
                        if (!diagnostic.node_id.empty() && ImGui::SmallButton("Locate")) {
                            const auto target = resolve_composition_diagnostic(*document.snapshot(), diagnostic);
                            if (target && !target.value.node_id.empty()) {
                                address_ = target.value.graph;
                                position_scope_ = graph_scope(address_);
                                positions_.clear();
                                selected_node_ = target.value.node_id;
                                selected_edge_ = target.value.edge_id;
                                last_action_ = "Located " + target.value.node_id;
                            }
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Find Results")) {
                ImGui::TextDisabled("Search results will appear here when graph search is added.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Graph Stats")) {
                ImGui::Text("Nodes  %zu", view.nodes.size());
                ImGui::Text("Data flows  %zu", data_flow_count);
                ImGui::Text("Structural relations  %zu", structural_relation_count);
                ImGui::Text("Zoom  %.2f", zoom_);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::EndChild();

    ImGui::TableNextColumn();
    if (ImGui::BeginChild("##flowgraph-inspector", ImVec2{0.0f, 0.0f}, true)) {
        ImGui::TextUnformatted("Inspector");
        ImGui::Separator();
        draw_selection_editor();
        ImGui::Separator();
        ImGui::TextDisabled("Contract summary");
        const auto* inspector_node = find_node(view, selected_node_);
        if (inspector_node == nullptr) {
            ImGui::TextDisabled("Select a node to inspect its contract.");
            ImGui::TextWrapped("Inputs describe the data required by the node. Outputs are the values that can flow to later nodes.");
        } else {
            const char* kind_label = inspector_node->kind == EditorGraphNodeKind::kPipeline
                                         ? "Pipeline"
                                          : inspector_node->kind == EditorGraphNodeKind::kStage
                                                ? "Stage"
                                                : inspector_node->kind == EditorGraphNodeKind::kFeaturePoint
                                                      ? "Feature Point"
                                         : inspector_node->kind == EditorGraphNodeKind::kFeature
                                               ? "Feature"
                                               : inspector_node->kind == EditorGraphNodeKind::kPass
                                                     ? "Pass"
                                                     : inspector_node->kind == EditorGraphNodeKind::kResource
                                                           ? "Resource"
                                                           : "Graph node";
            ImGui::TextWrapped("%s", inspector_node->node_id.c_str());
            ImGui::TextDisabled("%s%s", kind_label, inspector_node->fixed ? "  |  fixed" : "  |  editable");
            if (const auto* type = type_for_node(document, address_, *inspector_node); type != nullptr)
                ImGui::TextDisabled("Type: %s", type->display_name.empty() ? type->name.c_str() : type->display_name.c_str());
            if (inspector_node->unknown)
                ImGui::TextColored(ImVec4{1.0f, 0.72f, 0.22f, 1.0f}, "Type metadata unavailable");

            int input_count = 0;
            int output_count = 0;
            for (const auto& port : inspector_node->ports)
                port.input ? ++input_count : ++output_count;
            ImGui::Spacing();
            ImGui::Text("Inputs  %d", input_count);
            for (const auto& port : inspector_node->ports) {
                if (!port.input)
                    continue;
                ImGui::BulletText("%s%s  (%s)", port.label.c_str(), port.required ? " *" : "",
                                  port.max_connections > 1 ? "multiple" : "single");
            }
            ImGui::Spacing();
            ImGui::Text("Outputs  %d", output_count);
            for (const auto& port : inspector_node->ports) {
                if (port.input)
                    continue;
                ImGui::BulletText("%s  (%s)", port.label.c_str(),
                                  port.max_connections > 1 ? "multiple" : "single");
            }
            ImGui::Spacing();
            ImGui::TextDisabled("Edit input sources and parameters in Selection below the graph toolbar.");
        }
    }
    ImGui::EndChild();
    // TableGetColumnWidth reflects the user-dragged separator. Persist it so
    // the palette keeps its chosen width across frames and native-window
    // redraws, while clamping extreme drags to a usable range.
    if (const ImGuiTable* current_table = ImGui::GetCurrentTable(); current_table != nullptr)
        palette_width_ = std::clamp(current_table->Columns[0].WidthGiven, 180.0f, 380.0f);
    ImGui::EndTable();
    }
    draw_node_definition_window(document, view, project_root_);
    ImGui::End();
}

void EditorFlowGraphPanel::draw_node_definition_window(EditorCompositionDocument& document,
                                                        const EditorCompositionGraphView& view,
                                                        const std::filesystem::path& project_root) {
    if (definition_popup_requested_) {
        definition_popup_requested_ = false;
        const auto* node = find_node(view, definition_node_);
        const auto* type = node == nullptr ? nullptr : type_for_node(document, address_, *node);
        if (type != nullptr) {
            definition_draft_ = {};
            definition_draft_.entry = *type;
            definition_draft_.source = NodeContractSource::kEditorDraft;
            definition_draft_.generated = true;
            bool draft_loaded = false;
            bool generated_contract_loaded = false;
            if (!project_root.empty()) {
                const auto scanned = scan_editor_node_contracts(project_root);
                if (scanned) {
                    for (const auto& file : scanned.value) {
                        if (file.draft.entry.key != type->key)
                            continue;
                        if (file.draft.generated) {
                            generated_contract_loaded = true;
                        } else {
                            definition_draft_ = file.draft;
                            draft_loaded = true;
                        }
                    }
                }
            }
            definition_read_only_ = node->unknown ||
                                    (!draft_loaded &&
                                     type->owner_module.value() != pipeline::ModuleId("Editor.FlowGraph").value()) ||
                                    (draft_loaded && generated_contract_loaded);
            definition_disconnect_confirmed_ = false;
            definition_status_.clear();
            definition_name_buffers_.clear();
            definition_display_buffers_.clear();
            definition_parameter_buffers_.clear();
            definition_parameter_display_buffers_.clear();
            definition_parameter_default_buffers_.clear();
            definition_window_minimized_ = false;
            definition_window_maximized_ = false;
            definition_window_open_ = true;
            if (draft_loaded && generated_contract_loaded)
                definition_status_ = "Draft conflicts with the generated contract; resolve the source before saving.";
            else if (!draft_loaded && generated_contract_loaded && definition_read_only_)
                definition_status_ = "A compiled contract is authoritative; edit its source module to change it.";
        }
    }
    if (!definition_window_open_)
        return;

    const auto* viewport = ImGui::GetMainViewport();
    if (definition_window_maximized_) {
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
    } else {
        ImGui::SetNextWindowSize(ImVec2{720.0f, 680.0f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2{viewport->WorkPos.x + viewport->WorkSize.x * 0.5f - 360.0f,
                                       viewport->WorkPos.y + viewport->WorkSize.y * 0.5f - 340.0f},
                                ImGuiCond_FirstUseEver);
    }
    constexpr ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                               ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                               ImGuiWindowFlags_NoDocking;
    ImGui::Begin("Node Definition###FlowGraphNodeDefinition", nullptr, window_flags);
    aeg::editor::EditorWindowChromeActions chrome;
    chrome.minimize = [this] {
        definition_window_minimized_ = !definition_window_minimized_;
        return foundation::Result::success();
    };
    chrome.maximize = [this] {
        definition_window_maximized_ = !definition_window_maximized_;
        definition_window_minimized_ = false;
        return foundation::Result::success();
    };
    chrome.close = [this] {
        definition_window_open_ = false;
        return foundation::Result::success();
    };
    const auto& entry = definition_draft_.entry;
    const std::string status = definition_read_only_ ? "Read-only contract" :
                                (definition_draft_.generated ? "Editor draft" : "Draft not generated");
    aeg::editor::draw_editor_window_chrome(entry.display_name.empty() ? entry.name : entry.display_name, status,
                                           chrome);
    if (!definition_window_minimized_) {
        ImGui::BeginChild("##node-definition-body", ImVec2{0.0f, 0.0f}, true);
        ImGui::TextDisabled("Type: %s   TypeId: %llu   OwnerId: %llu", entry.name.c_str(),
                            static_cast<unsigned long long>(entry.key.id),
                            static_cast<unsigned long long>(entry.owner_module.value()));
        ImGui::TextDisabled("OwnerModule: %llu   Source: %s",
                            static_cast<unsigned long long>(entry.owner_module.value()),
                            definition_read_only_ ? "compiled/read-only" : "Editor.FlowGraph draft");
        ImGui::TextDisabled("Version %u.%u.%u  |  %zu instance port(s) in the active catalog", entry.version.major,
                            entry.version.minor, entry.version.patch, entry.ports.size());
        auto& display_name = definition_display_buffers_[0];
        if (display_name.front() == '\0') {
            std::strncpy(display_name.data(), entry.display_name.c_str(), display_name.size() - 1);
            display_name.back() = '\0';
        }
        if (!definition_read_only_ && ImGui::InputText("Display name", display_name.data(), display_name.size()))
            definition_draft_.entry.display_name = display_name.data();
        ImGui::Separator();

        auto next_id = [&] {
            uint64_t value = 0;
            for (const auto& port : definition_draft_.entry.ports)
                value = std::max(value, port.id);
            for (const auto& parameter : definition_draft_.entry.parameters)
                value = std::max(value, parameter.id);
            for (const auto id : definition_draft_.retired_port_ids)
                value = std::max(value, id);
            for (const auto id : definition_draft_.retired_parameter_ids)
                value = std::max(value, id);
            return value + 1u;
        };
        const auto button_enabled = [&](const char* label) {
            if (definition_read_only_)
                ImGui::BeginDisabled();
            const bool clicked = ImGui::Button(label);
            if (definition_read_only_)
                ImGui::EndDisabled();
            return clicked;
        };
        const auto schema_editor = [&](pipeline::PortDescriptor& port) {
            const auto schemas = node_port_schemas();
            const auto current = std::find_if(schemas.begin(), schemas.end(), [&](const auto& schema) {
                return schema.kind == port.value_kind && schema.id == port.schema_id;
            });
            const char* preview = current == schemas.end() ? "Unknown schema" : current->display_name;
            if (definition_read_only_) {
                ImGui::TextDisabled("Schema: %s", preview);
                return;
            }
            if (ImGui::BeginCombo("Schema", preview)) {
                for (const auto& schema : schemas) {
                    const bool selected = schema.kind == port.value_kind && schema.id == port.schema_id;
                    if (ImGui::Selectable(schema.display_name, selected)) {
                        port.value_kind = schema.kind;
                        port.schema_id = schema.id;
                        port.schema_version = 1;
                        port.resource_constraints.resource_class = schema.resource_class;
                    }
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        };

        ImGui::Text("Inputs");
        if (button_enabled("Add Input")) {
            const uint64_t id = next_id();
            pipeline::PortDescriptor port{};
            port.id = id;
            port.name = "Input" + std::to_string(id);
            port.direction = pipeline::PortDirection::kInput;
            port.value_kind = pipeline::PortValueKind::kTexture;
            port.schema_id = 1;
            port.schema_version = 1;
            port.binding_id = id;
            port.max_connections = 1;
            port.resource_constraints.resource_class = pipeline::ResourceClass::kTexture;
            port.input = pipeline::InputPortContract{.allowed_sources = pipeline::input_source_mask(
                                                         pipeline::InputSourceKind::kEdge),
                                                     .presence = pipeline::InputPresenceRequirement::kOptional,
                                                     .min_edges = 0,
                                                     .max_edges = 1};
            definition_draft_.entry.ports.push_back(std::move(port));
        }
        for (std::size_t index = 0; index < definition_draft_.entry.ports.size(); ++index) {
            auto& port = definition_draft_.entry.ports[index];
            if (port.direction != pipeline::PortDirection::kInput)
                continue;
            if (!port.input && !definition_read_only_)
                port.input = pipeline::InputPortContract{.allowed_sources = pipeline::input_source_mask(
                                                              pipeline::InputSourceKind::kEdge),
                                                          .presence = port.required
                                                              ? pipeline::InputPresenceRequirement::kRequired
                                                              : pipeline::InputPresenceRequirement::kOptional,
                                                          .min_edges = port.min_connections,
                                                          .max_edges = std::min(port.max_connections, 1u)};
            ImGui::PushID(static_cast<int>(port.id));
            ImGui::Separator();
            ImGui::Text("PortId %llu", static_cast<unsigned long long>(port.id));
            auto& name = definition_name_buffers_[port.id];
            if (name.front() == '\0') {
                std::strncpy(name.data(), port.name.c_str(), name.size() - 1);
                name.back() = '\0';
            }
            if (!definition_read_only_ && ImGui::InputText("Name", name.data(), name.size()))
                port.name = name.data();
            else
                ImGui::TextDisabled("Name: %s", port.name.c_str());
            unsigned long long schema = port.schema_id;
            if (!definition_read_only_ && ImGui::InputScalar("Schema ID", ImGuiDataType_U64, &schema))
                port.schema_id = schema;
            schema_editor(port);
            ImGui::SameLine();
            ImGui::TextDisabled("v%u", port.schema_version);
            bool required = port.required;
            if (!definition_read_only_ && ImGui::Checkbox("Required", &required)) {
                port.required = required;
                if (port.input)
                    port.input->presence = required ? pipeline::InputPresenceRequirement::kRequired
                                                    : pipeline::InputPresenceRequirement::kOptional;
            }
            if (port.input) {
                ImGui::TextDisabled("Allowed sources");
                constexpr std::array<std::pair<const char*, pipeline::InputSourceKind>, 5> source_options{{
                    {"Edge", pipeline::InputSourceKind::kEdge},
                    {"Asset", pipeline::InputSourceKind::kAsset},
                    {"Literal", pipeline::InputSourceKind::kLiteral},
                    {"Default", pipeline::InputSourceKind::kDefault},
                    {"Context", pipeline::InputSourceKind::kContext}}};
                for (const auto& [label, source] : source_options) {
                    bool enabled = (port.input->allowed_sources & pipeline::input_source_mask(source)) != 0;
                    if (!definition_read_only_ && ImGui::Checkbox(label, &enabled)) {
                        const auto mask = pipeline::input_source_mask(source);
                        if (enabled)
                            port.input->allowed_sources |= mask;
                        else
                            port.input->allowed_sources &= ~mask;
                    }
                    if (source != source_options.back().second)
                        ImGui::SameLine();
                }
                unsigned int min_edges = port.input->min_edges;
                unsigned int max_edges = port.input->max_edges;
                if (!definition_read_only_ && ImGui::InputScalar("Min edges", ImGuiDataType_U32, &min_edges)) {
                    port.input->min_edges = min_edges;
                    port.min_connections = min_edges;
                }
                if (!definition_read_only_ && ImGui::InputScalar("Max edges", ImGuiDataType_U32, &max_edges)) {
                    port.input->max_edges = max_edges;
                    port.max_connections = max_edges;
                }
            }
            if (!definition_read_only_) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Up") && index > 0)
                    std::swap(definition_draft_.entry.ports[index], definition_draft_.entry.ports[index - 1]);
                ImGui::SameLine();
                if (ImGui::SmallButton("Down") && index + 1 < definition_draft_.entry.ports.size())
                    std::swap(definition_draft_.entry.ports[index], definition_draft_.entry.ports[index + 1]);
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) {
                    definition_draft_.retired_port_ids.push_back(port.id);
                    definition_draft_.entry.ports.erase(definition_draft_.entry.ports.begin() +
                                                        static_cast<std::ptrdiff_t>(index));
                    ImGui::PopID();
                    break;
                }
            }
            ImGui::PopID();
        }

        ImGui::Spacing();
        ImGui::Text("Outputs");
        if (button_enabled("Add Output")) {
            const uint64_t id = next_id();
            pipeline::PortDescriptor output{};
            output.id = id;
            output.name = "Output" + std::to_string(id);
            output.direction = pipeline::PortDirection::kOutput;
            output.value_kind = pipeline::PortValueKind::kTexture;
            output.schema_id = 1;
            output.schema_version = 1;
            output.max_connections = 1;
            output.binding_id = id;
            output.resource_constraints.resource_class = pipeline::ResourceClass::kTexture;
            definition_draft_.entry.ports.push_back(std::move(output));
        }
        for (std::size_t index = 0; index < definition_draft_.entry.ports.size(); ++index) {
            auto& port = definition_draft_.entry.ports[index];
            if (port.direction != pipeline::PortDirection::kOutput)
                continue;
            ImGui::PushID(static_cast<int>(port.id));
            ImGui::Separator();
            ImGui::Text("PortId %llu", static_cast<unsigned long long>(port.id));
            auto& name = definition_name_buffers_[port.id];
            if (name.front() == '\0') {
                std::strncpy(name.data(), port.name.c_str(), name.size() - 1);
                name.back() = '\0';
            }
            if (!definition_read_only_ && ImGui::InputText("Name", name.data(), name.size()))
                port.name = name.data();
            else
                ImGui::TextDisabled("Name: %s", port.name.c_str());
            unsigned long long schema = port.schema_id;
            if (!definition_read_only_ && ImGui::InputScalar("Schema ID", ImGuiDataType_U64, &schema))
                port.schema_id = schema;
            schema_editor(port);
            unsigned int max_connections = port.max_connections;
            if (!definition_read_only_ && ImGui::InputScalar("Max connections", ImGuiDataType_U32, &max_connections))
                port.max_connections = max_connections;
            if (!definition_read_only_) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Up") && index > 0)
                    std::swap(definition_draft_.entry.ports[index], definition_draft_.entry.ports[index - 1]);
                ImGui::SameLine();
                if (ImGui::SmallButton("Down") && index + 1 < definition_draft_.entry.ports.size())
                    std::swap(definition_draft_.entry.ports[index], definition_draft_.entry.ports[index + 1]);
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) {
                    definition_draft_.retired_port_ids.push_back(port.id);
                    definition_draft_.entry.ports.erase(definition_draft_.entry.ports.begin() +
                                                        static_cast<std::ptrdiff_t>(index));
                    ImGui::PopID();
                    break;
                }
            }
            ImGui::PopID();
        }

        ImGui::Spacing();
        ImGui::Text("Compute Points");
        if (button_enabled("Add Compute Point")) {
            const uint64_t id = next_id();
            definition_draft_.entry.parameters.push_back({.id = id,
                                                           .name = "Parameter" + std::to_string(id),
                                                           .display_name = "Parameter " + std::to_string(id),
                                                           .kind = pipeline::ParameterKind::kFloat64,
                                                           .default_value = 0.0,
                                                           .minimum = 0.0,
                                                           .maximum = 1.0});
        }
        for (std::size_t index = 0; index < definition_draft_.entry.parameters.size(); ++index) {
            auto& parameter = definition_draft_.entry.parameters[index];
            ImGui::PushID(static_cast<int>(parameter.id));
            ImGui::Separator();
            ImGui::Text("ParameterId %llu", static_cast<unsigned long long>(parameter.id));
            auto& name = definition_parameter_buffers_[parameter.id];
            if (name.front() == '\0') {
                std::strncpy(name.data(), parameter.name.c_str(), name.size() - 1);
                name.back() = '\0';
            }
            if (!definition_read_only_ && ImGui::InputText("Name", name.data(), name.size()))
                parameter.name = name.data();
            else
                ImGui::TextDisabled("Name: %s", parameter.name.c_str());
            if (!definition_read_only_) {
                auto& parameter_display_buffer = definition_parameter_display_buffers_[parameter.id];
                if (parameter_display_buffer.front() == '\0') {
                    const std::string initial = parameter.display_name.empty() ? parameter.name : parameter.display_name;
                    const auto count = std::min(initial.size(), parameter_display_buffer.size() - 1u);
                    std::copy_n(initial.data(), count, parameter_display_buffer.data());
                    parameter_display_buffer[count] = '\0';
                }
                if (ImGui::InputText("Display name", parameter_display_buffer.data(), parameter_display_buffer.size()))
                    parameter.display_name = parameter_display_buffer.data();
                const char* kinds[] = {"Bool", "Int64", "UInt64", "Float64", "String", "Vector", "Enum", "Asset"};
                int kind = static_cast<int>(parameter.kind);
                if (ImGui::Combo("Type", &kind, kinds, static_cast<int>(std::size(kinds)))) {
                    parameter.kind = static_cast<pipeline::ParameterKind>(kind);
                    switch (parameter.kind) {
                        case pipeline::ParameterKind::kBool: parameter.default_value = false; break;
                        case pipeline::ParameterKind::kInt64: parameter.default_value = int64_t{0}; break;
                        case pipeline::ParameterKind::kUInt64: parameter.default_value = uint64_t{0}; break;
                        case pipeline::ParameterKind::kFloat64: parameter.default_value = 0.0; break;
                        case pipeline::ParameterKind::kString: parameter.default_value = std::string{}; break;
                        case pipeline::ParameterKind::kVector: parameter.default_value = std::vector<double>{}; break;
                        case pipeline::ParameterKind::kEnum:
                            parameter.default_value = parameter.enum_values.empty() ? std::string{} : parameter.enum_values.front();
                            break;
                        case pipeline::ParameterKind::kAsset: parameter.default_value = pipeline::AssetReference{}; break;
                    }
                }
                switch (parameter.kind) {
                    case pipeline::ParameterKind::kBool: {
                        bool value = std::get_if<bool>(&parameter.default_value) ? *std::get_if<bool>(&parameter.default_value) : false;
                        if (ImGui::Checkbox("Default", &value)) parameter.default_value = value;
                        break;
                    }
                    case pipeline::ParameterKind::kInt64: {
                        int64_t value = std::get_if<int64_t>(&parameter.default_value) ? *std::get_if<int64_t>(&parameter.default_value) : 0;
                        if (ImGui::InputScalar("Default", ImGuiDataType_S64, &value)) parameter.default_value = value;
                        break;
                    }
                    case pipeline::ParameterKind::kUInt64: {
                        uint64_t value = std::get_if<uint64_t>(&parameter.default_value) ? *std::get_if<uint64_t>(&parameter.default_value) : 0;
                        if (ImGui::InputScalar("Default", ImGuiDataType_U64, &value)) parameter.default_value = value;
                        break;
                    }
                    case pipeline::ParameterKind::kFloat64: {
                        double value = std::get_if<double>(&parameter.default_value) ? *std::get_if<double>(&parameter.default_value) : 0.0;
                        if (ImGui::InputDouble("Default", &value)) parameter.default_value = value;
                        break;
                    }
                    case pipeline::ParameterKind::kString: {
                        auto& value = definition_parameter_default_buffers_[parameter.id];
                        if (value.front() == '\0') {
                            const auto* text = std::get_if<std::string>(&parameter.default_value);
                            const auto count = std::min(text == nullptr ? std::size_t{0} : text->size(), value.size() - 1u);
                            if (text != nullptr) std::copy_n(text->data(), count, value.data());
                            value[count] = '\0';
                        }
                        if (ImGui::InputText("Default", value.data(), value.size()))
                            parameter.default_value = std::string{value.data()};
                        break;
                    }
                    case pipeline::ParameterKind::kVector:
                        ImGui::TextDisabled("Default: vector value editing is available in the generated contract");
                        break;
                    case pipeline::ParameterKind::kEnum:
                    case pipeline::ParameterKind::kAsset:
                        ImGui::TextDisabled("Default: configure in the generated contract source");
                        break;
                }
                double minimum = parameter.minimum.value_or(0.0);
                double maximum = parameter.maximum.value_or(1.0);
                if (ImGui::InputDouble("Minimum", &minimum))
                    parameter.minimum = minimum;
                if (ImGui::InputDouble("Maximum", &maximum))
                    parameter.maximum = maximum;
                ImGui::SameLine();
                if (ImGui::SmallButton("Up") && index > 0)
                    std::swap(definition_draft_.entry.parameters[index],
                              definition_draft_.entry.parameters[index - 1]);
                ImGui::SameLine();
                if (ImGui::SmallButton("Down") && index + 1 < definition_draft_.entry.parameters.size())
                    std::swap(definition_draft_.entry.parameters[index],
                              definition_draft_.entry.parameters[index + 1]);
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) {
                    definition_draft_.retired_parameter_ids.push_back(parameter.id);
                    definition_draft_.entry.parameters.erase(definition_draft_.entry.parameters.begin() +
                                                             static_cast<std::ptrdiff_t>(index));
                    ImGui::PopID();
                    break;
                }
            }
            ImGui::PopID();
        }

        const auto preview = node_definition_service_.preview(document, definition_draft_);
        const bool definition_valid = static_cast<bool>(preview);
        if (!preview) {
            ImGui::TextColored(ImVec4{1.0f, 0.35f, 0.25f, 1.0f}, "%s", preview.result.message.c_str());
        } else {
            const auto& diff = preview.value;
            const char* version = diff.version_level == NodeContractVersionLevel::kMajor ? "major" :
                                  diff.version_level == NodeContractVersionLevel::kMinor ? "minor" :
                                  diff.version_level == NodeContractVersionLevel::kPatch ? "patch" : "unchanged";
            ImGui::Separator();
            ImGui::TextDisabled("Impact: +%zu / -%zu ports, %zu affected edge(s), %zu binding(s), %s version",
                                diff.added_ports.size(), diff.removed_ports.size(), diff.affected_edge_ids.size(),
                                diff.affected_binding_ids.size(), version);
            if (!diff.impacts.empty()) {
                ImGui::BeginChild("##node-definition-impact", ImVec2{0.0f, 108.0f}, true);
                ImGui::TextUnformatted("Affected instances");
                for (const auto& impact : diff.impacts) {
                    ImGui::BulletText("%s / %s / Port %llu%s%s",
                                      impact.document_path.empty() ? "active graph" : impact.document_path.string().c_str(),
                                      impact.node_id.c_str(),
                                      static_cast<unsigned long long>(impact.port_id),
                                      impact.edge_id.empty() ? "" : " / Edge ",
                                      impact.edge_id.empty() ? "" : impact.edge_id.c_str());
                }
                ImGui::EndChild();
            }
            if (diff.requires_confirmation && !definition_read_only_) {
                ImGui::Checkbox("Apply and disconnect affected edges", &definition_disconnect_confirmed_);
                ImGui::TextColored(ImVec4{1.0f, 0.72f, 0.22f, 1.0f},
                                   "This change requires explicit disconnect confirmation.");
            }
        }
        if (!definition_status_.empty())
            ImGui::TextWrapped("%s", definition_status_.c_str());
        ImGui::Separator();
        if (!definition_read_only_) {
            if (!definition_valid)
                ImGui::BeginDisabled();
            if (ImGui::Button("Save Draft")) {
                const auto result = node_definition_service_.save(document,
                    {definition_draft_, false, false}, project_root);
                if (result) {
                    definition_draft_.generated = false;
                    definition_status_ = "Draft saved. Runtime contract remains unchanged.";
                } else {
                    definition_status_ = result.result.message;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Save & Generate Stub")) {
                const auto result = node_definition_service_.save(
                    document, {definition_draft_, true, definition_disconnect_confirmed_}, project_root);
                if (result) {
                    definition_draft_.generated = true;
                    definition_status_ = "Contract and generated stub saved.";
                    definition_window_open_ = false;
                } else {
                    definition_status_ = result.result.message;
                }
            }
            if (!definition_valid)
                ImGui::EndDisabled();
            ImGui::SameLine();
        }
        if (ImGui::Button("Cancel"))
            definition_window_open_ = false;
        ImGui::EndChild();
    }
    ImGui::End();
}

}  // namespace aeg::application
\n
