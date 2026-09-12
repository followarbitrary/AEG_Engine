#include <cassert>

#include "editor_error.hpp"

int main() {
    using aeg::editor::EditorErrorSeverity;
    using aeg::editor::EditorErrorState;
    using aeg::foundation::Result;
    using aeg::foundation::ResultCode;

    EditorErrorState state;
    const Result failure = Result::failure(ResultCode::kBackendError, "device submission failed");

    assert(state.report("run_editor_frame", failure, EditorErrorSeverity::kRecoverable));
    assert(state.has_pending());
    assert(state.consume_notification_request());

    const auto* notice = state.pending();
    assert(notice != nullptr);
    assert(notice->operation == "run_editor_frame");
    assert(notice->code == ResultCode::kBackendError);
    assert(notice->message == "device submission failed");
    assert(notice->severity == EditorErrorSeverity::kRecoverable);
    assert(notice->occurrence_count == 1);

    assert(state.report("run_editor_frame", failure, EditorErrorSeverity::kRecoverable));
    assert(!state.consume_notification_request());
    assert(state.pending()->occurrence_count == 2);

    const Result fatal = Result::failure(ResultCode::kDeviceLost, "GPU device was lost");
    assert(state.report("run_editor_frame", fatal, EditorErrorSeverity::kFatal));
    assert(state.consume_notification_request());
    assert(state.pending()->severity == EditorErrorSeverity::kFatal);
    assert(state.pending()->code == ResultCode::kDeviceLost);

    state.dismiss();
    assert(!state.has_pending());
    assert(state.pending() == nullptr);
    assert(!state.consume_notification_request());

    assert(state.report("ignored_success", Result::success(), EditorErrorSeverity::kRecoverable));
    assert(!state.has_pending());
    return 0;
}
