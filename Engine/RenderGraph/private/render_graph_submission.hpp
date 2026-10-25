#pragma once
#include <memory>
#include "result.hpp"

namespace aeg::render_graph {
/** Engine-only transaction participant, separate from frame lifetime leases. */
class RenderGraphSubmissionObserver {
public:
    virtual ~RenderGraphSubmissionObserver() = default;
    /** Complete transaction domain; weak control-block identity never retains its store. */
    [[nodiscard]] virtual std::weak_ptr<const void> transaction_domain() const noexcept = 0;
    [[nodiscard]] virtual foundation::Result prepare() const = 0;
    virtual void submitted() noexcept = 0;
    virtual void failed() noexcept = 0;
};
}
