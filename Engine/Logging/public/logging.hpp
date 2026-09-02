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
    enum class Level : uint8_t { kTrace, kDebug, kInfo, kNotice, kWarning, kError, kCritical, kOff };

    enum class Category : uint8_t {
        kCore, kRhi, kRenderGraph, kResource, kApplication, kShader, kAsset, kTest, kCount
    };

    inline constexpr std::size_t kCategoryCount = static_cast<std::size_t>(Category::kCount);

#if defined(NDEBUG)
    inline constexpr Level kDefaultRuntimeLevel = Level::kInfo;
#else
    inline constexpr Level kDefaultRuntimeLevel = Level::kDebug;
#endif

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

    struct Status {
        bool succeeded{true};
        std::string message{};

        [[nodiscard]] explicit operator bool() const noexcept { return succeeded; }

        [[nodiscard]] static Status success() { return {}; }

        [[nodiscard]] static Status failure(std::string error_message) {
            return Status{false, std::move(error_message)};
        }
    };

    [[nodiscard]] std::string_view category_name(Category category) noexcept;

    [[nodiscard]] Status initialize(const Config &config = {});

    void shutdown() noexcept;

    void flush() noexcept;

    [[nodiscard]] bool is_initialized() noexcept;

    [[nodiscard]] bool set_category_level(Category category, Level level) noexcept;

    [[nodiscard]] Level category_level(Category category) noexcept;

    void set_thread_context(std::string_view key, std::string_view value) noexcept;

    void erase_thread_context(std::string_view key) noexcept;

    void clear_thread_context() noexcept;

    namespace detail {
        struct QuillFrontendOptions : quill::FrontendOptions {
            // A bounded blocking queue keeps logging lossless while placing an upper
            // bound on per-thread memory use during bursts.
            static constexpr quill::QueueType kQueueType = quill::QueueType::BoundedBlocking;
            static constexpr std::size_t kInitialQueueCapacity = 256u * 1024u;
            static constexpr uint32_t kBlockingQueueRetryIntervalNs = 1000u;
        };

        using QuillLogger = quill::LoggerImpl<QuillFrontendOptions>;

        [[nodiscard]] QuillLogger *logger(Category category) noexcept;
    } // namespace detail
} // namespace aeg::logging

// Keep compile-time filtering in Quill's macro layer. Disabled levels are
// removed before argument evaluation and therefore have no runtime overhead.
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
