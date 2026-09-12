/**
 * @file editor_error.hpp
 * @brief Backend-neutral editor error notification state.
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "result.hpp"

namespace aeg::editor {
/** @brief Controls whether an editor error is non-blocking or requires termination. */
enum class EditorErrorSeverity : uint8_t {
    kRecoverable,
    kFatal,
};

/** @brief One user-visible error captured by the editor application. */
struct EditorErrorNotice final {
    /** @brief Operation that produced the error. */
    std::string operation{};
    /** @brief Stable Foundation result category for the failure. */
    foundation::ResultCode code{foundation::ResultCode::kUnknown};
    /** @brief Diagnostic text copied from the failed operation. */
    std::string message{};
    /** @brief Whether the editor can continue after this failure. */
    EditorErrorSeverity severity{EditorErrorSeverity::kRecoverable};
    /** @brief Number of equivalent failures coalesced into this notice. */
    uint32_t occurrence_count{0};
};

/** @brief Owns the current editor error notice and its UI notification state. */
class EditorErrorState final {
public:
    /** @brief Captures one failed operation without throwing across the editor boundary. */
    [[nodiscard]] foundation::Result report(std::string_view operation, const foundation::Result& result,
                                             EditorErrorSeverity severity) noexcept;
    /** @brief Returns whether a notice is waiting to be shown or dismissed. */
    [[nodiscard]] bool has_pending() const noexcept;
    /** @brief Returns the current notice or nullptr when no notice is pending. */
    [[nodiscard]] const EditorErrorNotice* pending() const noexcept;
    /** @brief Returns and clears the request to show a newly captured notice. */
    [[nodiscard]] bool consume_notification_request() noexcept;
    /** @brief Removes the current notice and any pending notification request. */
    void dismiss() noexcept;

private:
    EditorErrorNotice notice_{};
    bool has_notice_{false};
    bool notification_requested_{false};
};
}  // namespace aeg::editor
