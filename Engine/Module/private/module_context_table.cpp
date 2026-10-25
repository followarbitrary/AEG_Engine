#include "module_context_table.hpp"

#include <atomic>
#include <limits>
#include <new>

namespace aeg::module {
namespace {

std::atomic<uint64_t> next_owner_cookie{1};

[[nodiscard]] const char* kind_name(ModuleContextKind kind) noexcept {
    switch (kind) {
        case ModuleContextKind::kModule:
            return "module";
        case ModuleContextKind::kFeatureDefinition:
            return "feature definition";
        case ModuleContextKind::kFeatureInstance:
            return "feature instance";
        case ModuleContextKind::kProcess:
            return "process";
        case ModuleContextKind::kRenderGraph:
            return "render graph";
        case ModuleContextKind::kRenderWorkItem:
            return "render work item";
        case ModuleContextKind::kReflection:
            return "reflection";
    }
    return "unknown";
}

}  // namespace

ModuleContextTable::ModuleContextTable(uint64_t owner_cookie) noexcept
    : owner_cookie_(owner_cookie != 0 ? owner_cookie : next_owner_cookie.fetch_add(1, std::memory_order_relaxed)) {}

foundation::ResultValue<AegAbiContext> ModuleContextTable::allocate(ModuleContextKind kind) {
    std::lock_guard lock(mutex_);
    uint32_t slot_index = 0;
    for (; slot_index < slots_.size(); ++slot_index) {
        if (!slots_[slot_index].active)
            break;
    }
    if (slot_index == slots_.size()) {
        try {
            slots_.push_back({});
        } catch (const std::bad_alloc&) {
            return {foundation::out_of_memory("module context slot allocation failed"), {}};
        }
    }
    Slot& slot = slots_[slot_index];
    if (slot.generation == std::numeric_limits<uint32_t>::max()) {
        return {foundation::invalid_state("module context generation exhausted"), {}};
    }
    ++slot.generation;
    slot.kind = kind;
    slot.active = true;
    return {foundation::Result::success(),
            AegAbiContext{owner_cookie_, slot_index, slot.generation, static_cast<uint32_t>(kind), 0}};
}

foundation::ResultValue<ModuleContextLease> ModuleContextTable::acquire(ModuleContextKind kind) {
    auto context = allocate(kind);
    if (!context)
        return {context.result, {}};
    return {foundation::Result::success(), ModuleContextLease(*this, context.value, kind)};
}

foundation::Result ModuleContextTable::validate_locked(AegAbiContext context,
                                                       ModuleContextKind expected) const noexcept {
    if (context.owner_cookie != owner_cookie_) {
        return foundation::invalid_state("module context belongs to another owner");
    }
    if (context.reserved != 0 || context.kind != static_cast<uint32_t>(expected)) {
        return foundation::invalid_argument("module context kind or reserved bits are invalid");
    }
    if (context.slot >= slots_.size()) {
        return foundation::invalid_state("module context slot is stale");
    }
    const Slot& slot = slots_[context.slot];
    if (!slot.active || slot.generation != context.generation || slot.kind != expected) {
        return foundation::invalid_state(std::string("module ") + kind_name(expected) + " context is stale");
    }
    return foundation::Result::success();
}

foundation::Result ModuleContextTable::validate(AegAbiContext context, ModuleContextKind expected) const noexcept {
    std::lock_guard lock(mutex_);
    return validate_locked(context, expected);
}

foundation::Result ModuleContextTable::release(AegAbiContext context, ModuleContextKind expected) noexcept {
    std::lock_guard lock(mutex_);
    const foundation::Result result = validate_locked(context, expected);
    if (!result)
        return result;
    slots_[context.slot].active = false;
    return foundation::Result::success();
}

std::size_t ModuleContextTable::active_count() const noexcept {
    std::lock_guard lock(mutex_);
    std::size_t count = 0;
    for (const Slot& slot : slots_) {
        if (slot.active)
            ++count;
    }
    return count;
}

void ModuleContextLease::reset() noexcept {
    if (table_ != nullptr) {
        static_cast<void>(table_->release(context_, kind_));
        table_ = nullptr;
    }
}

ModuleContextLease::~ModuleContextLease() {
    reset();
}

ModuleContextLease::ModuleContextLease(ModuleContextLease&& other) noexcept
    : table_(other.table_), context_(other.context_), kind_(other.kind_) {
    other.table_ = nullptr;
}

ModuleContextLease& ModuleContextLease::operator=(ModuleContextLease&& other) noexcept {
    if (this != &other) {
        reset();
        table_ = other.table_;
        context_ = other.context_;
        kind_ = other.kind_;
        other.table_ = nullptr;
    }
    return *this;
}

}  // namespace aeg::module
