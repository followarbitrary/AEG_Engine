#pragma once
#include <memory>
#include <string>
#include <vector>

#include "pipeline_authoring.hpp"

namespace aeg::engine {
struct PipelineHostBinding final {
    std::string name{};
    pipeline::PipelineScopeHost* host{};
};
class PipelineCompositionRuntime final {
public:
    [[nodiscard]] foundation::Result stage(const pipeline::PreparedPipelineDocument& prepared,
                                           std::span<const PipelineHostBinding> bindings);
    [[nodiscard]] foundation::Result apply_pending(bool at_safe_boundary = true);
    /** Deactivates active hosts and retires the current composition at a safe boundary. */
    [[nodiscard]] foundation::Result stop_and_retire(const pipeline::PipelineDeactivationContext& context = {});
    [[nodiscard]] bool has_pending() const noexcept { return pending_; }
    [[nodiscard]] std::size_t active_host_count() const noexcept { return active_.size(); }
    [[nodiscard]] std::size_t active_configuration_count() const noexcept { return active_configurations_.size(); }

private:
    std::vector<PipelineHostBinding> active_{};
    std::vector<PipelineHostBinding> pending_hosts_{};
    std::vector<std::shared_ptr<const pipeline::PipelineInstanceConfiguration>> active_configurations_{};
    std::vector<std::shared_ptr<const pipeline::PipelineInstanceConfiguration>> pending_configurations_{};
    bool pending_{false};
};
}  // namespace aeg::engine
