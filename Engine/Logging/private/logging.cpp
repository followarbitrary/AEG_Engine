#include "../public/logging.hpp"

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/sinks/ConsoleSink.h>
#include <quill/sinks/NullSink.h>
#include <quill/sinks/RotatingFileSink.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace aeg::logging {
    namespace {
        using Frontend = quill::FrontendImpl<detail::QuillFrontendOptions>;
        using Logger = detail::QuillLogger;

        enum class Lifecycle : uint8_t { kUninitialized, kInitializing, kRunning, kShuttingDown, kShutdown };

        struct State {
            std::mutex mutex{};
            std::condition_variable condition{};
            Lifecycle lifecycle{Lifecycle::kUninitialized};
            Config config{};
            std::array<Logger *, kCategoryCount> loggers{};
            std::vector<std::shared_ptr<quill::Sink> > sinks{};
            bool backend_started{false};
            std::string error{};
            std::string backend_error{};
        };

        // The state intentionally lives until process termination. Quill owns global
        // frontend/backend singletons, so this avoids static-destruction ordering
        // hazards between logger pointers, sinks, and the backend thread.
        State &state() noexcept {
            static State *instance = new State();
            return *instance;
        }

        [[nodiscard]] std::optional<std::size_t> category_index(Category category) noexcept {
            const auto value = static_cast<std::size_t>(category);
            if (value >= kCategoryCount) {
                return std::nullopt;
            }
            return value;
        }

        [[nodiscard]] bool is_valid_level(Level level) noexcept {
            return static_cast<uint8_t>(level) <= static_cast<uint8_t>(Level::kOff);
        }

        [[nodiscard]] quill::LogLevel to_quill_level(Level level) noexcept {
            switch (level) {
                case Level::kTrace:
                    return quill::LogLevel::TraceL3;
                case Level::kDebug:
                    return quill::LogLevel::Debug;
                case Level::kInfo:
                    return quill::LogLevel::Info;
                case Level::kNotice:
                    return quill::LogLevel::Notice;
                case Level::kWarning:
                    return quill::LogLevel::Warning;
                case Level::kError:
                    return quill::LogLevel::Error;
                case Level::kCritical:
                    return quill::LogLevel::Critical;
                case Level::kOff:
                    return quill::LogLevel::None;
            }
            return quill::LogLevel::None;
        }

        [[nodiscard]] Level from_quill_level(quill::LogLevel level) noexcept {
            switch (level) {
                case quill::LogLevel::TraceL3:
                case quill::LogLevel::TraceL2:
                case quill::LogLevel::TraceL1:
                    return Level::kTrace;
                case quill::LogLevel::Debug:
                    return Level::kDebug;
                case quill::LogLevel::Info:
                    return Level::kInfo;
                case quill::LogLevel::Notice:
                    return Level::kNotice;
                case quill::LogLevel::Warning:
                    return Level::kWarning;
                case quill::LogLevel::Error:
                    return Level::kError;
                case quill::LogLevel::Critical:
                    return Level::kCritical;
                case quill::LogLevel::None:
                    return Level::kOff;
                case quill::LogLevel::Backtrace:
                    return Level::kCritical;
            }
            return Level::kOff;
        }

        [[nodiscard]] Level configured_level(const Config &config, std::size_t index) noexcept {
            if (config.category_levels[index].has_value()) {
                return *config.category_levels[index];
            }
            return config.default_level;
        }

        [[nodiscard]] Status validate_config(const Config &config) {
            if (!is_valid_level(config.default_level)) {
                return Status::failure("Invalid default logging level");
            }

            for (const std::optional<Level> &level: config.category_levels) {
                if (level.has_value() && !is_valid_level(*level)) {
                    return Status::failure("Invalid category logging level");
                }
            }

            if (config.file_enabled && config.file_path.empty()) {
                return Status::failure("File logging is enabled but file_path is empty");
            }
            if (config.file_rotation_bytes != 0 && config.file_rotation_bytes < 512) {
                return Status::failure("file_rotation_bytes must be zero or at least 512 bytes");
            }
            if (config.immediate_flush && config.immediate_flush_every_n_messages == 0) {
                return Status::failure("immediate_flush_every_n_messages must be greater than zero");
            }
            if (config.format_pattern.empty()) {
                return Status::failure("format_pattern must not be empty");
            }
            if (config.timestamp_pattern.empty()) {
                return Status::failure("timestamp_pattern must not be empty");
            }
            return Status::success();
        }

        [[nodiscard]] std::string exception_message() {
            try {
                throw;
            } catch (const std::exception &exception) {
                return exception.what();
            } catch (...) {
                return "unknown exception";
            }
        }

        [[nodiscard]] Status initialize_impl(State &state, const Config &config) {
            const Status validation = validate_config(config);
            if (!validation) {
                return validation;
            }

            std::vector<std::shared_ptr<quill::Sink> > sinks;
            try {
                const auto timezone = config.use_utc_timestamps ? quill::Timezone::GmtTime : quill::Timezone::LocalTime;
                quill::PatternFormatterOptions pattern{
                    config.format_pattern, config.timestamp_pattern, timezone, true, '\n'
                };
                pattern.source_location_path_strip_prefix = config.source_location_path_strip_prefix;
                pattern.source_location_remove_relative_paths = true;

                if (config.console_enabled) {
                    quill::ConsoleSinkConfig console_config{};
                    console_config.set_stream("stderr");
                    console_config.set_colour_mode(config.console_colors
                                                       ? quill::ConsoleSinkConfig::ColourMode::Automatic
                                                       : quill::ConsoleSinkConfig::ColourMode::Never);
                    auto console_sink = Frontend::template create_sink<quill::ConsoleSink>(
                        "AEG.Console", console_config);
                    console_sink->set_log_level_filter(quill::LogLevel::TraceL3);
                    sinks.push_back(std::move(console_sink));
                }

                if (config.file_enabled) {
                    quill::RotatingFileSinkConfig file_config{};
                    file_config.set_open_mode("a");
                    file_config.set_max_backup_files(config.file_max_backup_files);
                    file_config.set_overwrite_rolled_files(true);
                    file_config.set_remove_old_files(false);
                    file_config.set_rotation_on_creation(config.rotate_file_on_startup);
                    file_config.set_timezone(timezone);
                    if (config.file_rotation_bytes != 0) {
                        file_config.set_rotation_max_file_size(config.file_rotation_bytes);
                    }

                    auto file_sink =
                            Frontend::template create_sink<quill::RotatingFileSink>(
                                config.file_path.string(), file_config);
                    file_sink->set_log_level_filter(quill::LogLevel::TraceL3);
                    sinks.push_back(std::move(file_sink));
                }

                if (sinks.empty()) {
                    auto null_sink = Frontend::template create_sink<quill::NullSink>("AEG.Null");
                    null_sink->set_log_level_filter(quill::LogLevel::TraceL3);
                    sinks.push_back(std::move(null_sink));
                }

                quill::BackendOptions backend_options{};
                backend_options.thread_name = "AEG.LogBackend";
                backend_options.sleep_duration = std::chrono::microseconds{100};
                backend_options.log_timestamp_ordering_grace_period = std::chrono::microseconds{5};
                backend_options.ensure_monotonic_output_timestamps = true;
                backend_options.wait_for_queues_to_empty_before_exit = true;
                backend_options.sink_min_flush_interval = std::chrono::milliseconds{200};
                backend_options.mdc_format_pattern = " [{}: {}, ]";
                backend_options.error_notifier = [&state](const std::string &message) {
                    std::lock_guard lock{state.mutex};
                    state.backend_error = message;
                };

                quill::Backend::start(backend_options);
                state.backend_started = true;

                for (std::size_t index = 0; index < kCategoryCount; ++index) {
                    const Category category = static_cast<Category>(index);
                    std::vector<std::shared_ptr<quill::Sink> > logger_sinks = sinks;
                    Logger *logger = Frontend::create_logger(std::string{category_name(category)},
                                                             std::move(logger_sinks),
                                                             pattern, quill::ClockSourceType::System);
                    logger->set_log_level(to_quill_level(configured_level(config, index)));
                    if (config.immediate_flush) {
                        logger->set_immediate_flush(config.immediate_flush_every_n_messages);
                    }
                    state.loggers[index] = logger;
                }

                state.sinks = std::move(sinks);
                return Status::success();
            } catch (...) {
                for (Logger *&logger: state.loggers) {
                    if (logger != nullptr && state.backend_started) {
                        try {
                            Frontend::remove_logger_blocking(logger);
                        } catch (...) {
                        }
                    }
                    logger = nullptr;
                }
                if (state.backend_started) {
                    try {
                        quill::Backend::stop();
                    } catch (...) {
                    }
                    state.backend_started = false;
                }
                return Status::failure("Failed to initialize logging: " + exception_message());
            }
        }
    } // namespace

    std::string_view category_name(Category category) noexcept {
        switch (category) {
            case Category::kCore:
                return "Core";
            case Category::kRhi:
                return "RHI";
            case Category::kRenderGraph:
                return "RenderGraph";
            case Category::kResource:
                return "Resource";
            case Category::kApplication:
                return "Application";
            case Category::kShader:
                return "Shader";
            case Category::kAsset:
                return "Asset";
            case Category::kTest:
                return "Test";
            case Category::kCount:
                break;
        }
        return "Unknown";
    }

    Status initialize(const Config &config) {
        State &current = state();
        std::unique_lock lock{current.mutex};

        for (;;) {
            switch (current.lifecycle) {
                case Lifecycle::kRunning:
                    return Status::success();
                case Lifecycle::kInitializing:
                    current.condition.wait(lock, [&current] { return current.lifecycle != Lifecycle::kInitializing; });
                    continue;
                case Lifecycle::kShuttingDown:
                    return Status::failure("Logging is shutting down");
                case Lifecycle::kShutdown:
                    return Status::failure(current.error.empty() ? "Logging is already shut down" : current.error);
                case Lifecycle::kUninitialized:
                    current.lifecycle = Lifecycle::kInitializing;
                    current.config = config;
                    break;
            }
            break;
        }

        lock.unlock();
        const Status result = initialize_impl(current, config);
        lock.lock();
        if (result) {
            current.lifecycle = Lifecycle::kRunning;
        } else {
            current.error = result.message;
            current.lifecycle = Lifecycle::kShutdown;
        }
        current.condition.notify_all();
        return result;
    }

    void shutdown() noexcept {
        State &current = state();
        std::array<Logger *, kCategoryCount> loggers{};
        bool backend_started = false;

        {
            std::lock_guard lock{current.mutex};
            if (current.lifecycle != Lifecycle::kRunning) {
                return;
            }
            current.lifecycle = Lifecycle::kShuttingDown;
            loggers = current.loggers;
            backend_started = current.backend_started;
        }

        for (Logger *logger: loggers) {
            if (logger == nullptr) {
                continue;
            }
            try {
                logger->flush_log();
            } catch (...) {
            }
        }

        for (Logger *logger: loggers) {
            if (logger == nullptr) {
                continue;
            }
            try {
                Frontend::remove_logger_blocking(logger);
            } catch (...) {
            }
        }

        if (backend_started) {
            try {
                quill::Backend::stop();
            } catch (...) {
            }
        }

        {
            std::lock_guard lock{current.mutex};
            current.loggers.fill(nullptr);
            current.sinks.clear();
            current.backend_started = false;
            current.lifecycle = Lifecycle::kShutdown;
            current.condition.notify_all();
        }
    }

    void flush() noexcept {
        State &current = state();
        std::array<Logger *, kCategoryCount> loggers{};
        {
            std::lock_guard lock{current.mutex};
            if (current.lifecycle != Lifecycle::kRunning) {
                return;
            }
            loggers = current.loggers;
        }

        for (Logger *logger: loggers) {
            if (logger == nullptr) {
                continue;
            }
            try {
                logger->flush_log();
            } catch (...) {
            }
        }
    }

    bool is_initialized() noexcept {
        State &current = state();
        std::lock_guard lock{current.mutex};
        return current.lifecycle == Lifecycle::kRunning;
    }

    bool set_category_level(Category category, Level level) noexcept {
        const auto index = category_index(category);
        if (!index.has_value() || !is_valid_level(level)) {
            return false;
        }

        if (!is_initialized() && !initialize()) {
            return false;
        }

        State &current = state();
        std::lock_guard lock{current.mutex};
        if (current.lifecycle != Lifecycle::kRunning || current.loggers[*index] == nullptr) {
            return false;
        }

        current.loggers[*index]->set_log_level(to_quill_level(level));
        current.config.category_levels[*index] = level;
        return true;
    }

    Level category_level(Category category) noexcept {
        const auto index = category_index(category);
        if (!index.has_value()) {
            return Level::kOff;
        }

        State &current = state();
        std::lock_guard lock{current.mutex};
        if (current.lifecycle != Lifecycle::kRunning || current.loggers[*index] == nullptr) {
            return Level::kOff;
        }
        return from_quill_level(current.loggers[*index]->get_log_level());
    }

    namespace detail {
        QuillLogger *logger(Category category) noexcept {
            const auto index = category_index(category);
            if (!index.has_value()) {
                return nullptr;
            }

            try {
                if (!is_initialized()) {
                    const Status result = initialize();
                    if (!result) {
                        return nullptr;
                    }
                }

                State &current = state();
                std::lock_guard lock{current.mutex};
                if (current.lifecycle != Lifecycle::kRunning) {
                    return nullptr;
                }
                return current.loggers[*index];
            } catch (...) {
                return nullptr;
            }
        }
    } // namespace detail

    void set_thread_context(std::string_view key, std::string_view value) noexcept {
        if (key.empty()) {
            return;
        }
        if (Logger *logger = detail::logger(Category::kCore); logger != nullptr) {
            try {
                logger->set_mdc(std::string{key}, std::string{value});
            } catch (...) {
            }
        }
    }

    void erase_thread_context(std::string_view key) noexcept {
        if (key.empty()) {
            return;
        }
        if (Logger *logger = detail::logger(Category::kCore); logger != nullptr) {
            try {
                logger->erase_mdc(std::string{key});
            } catch (...) {
            }
        }
    }

    void clear_thread_context() noexcept {
        if (Logger *logger = detail::logger(Category::kCore); logger != nullptr) {
            try {
                logger->clear_mdc();
            } catch (...) {
            }
        }
    }
} // namespace aeg::logging
