/**
 * @file feature_clock_admission_smoke.cpp
 * @brief Host approval, automatic workload and Module rollback acceptance.
 */
#include <exception>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <new>
#include <span>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <DbgHelp.h>
#include <crtdbg.h>
#include <stdlib.h>
#endif

#include <feature_registry.hpp>
#include <module.hpp>

#include "../Engine/Feature/private/feature_clock_admission.hpp"

namespace clock_allocation_fault {
thread_local int remaining = -1;
thread_local bool injected = false;
thread_local bool inspecting_allocation = false;
thread_local bool proxy_probe = false;
thread_local bool proxy_probe_observed = false;
thread_local unsigned ignored_debug_proxies = 0;
bool debug_symbols_ready = false;

bool is_debug_proxy_allocation() noexcept {
#if defined(_WIN32) && defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL != 0
    if (!debug_symbols_ready || inspecting_allocation) return false;
    // DbgHelp may allocate internally. Those diagnostic allocations must not
    // recursively inspect themselves or consume an engine allocation index.
    inspecting_allocation = true;
    void* frames[24]{};
    const auto count = CaptureStackBackTrace(0, 24, frames, nullptr);
    alignas(SYMBOL_INFO) unsigned char storage[sizeof(SYMBOL_INFO) + 1024]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 1024;
    bool is_proxy = false;
    for (unsigned index = 0; index < count; ++index) {
        DWORD64 displacement = 0;
        if (!SymFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(frames[index]),
                         &displacement, symbol)) continue;
        // Match the allocator's type, never its allocation size: real Clock
        // binding/ownership payloads can also be 16 bytes in this Debug build.
        if (std::strstr(symbol->Name, "std::allocator<struct std::_Container_proxy>::allocate") != nullptr ||
            std::strstr(symbol->Name, "std::allocator<std::_Container_proxy>::allocate") != nullptr) {
            is_proxy = true;
            break;
        }
    }
    inspecting_allocation = false;
    return is_proxy;
#else
    return false;
#endif
}

bool initialize_debug_proxy_filter() {
#if defined(_WIN32) && defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL != 0
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
    if (!SymInitialize(GetCurrentProcess(), nullptr, TRUE)) {
        std::fprintf(stderr, "Clock allocation symbol initialization failed error=%lu\n", GetLastError());
        std::fflush(stderr);
        return false;
    }
    debug_symbols_ready = true;
    // Resolve the exact Debug allocator before any injection. A missing PDB or
    // changed symbol spelling is a fixture error, never permission to skip OOM.
    proxy_probe = true;
    std::allocator<std::_Container_proxy> allocator;
    auto* proxy = allocator.allocate(1);
    allocator.deallocate(proxy, 1);
    proxy_probe = false;
    std::fprintf(stderr, "Clock allocation Debug proxy filter ready=%d\n", static_cast<int>(proxy_probe_observed));
    std::fflush(stderr);
    return proxy_probe_observed;
#else
    return true;
#endif
}

bool fail_now() noexcept {
    if (remaining < 0) return false;
    if (remaining == 0) {
        remaining = -1; // Only one allocation fails; reporting/rollback can proceed.
        injected = true;
        return true;
    }
    --remaining;
    return false;
}

bool should_fail() noexcept {
    if (inspecting_allocation) return false;
    if ((remaining >= 0 || proxy_probe) && is_debug_proxy_allocation()) {
        ++ignored_debug_proxies;
        if (proxy_probe) proxy_probe_observed = true;
        return false;
    }
    return fail_now();
}

void report_injection(std::size_t size) noexcept {
    // fail_now has already disarmed injection. Keep the evidence path to fixed
    // buffers and C I/O; preserve raw frames for independent offline diagnosis.
    std::fprintf(stderr, "Clock allocation injected size=%zu\n", size);
#if defined(_WIN32)
    void* frames[24]{};
    const auto count = CaptureStackBackTrace(0, 24, frames, nullptr);
    const auto module = GetModuleHandleW(nullptr);
    std::fprintf(stderr, "Clock allocation executable-base=%p frames=%u\n",
                 static_cast<void*>(module), static_cast<unsigned>(count));
    for (unsigned index = 0; index < count; ++index) {
        std::fprintf(stderr, "Clock allocation frame[%u]=%p\n", index, frames[index]);
    }
#endif
    std::fflush(stderr);
}

struct Scope final {
    Scope(int index, bool& observed) : observed_(observed), index_(index) {
        // Diagnostics happen before arming so they cannot consume a fault slot.
        std::fprintf(stderr, "Clock allocation scope begin index=%d uncaught=%d\n", index_, std::uncaught_exceptions());
        std::fflush(stderr);
        remaining = index;
        injected = false;
        ignored_debug_proxies = 0;
    }
    ~Scope() {
        const auto remaining_at_exit = remaining;
        observed_ = injected;
        remaining = -1;
        // Disarm before reporting, including when unwinding a thrown exception.
        std::fprintf(stderr, "Clock allocation scope end index=%d injected=%d remaining=%d uncaught=%d ignored-debug-proxies=%u\n",
                     index_, static_cast<int>(observed_), remaining_at_exit, std::uncaught_exceptions(), ignored_debug_proxies);
        std::fflush(stderr);
    }
    bool& observed_;
    int index_;
};
}  // namespace clock_allocation_fault

void* operator new(std::size_t size) {
    if (clock_allocation_fault::should_fail()) {
        clock_allocation_fault::report_injection(size);
        throw std::bad_alloc{};
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
using namespace aeg::feature;
namespace admission = aeg::feature::detail;
using aeg::foundation::Result;

template <typename Value>
bool require(const Value& value, const char* message) {
    const bool passed = static_cast<bool>(value);
    if (!passed) std::cerr << message << '\n';
    return passed;
}

FeatureDefinition definition(FeatureDefinitionId id, ClockId clock,
                             ExecutionKind execution = ExecutionKind::kCall,
                             uint64_t budget = 250'000, uint32_t frequency = 60,
                             uint64_t deadline = 20'000'000,
                             FeatureDomain domain = FeatureDomain::kLogic) {
    FeatureDefinition result{};
    result.id = id;
    result.version = {1, 0, 0};
    result.contract.definition_id = id;
    result.contract.version = result.version;
    result.contract.domains = to_mask(domain);
    result.contract.execution = execution;
    result.contract.state = execution == ExecutionKind::kCall
        ? StatePolicy::kStateless : StatePolicy::kInstanceState;
    result.contract.scope = {ScopeId("World")};
    result.contract.clock = {clock, frequency, 2, LateDataPolicy::kQueue};
    result.contract.clock.capability_version = {1, 0, 0};
    result.contract.clock.timing = ClockTiming{0, deadline, DeadlinePolicy::kObserve};
    result.contract.clock.work_budget_ns = budget;
    result.contract.permissions.version = 1;
    return result;
}

bool admit_and_register(FeatureRegistry& registry, const FeatureDefinition& candidate) {
    const auto approved = admission::admit_clock_request(registry, candidate.contract);
    if (!require(approved, "valid host Clock request was rejected")) return false;
    return require(registry.register_definition(candidate), "approved Feature registration failed");
}

admission::ClockWorkDemand demand(uint64_t identity, const FeatureDefinition& candidate) {
    admission::ClockWorkDemand result{};
    result.identity = identity;
    result.contract = candidate.contract;
    result.owner_path.nodes = {FeatureNodeId(identity)};
    return result;
}

bool test_host_context_and_explicit_approval_are_required() {
    const auto candidate = definition(FeatureDefinitionId("Admission.Direct"), ClockId("Admission.Unknown"));
    FeatureRegistry without_host;
    bool passed = require(!without_host.register_definition(candidate),
                          "no-context Registry approved a Clock definition");
    passed &= require(!admission::admit_clock_request(without_host, candidate.contract),
                      "admission succeeded without a host context");
    const auto host = admission::make_clock_host_context(admission::ClockHostLimits{});
    if (!require(host, "normal Clock host context creation failed")) return false;
    FeatureRegistry registry(host.value);
    const auto original_digest = registry.contract_digest();
    passed &= require(!registry.register_definition(candidate) &&
                      !registry.find_definition(candidate.id) &&
                      registry.contract_digest() == original_digest,
                      "unapproved Clock registration changed the Registry");
    const auto approved = admission::admit_clock_request(registry, candidate.contract);
    if (!require(approved, "explicit host approval failed")) return false;
    passed &= require(approved.value.inserted, "first approval did not report a new binding");
    passed &= require(registry.register_definition(candidate), "host-approved definition was rejected");
    const auto reused = admission::admit_clock_request(registry, candidate.contract);
    passed &= require(reused && !reused.value.inserted, "repeat approval did not reuse the host binding");
    return passed;
}

bool test_host_limits_and_binding_capacity() {
    admission::ClockHostLimits limits{};
    limits.allowed_domains = to_mask(FeatureDomain::kLogic);
    limits.max_frequency_hz = 1000;
    limits.max_backlog = 4;
    limits.max_bindings = 1;
    limits.max_relative_deadline_ns = 1'000'000'000;
    limits.max_plan_budget_ns = 1'000'000;
    const auto host = admission::make_clock_host_context(limits);
    if (!require(host, "bounded Clock host context creation failed")) return false;
    FeatureRegistry registry(host.value);
    const auto baseline = definition(FeatureDefinitionId("Limits.Valid"), ClockId("Limits.Clock"));
    const struct {
        const char* name;
        void (*mutate)(FeatureContract&);
    } cases[]{
        {"unsupported capability version was approved", [](FeatureContract& c) { c.clock.capability_version = {2, 0, 0}; }},
        {"disallowed domain was approved", [](FeatureContract& c) { c.domains = to_mask(FeatureDomain::kAudio); }},
        {"over-limit frequency was approved", [](FeatureContract& c) { c.clock.frequency_hz = 1001; }},
        {"over-limit backlog was approved", [](FeatureContract& c) { c.clock.max_backlog = 5; }},
        {"over-limit relative deadline was approved", [](FeatureContract& c) { c.clock.timing->relative_deadline_ns = 1'000'000'001; }},
        {"over-limit automatic work budget was approved", [](FeatureContract& c) {
            c.execution = ExecutionKind::kProcess;
            c.state = StatePolicy::kInstanceState;
            c.clock.work_budget_ns = 1'000'001;
        }},
    };
    bool passed = true;
    for (const auto& test : cases) {
        auto candidate = baseline;
        test.mutate(candidate.contract);
        passed &= require(!admission::admit_clock_request(registry, candidate.contract), test.name);
    }
    // Rejected requests must not consume the sole binding slot.
    if (!admit_and_register(registry, baseline)) return false;
    const auto another = definition(FeatureDefinitionId("Limits.Another"), ClockId("Limits.AnotherClock"));
    passed &= require(!admission::admit_clock_request(registry, another.contract),
                      "Clock binding capacity was exceeded");
    auto reused = baseline;
    reused.id = reused.contract.definition_id = FeatureDefinitionId("Limits.SameClockDifferentBudget");
    ++reused.contract.clock.work_budget_ns;
    passed &= require(admission::admit_clock_request(registry, reused.contract),
                      "existing Clock binding incorrectly consumed another capacity slot");
    passed &= require(registry.register_definition(reused),
                      "different leaf budgets were incorrectly treated as a shared schedule conflict");
    auto explicit_call = baseline;
    explicit_call.id = explicit_call.contract.definition_id = FeatureDefinitionId("Limits.ExplicitCall");
    explicit_call.contract.clock.work_budget_ns = 2'000'000;
    passed &= require(admission::admit_clock_request(registry, explicit_call.contract) &&
                      registry.register_definition(explicit_call),
                      "explicit Call single budget was incorrectly capped by the automatic plan budget");
    auto automatic = explicit_call;
    automatic.id = automatic.contract.definition_id = FeatureDefinitionId("Limits.AutomaticProcess");
    automatic.contract.execution = ExecutionKind::kProcess;
    automatic.contract.state = StatePolicy::kInstanceState;
    passed &= require(!admission::admit_clock_request(registry, automatic.contract),
                      "automatic Process escaped the same host plan budget cap");
    return passed;
}

bool test_invalid_host_limits_are_not_unbounded_defaults() {
    const struct {
        const char* name;
        void (*mutate)(admission::ClockHostLimits&);
    } cases[]{
        {"missing host capability version", [](admission::ClockHostLimits& l) { l.capability_version = {}; }},
        {"empty host domain mask", [](admission::ClockHostLimits& l) { l.allowed_domains = 0; }},
        {"zero host frequency limit", [](admission::ClockHostLimits& l) { l.max_frequency_hz = 0; }},
        {"zero host backlog limit", [](admission::ClockHostLimits& l) { l.max_backlog = 0; }},
        {"zero host binding limit", [](admission::ClockHostLimits& l) { l.max_bindings = 0; }},
        {"zero host deadline limit", [](admission::ClockHostLimits& l) { l.max_relative_deadline_ns = 0; }},
        {"zero host plan budget", [](admission::ClockHostLimits& l) { l.max_plan_budget_ns = 0; }},
        {"zero host domain utilization budget", [](admission::ClockHostLimits& l) { l.max_domain_automatic_budget_per_second_ns = 0; }},
    };
    bool passed = true;
    for (const auto& test : cases) {
        admission::ClockHostLimits limits{};
        test.mutate(limits);
        passed &= require(!admission::make_clock_host_context(limits), test.name);
    }
    return passed;
}

bool test_native_unknown_enums_cannot_bypass_host_budget() {
    const auto host = admission::make_clock_host_context(admission::ClockHostLimits{});
    if (!require(host, "native enum host creation failed")) return false;
    bool passed = true;
    for (const bool invalid_structure : {true, false}) {
        FeatureRegistry registry(host.value);
        auto candidate = definition(FeatureDefinitionId("Native.InvalidEnum"), ClockId("Native.InvalidClock"));
        if (invalid_structure) {
            candidate.contract.structure = static_cast<FeatureStructure>(255);
            candidate.contract.execution = ExecutionKind::kProcess;
            candidate.contract.state = StatePolicy::kInstanceState;
            candidate.contract.clock.work_budget_ns = 0;
        } else {
            candidate.contract.execution = static_cast<ExecutionKind>(255);
        }
        const auto approved = admission::admit_clock_request(registry, candidate.contract);
        passed &= require(!approved && approved.result.code == aeg::foundation::ResultCode::kInvalidArgument,
                          invalid_structure ? "host approved unknown Structure and zero Process budget"
                                            : "host approved unknown Execution outside automatic workload accounting");
        const auto registered = registry.register_definition(candidate);
        passed &= require(!registered && registered.code == aeg::foundation::ResultCode::kInvalidArgument &&
                          !registry.find_definition(candidate.id),
                          "Registry accepted a native unknown enum after host admission");
    }
    return passed;
}

bool test_shared_schedule_fields_and_replacement() {
    const auto host = admission::make_clock_host_context(admission::ClockHostLimits{});
    if (!require(host, "shared-schedule host creation failed")) return false;
    FeatureRegistry registry(host.value);
    const auto original = definition(FeatureDefinitionId("Schedule.Original"), ClockId("Schedule.Shared"));
    auto peer = original;
    peer.id = peer.contract.definition_id = FeatureDefinitionId("Schedule.Peer");
    peer.contract.clock.work_budget_ns += 1;
    if (!admit_and_register(registry, original) || !admit_and_register(registry, peer)) return false;
    const auto original_digest = registry.contract_digest();
    const auto original_definition_digest = feature_contract_digest(original);
    const struct {
        const char* name;
        void (*mutate)(ClockContract&);
    } cases[]{
        {"capability version", [](ClockContract& c) { ++c.capability_version.minor; }},
        {"frequency", [](ClockContract& c) { ++c.frequency_hz; }},
        {"phase", [](ClockContract& c) { ++c.timing->phase_ns; }},
        {"relative deadline", [](ClockContract& c) { ++c.timing->relative_deadline_ns; }},
        {"deadline policy", [](ClockContract& c) { c.timing->deadline_policy = DeadlinePolicy::kRejectCommit; }},
        {"backlog", [](ClockContract& c) { ++c.max_backlog; }},
        {"late data policy", [](ClockContract& c) { c.late_data_policy = LateDataPolicy::kDrop; }},
    };
    bool passed = true;
    for (const auto& test : cases) {
        auto candidate = original;
        candidate.id = candidate.contract.definition_id = FeatureDefinitionId("Schedule.Conflict");
        test.mutate(candidate.contract.clock);
        if (registry.register_definition(candidate)) {
            std::cerr << "shared Clock accepted conflicting " << test.name << '\n';
            passed = false;
        }
        candidate.id = candidate.contract.definition_id = original.id;
        if (registry.replace_definition(candidate)) {
            std::cerr << "replacement accepted conflicting " << test.name << '\n';
            passed = false;
        }
        const auto* retained = registry.find_definition(original.id);
        passed &= require(retained && feature_contract_digest(*retained) == original_definition_digest &&
                          registry.contract_digest() == original_digest,
                          "failed schedule registration/replacement changed last-good definitions");
    }
    return passed;
}

bool test_actual_leaf_workload_and_call_classification() {
    const auto host = admission::make_clock_host_context(admission::ClockHostLimits{});
    if (!require(host, "workload host creation failed")) return false;
    FeatureRegistry registry(host.value);
    const auto process = definition(FeatureDefinitionId("Work.Process"), ClockId("Work.Automatic"),
        ExecutionKind::kProcess, 400'000'000, 1, 1'000'000'000);
    auto continuous = process;
    continuous.id = continuous.contract.definition_id = FeatureDefinitionId("Work.Continuous");
    continuous.contract.execution = ExecutionKind::kContinuous;
    const auto call = definition(FeatureDefinitionId("Work.Call"), ClockId("Work.Explicit"),
        ExecutionKind::kCall, 600'000'000, 60, 1'000'000'000);
    auto composite = process;
    composite.id = composite.contract.definition_id = FeatureDefinitionId("Work.Composite");
    composite.contract.structure = FeatureStructure::kComposite;
    composite.contract.execution = ExecutionKind::kCall;
    composite.contract.state = StatePolicy::kStateless;
    composite.contract.children = {process.id};
    composite.contract.clock.work_budget_ns = 0;
    if (!admit_and_register(registry, process) || !admit_and_register(registry, continuous) ||
        !admit_and_register(registry, call) || !admit_and_register(registry, composite)) return false;

    std::vector<admission::ClockWorkDemand> automatic{demand(1, process), demand(2, process), demand(3, composite)};
    bool passed = require(admission::validate_clock_workload(automatic, registry),
                          "two repeated-definition owners within budget were rejected");
    automatic.push_back(demand(4, continuous));
    passed &= require(!admission::validate_clock_workload(automatic, registry),
                      "Process/Continuous actual owners exceeding Clock budget were accepted");
    std::vector<admission::ClockWorkDemand> explicit_calls{
        demand(10, call), demand(11, call), demand(12, call), demand(13, call)};
    passed &= require(admission::validate_clock_workload(explicit_calls, registry),
                      "explicit Call owners were charged as periodic work or a combined automatic plan");
    auto unknown = demand(20, process);
    unknown.contract.clock.id = ClockId("Work.Unapproved");
    const std::vector<admission::ClockWorkDemand> unapproved{unknown};
    passed &= require(!admission::validate_clock_workload(unapproved, registry),
                      "workload validation bypassed Clock binding approval");
    return passed;
}

bool test_minimum_deadline_and_cross_clock_domain_budget() {
    const auto host = admission::make_clock_host_context(admission::ClockHostLimits{});
    if (!require(host, "domain workload host creation failed")) return false;
    FeatureRegistry registry(host.value);
    const auto short_deadline = definition(FeatureDefinitionId("Work.Short"), ClockId("Work.ShortClock"),
        ExecutionKind::kProcess, 300'000'000, 1, 500'000'000);
    const auto logic_a = definition(FeatureDefinitionId("Work.LogicA"), ClockId("Work.ClockA"),
        ExecutionKind::kProcess, 600'000'000, 1, 1'000'000'000);
    const auto logic_b = definition(FeatureDefinitionId("Work.LogicB"), ClockId("Work.ClockB"),
        ExecutionKind::kProcess, 600'000'000, 1, 1'000'000'000);
    auto audio_b = logic_b;
    audio_b.id = audio_b.contract.definition_id = FeatureDefinitionId("Work.AudioB");
    audio_b.contract.domains = to_mask(FeatureDomain::kAudio);
    const auto rate_limited = definition(FeatureDefinitionId("Work.TenHz"), ClockId("Work.TenHzClock"),
        ExecutionKind::kProcess, 100'000'000, 10, 1'000'000'000);
    if (!admit_and_register(registry, short_deadline) || !admit_and_register(registry, logic_a) ||
        !admit_and_register(registry, logic_b) || !admit_and_register(registry, audio_b) ||
        !admit_and_register(registry, rate_limited)) return false;
    const std::vector<admission::ClockWorkDemand> deadline_overload{
        demand(1, short_deadline), demand(2, short_deadline)};
    bool passed = require(!admission::validate_clock_workload(deadline_overload, registry),
                          "automatic plan budget exceeded the Clock's minimum relative deadline");
    const std::vector<admission::ClockWorkDemand> same_domain{demand(3, logic_a), demand(4, logic_b)};
    passed &= require(!admission::validate_clock_workload(same_domain, registry),
                      "same-domain automatic utilization across Clocks exceeded one second per second");
    const std::vector<admission::ClockWorkDemand> different_domains{demand(3, logic_a), demand(4, audio_b)};
    passed &= require(admission::validate_clock_workload(different_domains, registry),
                      "independent domains were incorrectly combined into one utilization budget");
    std::vector<admission::ClockWorkDemand> rate_demand{demand(5, rate_limited)};
    passed &= require(admission::validate_clock_workload(rate_demand, registry),
                      "automatic utilization equal to one second per second was rejected");
    rate_demand.push_back(demand(6, rate_limited));
    passed &= require(!admission::validate_clock_workload(rate_demand, registry),
                      "automatic utilization ignored the Clock frequency multiplier");
    return passed;
}

bool test_tooling_background_requires_specific_host_approval() {
    const auto stage = definition(FeatureDefinitionId("Tooling.Stage"),
        ClockId("Engine.Tooling.ProjectOperationClock"), ExecutionKind::kProcess,
        300'000'000'000ull, 60, 1'800'000'000'000ull);
    const auto ordinary_host = admission::make_clock_host_context(admission::ClockHostLimits{});
    if (!require(ordinary_host, "ordinary Tooling-negative host creation failed")) return false;
    FeatureRegistry ordinary(ordinary_host.value);
    bool passed = require(!admission::admit_clock_request(ordinary, stage.contract),
                          "ordinary host implicitly approved the Tooling background Clock");
    admission::ClockHostLimits limits{};
    limits.allow_tooling_background = true;
    const auto host = admission::make_clock_host_context(limits);
    if (!require(host, "explicit Tooling host creation failed")) return false;
    FeatureRegistry registry(host.value);
    if (!admit_and_register(registry, stage)) return false;
    std::vector<admission::ClockWorkDemand> stages;
    for (uint64_t identity = 1; identity != 7; ++identity) stages.push_back(demand(identity, stage));
    passed &= require(admission::validate_clock_workload(stages, registry),
                      "approved background plan did not exempt only periodic utilization");
    stages.push_back(demand(7, stage));
    passed &= require(!admission::validate_clock_workload(stages, registry),
                      "background Clock escaped its finite 1800-second plan budget/deadline");
    auto rejected_policy = stage;
    rejected_policy.contract.clock.timing->deadline_policy = DeadlinePolicy::kRejectCommit;
    passed &= require(!admission::admit_clock_request(registry, rejected_policy.contract),
                      "Tooling background host accepted a non-Observe policy");
    auto excessive_rate = stage;
    excessive_rate.contract.clock.frequency_hz = 192001;
    passed &= require(!admission::admit_clock_request(registry, excessive_rate.contract),
                      "background Clock escaped the ordinary frequency limit");
    auto unrelated = definition(FeatureDefinitionId("Tooling.Unrelated"), ClockId("Tooling.NotThePrivateClock"));
    unrelated.contract.clock.timing->relative_deadline_ns = 600'000'000'001ull;
    passed &= require(!admission::admit_clock_request(registry, unrelated.contract),
                      "Tooling background opt-in exempted an unrelated Clock's deadline");
    return passed;
}

struct ModuleProbe final {
    bool first_registered{false};
    bool second_attempted{false};
    unsigned unloads{0};
    bool allocation_injected{false};
};

class ClockModule final : public aeg::module::Module {
public:
    ClockModule(aeg::module::ModuleId id, FeatureDefinition first,
                aeg::reflection::TypeId type, ModuleProbe& probe,
                FeatureDefinition collision = {}, bool throw_after_first = false, int allocation_fault_index = -1)
        : descriptor_{id, "Clock admission fixture", {1, 0, 0}, {}},
          first_(std::move(first)), collision_(std::move(collision)), type_(type), probe_(probe),
          throw_after_first_(throw_after_first), allocation_fault_index_(allocation_fault_index) {}

    const aeg::module::ModuleDescriptor& descriptor() const noexcept override { return descriptor_; }
    Result register_module(aeg::module::ModuleRegistrationContext& context) override {
        if (type_.is_valid()) {
            const auto reflected = context.register_type({type_, "ClockFixture", 1,
                aeg::reflection::TypeKind::kStruct, sizeof(uint32_t), alignof(uint32_t), {}});
            if (!reflected) return reflected;
        }
        first_.owner_module = context.module_id();
        const auto first_result = [&] {
            if (allocation_fault_index_ < 0) return context.register_feature(first_);
            clock_allocation_fault::Scope fault(allocation_fault_index_, probe_.allocation_injected);
            return context.register_feature(first_);
        }();
        if (!first_result) return first_result;
        probe_.first_registered = true;
        if (throw_after_first_) throw std::bad_alloc{};
        if (collision_.id.is_valid()) {
            probe_.second_attempted = true;
            collision_.owner_module = context.module_id();
            return context.register_feature(collision_);
        }
        return Result::success();
    }
    Result unregister_module() override { ++probe_.unloads; return Result::success(); }

private:
    aeg::module::ModuleDescriptor descriptor_{};
    FeatureDefinition first_{};
    FeatureDefinition collision_{};
    aeg::reflection::TypeId type_{};
    ModuleProbe& probe_;
    bool throw_after_first_{false};
    int allocation_fault_index_{-1};
};

bool test_internal_module_allocation_failures_leave_no_new_authority() {
    unsigned injected_failures = 0;
    bool reached_success = false;
    bool passed = true;
    for (int index = 0; index != 128; ++index) {
        std::fprintf(stderr, "Clock admission fault-index=%d begin\n", index);
        std::fflush(stderr);
        admission::ClockHostLimits limits{};
        limits.max_bindings = 2;
        const auto host = admission::make_clock_host_context(limits);
        if (!require(host, "allocation sweep host creation failed")) return false;
        FeatureRegistry registry(host.value);
        const auto retained = definition(FeatureDefinitionId("Allocation.Retained"), ClockId("Allocation.OldClock"));
        const auto transient = definition(FeatureDefinitionId("Allocation.Transient"), ClockId("Allocation.NewClock"));
        const auto module = aeg::module::ModuleId("Allocation.Module");
        if (!admit_and_register(registry, retained)) return false;
        const auto old_digest = registry.contract_digest();
        ModuleProbe probe{};
        aeg::module::ModuleRegistry modules(registry);
        const auto registered = modules.register_module([&] {
            return std::make_unique<ClockModule>(module, transient, aeg::reflection::TypeId{}, probe,
                                                  FeatureDefinition{}, false, index);
        });
        std::fprintf(stderr, "Clock admission fault-index=%d registration-return code=%d injected=%d first-registered=%d\n",
                     index, static_cast<int>(registered.code), static_cast<int>(probe.allocation_injected),
                     static_cast<int>(probe.first_registered));
        std::fflush(stderr);
        if (!probe.allocation_injected) {
            reached_success = true;
            passed &= require(registered && probe.first_registered && modules.contains(module),
                              "allocation sweep did not reach a successful no-fault registration");
            std::fprintf(stderr, "Clock admission fault-index=%d end no-fault-success=%d\n", index, static_cast<int>(passed));
            std::fflush(stderr);
            break;
        }
        ++injected_failures;
        passed &= require(!registered && !modules.contains(module) && !registry.find_definition(transient.id) &&
                          registry.find_definition(retained.id) && registry.contract_digest() == old_digest,
                          "internal Module allocation failure published partial ownership or changed last-good state");
        passed &= require(!registry.register_definition(transient),
                          "internal Module allocation failure leaked new Clock approval");
        const auto capacity_probe = definition(FeatureDefinitionId("Allocation.Probe"), ClockId("Allocation.ProbeClock"));
        passed &= require(admission::admit_clock_request(registry, capacity_probe.contract),
                          "internal Module allocation failure leaked bounded binding capacity");
        std::fprintf(stderr, "Clock admission fault-index=%d end rollback-oracles=%d\n", index, static_cast<int>(passed));
        std::fflush(stderr);
    }
    // Two ownership reserves, binding storage growth, and the definition map node
    // each allocate in the controlled register_feature region.
    passed &= require(reached_success && injected_failures >= 4,
                      "allocation sweep did not exercise ownership, binding and definition allocation failures");
    return passed;
}

bool test_module_partial_registration_rolls_back_only_new_bindings() {
    admission::ClockHostLimits limits{};
    limits.max_bindings = 2;
    const auto host = admission::make_clock_host_context(limits);
    if (!require(host, "Module rollback host creation failed")) return false;
    FeatureRegistry registry(host.value);
    aeg::reflection::TypeRegistry types;
    aeg::module::ModuleRegistry modules(registry, &types);
    const auto retained = definition(FeatureDefinitionId("Rollback.Retained"), ClockId("Rollback.OldClock"));
    const auto transient = definition(FeatureDefinitionId("Rollback.Transient"), ClockId("Rollback.NewClock"));
    const auto type = aeg::reflection::TypeId("Rollback.TransientType");
    const auto module = aeg::module::ModuleId("Rollback.FailingModule");
    if (!admit_and_register(registry, retained)) return false;
    const auto old_digest = registry.contract_digest();
    bool passed = true;
    for (unsigned attempt = 0; attempt != 8; ++attempt) {
        ModuleProbe probe{};
        const auto failed = modules.register_module([&] {
            return std::make_unique<ClockModule>(module, transient, type, probe, retained);
        });
        passed &= require(!failed && probe.first_registered && probe.second_attempted,
                          "Module did not reach the intended second-Feature registration failure");
        passed &= require(!modules.contains(module) && !registry.find_definition(transient.id) &&
                          !types.contains(type) && registry.find_definition(retained.id) &&
                          registry.contract_digest() == old_digest,
                          "failed Module retained partial Feature/type state or changed last-good state");
        passed &= require(!registry.register_definition(transient),
                          "failed Module left its new Clock binding authorized");
    }
    ModuleProbe allocation_probe{};
    const auto allocation_failure = modules.register_module([&] {
        return std::make_unique<ClockModule>(module, transient, type, allocation_probe,
                                              FeatureDefinition{}, true);
    });
    passed &= require(!allocation_failure && allocation_probe.first_registered &&
                      !modules.contains(module) && !registry.find_definition(transient.id) &&
                      !types.contains(type) && registry.contract_digest() == old_digest &&
                      !registry.register_definition(transient),
                      "Module allocation exception after first Feature did not roll back its new binding");
    // With only two slots, a leaked failed-transaction binding blocks this probe.
    const auto capacity_probe = definition(FeatureDefinitionId("Rollback.CapacityProbe"), ClockId("Rollback.CapacityClock"));
    passed &= require(admission::admit_clock_request(registry, capacity_probe.contract),
                      "repeated failed Modules leaked Clock binding capacity");
    auto old_binding_probe = retained;
    old_binding_probe.id = old_binding_probe.contract.definition_id = FeatureDefinitionId("Rollback.OldBindingProbe");
    passed &= require(registry.register_definition(old_binding_probe),
                      "rollback deleted a pre-existing shared Clock binding");
    return passed;
}

bool test_module_unload_preserves_host_binding_and_context() {
    auto host = admission::make_clock_host_context(admission::ClockHostLimits{});
    if (!require(host, "Module lifecycle host creation failed")) return false;
    std::weak_ptr<const admission::ClockHostContext> weak_host = host.value;
    FeatureRegistry registry(host.value);
    host.value.reset();
    aeg::module::ModuleRegistry modules(registry);
    ModuleProbe first_probe{}, second_probe{};
    const auto first_id = aeg::module::ModuleId("Shared.FirstModule");
    const auto second_id = aeg::module::ModuleId("Shared.SecondModule");
    const auto first = definition(FeatureDefinitionId("Shared.First"), ClockId("Shared.HostClock"));
    const auto second = definition(FeatureDefinitionId("Shared.Second"), ClockId("Shared.HostClock"));
    if (!require(modules.register_module([&] {
            return std::make_unique<ClockModule>(first_id, first, aeg::reflection::TypeId{}, first_probe);
        }), "first shared-Clock Module failed") ||
        !require(modules.register_module([&] {
            return std::make_unique<ClockModule>(second_id, second, aeg::reflection::TypeId{}, second_probe);
        }), "second shared-Clock Module failed")) return false;
    bool passed = require(modules.unregister_module(first_id), "first shared-Clock Module unload failed");
    passed &= require(!registry.find_definition(first.id) && registry.find_definition(second.id) &&
                      first_probe.unloads == 1 && !weak_host.expired(),
                      "Module unload changed another Feature or retired the host context");
    passed &= require(modules.unregister_module(second_id), "second shared-Clock Module unload failed");
    passed &= require(!registry.find_definition(second.id) && second_probe.unloads == 1 && !weak_host.expired(),
                      "last Module unload retained its Feature or retired the host context");
    auto after_unload = first;
    after_unload.id = after_unload.contract.definition_id = FeatureDefinitionId("Shared.AfterAllModulesUnloaded");
    passed &= require(registry.register_definition(after_unload),
                      "successful Module unload deleted a host-lifetime Clock binding");
    return passed;
}

bool test_explicit_binding_rollback_preserves_referenced_binding() {
    admission::ClockHostLimits limits{};
    limits.max_bindings = 2;
    const auto host = admission::make_clock_host_context(limits);
    if (!require(host, "explicit binding rollback host creation failed")) return false;
    FeatureRegistry registry(host.value);
    const auto retained = definition(FeatureDefinitionId("Binding.Retained"), ClockId("Binding.RetainedClock"));
    const auto unused = definition(FeatureDefinitionId("Binding.Unused"), ClockId("Binding.UnusedClock"));
    const auto old_binding = admission::admit_clock_request(registry, retained.contract);
    if (!require(old_binding, "retained binding approval failed") ||
        !require(registry.register_definition(retained), "retained binding registration failed")) return false;
    const auto new_binding = admission::admit_clock_request(registry, unused.contract);
    if (!require(new_binding, "unused binding approval failed")) return false;
    const std::vector<admission::ClockBindingKey> keys{old_binding.value.key, new_binding.value.key};
    bool passed = require(admission::rollback_new_clock_bindings(registry, keys), "binding rollback failed");
    passed &= require(!registry.register_definition(unused), "rollback retained an unused new binding");
    auto peer = retained;
    peer.id = peer.contract.definition_id = FeatureDefinitionId("Binding.RetainedPeer");
    passed &= require(registry.register_definition(peer), "binding rollback deleted a referenced binding");
    return passed;
}
}  // namespace

int main() {
#if defined(_WIN32)
    _set_error_mode(_OUT_TO_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
#endif
    if (!clock_allocation_fault::initialize_debug_proxy_filter()) {
        std::fprintf(stderr, "Clock allocation Debug proxy filter fixture initialization failed\n");
        std::fflush(stderr);
        return 2;
    }
    const struct {
        const char* name;
        bool (*run)();
    } cases[]{
        {"host approval", test_host_context_and_explicit_approval_are_required},
        {"host limits", test_host_limits_and_binding_capacity},
        {"invalid host limits", test_invalid_host_limits_are_not_unbounded_defaults},
        {"native unknown enum admission", test_native_unknown_enums_cannot_bypass_host_budget},
        {"shared schedule", test_shared_schedule_fields_and_replacement},
        {"actual leaf workload", test_actual_leaf_workload_and_call_classification},
        {"deadline/domain workload", test_minimum_deadline_and_cross_clock_domain_budget},
        {"Tooling background approval", test_tooling_background_requires_specific_host_approval},
        {"Module rollback", test_module_partial_registration_rolls_back_only_new_bindings},
        {"internal Module allocation rollback", test_internal_module_allocation_failures_leave_no_new_authority},
        {"Module lifecycle", test_module_unload_preserves_host_binding_and_context},
        {"explicit binding rollback", test_explicit_binding_rollback_preserves_referenced_binding},
    };
    int failures = 0;
    unsigned completed = 0;
    for (const auto& test : cases) {
        std::fprintf(stderr, "Clock admission case begin name=%s\n", test.name);
        std::fflush(stderr);
        try {
            const bool passed = test.run();
            failures += !passed;
            std::fprintf(stderr, "Clock admission case end name=%s passed=%d\n", test.name, static_cast<int>(passed));
            std::fflush(stderr);
        } catch (const std::exception& exception) {
            std::cerr << test.name << ": unexpected exception: " << exception.what() << '\n';
            ++failures;
            std::fprintf(stderr, "Clock admission case end name=%s caught=standard\n", test.name);
            std::fflush(stderr);
        } catch (...) {
            std::cerr << test.name << ": unexpected non-standard exception\n";
            ++failures;
            std::fprintf(stderr, "Clock admission case end name=%s caught=nonstandard\n", test.name);
            std::fflush(stderr);
        }
        ++completed;
    }
    std::fprintf(stderr, "Clock admission summary completed=%u expected=%u failures=%d\n", completed,
                 static_cast<unsigned>(sizeof(cases) / sizeof(cases[0])), failures);
    std::fflush(stderr);
    if (failures != 0) {
        std::cerr << "Feature Clock admission smoke failed: " << failures << " group(s)\n";
        return 1;
    }
    return 0;
}
