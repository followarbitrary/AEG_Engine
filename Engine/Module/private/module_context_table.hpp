#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "../abi/module_api.h"
#include "result.hpp"

namespace aeg::module {

/** @brief Kinds accepted by the checked module context table. */
enum class ModuleContextKind : uint32_t {
    kModule = 1,
    kPipeline = 2,
    kFeature = 3,
    kTask = 4,
    kRenderGraph = 5,
    kRenderPass = 6,
    kReflection = 7,
};

class ModuleContextTable;

/** @brief A scoped borrow that keeps one context slot live until release. */
class ModuleContextLease final {
public:
    ModuleContextLease() noexcept = default;
    ~ModuleContextLease();
    ModuleContextLease(const ModuleContextLease&) = delete;
    ModuleContextLease& operator=(const ModuleContextLease&) = delete;
    ModuleContextLease(ModuleContextLease&& other) noexcept;
    ModuleContextLease& operator=(ModuleContextLease&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return table_ != nullptr; }
    [[nodiscard]] const AegAbiContext& context() const noexcept { return context_; }

private:
    ModuleContextLease(ModuleContextTable& table, AegAbiContext context, ModuleContextKind kind) noexcept
        : table_(&table), context_(context), kind_(kind) {}

    void reset() noexcept;

    ModuleContextTable* table_{};
    AegAbiContext context_{};
    ModuleContextKind kind_{ModuleContextKind::kModule};

    friend class ModuleContextTable;
};

/**
 * @brief Host-owned checked table for opaque C ABI contexts.
 *
 * Context values contain no addresses. Every operation checks the table's
 * owner cookie, slot generation, active state, and expected kind.
 */
class ModuleContextTable final {
public:
    explicit ModuleContextTable(uint64_t owner_cookie = 0) noexcept;
    ~ModuleContextTable() = default;

    ModuleContextTable(const ModuleContextTable&) = delete;
    ModuleContextTable& operator=(const ModuleContextTable&) = delete;

    [[nodiscard]] uint64_t owner_cookie() const noexcept { return owner_cookie_; }
    [[nodiscard]] foundation::ResultValue<AegAbiContext> allocate(ModuleContextKind kind);
    [[nodiscard]] foundation::ResultValue<ModuleContextLease> acquire(ModuleContextKind kind);
    [[nodiscard]] foundation::Result validate(AegAbiContext context, ModuleContextKind expected) const noexcept;
    [[nodiscard]] foundation::Result release(AegAbiContext context, ModuleContextKind expected) noexcept;
    [[nodiscard]] std::size_t active_count() const noexcept;

private:
    struct Slot final {
        uint32_t generation{0};
        ModuleContextKind kind{ModuleContextKind::kModule};
        bool active{false};
    };

    [[nodiscard]] foundation::Result validate_locked(AegAbiContext context, ModuleContextKind expected) const noexcept;

    uint64_t owner_cookie_{};
    mutable std::mutex mutex_{};
    std::vector<Slot> slots_{};
};

}  // namespace aeg::module
