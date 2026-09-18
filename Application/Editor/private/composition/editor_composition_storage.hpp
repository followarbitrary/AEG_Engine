#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "editor_composition_document.hpp"
#include "pipeline_document_io.hpp"

namespace aeg::application {

struct LegacyIdentityMapping final {
    std::string source{};
    uint64_t stage{};
    uint64_t task{};
    std::string node_id{};
    uint64_t port_id{};
};

struct LegacyMigrationRecord final {
    uint32_t line{};
    std::string raw{};
    std::string reason{};
};

struct LegacyCompositionMigration final {
    pipeline::PipelineDocument document{};
    std::vector<LegacyMigrationRecord> unresolved{};
    std::string original_bytes{};
};

[[nodiscard]] foundation::Result save_editor_composition(const std::filesystem::path& path,
                                                         EditorCompositionDocument& editor);

[[nodiscard]] foundation::ResultValue<LegacyCompositionMigration> import_legacy_pipeline_edits(
    const std::filesystem::path& path, const pipeline::PipelineDocument& baseline,
    std::span<const LegacyIdentityMapping> mappings, const pipeline::TypeCatalog& catalog);

[[nodiscard]] foundation::Result save_legacy_migration_report(const std::filesystem::path& path,
                                                              const LegacyCompositionMigration& migration);

}  // namespace aeg::application
