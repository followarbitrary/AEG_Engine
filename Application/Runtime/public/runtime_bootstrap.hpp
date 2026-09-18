/**
 * @file runtime_bootstrap.hpp
 * @brief Package-only game startup boundary for static game executables.
 */
#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "game_module.hpp"
#include "result.hpp"

namespace aeg::application::runtime {

struct RuntimeBuildIdentity final {
    std::string build_id{};
    std::string target{};
    std::string configuration{};
};

struct RuntimeBootRequest final {
    std::filesystem::path manifest_path{};
    std::filesystem::path package_path{};
    uint32_t max_frames{};
    std::filesystem::path trace_path{};
    std::filesystem::path readback_path{};
    bool headless{false};
    bool offscreen{false};
    std::string window_title{"AEG Game"};
    uint32_t width{1280};
    uint32_t height{720};
};

using GameConfigurationCallback = foundation::Result (*)(RuntimeBootRequest&);

class RuntimeBootstrap final {
public:
    RuntimeBootstrap();
    ~RuntimeBootstrap();
    RuntimeBootstrap(const RuntimeBootstrap&) = delete;
    RuntimeBootstrap& operator=(const RuntimeBootstrap&) = delete;

    [[nodiscard]] foundation::Result initialize(const RuntimeBootRequest&, module::ModuleDefinition,
                                                const RuntimeBuildIdentity&);
    [[nodiscard]] foundation::Result tick();
    [[nodiscard]] foundation::Result shutdown();
    [[nodiscard]] uint32_t frame_count() const noexcept;
    /** @brief Number of startup Pipeline assets decoded and prepared from the package. */
    [[nodiscard]] std::size_t startup_pipeline_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_{};
};

/** @brief Applies project configuration, boots a static module, and runs bounded frames. */
[[nodiscard]] int run_game(int argc, char** argv, module::ModuleDefinition definition, const RuntimeBuildIdentity&,
                           GameConfigurationCallback configure = nullptr);

}  // namespace aeg::application::runtime
