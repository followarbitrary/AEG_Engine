#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

#include "result.hpp"

namespace aeg::application {
class EditorSceneRuntime;
class EditorFeatureGraphDocument;
namespace runtime { struct FeatureRuntimeNodeSnapshot; }
}
namespace aeg::tooling {
struct ProjectOperationRequest;
struct ProjectOperationStatus;
}

namespace aeg::editor_test {

/** Test-only inputs borrowed from the actual Main session; no second Scene/Runtime is created. */
struct FeatureWorkflowContext final {
    application::EditorSceneRuntime& scene;
    application::EditorFeatureGraphDocument* document;
    std::filesystem::path document_path;
    const tooling::ProjectOperationRequest& request;
    const tooling::ProjectOperationStatus& operation;
    std::function<foundation::Result(std::string_view)> invoke_view_action;
};

/** Drives actual ImGui controls. Business loading/publication/persistence stays in their handlers. */
class MainFeatureWorkflowProbe final {
public:
    MainFeatureWorkflowProbe();
    ~MainFeatureWorkflowProbe();
    MainFeatureWorkflowProbe(const MainFeatureWorkflowProbe&) = delete;
    MainFeatureWorkflowProbe& operator=(const MainFeatureWorkflowProbe&) = delete;

    [[nodiscard]] foundation::Result before_ui(const FeatureWorkflowContext&);
    [[nodiscard]] foundation::Result after_ui(const FeatureWorkflowContext&);
    /** Called only by Main's actual inspect_node UI callback, before returning its result. */
    void record_inspection(const foundation::ResultValue<application::runtime::FeatureRuntimeNodeSnapshot>&);
    [[nodiscard]] bool complete() const noexcept;
    [[nodiscard]] foundation::Result finish() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aeg::editor_test
