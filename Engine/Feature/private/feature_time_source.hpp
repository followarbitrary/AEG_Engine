#pragma once
#include <chrono>
#include <exception>
#include <functional>
#include <mutex>
#include "concurrency.hpp"
#include "feature_time.hpp"

namespace aeg::feature::detail {
/** Shared with workers; never contains a Runtime/container pointer. */
class FeatureTimeSource final {
public:
    explicit FeatureTimeSource(std::function<uint64_t()> source)
        : source_(std::move(source)), domain_(concurrency::allocate_unique_identity()) {}
    [[nodiscard]] foundation::ResultValue<TransportTimePoint> read() const {
        std::lock_guard lock(mutex_);
        try {
            const auto value = source_ ? source_() : static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            if (last_ && value < *last_)
                return {foundation::Result::failure(foundation::ResultCode::kInvalidState,
                    "Runtime monotonic time source moved backwards"), {}};
            last_ = value;
            return {foundation::Result::success(), {domain_, value}};
        } catch (const std::exception& error) {
            return {foundation::unknown_exception(error), {}};
        } catch (...) {
            return {foundation::Result::failure(foundation::ResultCode::kUnknown,
                "Runtime time source threw an exception"), {}};
        }
    }
private:
    std::function<uint64_t()> source_;
    RuntimeGeneration domain_;
    mutable std::mutex mutex_;
    mutable std::optional<uint64_t> last_;
};
}
