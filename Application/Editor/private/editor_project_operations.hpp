/** @brief Editor-owned controls for operations on the selected saved project files. */
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

#include "project_operation.hpp"

namespace aeg::application {

/** Owns the real asynchronous Tooling service; no editor draft is saved implicitly. */
class EditorProjectOperations final {
public:
    EditorProjectOperations();
    ~EditorProjectOperations();
    EditorProjectOperations(const EditorProjectOperations&) = delete;
    EditorProjectOperations& operator=(const EditorProjectOperations&) = delete;

    /** Selects this exact project file. An empty path closes the current operation session. */
    [[nodiscard]] foundation::Result set_project(const std::filesystem::path& project_file);
    /** Copies all explicit inputs; the selected project file must match exactly. */
    [[nodiscard]] foundation::Result set_request(const tooling::ProjectOperationRequest& request);
    [[nodiscard]] foundation::Result start(tooling::ProjectOperation operation);
    [[nodiscard]] foundation::Result cancel();
    /** Draws window contents only; the caller owns ImGui Begin/End and visibility. */
    void draw();
    [[nodiscard]] foundation::Result poll();
    [[nodiscard]] const tooling::ProjectOperationStatus& status() const noexcept { return status_; }
    [[nodiscard]] const tooling::ProjectOperationRequest& request() const noexcept { return request_; }
    /** A cancellation request remains active until the actual worker has retired. */
    [[nodiscard]] bool active() const noexcept;
    /** Requests cancellation, then destroys the service and joins its actual worker. */
    void shutdown() noexcept;

private:
    std::filesystem::path project_file_{};
    tooling::ProjectOperationRequest request_{};
    tooling::ProjectOperationStatus status_{};
    foundation::Result action_error_{};
    std::unique_ptr<tooling::ProjectOperationService> service_{};
    uint64_t operation_id_{0};
};

}  // namespace aeg::application
