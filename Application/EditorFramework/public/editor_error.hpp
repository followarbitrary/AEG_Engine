/**
 * @file editor_error.hpp
 * @brief Backend-neutral editor error notification state.
 */
#pragma once

#include <cstdint>
#include <exception>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include "result.hpp"

namespace aeg::editor {
/** @brief Converts exceptions at an editor operation boundary into ordinary failures. */
template <typename Operation>
[[nodiscard]] foundation::Result guard_editor_operation(Operation&& operation) noexcept {
    try {
        return std::forward<Operation>(operation)();
    } catch (const std::bad_alloc&) {
        return foundation::Result::failure(foundation::ResultCode::kOutOfMemory);
    } catch (const std::exception& exception) {
        try {
            return foundation::unknown_exception(exception);
        } catch (...) {
            return foundation::Result::failure(foundation::ResultCode::kUnknown);
        }
    } catch (...) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown);
    }
}

/** @brief Controls whether an editor error is non-blocking or requires termination. */
enum class EditorErrorSeverity : uint8_t {
    kRecoverable,
    kFatal,
};

/** @brief Runtime failures requiring a controlled shutdown rather than another operation. */
[[nodiscard]] constexpr EditorErrorSeverity editor_error_severity(foundation::ResultCode code) noexcept {
    return code == foundation::ResultCode::kDeviceLost || code == foundation::ResultCode::kOutOfMemory
               ? EditorErrorSeverity::kFatal : EditorErrorSeverity::kRecoverable;
}

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
