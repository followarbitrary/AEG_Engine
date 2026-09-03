/**
 * @file result.hpp
 * @brief Common success and failure values shared by engine subsystems.
 */
#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace aeg::foundation {
    /**
     * @brief Categorizes the outcome of an engine operation.
     *
     * Subsystems should keep backend-specific details in the diagnostic
     * message or in a subsystem-specific result payload.
     */
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

    /**
     * @brief Operation status with an optional diagnostic message.
     *
     * A default-constructed value represents success. The message is intended
     * for diagnostics and is not a stable machine-readable error identifier.
     */
    struct Result {
        ResultCode code{ResultCode::kSuccess};
        std::string message{};

        [[nodiscard]] static Result success() { return {}; }

        [[nodiscard]] static Result failure(ResultCode result_code, std::string result_message = {}) {
            return Result{result_code, std::move(result_message)};
        }

        [[nodiscard]] explicit operator bool() const noexcept { return code == ResultCode::kSuccess; }
    };

    /**
     * @brief Couples an operation status with a returned value.
     *
     * The value is meaningful only when `result` is successful. On failure it
     * is value-initialized unless the producing API documents another policy.
     */
    template<typename T>
    struct ResultValue {
        Result result{};
        T value{};

        [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(result); }
    };

    /** @brief Returns the stable textual name of a result code. */
    [[nodiscard]] const char* result_code_name(ResultCode code) noexcept;
} // namespace aeg::foundation
