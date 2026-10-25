#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "editor_node_definition.hpp"

namespace aeg::application {

struct NodeContractFile final {
    std::filesystem::path path{};
    NodeDefinitionDraft draft{};
    std::string digest{};
    NodeContractSource source{NodeContractSource::kEditorDraft};
};

[[nodiscard]] foundation::ResultValue<std::vector<NodeContractFile>> scan_editor_node_contracts(
    const std::filesystem::path& project_root, std::vector<std::string>* diagnostics = nullptr);
/** Scans only FlowGraph under the caller's selected/configured source directory. */
[[nodiscard]] foundation::ResultValue<std::vector<NodeContractFile>> scan_editor_node_contracts(
    const std::filesystem::path& project_root, const std::filesystem::path& source_directory,
    std::vector<std::string>* diagnostics = nullptr);
[[nodiscard]] foundation::ResultValue<NodeContractFile> read_editor_node_contract(
    const std::filesystem::path& path);
/** Encodes the canonical Feature contract plus the editor draft envelope. */
[[nodiscard]] foundation::ResultValue<std::string> encode_editor_node_contract(const NodeContractFile& file);
[[nodiscard]] foundation::Result write_editor_node_contract_atomic(const NodeContractFile& file,
                                                                   const std::filesystem::path& project_root);

}  // namespace aeg::application
