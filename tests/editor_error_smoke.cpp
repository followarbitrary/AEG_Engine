#include <cassert>
#include <cstdlib>
#include <new>
#include <stdexcept>

#include "editor_error.hpp"

namespace {
bool reject_allocations = false;
}

void* operator new(std::size_t size) {
    if (!reject_allocations) {
        if (void* allocation = std::malloc(size == 0 ? 1 : size)) return allocation;
    }
    throw std::bad_alloc{};
}
void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { std::free(allocation); }

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

    // A failed notification allocation must not upgrade a recoverable operation
    // into a fatal error; retain a code-only notice that the UI can still show.
    reject_allocations = true;
    const Result notification_result = state.report("recoverable_operation_with_long_name", failure,
                                                     EditorErrorSeverity::kRecoverable);
    reject_allocations = false;
    assert(notification_result);
    assert(state.has_pending());
    assert(state.pending()->code == ResultCode::kBackendError);
    assert(state.pending()->severity == EditorErrorSeverity::kRecoverable);
    assert(state.consume_notification_request());
    state.dismiss();

    const auto thrown = aeg::editor::guard_editor_operation([]() -> Result {
        throw std::runtime_error("recoverable action failure");
    });
    assert(!thrown && thrown.code == ResultCode::kUnknown);
    assert(thrown.message == "recoverable action failure");
    assert(aeg::editor::editor_error_severity(thrown.code) == EditorErrorSeverity::kRecoverable);
    const auto nonstandard = aeg::editor::guard_editor_operation([]() -> Result { throw 42; });
    assert(!nonstandard && nonstandard.code == ResultCode::kUnknown);
    const auto allocation = aeg::editor::guard_editor_operation([]() -> Result { throw std::bad_alloc{}; });
    assert(!allocation && allocation.code == ResultCode::kOutOfMemory);
    assert(aeg::editor::editor_error_severity(allocation.code) == EditorErrorSeverity::kFatal);
    assert(aeg::editor::editor_error_severity(ResultCode::kDeviceLost) == EditorErrorSeverity::kFatal);
    assert(aeg::editor::guard_editor_operation([] { return Result::success(); }));
    const std::runtime_error conversion_failure("diagnostic allocation failed");
    reject_allocations = true;
    const auto code_only = aeg::editor::guard_editor_operation([&]() -> Result { throw conversion_failure; });
    reject_allocations = false;
    assert(!code_only && code_only.code == ResultCode::kUnknown);
    assert(code_only.message.empty());
    return 0;
}
