/**
 * @file feature_graph_io.hpp
 * @brief FeatureGraph authoring document persistence.
 */
#pragma once

#include <filesystem>
#include <string>

#include "feature_graph.hpp"

namespace aeg::application::authoring {

/** Exact source bytes captured with the decoded document; equality detects external edits. */
struct FeatureGraphFileRevision final {
    bool exists{false};
    std::string contents{};
    friend bool operator==(const FeatureGraphFileRevision&, const FeatureGraphFileRevision&) = default;
};

struct FeatureGraphSourceSnapshot final {
    feature::FeatureGraphDocument document{};
    FeatureGraphFileRevision revision{};
};

[[nodiscard]] foundation::ResultValue<FeatureGraphFileRevision> inspect_feature_graph_file_revision(
    const std::filesystem::path& file);
[[nodiscard]] foundation::ResultValue<FeatureGraphSourceSnapshot> load_feature_graph_source(
    const std::filesystem::path& file);
/** Returns exactly the bytes this operation published, without rereading a possibly newer writer. */
[[nodiscard]] foundation::ResultValue<FeatureGraphFileRevision> save_feature_graph_source(
    const std::filesystem::path& file, const feature::FeatureGraphDocument& graph,
    const FeatureGraphFileRevision* expected = nullptr);

[[nodiscard]] foundation::ResultValue<feature::FeatureGraphDocument> load_feature_graph_document(
    const std::filesystem::path& file);
[[nodiscard]] foundation::Result save_feature_graph_document(
    const std::filesystem::path& file, const feature::FeatureGraphDocument& graph);
/** Rejects an external change under the same per-file writer lock; leaves both drafts intact. */
[[nodiscard]] foundation::Result save_feature_graph_document(
    const std::filesystem::path& file, const feature::FeatureGraphDocument& graph,
    const FeatureGraphFileRevision& expected);
/** Explicitly restores a validated .bak without overwriting it with damaged current bytes. */
[[nodiscard]] foundation::Result recover_feature_graph_document(
    const std::filesystem::path& file, const FeatureGraphFileRevision& expected,
    const FeatureGraphFileRevision* expected_backup = nullptr);

}  // namespace aeg::application::authoring
