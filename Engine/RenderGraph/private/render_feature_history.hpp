#pragma once

#include <memory>
#include <span>

#include "feature_runtime.hpp"
#include "rhi.hpp"
#include "render_graph_submission.hpp"

namespace aeg::render_graph {
class RenderFeatureHistoryTransaction;
namespace detail {
struct RenderFeatureHistoryRuntimeAccess;
}

/** Stable host-supplied identity that isolates temporal storage per RenderView. */
struct RenderFeatureHistoryKey final {
    uint64_t value{0};
    friend constexpr bool operator==(RenderFeatureHistoryKey, RenderFeatureHistoryKey) = default;
};

struct RenderFeatureHistoryTexture final {
    rhi::TextureHandle texture{};
    std::weak_ptr<const void> lease{};
};

/** Feature-owned GPU memory across graph resets. Device outlives this store and frame leases. */
class RenderFeatureHistory final {
public:
    explicit RenderFeatureHistory(rhi::Device& device);
    ~RenderFeatureHistory();
    RenderFeatureHistory(const RenderFeatureHistory&) = delete;
    RenderFeatureHistory& operator=(const RenderFeatureHistory&) = delete;
    [[nodiscard]] foundation::Result reconcile(std::span<const feature::RenderResourceOwner> owners);
    [[nodiscard]] foundation::Result reconcile(RenderFeatureHistoryKey key,
                                               std::span<const feature::RenderResourceOwner> owners);
    void clear() noexcept;
    [[nodiscard]] size_t size() const noexcept;
    [[nodiscard]] foundation::ResultValue<RenderFeatureHistoryTexture> find_texture(
        feature::FeatureInstanceId instance, feature::RenderResourceId resource) const;
    [[nodiscard]] foundation::ResultValue<RenderFeatureHistoryTexture> find_texture(
        RenderFeatureHistoryKey key, feature::FeatureInstanceId instance, feature::RenderResourceId resource) const;
private:
    friend struct detail::RenderFeatureHistoryRuntimeAccess;
    friend class RenderFeatureHistoryTransaction;
    struct State;
    std::shared_ptr<State> state_;
    [[nodiscard]] bool uses_device(const rhi::Device& device) const noexcept;
    [[nodiscard]] std::shared_ptr<RenderFeatureHistoryTransaction> stage();
    [[nodiscard]] std::shared_ptr<RenderFeatureHistoryTransaction> stage(RenderFeatureHistoryKey key);
};

struct RenderHistoryAllocation final {
    rhi::TextureHandle texture{};
    std::shared_ptr<const void> owner{};
    rhi::TextureDesc description{};
};
class RenderFeatureHistoryTransaction final : public RenderGraphSubmissionObserver {
public:
    struct Impl;
    explicit RenderFeatureHistoryTransaction(std::unique_ptr<Impl> impl);
    ~RenderFeatureHistoryTransaction();
    [[nodiscard]] foundation::Result declare(const feature::RenderResourceOwner& owner,
                                             const feature::RenderTextureDeclaration& declaration);
    [[nodiscard]] foundation::ResultValue<RenderHistoryAllocation> read(const feature::RenderResourceOwner& owner,
                                                                      feature::RenderResourceId resource) const;
    [[nodiscard]] foundation::ResultValue<RenderHistoryAllocation> write(const feature::RenderResourceOwner& owner,
                                                                       feature::RenderResourceId resource);
    [[nodiscard]] foundation::Result retain_declarations(const feature::RenderFeatureOutput& output);
    [[nodiscard]] std::weak_ptr<const void> transaction_domain() const noexcept override;
    [[nodiscard]] foundation::Result prepare() const override;
    void submitted() noexcept override;
    void failed() noexcept override;
private:
    std::unique_ptr<Impl> impl_;
};
}
