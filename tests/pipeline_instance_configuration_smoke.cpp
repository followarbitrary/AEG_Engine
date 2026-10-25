/** @brief Retired instance authoring stays absent; technical hosts retain distinct owning plans. */
#include <iostream>
#include <memory>

#include "pipeline.hpp"
#include "../Engine/Pipeline/private/pipeline_scope_host_execution.hpp"

template <class Host>
constexpr bool exposes_configuration = requires(Host& host) { host.configuration(); } ||
    requires { &Host::set_configuration; };
template <class Input>
constexpr bool exposes_configuration_input = requires(Input input) { input.configuration; };
static_assert(!exposes_configuration<aeg::pipeline::PipelineScopeHost>);
static_assert(!exposes_configuration_input<aeg::pipeline::PipelineCompileInput>);
#if __has_include("pipeline_configuration.hpp")
#error Retired Pipeline instance authoring header must be physically absent
#endif

int main() {
    using namespace aeg::pipeline;
    PipelineBuilder builder({.id = PipelineId("Fixed.Technical"), .name = "Fixed technical topology",
        .supported_scopes = to_mask(PipelineScope::kFrame)});
    if (!builder.add_stage({StageId("Fixed.Stage"), EnginePhase::kRenderBuild})) return 1;
    auto definition = builder.finish();
    PipelineRegistry registry;
    if (!definition || !registry.register_pipeline(std::move(definition.value))) return 2;
    const auto registered = registry.find_definition(PipelineId("Fixed.Technical"));
    PipelineScopeHost left(PipelineScope::kFrame, PipelineInstanceId("View.Left"));
    PipelineScopeHost right(PipelineScope::kFrame, PipelineInstanceId("View.Right"));
    if (!registered || !left.bind_pipeline(PipelineId("Fixed.Technical")) || !left.rebuild(registry) ||
        !right.bind_pipeline(PipelineId("Fixed.Technical")) || !right.rebuild(registry) ||
        detail::PipelineScopeHostExecution::has_pending_plan(left) ||
        detail::PipelineScopeHostExecution::has_pending_plan(right)) return 3;
    const auto left_plan = detail::PipelineScopeHostExecution::active_plan_snapshot(left);
    const auto right_plan = detail::PipelineScopeHostExecution::active_plan_snapshot(right);
    if (!left_plan || !right_plan || left_plan == right_plan || left.instance_id() == right.instance_id() ||
        left_plan->instance != left.instance_id() || right_plan->instance != right.instance_id() ||
        left_plan->definition_owner != registered.value || right_plan->definition_owner != registered.value ||
        left_plan->nodes.size() != 1 || right_plan->nodes.size() != 1) return 4;
    if (!registry.unregister_pipeline(PipelineId("Fixed.Technical")) ||
        left.rebuild(registry) || right.rebuild(registry) ||
        detail::PipelineScopeHostExecution::active_plan_snapshot(left) != left_plan ||
        detail::PipelineScopeHostExecution::active_plan_snapshot(right) != right_plan) return 5;
    std::cout << "Retired instance authoring absent; two technical hosts retain independent plans and definition owners\n";
    return 0;
}
