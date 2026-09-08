/**
 * @file result.cpp
 * @brief Implements the textual mapping for common operation results.
 */
#include "../public/result.hpp"

namespace aeg::foundation {
Result invalid_argument(std::string message) {
    return Result::failure(ResultCode::kInvalidArgument, std::move(message));
}

Result invalid_state(std::string message) {
    return Result::failure(ResultCode::kInvalidState, std::move(message));
}

Result unsupported(std::string message) {
    return Result::failure(ResultCode::kUnsupported, std::move(message));
}

Result not_found(std::string message) {
    return Result::failure(ResultCode::kNotFound, std::move(message));
}

Result not_ready(std::string message) {
    return Result::failure(ResultCode::kNotReady, std::move(message));
}

Result out_of_memory(std::string message) {
    return Result::failure(ResultCode::kOutOfMemory, std::move(message));
}

Result unknown_exception(const std::exception& exception) {
    return Result::failure(ResultCode::kUnknown, exception.what());
}

const char* result_code_name(ResultCode code) noexcept {
    switch (code) {
        case ResultCode::kSuccess:
            return "Success";
        case ResultCode::kInvalidArgument:
            return "InvalidArgument";
        case ResultCode::kInvalidState:
            return "InvalidState";
        case ResultCode::kUnsupported:
            return "Unsupported";
        case ResultCode::kNotFound:
            return "NotFound";
        case ResultCode::kOutOfMemory:
            return "OutOfMemory";
        case ResultCode::kCancelled:
            return "Cancelled";
        case ResultCode::kTimeout:
            return "Timeout";
        case ResultCode::kDeviceLost:
            return "DeviceLost";
        case ResultCode::kBackendError:
            return "BackendError";
        case ResultCode::kNotReady:
            return "NotReady";
        case ResultCode::kOutOfDate:
            return "OutOfDate";
        case ResultCode::kSurfaceLost:
            return "SurfaceLost";
        case ResultCode::kUnknown:
            return "Unknown";
    }
    return "Unknown";
}
}  // namespace aeg::foundation
