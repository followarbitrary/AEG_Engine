#pragma once

#include <array>
#include <memory>
#include <optional>
#include <vector>

#include "concurrency.hpp"
#include "feature_contract.hpp"
#include "feature_time_source.hpp"

namespace aeg::feature {
/** Owns every callback input; workers never refer to Runtime containers. */
struct FeatureInvocationWork final {
    FeatureInstanceId instance{};
    FeatureDefinition definition{};
    InvocationToken token{};
    InvocationToken origin{};
    StateSnapshot state{};
    std::vector<uint8_t> input{};
    std::vector<uint8_t> parameters{};
    std::vector<PortMessage> messages{};
    uint32_t attempt{1};
    std::shared_ptr<detail::FeatureTimeSource> time_source{};
    TransportTiming timing{};
    std::optional<TransportTimePoint> callback_started{}, callback_finished{};
    std::optional<foundation::ResultValue<StateDelta>> result{};

    void execute();
};

/** Five separate adapters; the generic lane is never a Feature owner ID. */
class FeatureExecution final {
public:
    FeatureExecution() noexcept : owner_(concurrency::current_thread_id()) {}
    ~FeatureExecution();
    [[nodiscard]] bool on_owner_thread() const noexcept;
    [[nodiscard]] bool busy(FeatureInstanceId instance) const noexcept;
    [[nodiscard]] size_t pending_count() const noexcept { return pending_.size(); }
    [[nodiscard]] foundation::Result submit(std::shared_ptr<FeatureInvocationWork> work);
    [[nodiscard]] std::vector<std::shared_ptr<FeatureInvocationWork>> take_ready();
    void cancel(FeatureInstanceId instance) noexcept;
    void cancel_all() noexcept;
    void shutdown();

private:
    struct Pending final {
        std::shared_ptr<FeatureInvocationWork> work{};
        concurrency::CancellationSource cancellation{};
        concurrency::Completion completion{};
    };
    concurrency::ThreadId owner_{};
    std::array<std::unique_ptr<concurrency::WorkerExecutor>, 5> executors_{};
    std::vector<Pending> pending_{};
};
}
