#pragma once

#include <functional>
#include <optional>
#include <string>

#include "editor_composition_document.hpp"
#include "editor_composition_view.hpp"

namespace aeg::application {

struct EditorConnectionRequest final {
    std::string edge_id{};
    pipeline::DocumentEdgeKind kind{pipeline::DocumentEdgeKind::kRenderResource};
    std::string source_node{};
    uint64_t source_port{};
    std::string destination_node{};
    uint64_t destination_port{};
    uint64_t binding_id{};
    uint64_t source_slot{};
    uint64_t destination_slot{};
};

class EditorCompositionCommands final {
public:
    explicit EditorCompositionCommands(EditorCompositionDocument& document) noexcept : document_(document) {}

    [[nodiscard]] foundation::Result add_type(const EditorGraphAddress&, pipeline::AuthoringTypeKey,
                                              std::string node_id, std::string attachment_id = {},
                                              std::string empty_draft_marker = {});
    /** Adds a node while publishing a catalog mutation in the same undo entry. */
    [[nodiscard]] foundation::Result add_type_with_catalog(const EditorGraphAddress&, pipeline::AuthoringTypeKey,
                                                           std::string node_id, pipeline::TypeCatalog next_catalog,
                                                           std::string attachment_id = {},
                                                           std::string empty_draft_marker = {});
    [[nodiscard]] foundation::Result erase_node(std::string_view node_id);
    /** Reorders an editable Pass among siblings in the same Feature part. */
    [[nodiscard]] foundation::Result move_pass(std::string_view node_id, int direction);
    [[nodiscard]] foundation::Result connect(const EditorConnectionRequest&);
    [[nodiscard]] foundation::Result disconnect(std::string_view edge_id);
    [[nodiscard]] foundation::Result set_parameter(std::string_view node_id, uint64_t parameter_id,
                                                   pipeline::ParameterValue value);
    [[nodiscard]] foundation::Result set_input_source(const pipeline::InputBindingDocument&);
    [[nodiscard]] foundation::Result add_variadic_slot(std::string_view node_id, uint64_t group_id);
    [[nodiscard]] foundation::Result remove_variadic_slot(std::string_view node_id, uint64_t group_id,
                                                          uint64_t slot_id);
    [[nodiscard]] foundation::Result move_variadic_slot(std::string_view node_id, uint64_t group_id,
                                                        uint64_t slot_id, int direction);
    [[nodiscard]] foundation::Result duplicate_node(std::string_view node_id, std::string new_node_id);

    [[nodiscard]] foundation::Result begin_transaction(std::string label);
    [[nodiscard]] foundation::Result update_transaction(
        std::function<foundation::Result(pipeline::PipelineDocument&)> mutation);
    [[nodiscard]] foundation::Result commit_transaction();
    [[nodiscard]] foundation::Result cancel_transaction() noexcept;
    [[nodiscard]] bool transaction_active() const noexcept { return transaction_.has_value(); }

private:
    struct Transaction final {
        std::string label{};
        std::shared_ptr<const pipeline::PipelineDocument> before{};
        pipeline::PipelineDocument draft{};
    };

    EditorCompositionDocument& document_;
    std::optional<Transaction> transaction_{};
};

}  // namespace aeg::application
