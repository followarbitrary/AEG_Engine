/**
 * @file result.hpp
 * @brief Common success and failure values shared by engine subsystems.
 */
#pragma once

#include <cstdint>
#include <exception>
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
    kUnknown,
    kCancelled
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

/** @brief Creates an invalid-argument failure with the supplied diagnostic. */
[[nodiscard]] Result invalid_argument(std::string message);

/** @brief Creates an invalid-state failure with the supplied diagnostic. */
[[nodiscard]] Result invalid_state(std::string message);

/** @brief Creates an unsupported-operation failure with the supplied diagnostic. */
[[nodiscard]] Result unsupported(std::string message);

/** @brief Creates a not-found failure with the supplied diagnostic. */
[[nodiscard]] Result not_found(std::string message);

/** @brief Creates a not-ready failure with the supplied diagnostic. */
[[nodiscard]] Result not_ready(std::string message);

/** @brief Creates an out-of-memory failure with the supplied diagnostic. */
[[nodiscard]] Result out_of_memory(std::string message);

/** @brief Converts an exception into an unknown-result failure. */
[[nodiscard]] Result unknown_exception(const std::exception& exception);

/**
 * @brief Couples an operation status with a returned value.
 *
 * The value is meaningful only when `result` is successful. On failure it
 * is value-initialized unless the producing API documents another policy.
 */
template <typename T>
struct ResultValue {
    Result result{};
    T value{};

    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(result); }
};

/** @brief Returns the stable textual name of a result code. */
[[nodiscard]] const char* result_code_name(ResultCode code) noexcept;
}  // namespace aeg::foundation
