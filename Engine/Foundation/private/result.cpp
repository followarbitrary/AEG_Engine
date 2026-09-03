/**
 * @file result.cpp
 * @brief Implements the textual mapping for common operation results.
 */
#include "../public/result.hpp"

namespace aeg::foundation {
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
} // namespace aeg::foundation
