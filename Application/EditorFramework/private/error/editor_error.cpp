/**
 * @file editor_error.cpp
 * @brief Implements editor error notification state.
 */
#include "editor_error.hpp"

#include <new>
#include <utility>

namespace aeg::editor {
foundation::Result EditorErrorState::report(std::string_view operation, const foundation::Result& result,
                                            EditorErrorSeverity severity) noexcept {
    if (result) {
        return foundation::Result::success();
    }
    if (operation.empty()) {
        return foundation::Result::failure(foundation::ResultCode::kInvalidArgument);
    }

    try {
        const bool same_notice = has_notice_ && notice_.operation == operation && notice_.code == result.code &&
                                 notice_.message == result.message && notice_.severity == severity;
        if (same_notice) {
            if (notice_.occurrence_count != UINT32_MAX) {
                ++notice_.occurrence_count;
            }
            return foundation::Result::success();
        }

        EditorErrorNotice next_notice{};
        next_notice.operation.assign(operation);
        next_notice.code = result.code;
        next_notice.message = result.message;
        next_notice.severity = severity;
        next_notice.occurrence_count = 1;
        notice_ = std::move(next_notice);
        has_notice_ = true;
        notification_requested_ = true;
        return foundation::Result::success();
    } catch (...) {
        // Reporting a recoverable failure must not itself become a fatal one.
        // Preserve classification without allocating when diagnostic text fails.
        notice_.operation.clear();
        notice_.message.clear();
        notice_.code = result.code;
        notice_.severity = severity;
        notice_.occurrence_count = 1;
        has_notice_ = true;
        notification_requested_ = true;
        return foundation::Result::success();
    }
}

bool EditorErrorState::has_pending() const noexcept {
    return has_notice_;
}

const EditorErrorNotice* EditorErrorState::pending() const noexcept {
    return has_notice_ ? &notice_ : nullptr;
}

bool EditorErrorState::consume_notification_request() noexcept {
    const bool requested = notification_requested_;
    notification_requested_ = false;
    return requested;
}

void EditorErrorState::dismiss() noexcept {
    notice_ = {};
    has_notice_ = false;
    notification_requested_ = false;
}
}  // namespace aeg::editor
