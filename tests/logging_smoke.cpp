#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <logging.hpp>
#include <string>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {
    [[nodiscard]] bool contains(const std::string &text, const char *marker) {
        return text.find(marker) != std::string::npos;
    }
} // namespace

int main() {
    const auto process_id =
#if defined(_WIN32)
            static_cast<unsigned long long>(_getpid());
#else
    static_cast<unsigned long long>(getpid());
#endif
    const std::filesystem::path test_directory =
            std::filesystem::temp_directory_path() / ("aeg-engine-logging-smoke-" + std::to_string(process_id));
    const std::filesystem::path log_path = test_directory / "logging.log";

    std::error_code error;
    std::filesystem::remove_all(test_directory, error);

    aeg::logging::Config config{};
    config.default_level = aeg::logging::Level::kTrace;
    config.category_levels[static_cast<std::size_t>(aeg::logging::Category::kTest)] = aeg::logging::Level::kDebug;
    config.console_enabled = false;
    config.file_enabled = true;
    config.file_path = log_path;
    config.file_rotation_bytes = 4096;
    config.file_max_backup_files = 2;
    config.rotate_file_on_startup = true;
    config.immediate_flush = true;

    const aeg::logging::Status status = aeg::logging::initialize(config);
    if (!status) {
        return 1;
    }

    AEG_LOG_TRACE(aeg::logging::Category::kTest, "AEG_LOGGING_SMOKE_TRACE");
    AEG_LOG_DEBUG(aeg::logging::Category::kTest, "AEG_LOGGING_SMOKE_DEBUG");
    AEG_LOG_INFO(aeg::logging::Category::kTest, "AEG_LOGGING_SMOKE_INFO");
    AEG_LOG_NOTICE(aeg::logging::Category::kTest, "AEG_LOGGING_SMOKE_NOTICE");
    AEG_LOG_WARNING(aeg::logging::Category::kTest, "AEG_LOGGING_SMOKE_WARNING");

    if (!aeg::logging::set_category_level(aeg::logging::Category::kTest, aeg::logging::Level::kWarning) ||
        aeg::logging::category_level(aeg::logging::Category::kTest) != aeg::logging::Level::kWarning) {
        aeg::logging::shutdown();
        return 2;
    }

    aeg::logging::set_thread_context("test_id", "42");
    AEG_LOG_INFO(aeg::logging::Category::kTest, "AEG_LOGGING_SMOKE_FILTERED");
    AEG_LOG_WARNING(aeg::logging::Category::kTest, "AEG_LOGGING_SMOKE_VISIBLE_WARNING");
    AEG_LOG_ERROR(aeg::logging::Category::kTest, "AEG_LOGGING_SMOKE_VISIBLE_ERROR");
    AEG_LOG_CRITICAL(aeg::logging::Category::kTest, "AEG_LOGGING_SMOKE_VISIBLE_CRITICAL");
    aeg::logging::flush();
    aeg::logging::clear_thread_context();
    aeg::logging::shutdown();

    std::ifstream log_file{log_path, std::ios::binary};
    if (!log_file) {
        return 3;
    }
    const std::string content{std::istreambuf_iterator<char>{log_file}, std::istreambuf_iterator<char>{}};

#if QUILL_COMPILE_ACTIVE_LOG_LEVEL <= QUILL_COMPILE_ACTIVE_LOG_LEVEL_WARNING
    if (!contains(content, "AEG_LOGGING_SMOKE_VISIBLE_WARNING")) {
        return 4;
    }
#endif
#if QUILL_COMPILE_ACTIVE_LOG_LEVEL <= QUILL_COMPILE_ACTIVE_LOG_LEVEL_ERROR
    if (!contains(content, "AEG_LOGGING_SMOKE_VISIBLE_ERROR")) {
        return 5;
    }
#endif
#if QUILL_COMPILE_ACTIVE_LOG_LEVEL <= QUILL_COMPILE_ACTIVE_LOG_LEVEL_CRITICAL
    if (!contains(content, "AEG_LOGGING_SMOKE_VISIBLE_CRITICAL")) {
        return 6;
    }
#endif

#if QUILL_COMPILE_ACTIVE_LOG_LEVEL <= QUILL_COMPILE_ACTIVE_LOG_LEVEL_INFO
    if (contains(content, "AEG_LOGGING_SMOKE_FILTERED")) {
        return 7;
    }
#endif

#if QUILL_COMPILE_ACTIVE_LOG_LEVEL <= QUILL_COMPILE_ACTIVE_LOG_LEVEL_CRITICAL
    if (!contains(content, "Test") || !contains(content, "test_id: 42")) {
        return 8;
    }
#endif

    std::filesystem::remove_all(test_directory, error);
    return 0;
}
