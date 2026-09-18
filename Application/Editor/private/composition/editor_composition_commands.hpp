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
};

class EditorCompositionCommands final {
public:
    explicit EditorCompositionCommands(EditorCompositionDocument& document) noexcept : document_(document) {}

    [[nodiscard]] foundation::Result add_type(const EditorGraphAddress&, pipeline::AuthoringTypeKey,
                                              std::string node_id, std::string attachment_id = {});
    [[nodiscard]] foundation::Result erase_node(std::string_view node_id);
    [[nodiscard]] foundation::Result connect(const EditorConnectionRequest&);
    [[nodiscard]] foundation::Result disconnect(std::string_view edge_id);
    [[nodiscard]] foundation::Result set_parameter(std::string_view node_id, uint64_t parameter_id,
                                                   pipeline::ParameterValue value);

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
