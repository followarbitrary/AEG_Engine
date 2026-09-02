#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace aeg::foundation {
    // Shared operation status for engine subsystems. Subsystems should keep
    // backend-specific details in the diagnostic message or result payload.
    enum class ResultCode : uint8_t {
        kSuccess,
        kInvalidArgument,
        kInvalidState,
        kUnsupported,
        kNotFound,
        kOutOfMemory,
        kTimeout,
        kDeviceLost,
        kBackendError,
        kNotReady,
        kOutOfDate,
        kSurfaceLost,
        kUnknown
    };

    struct Result {
        ResultCode code{ResultCode::kSuccess};
        std::string message{};

        [[nodiscard]] static Result success() { return {}; }

        [[nodiscard]] static Result failure(ResultCode result_code, std::string result_message = {}) {
            return Result{result_code, std::move(result_message)};
        }

        [[nodiscard]] explicit operator bool() const noexcept { return code == ResultCode::kSuccess; }
    };

    template<typename T>
    struct ResultValue {
        Result result{};
        T value{};

        [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(result); }
    };

    [[nodiscard]] const char* result_code_name(ResultCode code) noexcept;
} // namespace aeg::foundation
