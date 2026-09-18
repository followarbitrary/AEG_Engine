#pragma once

#include <functional>
#include <optional>
#include <string>

#include "project_operation.hpp"
#include "result.hpp"

namespace aeg::application {

class EditorProjectOperationController final {
public:
    using Start = std::function<foundation::ResultValue<uint64_t>(const tooling::ProjectOperationRequest&)>;
    using Poll = std::function<foundation::ResultValue<tooling::ProjectOperationStatus>(uint64_t)>;
    using Cancel = std::function<foundation::Result(uint64_t)>;

    EditorProjectOperationController(Start start, Poll poll, Cancel cancel);
    explicit EditorProjectOperationController(tooling::ProjectOperationService& service);

    [[nodiscard]] foundation::Result start(const tooling::ProjectOperationRequest& request);
    [[nodiscard]] foundation::Result tick();
    [[nodiscard]] foundation::Result cancel();
    [[nodiscard]] const std::optional<tooling::ProjectOperationStatus>& status() const noexcept { return status_; }
    [[nodiscard]] bool busy() const noexcept { return operation_id_.has_value(); }

private:
    Start start_{};
    Poll poll_{};
    Cancel cancel_{};
    std::optional<uint64_t> operation_id_{};
    std::optional<tooling::ProjectOperationStatus> status_{};
};

}  // namespace aeg::application
