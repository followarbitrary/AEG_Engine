/**
 * @file logging.hpp
 * @brief Public logging configuration, lifecycle, context, and macros.
 */
#pragma once

// Quill also provides non-prefixed LOG_* macros. Keep those names private to
// the logging implementation so engine modules expose only the AEG API.
#ifndef QUILL_DISABLE_NON_PREFIXED_MACROS
#define QUILL_DISABLE_NON_PREFIXED_MACROS 1
#endif

#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/core/FrontendOptions.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace aeg::logging {
    /** @brief Severity threshold used by the logging facade. */
    enum class Level : uint8_t { kTrace, kDebug, kInfo, kNotice, kWarning, kError, kCritical, kOff };

    /** @brief Independent logger channels exposed by the engine. */
    enum class Category : uint8_t {
        kCore, kRhi, kRenderGraph, kResource, kApplication, kShader, kAsset, kTest, kCount
    };

    inline constexpr std::size_t kCategoryCount = static_cast<std::size_t>(Category::kCount);

#if defined(NDEBUG)
    inline constexpr Level kDefaultRuntimeLevel = Level::kInfo;
#else
    inline constexpr Level kDefaultRuntimeLevel = Level::kDebug;
#endif

    /**
     * @brief Complete configuration used by initialize().
     *
     * Optional category levels override default_level. String and path values
     * are copied by the logging system during initialization.
     */
    struct Config {
        Level default_level{kDefaultRuntimeLevel};
        std::array<std::optional<Level>, kCategoryCount> category_levels{};

        bool console_enabled{true};
        bool console_colors{true};

        bool file_enabled{false};
        std::filesystem::path file_path{"logs/aeg-engine.log"};
        std::size_t file_rotation_bytes{16u * 1024u * 1024u};
        uint32_t file_max_backup_files{4};
        bool rotate_file_on_startup{false};

        bool immediate_flush{false};
        uint32_t immediate_flush_every_n_messages{1};

        std::string format_pattern{
            "%(time) [%(thread_id)] %(logger:<12) %(log_level:<9) "
            "%(short_source_location:<36) %(message)%(mdc)"
        };
        std::string timestamp_pattern{"%Y-%m-%d %H:%M:%S.%Qms"};
        std::string source_location_path_strip_prefix{};
        bool use_utc_timestamps{false};
    };

    /** @brief Status returned by logging lifecycle and configuration operations. */
    struct Status {
        bool succeeded{true};
        std::string message{};

        [[nodiscard]] explicit operator bool() const noexcept { return succeeded; }

        [[nodiscard]] static Status success() { return {}; }

        [[nodiscard]] static Status failure(std::string error_message) {
            return Status{false, std::move(error_message)};
        }
    };

    /** @brief Returns the stable display name for a logging category. */
    [[nodiscard]] std::string_view category_name(Category category) noexcept;

    /**
     * @brief Initializes the logging backend and its sinks.
     *
     * Calling initialize() while logging is already active is idempotent when
     * the existing configuration is still usable.
     */
    [[nodiscard]] Status initialize(const Config &config = {});

    /**
     * @brief Stops logging and releases backend-owned resources.
     *
     * The function is safe to call more than once. Calls made after shutdown
     * are ignored until the next successful initialize().
     */
    void shutdown() noexcept;

    /** @brief Flushes all active sinks, if logging is initialized. */
    void flush() noexcept;

    /** @brief Reports whether the logging runtime is currently initialized. */
    [[nodiscard]] bool is_initialized() noexcept;

    /** @brief Changes the runtime threshold for one category. */
    [[nodiscard]] bool set_category_level(Category category, Level level) noexcept;

    /** @brief Reads the effective runtime threshold for a category. */
    [[nodiscard]] Level category_level(Category category) noexcept;

    /** @brief Adds or replaces a key/value pair on the calling thread's log context. */
    void set_thread_context(std::string_view key, std::string_view value) noexcept;

    /** @brief Removes one key from the calling thread's log context. */
    void erase_thread_context(std::string_view key) noexcept;

    /** @brief Removes all context values associated with the calling thread. */
    void clear_thread_context() noexcept;

    namespace detail {
        /**
         * @brief Quill frontend policy used by the AEG logging facade.
         *
         * This type is public only because Quill's logger template requires it;
         * application code should use the AEG_LOG_* macros instead.
         */
        struct QuillFrontendOptions : quill::FrontendOptions {
            // A bounded blocking queue keeps logging lossless while placing an upper
            // bound on per-thread memory use during bursts.
            static constexpr quill::QueueType kQueueType = quill::QueueType::BoundedBlocking;
            static constexpr std::size_t kInitialQueueCapacity = 256u * 1024u;
            static constexpr uint32_t kBlockingQueueRetryIntervalNs = 1000u;
        };

        /** @brief Quill logger type configured for AEG's frontend policy. */
        using QuillLogger = quill::LoggerImpl<QuillFrontendOptions>;

        /** @brief Returns the logger for a category without throwing. */
        [[nodiscard]] QuillLogger *logger(Category category) noexcept;
    } // namespace detail
} // namespace aeg::logging

/**
 * @brief Compile-time filtered logging macros.
 *
 * Disabled levels are removed before argument evaluation, so their arguments
 * have no runtime cost. The category argument must be an aeg::logging::Category.
 */
#if QUILL_COMPILE_ACTIVE_LOG_LEVEL <= QUILL_COMPILE_ACTIVE_LOG_LEVEL_TRACE_L3
#define AEG_LOG_TRACE_L3(category, fmt, ...)                                          \
    do {                                                                              \
        auto* const aeg_logging_logger__ = ::aeg::logging::detail::logger(category);  \
        if (aeg_logging_logger__ != nullptr) {                                        \
            QUILL_LOG_TRACE_L3(aeg_logging_logger__, fmt __VA_OPT__(, ) __VA_ARGS__); \
        }                                                                             \
    } while (false)
#else
#define AEG_LOG_TRACE_L3(category, fmt, ...) (void)0
#endif

#if QUILL_COMPILE_ACTIVE_LOG_LEVEL <= QUILL_COMPILE_ACTIVE_LOG_LEVEL_TRACE_L2
#define AEG_LOG_TRACE_L2(category, fmt, ...)                                          \
    do {                                                                              \
        auto* const aeg_logging_logger__ = ::aeg::logging::detail::logger(category);  \
        if (aeg_logging_logger__ != nullptr) {                                        \
            QUILL_LOG_TRACE_L2(aeg_logging_logger__, fmt __VA_OPT__(, ) __VA_ARGS__); \
        }                                                                             \
    } while (false)
#else
#define AEG_LOG_TRACE_L2(category, fmt, ...) (void)0
#endif

#if QUILL_COMPILE_ACTIVE_LOG_LEVEL <= QUILL_COMPILE_ACTIVE_LOG_LEVEL_TRACE_L1
#define AEG_LOG_TRACE_L1(category, fmt, ...)                                          \
    do {                                                                              \
        auto* const aeg_logging_logger__ = ::aeg::logging::detail::logger(category);  \
        if (aeg_logging_logger__ != nullptr) {                                        \
            QUILL_LOG_TRACE_L1(aeg_logging_logger__, fmt __VA_OPT__(, ) __VA_ARGS__); \
        }                                                                             \
    } while (false)
#else
#define AEG_LOG_TRACE_L1(category, fmt, ...) (void)0
#endif

#define AEG_LOG_TRACE(category, fmt, ...) AEG_LOG_TRACE_L1(category, fmt __VA_OPT__(, ) __VA_ARGS__)

#if QUILL_COMPILE_ACTIVE_LOG_LEVEL <= QUILL_COMPILE_ACTIVE_LOG_LEVEL_DEBUG
#define AEG_LOG_DEBUG(category, fmt, ...)                                            \
    do {                                                                             \
        auto* const aeg_logging_logger__ = ::aeg::logging::detail::logger(category); \
        if (aeg_logging_logger__ != nullptr) {                                       \
            QUILL_LOG_DEBUG(aeg_logging_logger__, fmt __VA_OPT__(, ) __VA_ARGS__);   \
        }                                                                            \
    } while (false)
#else
#define AEG_LOG_DEBUG(category, fmt, ...) (void)0
#endif

#if QUILL_COMPILE_ACTIVE_LOG_LEVEL <= QUILL_COMPILE_ACTIVE_LOG_LEVEL_INFO
#define AEG_LOG_INFO(category, fmt, ...)                                             \
    do {                                                                             \
        auto* const aeg_logging_logger__ = ::aeg::logging::detail::logger(category); \
        if (aeg_logging_logger__ != nullptr) {                                       \
            QUILL_LOG_INFO(aeg_logging_logger__, fmt __VA_OPT__(, ) __VA_ARGS__);    \
        }                                                                            \
    } while (false)
#else
#define AEG_LOG_INFO(category, fmt, ...) (void)0
#endif

#if QUILL_COMPILE_ACTIVE_LOG_LEVEL <= QUILL_COMPILE_ACTIVE_LOG_LEVEL_NOTICE
#define AEG_LOG_NOTICE(category, fmt, ...)                                           \
    do {                                                                             \
        auto* const aeg_logging_logger__ = ::aeg::logging::detail::logger(category); \
        if (aeg_logging_logger__ != nullptr) {                                       \
            QUILL_LOG_NOTICE(aeg_logging_logger__, fmt __VA_OPT__(, ) __VA_ARGS__);  \
        }                                                                            \
    } while (false)
#else
#define AEG_LOG_NOTICE(category, fmt, ...) (void)0
#endif

#if QUILL_COMPILE_ACTIVE_LOG_LEVEL <= QUILL_COMPILE_ACTIVE_LOG_LEVEL_WARNING
#define AEG_LOG_WARNING(category, fmt, ...)                                          \
    do {                                                                             \
        auto* const aeg_logging_logger__ = ::aeg::logging::detail::logger(category); \
        if (aeg_logging_logger__ != nullptr) {                                       \
            QUILL_LOG_WARNING(aeg_logging_logger__, fmt __VA_OPT__(, ) __VA_ARGS__); \
        }                                                                            \
    } while (false)
#else
#define AEG_LOG_WARNING(category, fmt, ...) (void)0
#endif

#define AEG_LOG_WARN(category, fmt, ...) AEG_LOG_WARNING(category, fmt __VA_OPT__(, ) __VA_ARGS__)

#if QUILL_COMPILE_ACTIVE_LOG_LEVEL <= QUILL_COMPILE_ACTIVE_LOG_LEVEL_ERROR
#define AEG_LOG_ERROR(category, fmt, ...)                                            \
    do {                                                                             \
        auto* const aeg_logging_logger__ = ::aeg::logging::detail::logger(category); \
        if (aeg_logging_logger__ != nullptr) {                                       \
            QUILL_LOG_ERROR(aeg_logging_logger__, fmt __VA_OPT__(, ) __VA_ARGS__);   \
        }                                                                            \
    } while (false)
#else
#define AEG_LOG_ERROR(category, fmt, ...) (void)0
#endif

#if QUILL_COMPILE_ACTIVE_LOG_LEVEL <= QUILL_COMPILE_ACTIVE_LOG_LEVEL_CRITICAL
#define AEG_LOG_CRITICAL(category, fmt, ...)                                          \
    do {                                                                              \
        auto* const aeg_logging_logger__ = ::aeg::logging::detail::logger(category);  \
        if (aeg_logging_logger__ != nullptr) {                                        \
            QUILL_LOG_CRITICAL(aeg_logging_logger__, fmt __VA_OPT__(, ) __VA_ARGS__); \
        }                                                                             \
    } while (false)
#else
#define AEG_LOG_CRITICAL(category, fmt, ...) (void)0
#endif
